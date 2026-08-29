/*
 * SPDX-License-Identifier: BSD-4-Clause-UC
 *
 * Copyright (c) 1983 Regents of the University of California.
 * Copyright (c) 1999-2009 H. Peter Anvin
 * Copyright (c) 2011-2014 Intel Corporation; author: H. Peter Anvin
 * All rights reserved.
 */

#include "config.h"             /* Must be included first */
#include "tftpd.h"
#include "path.h"
#include "strlist.h"

/*
 * Trivial file transfer protocol server.
 *
 * This version includes many modifications by Jim Guyton <guyton@rand-unix>
 */

#include <signal.h>
#include <ctype.h>
#include <pwd.h>
#include <limits.h>
#include <syslog.h>

#include "common/tftpsubs.h"
#include "common/pollset.h"
#include "recvfrom.h"
#include "remap.h"
#ifdef HAVE_PTHREAD
#include "tftpio.h"
#endif

#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>          /* Necessary for FIONBIO on Solaris */
#endif

#ifdef HAVE_IPV6
static int ai_fam = AF_UNSPEC;
#else
static int ai_fam = AF_INET;
#endif

#define	TIMEOUT 1000000         /* Default timeout (us) */
#define TRIES   6               /* Number of attempts to send each packet */
#define TIMEOUT_LIMIT ((1 << TRIES)-1)

/* Default daemon wait timeout when not running standalone */
#define DEFAULT_WAITTIME	(900*1000000)

static int peer;
static unsigned long timeout  = TIMEOUT;        /* Current timeout value */
static unsigned long rexmtval = TIMEOUT;       /* Basic timeout value */
static unsigned long maxtimeout = TIMEOUT_LIMIT * TIMEOUT;
static int timeout_quit = 0;
static sigjmp_buf timeoutbuf;
static uint16_t rollover_val = 0;

#define	PKTSIZE	MAX_SEGSIZE+4
#define IO_RING_MIN_BYTES (256U * 1024U)
#define MAX_MAX_WINDOWSIZE	32768	/* More than this gets dangerous */
#ifndef MAX_WINDOWSIZE
# define MAX_WINDOWSIZE		MAX_MAX_WINDOWSIZE
#endif
#ifndef MAX_WINDOWBYTES
# define MAX_WINDOWBYTES 0
#endif
#if MAX_WINDOWSIZE < 1
# error MAX_WINDOWSIZE must be at least 1
#endif
static char buf[PKTSIZE];
static char ackbuf[PKTSIZE];
static unsigned int max_blksize = MAX_SEGSIZE;
static unsigned int max_windowsize = MAX_WINDOWSIZE;
static uintmax_t max_windowbytes = MAX_WINDOWBYTES;
static unsigned int windowsize = 1;
static uintmax_t requested_windowsize;

static char tmpbuf[INET6_ADDRSTRLEN], *tmp_p;

static union sock_addr from;
static off_t tsize;
static int tsize_ok;

static int ndirs;
static const char * const **dirs;

static int secure = 0;
int cancreate = 0;
int unixperms = 0;
int portrange = 0;
static int reject_all_options;
unsigned int portrange_from, portrange_to;
int verbosity = 0;

#ifdef WITH_REGEX
static struct rule *rewrite_rules = NULL;
static void rewrite_test(FILE *);
#endif

static FILE *file;

int tftp(struct tftphdr *, int);
static void nak(int, const char *);
static void timer(int);
static void do_opt(const char *, const char *, char **);
static void negotiate_windowsize(char **);
#ifdef HAVE_PTHREAD
static unsigned int io_ring_slots(void);
#endif

static int set_blksize(uintmax_t *);
static int set_blksize2(uintmax_t *);
static int set_tsize(uintmax_t *);
static int set_timeout(uintmax_t *);
static int set_utimeout(uintmax_t *);
static int set_rollover(uintmax_t *);
static int set_windowsize(uintmax_t *);

struct options {
    const char *o_opt;
    int (*o_fnc)(uintmax_t *);
} options[] = {
    {"blksize",  set_blksize},
    {"blksize2", set_blksize2},
    {"tsize",    set_tsize},
    {"timeout",  set_timeout},
    {"utimeout", set_utimeout},
    {"rollover", set_rollover},
    {"windowsize", set_windowsize},
    {NULL, NULL}
};

/* Signal handlers: just set a variable and return */
static volatile sig_atomic_t reload_signal = 0;
static volatile sig_atomic_t exit_signal = 0;

static void handle_exit(int sig)
{
    exit_signal = sig;
}
static void handle_reload(int sig)
{
    reload_signal = sig;
}


/* Handle timeout signal or timeout event */
static void timer(int sig)
{
    (void)sig;                  /* Suppress unused warning */
    timeout <<= 1;
    if (timeout >= maxtimeout || timeout_quit)
        exit(0);
    siglongjmp(timeoutbuf, 1);
}

static const char *prio_name(int priority)
{
    switch (priority) {
    case LOG_EMERG:
        return "emergency: ";
    case LOG_ALERT:
        return "alert: ";
    case LOG_CRIT:
        return "critical: ";
    case LOG_ERR:
        return "error: ";
    case LOG_WARNING:
        return "warning: ";
    case LOG_NOTICE:
        return "notice: ";
    case LOG_INFO:
        return "info: ";
    case LOG_DEBUG:
        return "debug: ";
    default:
        return "";
    }
}

static void tftpd_log_stderr(int priority, const char *fmt, ...)
{
    va_list ap;

    fputs(prio_name(priority), stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    putc('\n', stderr);
}

static void tftpd_reopenlog_stderr(void)
{
    fflush(stderr);             /* Should normally be a noop */
}

log_func tftpd_log = tftpd_log_stderr;
static void (*tftpd_reopenlog)(void) = tftpd_reopenlog_stderr;

static void tftpd_openlog(void);

static void tftpd_reopenlog_syslog(void)
{
    closelog();
    tftpd_openlog();
}
static void tftpd_openlog(void)
{
    openlog(_progname, LOG_PID | LOG_NDELAY, LOG_DAEMON);
    tftpd_log = syslog;
    tftpd_reopenlog = tftpd_reopenlog_syslog;
}

#ifdef WITH_REGEX
static struct rule *read_remap_rules(const char *rulefile)
{
    FILE *f;
    struct rule *rulep;

    f = fopen(rulefile, "rt");
    if (!f) {
        tftpd_log(LOG_ERR, "Cannot open map file: %s: %m", rulefile);
        exit(EX_NOINPUT);
    }
    rulep = parserulefile(f);
    fclose(f);

    return rulep;
}
#endif

/*
 * Rules for locking files; return 0 on success, -1 on failure
 */
static int lock_file(int fd, int lock_write)
{
    (void)lock_write;
#if defined(HAVE_FCNTL) && HAVE_DECL_F_SETLK
  struct flock fl;

  fl.l_type   = lock_write ? F_WRLCK : F_RDLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start  = 0;
  fl.l_len    = 0;		/* Whole file */
  return fcntl(fd, F_SETLK, &fl);
#elif defined(HAVE_FLOCK) && HAVE_DECL_LOCK_SH && HAVE_DECL_LOCK_EX
  return flock(fd, lock_write ? LOCK_EX|LOCK_NB : LOCK_SH|LOCK_NB);
#else
  return 0;			/* Hope & pray... */
#endif
}


/*
 * Receive packet with synchronous timeout; timeout is adjusted
 * to account for time spent waiting.
 */
static int recv_time(int s, void *rbuf, int len, unsigned int flags,
                     unsigned long *timeout_us_p)
{
    struct timeval t0, t1;
    int rv, err = errno;
    intmax_t timeout_us = *timeout_us_p;
    intmax_t timeout_left, dt;
    struct pollset *set = pollset_add(NULL, s);

    gettimeofday(&t0, NULL);
    timeout_left = timeout_us;

    do {
        do {
            rv = pollset_poll(set, POLLSET_IN, timeout_left);
            err = errno;

            gettimeofday(&t1, NULL);

            dt = (t1.tv_sec - t0.tv_sec) * (intmax_t)1000000 +
		 (t1.tv_usec - t0.tv_usec);
            *timeout_us_p = timeout_left =
                (dt >= timeout_us) ? 1 : (timeout_us - dt);
        } while (rv == -1 && err == EINTR);

        if (rv == 0) {
            timer(0);           /* Should not return */
            rv = -1;
            break;
        }

#ifdef MSG_DONTWAIT
        rv = recv(s, rbuf, len, flags | MSG_DONTWAIT);
        err = errno;
#else
        set_socket_nonblock(s, 1);
        rv = recv(s, rbuf, len, flags);
        err = errno;
        set_socket_nonblock(s, 0);
#endif
    } while (rv < 0 && (E_WOULD_BLOCK(err) || err == EINTR));

    pollset_free(&set);
    errno = err;
    return rv;
}

static void tftpd_out_of_memory(void)
{
    tftpd_log(LOG_ERR, "fatal error: %m");
    exit(EX_OSERR);
}

static long getenv_ulong(const char *var)
{
    const char *val = getenv(var);
    char *ep;
    unsigned long n;

    if (!val || !*val)
        return -1;

    errno = 0;
    n = strtoul(val, &ep, 0);
    if (errno || ep == val || *ep || n > LONG_MAX)
        return -1;

    return n;
}

enum long_only_options {
    OPT_VERBOSITY	= 256,
    OPT_STDERR,
    OPT_MAP_TEST,
    OPT_MAP_STEPS,
    OPT_SYSTEMD,
    OPT_WINDOW_BYTES,
    OPT_REJECT_ALL
};

static struct option long_options[] = {
    { "ipv4",        0, NULL, '4' },
    { "ipv6",        0, NULL, '6' },
    { "create",      0, NULL, 'c' },
    { "secure",      0, NULL, 's' },
    { "chroot",      0, NULL, 's' },
    { "permissive",  0, NULL, 'p' },
    { "verbose",     0, NULL, 'v' },
    { "verbosity",   1, NULL, OPT_VERBOSITY },
    { "version",     0, NULL, 'V' },
    { "listen",      0, NULL, 'l' },
    { "foreground",  0, NULL, 'L' },
    { "address",     1, NULL, 'a' },
    { "blocksize",   1, NULL, 'B' },
    { "windowsize",  1, NULL, 'W' },
    { "window-bytes", 1, NULL, OPT_WINDOW_BYTES },
    { "user",        1, NULL, 'u' },
    { "umask",       1, NULL, 'U' },
    { "refuse",      1, NULL, 'r' },
    { "reject-all",  0, NULL, OPT_REJECT_ALL },
    { "timeout",     1, NULL, 't' },
    { "retransmit",  1, NULL, 'T' },
    { "port-range",  1, NULL, 'R' },
    { "ports",       1, NULL, 'R' },
    { "service",     1, NULL, 'S' },
    { "port",        1, NULL, 'S' },
    { "map-file",    1, NULL, 'm' },
    { "map-steps",   1, NULL, OPT_MAP_STEPS },
    { "pidfile",     1, NULL, 'P' },
    { "stderr",      0, NULL, OPT_STDERR },
    { "map-test",    1, NULL, OPT_MAP_TEST },
    { "systemd",     0, NULL, OPT_SYSTEMD },
    { NULL, 0, NULL, 0 }
};
static const char short_options[] = "46cspvVlLa:B:W:u:U:r:t:T:R:S:m:P:";

static struct pollset *listen_set;

static void close_listen_set(void)
{
    if (listen_set)
        pollset_close(&listen_set);
}

int main(int argc, char **argv)
{
    struct tftphdr *tp;
    struct passwd *pw;
    struct options *opt;
    union sock_addr myaddr;
    int n;
    int fd = -1;
    int standalone = 0;         /* Standalone (listen) mode */
    int nodaemon = 0;           /* Do not detach process */
    int systemd = 0;            /* Not using systemd socket activation */
    pid_t pid;
    mode_t my_umask = 0;
    int spec_umask = 0;
    int c;
    int setrv;
    int die;
    intmax_t waittime = -1;      /* No waittime specified (yet) */
    const char *user = "nobody";   /* Default user */
    char *ep;
    int use_stderr = 0;
    const char *map_test_file = NULL;
#ifdef WITH_REGEX
    char *rewrite_file = NULL;
#endif
    const char *pidfile = NULL;
    uint16_t tp_opcode;
    bool patherr;
    pollset_cursor cursor;
    struct strlist listen_addrs;
    int nullfd;

#ifdef HAVE_LOCALE_H
    setlocale(LC_CTYPE, "");     /* For to(w)(lower|upper)() */
#endif

    set_progname(argv[0]);
    out_of_memory = tftpd_out_of_memory;

    /* rand() is used for TFTP backoff; it doesn't have to be good */
    srand(time(NULL) ^ getpid());

    listen_set = pollset_new();
    atexit(close_listen_set);

    strlist_init(&listen_addrs);

    /*
     * This creates a /dev/null file descriptor, and backfills any
     * standard file descriptors left unopened with that descriptor.
     */
    nullfd = get_nullfd();
    if (nullfd < 0) {
        tftpd_log(LOG_ERR, "opening %s failed: %s",
                  _PATH_DEVNULL, strerror(errno));
        exit(EX_OSFILE);
    }

    while ((c = getopt_long(argc, argv, short_options, long_options, NULL))
           != -1)
        switch (c) {
        case '4':
            ai_fam = AF_INET;
            break;
#ifdef HAVE_IPV6
        case '6':
            ai_fam = AF_INET6;
            break;
#endif
        case 'c':
            cancreate = 1;
            break;
        case 's':
            secure = 1;
            break;
        case 'p':
            unixperms = 1;
            break;
        case 'l':
            standalone = 1;
            break;
        case 'L':
            standalone = 1;
            nodaemon = 1;
            break;
        case 'a':
            standalone = 1;
            strlist_add(&listen_addrs, optarg);
            break;
        case 't':
            waittime = strtoul(optarg, NULL, 10) * (intmax_t)1000000;
            break;
        case 'S':
            if (!optarg || !*optarg) {
                tftpd_log(LOG_ERR, "Missing service name");
                exit(EX_USAGE);
            }
            default_service = optarg;
            break;
        case 'B':
            {
                char *vp;
                max_blksize = (unsigned int)strtoul(optarg, &vp, 10);
                if (max_blksize < 512 || max_blksize > MAX_SEGSIZE || *vp) {
                    tftpd_log(LOG_ERR,
                           "Bad maximum blocksize value (range 512-%d): %s",
                           MAX_SEGSIZE, optarg);
                    exit(EX_USAGE);
                }
            }
            break;
        case 'W':
            {
                char *vp;
                long value = strtol(optarg, &vp, 10);

                if (*optarg == '\0' || *vp || value < 0) {
                    tftpd_log(LOG_ERR,
                              "Invalid maximum windowsize value: %s\n", optarg);
                    exit(EX_USAGE);
                }

                if (value < 1) {
                    /* Treat -W 0 as -W 1 */
                    value = 1;
                } else if (value > MAX_MAX_WINDOWSIZE) {
                    value = MAX_MAX_WINDOWSIZE;
                    tftpd_log(LOG_WARNING,
                              "Bad maximum windowsize value: %s "
                              "(valid range 1-%u, capping at %ld)",
                              optarg, (unsigned int)MAX_MAX_WINDOWSIZE, value);
                }
                max_windowsize = (unsigned int)value;
            }
            break;
        case OPT_WINDOW_BYTES:
            {
                char *vp;

                errno = 0;
                max_windowbytes = strtoumax(optarg, &vp, 10);
                if (errno || *optarg == '\0' || *vp) {
                    tftpd_log(LOG_ERR, "Bad window-bytes value: %s", optarg);
                    exit(EX_USAGE);
                }
            }
            break;
        case 'T':
            {
                char *vp;
                unsigned long tov = strtoul(optarg, &vp, 10);
                if (tov < 10000UL || tov > 255000000UL || *vp) {
                    tftpd_log(LOG_ERR, "Bad timeout value: %s", optarg);
                    exit(EX_USAGE);
                }
                rexmtval = timeout = tov;
                maxtimeout = rexmtval * TIMEOUT_LIMIT;
            }
            break;
        case 'R':
            {
                if (sscanf(optarg, "%u:%u", &portrange_from, &portrange_to)
                    != 2 || portrange_from > portrange_to
                    || portrange_to >= 65535) {
                    tftpd_log(LOG_ERR, "Bad port range: %s", optarg);
                    exit(EX_USAGE);
                }
                portrange = 1;
            }
            break;
        case 'u':
            user = optarg;
            break;
        case 'U':
            my_umask = strtoul(optarg, &ep, 8);
            if (*ep) {
                tftpd_log(LOG_ERR, "Invalid umask: %s", optarg);
                exit(EX_USAGE);
            }
            spec_umask = 1;
            break;
        case 'r':
            for (opt = options; opt->o_opt; opt++) {
                if (!strcasecmp(optarg, opt->o_opt)) {
                    opt->o_opt = "";    /* Don't support this option */
                    break;
                }
            }
            if (!opt->o_opt) {
                tftpd_log(LOG_ERR, "Unknown option: %s", optarg);
                exit(EX_USAGE);
            }
            break;
        case OPT_REJECT_ALL:
            reject_all_options = 1;
            break;
#ifdef WITH_REGEX
        case 'm':
            if (rewrite_file) {
                tftpd_log(LOG_ERR, "Multiple -m options");
                exit(EX_USAGE);
            }
            rewrite_file = optarg;
            break;
        case OPT_MAP_STEPS:
        {
            unsigned long steps = strtoul(optarg, &ep, 0);
            if (*optarg && !*ep && steps > 0 && steps <= INT_MAX) {
                deadman_max_steps = steps;
            } else {
                tftpd_log(LOG_ERR, "Bad --map-steps option: %s", optarg);
                exit(EX_USAGE);
            }
            break;
        }
        case OPT_MAP_TEST:
            map_test_file = optarg;
            use_stderr = 1;
            break;
#endif
        case 'v':
            verbosity++;
            break;
        case OPT_VERBOSITY:
            verbosity = atoi(optarg);
            break;
        case OPT_STDERR:
            use_stderr = 1;
            break;
        case OPT_SYSTEMD:
            nodaemon = 1;
            systemd = 1;
            break;
        case 'V':
            /* Print configuration to stdout and exit */
            printf("%s\n", TFTPD_CONFIG_STR);
            exit(0);
            break;
        case 'P':
            pidfile = optarg;
            break;
        default:
            tftpd_log(LOG_ERR, "Unknown option: '%c'", optopt);
            break;
        }

    if (!use_stderr)
        tftpd_openlog();

#ifdef WITH_REGEX
    if (rewrite_file)
        rewrite_rules = read_remap_rules(rewrite_file);

    if (map_test_file) {
        FILE *tf = fopen(map_test_file, "r");
        if (!tf) {
            tftpd_log(LOG_ERR, "%s: cannot open map test file: %m",
                      map_test_file);
            exit(EX_NOINPUT);
        }
        rewrite_test(tf);
        fclose(tf);
        exit(0);
    }
#endif

    dirs = xmalloc((argc - optind + 1) * sizeof(char *));
    patherr = false;
    for (ndirs = 0; optind != argc; optind++) {
        const char *path = argv[optind];
        const char * const *pathlist = parse_path(path, !secure);
        if (!pathlist) {
            tftpd_log(LOG_ERR, "invalid path: %s", path);
            patherr = true;
        }
        dirs[ndirs++] = pathlist;
    }
    dirs[ndirs] = NULL;

    if (patherr)
        exit(EX_DATAERR);

    if (!ndirs) {
        tftpd_log(LOG_ERR, "directory list not specified");
        exit(EX_USAGE);
    }

    if (secure) {
        if (ndirs != 1) {
            tftpd_log(LOG_ERR, "-s requires exactly one directory");
            exit(EX_USAGE);
        }

        char *securepath = build_path(dirs[0]);
        if (chdir(securepath)) {
            tftpd_log(LOG_ERR, "%s: %m", securepath);
            exit(EX_NOINPUT);
        }
        free(securepath);
    }

    pw = getpwnam(user);
    if (!pw) {
        tftpd_log(LOG_ERR, "no user %s: %m", user);
        exit(EX_NOUSER);
    }

    if (pidfile && !standalone) {
        tftpd_log(LOG_WARNING, "not in standalone mode, ignoring pid file");
        pidfile = NULL;
    }

    /*
     * If no wait time is specified, set it to infinite if
     * standalone, otherwise to DEFAULT_WAITTIME.
     *
     * If a wait time of 0 is specified, set it to infinite.
     */
    if (waittime < 0)
        waittime = standalone ? -1 : DEFAULT_WAITTIME;
    else if (!waittime)
        waittime = -1;

    /*
     * If we're running standalone, open the listening sockets,
     * daemonize the process and add a pid file if requested.
     */
    if (standalone || systemd) {
        struct liststr *ls;

        if (systemd) {
            int startfd = 3;    /* Fixed by systemd protocol */
            long nfds = getenv_ulong("LISTEN_FDS");
            long listen_pid = getenv_ulong("LISTEN_PID");

            if (standalone) {
                tftpd_log(LOG_ERR, "--systemd is mutually exclusive with the --address, --standalone and --foreground options");
                exit(EX_USAGE);
            }

            if (listen_pid == getpid() && nfds > 0
                && nfds <= INT_MAX-startfd) {
                while (nfds--)
                    pollset_add(listen_set, startfd++);
            } else {
                tftpd_log(LOG_ERR, "--systemd specified, but no file descriptors passed");
                exit(EX_NOINPUT);
            }
        } else {
            if (strlist_isempty(&listen_addrs))
                strlist_add(&listen_addrs, ":");

            for (ls = listen_addrs.list; ls; ls = ls->next)
                listen_to(listen_set, ls->str, ai_fam);
        }

        strlist_free(&listen_addrs);

        if (pollset_isempty(listen_set)) {
            tftpd_log(LOG_ERR, "no listen addresses available");
            exit(EX_NOINPUT);
        }

        /* Daemonize this process */
        /* Note: when running in secure mode (-s), we must not chdir, since
           we are already in the proper directory. */
        if (!nodaemon && daemon(secure, 1) < 0) {
            tftpd_log(LOG_ERR, "cannot daemonize: %m");
            exit(EX_OSERR);
        }
    } else {
        if (pollset_isempty(listen_set)) {
            /*
             * inetd mode: 0 is our socket descriptor. dup() it so it is
             * not one of the special descriptor numbers.
             */
            fd = dup(0);
            if (fd < 0) {
                tftpd_log(LOG_ERR, "dup failed: %s", strerror(errno));
                exit(EX_OSERR);
            }
            pollset_add(listen_set, fd);
        }
    }

    dup2(nullfd, 0);
    dup2(nullfd, 1);
    if (!use_stderr)
        dup2(nullfd, 2);

    if (pidfile) {
        FILE *pf = fopen(pidfile, "w");
        if (!pf) {
            tftpd_log(LOG_ERR, "cannot open pid file '%s' for writing: %m", pidfile);
            pidfile = NULL;
        } else {
            bool err = fprintf(pf, "%d\n", getpid()) < 0;
            err |= ferror(pf);
            err |= fclose(pf);
            if (err)
                tftpd_log(LOG_ERR, "error writing pid file '%s': %m", pidfile);
        }
    }

    cursor = 0;
    while ((fd = pollset_next(listen_set, &cursor, NULL)) >= 0) {
        tftpd_config_socket(fd, 0);
        cygwin_set_socket_nonblock(fd, 0);
    }

    /* This means we don't want to wait() for children */
#ifndef SA_NOCLDWAIT
#define SA_NOCLDWAIT 0
#endif
    set_signal(SIGCHLD, SIG_IGN, SA_NOCLDSTOP | SA_NOCLDWAIT);

    /*
     * These signals are handled synchronously: the handler simply
     * sets a flag, and expect pollset_poll() to return EINTR.
     */
    set_signal(SIGHUP,  standalone ? handle_reload : handle_exit, 0);
    set_signal(SIGTERM, handle_exit, 0);
    set_signal(SIGINT,  handle_exit, 0);

    if (spec_umask || !unixperms)
        umask(my_umask);

    while (1) {
        int what;
        int rv;

        if (exit_signal) {
            if (pidfile && unlink(pidfile)) {
                tftpd_log(LOG_WARNING, "error removing pid file '%s': %m", pidfile);
                exit(EX_OSERR);
            } else {
                exit(0);
            }
	}

        if (reload_signal) {
            reload_signal = 0;
            if (rewrite_file) {
                freerules(rewrite_rules);
                rewrite_rules = read_remap_rules(rewrite_file);
            }
        }

        rv = pollset_poll(listen_set, POLLSET_IN, waittime);
        if (rv == -1 && errno == EINTR)
            continue;           /* Signal caught, reloop */

        if (rv == -1) {
            tftpd_log(LOG_ERR, "listen loop: %m");
            exit(EX_IOERR);
        } else if (rv == 0) {
            exit(0);            /* Timeout, return to inetd */
        }

        cursor = 0;
        what = POLLSET_IN;
        fd = pollset_next(listen_set, &cursor, &what);
        if (fd <= 0)
            continue;

        cygwin_set_socket_nonblock(fd, 1);
        n = myrecvfrom(fd, buf, sizeof(buf), 0, &from, &myaddr);
        cygwin_set_socket_nonblock(fd, 0);

        if (n < 0) {
            if (E_WOULD_BLOCK(errno) || errno == EINTR) {
                continue;       /* Again, from the top */
            } else {
                tftpd_log(LOG_ERR, "recvfrom: %m");
                exit(EX_IOERR);
            }
        }

#ifdef HAVE_IPV6
        if ((from.sa.sa_family != AF_INET) && (from.sa.sa_family != AF_INET6)) {
            tftpd_log(LOG_ERR, "received address was not AF_INET/AF_INET6,"
                   " please check your inetd config");
#else
        if (from.sa.sa_family != AF_INET) {
            tftpd_log(LOG_ERR, "received address was not AF_INET,"
                   " please check your inetd config");
#endif
            exit(EX_PROTOCOL);
        }

        if (standalone) {
            union sock_addr sa;
            socklen_t len = sizeof sa;
            if (((from.sa.sa_family == AF_INET) &&
                 (myaddr.si.sin_addr.s_addr == INADDR_ANY))
#ifdef HAVE_IPV6
                || ((from.sa.sa_family == AF_INET6) &&
                    IN6_IS_ADDR_UNSPECIFIED(&from.s6.sin6_addr))
#endif
                ) {
                /* myrecvfrom() didn't capture the source address; but we might
                   have bound to a specific address, if so we should use it */

                if (!getsockname(fd, &sa.sa, &len) &&
                    sa.sa.sa_family == from.sa.sa_family) {
                    switch (sa.sa.sa_family) {
                    case AF_INET:
                        myaddr.si.sin_addr = sa.si.sin_addr;
                        break;
#ifdef HAVE_IPV6
                    case AF_INET6:
                        myaddr.s6.sin6_addr = sa.s6.sin6_addr;
                        break;
#endif
                    default:
                        break;
                    }
                }
            }
        }

        /*
         * Now that we have read the request packet from the UDP
         * socket, we fork and go back to listening to the socket.
         */
        pid = fork();
        if (pid < 0) {
            tftpd_log(LOG_ERR, "fork: %m");
            exit(EX_OSERR);     /* Return to inetd, just in case */
        } else if (pid == 0)
            break;              /* Child exit, parent loop */
    }

    /* Child process: handle the actual request here */

    /* Ignore SIGHUP; make SIGTERM and SIGINT kill the process */
    set_signal(SIGHUP,  SIG_IGN, 0);
    set_signal(SIGTERM, SIG_DFL, 0);
    set_signal(SIGINT,  SIG_DFL, 0);

    /* Make sure the log socket is still connected.  This has to be
       done before the chroot, while /dev/log is still accessible,
       so depending on the automatic re-opening by syslog() is unsafe. */
    if (secure)
        tftpd_reopenlog();

    /* Close file descriptors we don't need */
    close_listen_set();

    /* Get a socket.  This has to be done before the chroot(), since
       some systems require access to /dev to create a socket. */

    peer = socket(myaddr.sa.sa_family, SOCK_DGRAM, 0);
    if (peer < 0) {
        tftpd_log(LOG_ERR, "socket: %m");
        exit(EX_IOERR);
    }

    /* Set up the supplementary group access list if possible
       /etc/group still need to be accessible at this point.
       If we get EPERM, this is already a restricted process, e.g.
       using user namespaces on Linux. */
    die = 0;
#ifdef HAVE_SETGROUPS
    setrv = setgroups(0, NULL);
    if (setrv && errno != EPERM) {
	tftpd_log(LOG_ERR, "cannot clear group list");
	die = EX_OSERR;
    }
#endif
#ifdef HAVE_INITGROUPS
    setrv = initgroups(user, pw->pw_gid);
    if (!setrv) {
	die = 0;
    } else if (errno != EPERM) {
        tftpd_log(LOG_ERR, "cannot set groups for user %s", user);
	die = EX_OSERR;
    }
#endif
    if (die)
	exit(die);

    /* Chroot and drop privileges */
    if (secure) {
        if (chroot(".")) {
            tftpd_log(LOG_ERR, "chroot: %m");
            exit(EX_OSERR);
        }
#ifdef __CYGWIN__
        chdir("/");             /* Cygwin chroot() bug workaround */
#endif
    }

#ifdef HAVE_SETRESGID
    setrv = setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid);
#elif defined(HAVE_SETREGID)
    setrv = setregid(pw->pw_gid, pw->pw_gid);
#else
    setrv = setegid(pw->pw_gid) || setgid(pw->pw_gid);
#endif
    if (setrv && errno == EPERM) {
	setrv = 0;		/* Assume already restricted by system policy */
    }

#ifdef HAVE_SETRESUID
    setrv = setrv || setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid);
#elif defined(HAVE_SETREUID)
    setrv = setrv || setreuid(pw->pw_uid, pw->pw_uid);
#else
    /* Important: setuid() must come first */
    setrv = setrv || setuid(pw->pw_uid) ||
        (geteuid() != pw->pw_uid && seteuid(pw->pw_uid));
#endif
    if (setrv && errno == EPERM) {
	setrv = 0;		/* Assume already restricted by system policy */
    }

    if (setrv) {
        tftpd_log(LOG_ERR, "cannot drop privileges: %m");
        exit(EX_OSERR);
    }

    /* Process the request... */
    if (pick_port_bind(peer, &myaddr, portrange_from, portrange_to) < 0) {
        tftpd_log(LOG_ERR, "bind: %m");
        exit(EX_IOERR);
    }

    if (connect(peer, &from.sa, SOCKLEN(&from)) < 0) {
        tftpd_log(LOG_ERR, "connect: %m");
        exit(EX_IOERR);
    }

    tftpd_config_socket(peer, 1);

    tp = (struct tftphdr *)buf;
    tp_opcode = ntohs(tp->th_opcode);
    if (tp_opcode == RRQ || tp_opcode == WRQ)
	tftp(tp, n);
    exit(0);
}

static char *rewrite_access(const struct formats *,
			    char *, int, int, const char **);
static int validate_access(char *, int, const struct formats *, const char **);
static void tftp_sendfile(const struct formats *, struct tftphdr *, int);
static void tftp_recvfile(const struct formats *, struct tftphdr *, int);

static const struct formats formats[] = {
    {
    "netascii", rewrite_access, validate_access, tftp_sendfile,
            tftp_recvfile, 1}, {
    "octet", rewrite_access, validate_access, tftp_sendfile,
            tftp_recvfile, 0}, {
    NULL, NULL, NULL, NULL, NULL, 0}
};

/*
 * Handle initial connection protocol.
 */
int tftp(struct tftphdr *tp, int size)
{
    char *cp, *end;
    int argn, ecode;
    const struct formats *pf = NULL;
    char *origfilename;
    char *filename, *mode = NULL;
    const char *errmsgptr;
    uint16_t tp_opcode = ntohs(tp->th_opcode);

    char *val = NULL, *opt = NULL;
    char *ap = ackbuf + 2;

    ((struct tftphdr *)ackbuf)->th_opcode = htons(OACK);
    windowsize = 1;
    requested_windowsize = 0;

    origfilename = cp = (char *)&(tp->th_stuff);
    argn = 0;

    end = (char *)tp + size;

    while (cp < end && *cp) {
        do {
            cp++;
        } while (cp < end && *cp);

        if (cp == end) {
            nak(EBADOP, "Request not null-terminated");
            exit(0);
        }

        argn++;
        if (argn == 1) {
            mode = ++cp;
        } else if (argn == 2) {
            for (cp = mode; *cp; cp++)
                *cp = tolower(*cp);
            for (pf = formats; pf->f_mode; pf++) {
                if (!strcmp(pf->f_mode, mode))
                    break;
            }
            if (!pf->f_mode) {
                nak(EBADOP, "Unknown mode");
                exit(0);
            }
	    file = NULL;
            if (!(filename = (*pf->f_rewrite)
		  (pf, origfilename, tp_opcode, from.sa.sa_family, &errmsgptr))) {
                nak(EACCESS, errmsgptr);        /* File denied by mapping rule */
                exit(0);
            }
            if (verbosity >= 1) {
                tmp_p = (char *)inet_ntop(from.sa.sa_family, SOCKADDR_P(&from),
                                          tmpbuf, INET6_ADDRSTRLEN);
                if (!tmp_p) {
                    tmp_p = tmpbuf;
                    strcpy(tmpbuf, "???");
                }
                if (filename == origfilename
                    || !strcmp(filename, origfilename))
                    tftpd_log(LOG_NOTICE, "%s from %s filename %s\n",
                           tp_opcode == WRQ ? "WRQ" : "RRQ",
                           tmp_p, filename);
                else
                    tftpd_log(LOG_NOTICE,
                           "%s from %s filename %s remapped to %s\n",
                           tp_opcode == WRQ ? "WRQ" : "RRQ",
                           tmp_p, origfilename,
                           filename);
            }
	    /*
	     * If "file" is already set, then a file was already validated
	     * and opened during remap processing.
	     */
	    if (!file) {
		ecode =
		    (*pf->f_validate) (filename, tp_opcode, pf, &errmsgptr);
		if (ecode) {
		    nak(ecode, errmsgptr);
		    exit(0);
		}
	    }
            opt = ++cp;
        } else if (argn & 1) {
            val = ++cp;
        } else {
            do_opt(opt, val, &ap);
            opt = ++cp;
        }
    }

    if (!pf) {
        nak(EBADOP, "Missing mode");
        exit(0);
    }

    negotiate_windowsize(&ap);
    tftp_set_socket_buffers(peer, segsize, windowsize, tp_opcode == RRQ);

    if (ap != (ackbuf + 2)) {
        if (tp_opcode == WRQ)
            (*pf->f_recv) (pf, (struct tftphdr *)ackbuf, ap - ackbuf);
        else
            (*pf->f_send) (pf, (struct tftphdr *)ackbuf, ap - ackbuf);
    } else {
        if (tp_opcode == WRQ)
            (*pf->f_recv) (pf, NULL, 0);
        else
            (*pf->f_send) (pf, NULL, 0);
    }
    exit(0);                    /* Request completed */
}

static int blksize_set;

/*
 * Set a non-standard block size (c.f. RFC2348)
 */
static int set_blksize(uintmax_t *vp)
{
    uintmax_t sz = *vp;

    if (blksize_set)
        return 0;

    if (sz < 8)
        return 0;
    else if (sz > max_blksize)
        sz = max_blksize;

    *vp = segsize = sz;
    blksize_set = 1;
    return 1;
}

/*
 * Set a power-of-two block size (nonstandard)
 */
static int set_blksize2(uintmax_t *vp)
{
    uintmax_t sz = *vp;

    if (blksize_set)
        return 0;

    if (sz < 8)
        return (0);
    else if (sz > max_blksize)
        sz = max_blksize;
    else

    /* Convert to a power of two */
    if (sz & (sz - 1)) {
        unsigned int sz1 = 1;
        /* Not a power of two - need to convert */
        while (sz >>= 1)
            sz1 <<= 1;
        sz = sz1;
    }

    *vp = segsize = sz;
    blksize_set = 1;
    return 1;
}

/*
 * Set the block number rollover value
 */
static int set_rollover(uintmax_t *vp)
{
    uintmax_t ro = *vp;

    if (ro > 65535)
	return 0;

    rollover_val = (uint16_t)ro;
    return 1;
}

/*
 * Set the number of DATA packets sent before an ACK is expected
 * (RFC 7440).  Limit this to the same conservative value exposed by
 * the client.
 */
#define OPTBUFSIZE	(sizeof(uintmax_t) * CHAR_BIT / 3 + 3)

static int set_windowsize(uintmax_t *vp)
{
    if (*vp < 1)
        return 0;

    requested_windowsize = *vp;

    return 1;
}

/*
 * windowsize depends on the negotiated block size, which can appear in
 * either order in an RFC 2347 request.  Add it to the OACK only after all
 * request options have been processed.
 */
static void negotiate_windowsize(char **ap)
{
    uintmax_t window;
    char retbuf[OPTBUFSIZE];
    size_t optlen, retlen;

    if (!requested_windowsize)
        return;

    window = requested_windowsize;
    if (window > max_windowsize)
        window = max_windowsize;
    if (max_windowbytes && window > max_windowbytes / (uintmax_t)segsize)
        window = max_windowbytes / (uintmax_t)segsize;

    if (window < 2)
        return;

    windowsize = (unsigned int)window;
    optlen = sizeof("windowsize");
    retlen = sprintf(retbuf, "%u", windowsize);
    if (*ap + optlen + retlen >= ackbuf + sizeof(ackbuf)) {
        nak(EOPTNEG, "Insufficient space for options");
        exit(0);
    }

    memcpy(*ap, "windowsize", optlen);
    *ap += optlen;
    memcpy(*ap, retbuf, retlen + 1);
    *ap += retlen + 1;
}

#ifdef HAVE_PTHREAD
/*
 * Keep at least one full window for retransmission, then for
 * asynchronous I/O allow for the largest of:
 * 1. one full TFTP window;
 * 2. two full TFTP blocks;
 * 3. IO_RING_MIN_BYTES (256 KiB by default).
 */
static unsigned int io_ring_slots(void)
{
    unsigned int io_slots;

    io_slots = (segsize + IO_RING_MIN_BYTES - 1) / segsize;
    if (io_slots < windowsize)
        io_slots = windowsize;
    if (io_slots < 2)
        io_slots = 2;

    return windowsize + io_slots;
}
#endif

/*
 * Return a file size (c.f. RFC2349)
 * For netascii mode, we don't know the size ahead of time;
 * so reject the option.
 */
static int set_tsize(uintmax_t *vp)
{
    uintmax_t sz = *vp;

    if (!tsize_ok)
        return 0;

    if (sz == 0)
        sz = tsize;

    *vp = sz;
    return 1;
}

/*
 * Set the timeout (c.f. RFC2349).  This is supposed
 * to be the (default) retransmission timeout, but being an
 * integer in seconds it seems a bit limited.
 */
static int set_timeout(uintmax_t *vp)
{
    uintmax_t to = *vp;

    if (to < 1 || to > 255)
        return 0;

    rexmtval = timeout = to * 1000000UL;
    maxtimeout = rexmtval * TIMEOUT_LIMIT;

    return 1;
}

/* Similar, but in microseconds.  We allow down to 10 ms. */
static int set_utimeout(uintmax_t *vp)
{
    uintmax_t to = *vp;

    if (to < 10000UL || to > 255000000UL)
        return 0;

    rexmtval = timeout = to;
    maxtimeout = rexmtval * TIMEOUT_LIMIT;

    return 1;
}

/*
 * Parse RFC2347 style options; we limit the arguments to positive
 * integers which matches all our current options.
 */
static void do_opt(const char *opt, const char *val, char **ap)
{
    struct options *po;
    char retbuf[OPTBUFSIZE];
    char *p = *ap;
    size_t optlen, retlen;
    char *vend;
    uintmax_t v;

    /* Global option-parsing variables initialization */
    blksize_set = 0;

    if (reject_all_options)
        return;

    if (!*opt || !*val)
        return;

    errno = 0;
    v = strtoumax(val, &vend, 10);
    if (*vend || errno == ERANGE)
	return;

    for (po = options; po->o_opt; po++)
        if (!strcasecmp(po->o_opt, opt)) {
            if (po->o_fnc(&v)) {
                if (!strcasecmp(opt, "windowsize"))
                    break;

		optlen = strlen(opt);
		retlen = sprintf(retbuf, "%"PRIuMAX, v);

                if (p + optlen + retlen + 2 >= ackbuf + sizeof(ackbuf)) {
                    nak(EOPTNEG, "Insufficient space for options");
                    exit(0);
                }

		memcpy(p, opt, optlen+1);
		p += optlen+1;
		memcpy(p, retbuf, retlen+1);
		p += retlen+1;
            }
            break;
        }

    *ap = p;
}

#ifdef WITH_REGEX

/*
 * This is called by the remap engine when it encounters macros such
 * as \i. It should put the output in a static buffer and put the
 * buffer address in *output, then return the length of the output
 * not including the terminal null.
 *
 * Return (size_t)-1 for an invalid macro, which then will be handled
 * by the substitution code.
 */
static char hexchar(unsigned char c)
{
    return c >= 10 ? (c + 'A' - 10) : c + '0';
}

static size_t rewrite_macros(char macro, char **output)
{
#ifdef INET6_ADDRSTRLEN
    static char obuf[INET_ADDRSTRLEN > 64 ? INET_ADDRSTRLEN : 64];
#else
    static char obuf[64];
#endif
    const unsigned char *cp;
    size_t bytes;

    *output = obuf;

    switch (from.sa.sa_family) {
    case AF_INET:
        cp = (const unsigned char *)&from.si.sin_addr;
        bytes = 4;
        break;
#ifdef HAVE_IPV6
    case AF_INET6:
        cp = (const unsigned char *)&from.s6.sin6_addr;
        bytes = 16;
        break;
#endif
    default:
        return -1;               /* Unknown address family... */
    }

    switch (macro) {
    case 'i':
    {
        const char *p = inet_ntop(from.sa.sa_family, SOCKADDR_P(&from),
                                  obuf, sizeof obuf);
        return p ? strlen(p) : 0;
    }

    case 'x':
    {
        char *p = obuf;
        while (bytes--) {
            unsigned char c = *cp++;
            *p++ = hexchar(c >> 4);
            *p++ = hexchar(c & 15);
        }
        return (size_t)(p - obuf);
    }

    default:
        return -1;              /* No such macro */
    }
}

/*
 * Modify the filename, if applicable.  If it returns NULL, deny the access.
 */
static char *rewrite_access(const struct formats *pf, char *filename,
			    int mode, int af, const char **msg)
{
    if (rewrite_rules) {
        char *newname =
            rewrite_string(pf, filename, rewrite_rules, mode, af,
                           rewrite_macros, msg);
        filename = newname;
    }
    return filename;
}

static int test_validate_fail(char *filename, int mode,
                              const struct formats *pf,
                              const char **errmsg)
{
    (void)filename;
    (void)mode;
    (void)pf;
    if (errmsg)
        *errmsg = "Just testing...";
    return EACCESS;
}

static void rewrite_test(FILE *tf)
{
    static const struct formats test_dummy_format =
        { "dummy", NULL, test_validate_fail, NULL, NULL, 0 };
#ifdef HAVE_IPV6
    /* Dummy addresses from netblocks assigned for documentation */
    static const char phony_ip6_addr[16] =
        { 0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
          0xfe, 0xed, 0xfa, 0xce, 0xde, 0xad, 0xbe, 0xef };
#endif
    static const char phony_ip4_addr[4] = { 192, 0, 2, 34 };
    int mode = cancreate ? WRQ : RRQ;
    int af = ai_fam;

    memset(&from, 0, sizeof from);

    switch (af) {
#ifdef HAVE_IPV6
    case AF_INET6:
        memcpy(&from.s6.sin6_addr, phony_ip6_addr, 16);
        break;
#endif
    default:
        af = AF_INET;
        memcpy(&from.si.sin_addr, phony_ip4_addr, 4);
        break;
    }
    from.sa.sa_family = af;

    while (fgets(buf, MAX_SEGSIZE+1, tf)) {
        const char *msg;
        char *out;
        char *nl = strchr(buf, '\n');
        if (!nl)
            continue;

        *nl = '\0';
        out = rewrite_string(&test_dummy_format, buf, rewrite_rules,
                             mode, af, rewrite_macros, &msg);

        if (out) {
            printf("%s\n", out);
            if (out != buf)
                xfree(out);
        } else {
            printf("ERROR: %s\n", msg);
        }
    }
}

#else
static char *rewrite_access(const struct formats *pf, char *filename,
			    int mode, int af, const char **msg)
{
    (void)pf;
    (void)mode;                 /* Avoid warning */
    (void)msg;
    (void)af;
    return filename;
}
#endif

/*
 * Validate file access and open the corresponding file.  Returns 0 if
 * OK and the global variable "file" contains an open file handle.  On
 * failure, returns an error code. The error code is negative if it is
 * an errno and strerror() should be used for the error message, or
 * positive if it is a TFTP status code and *errmsg is set.
 *
 * Since we have no uid or gid, for now require
 * file to exist and be publicly readable/writable, unless -p
 * specified.  If we were invoked with arguments from inetd then the
 * file must also be in one of the given directory prefixes.  Note
 * also, full path name must be given as we have no login directory.
 *
 * This function is also responsible for canonicalizing file paths.
 * If "secure" is set the file path is used as-is, as the kernel
 * is expected to enforce any namespace restrictions.
 */
static int validate_access(char *filename, int mode,
			   const struct formats *pf, const char **errmsg)
{
    struct stat stbuf;
    int fd, wmode, rmode;
    const char * const **dirp;
    char stdio_mode[3];

    tsize_ok = 0;
    *errmsg = NULL;

    if (!secure) {
        const char **pathlist = parse_path(filename, true);

        if (!pathlist) {
            *errmsg = "Invalid pathname specified";
            return (EACCESS);
        }

        for (dirp = dirs; *dirp; dirp++) {
            if (compare_paths(pathlist, *dirp) & 2)
                break;
        }
        if (!*dirp) {
            *errmsg = "Forbidden directory";
            return (EACCESS);
        }

        filename = build_path(pathlist);
        free(pathlist);
    }

    /*
     * We use different a different permissions scheme if `cancreate' is
     * set.
     */
    wmode = O_WRONLY | (cancreate ? O_CREAT : 0) | (pf->f_convert ? O_TEXT : O_BINARY);
    rmode = O_RDONLY | (pf->f_convert ? O_TEXT : O_BINARY);

#ifndef HAVE_FTRUNCATE
    wmode |= O_TRUNC;		/* This really sucks on a dupe */
#endif

    fd = open(filename, mode == RRQ ? rmode : wmode, 0666);
    if (fd < 0)
        fd = -errno;
    if (!secure)
        free(filename);
    if (fd < 0)
        return fd;

    if (fstat(fd, &stbuf) < 0)
        exit(EX_OSERR);         /* This shouldn't happen */

    /* A duplicate RRQ or (worse!) WRQ packet could really cause havoc... */
    if (lock_file(fd, mode != RRQ))
	exit(0);                /* Assume a transfer is already underway */

    if (mode == RRQ) {
        if (!unixperms && (stbuf.st_mode & (S_IREAD >> 6)) == 0) {
            *errmsg = "File must have global read permissions";
            return (EACCESS);
        }
        tsize = stbuf.st_size;
        /* We don't know the tsize if conversion is needed */
        tsize_ok = !pf->f_convert;
    } else {
        if (!unixperms) {
            if ((stbuf.st_mode & (S_IWRITE >> 6)) == 0) {
                *errmsg = "File must have global write permissions";
                return (EACCESS);
            }
        }

#ifdef HAVE_FTRUNCATE
	/* We didn't get to truncate the file at open() time */
	if (ftruncate(fd, (off_t) 0)) {
	  *errmsg = "Cannot reset file size";
	  return (EACCESS);
	}
#endif
        tsize = 0;
        tsize_ok = 1;
    }

    stdio_mode[0] = (mode == RRQ) ? 'r' : 'w';
    stdio_mode[1] = (pf->f_convert) ? 't' : 'b';
    stdio_mode[2] = '\0';

    file = fdopen(fd, stdio_mode);
    if (file == NULL)
        exit(EX_OSERR);         /* Internal error */

    return (0);
}

/*
 * Send the requested file.
 */
static void tftp_sendfile(const struct formats *pf, struct tftphdr *oap, int oacklen)
{
    struct tftphdr *dp;
    struct tftphdr *ap;         /* ack packet */
    static uint16_t block = 1;   /* Static to avoid longjmp funnies */
    uint16_t ap_opcode, ap_block;
    unsigned long r_timeout;
    int n;
#ifndef HAVE_PTHREAD
    int size;
#endif
#ifdef HAVE_PTHREAD
    static struct tftpio *io;
#endif

    if (oap) {
        timeout = rexmtval;
        (void)sigsetjmp(timeoutbuf, 1);
      oack:
        r_timeout = timeout;
        if (send(peer, oap, oacklen, 0) != oacklen) {
            tftpd_log(LOG_WARNING, "tftpd: oack: %m\n");
            goto abort;
        }
        for (;;) {
            n = recv_time(peer, ackbuf, sizeof(ackbuf), 0, &r_timeout);
            if (n < 0) {
                tftpd_log(LOG_WARNING, "tftpd: read: %m\n");
                goto abort;
            }
            ap = (struct tftphdr *)ackbuf;
            ap_opcode = ntohs((uint16_t) ap->th_opcode);
            ap_block = ntohs((uint16_t) ap->th_block);

            if (ap_opcode == ERROR) {
                tftpd_log(LOG_WARNING,
                       "tftp: client does not accept options\n");
                goto abort;
            }
            if (ap_opcode == ACK) {
                if (ap_block == 0)
                    break;
                /* Resynchronize with the other side */
                (void)synchnet(peer);
                goto oack;
            }
        }
    }

#ifdef HAVE_PTHREAD
    {
        static unsigned int packet_count;
        static int final;
        uint16_t expected_ack;

        io = tftpio_reader_start(file, pf->f_convert, windowsize,
                                 io_ring_slots(), segsize);
        if (!io) {
            nak(-errno, NULL);
            goto abort;
        }
        for (;;) {
            if (tftpio_reader_window(io, &packet_count, &final) < 0) {
                nak(-errno, NULL);
                goto abort;
            }
            for (n = 0; n < (int)packet_count; n++) {
                dp = tftpio_reader_packet(io, (unsigned int)n);
                dp->th_opcode = htons((uint16_t) DATA);
                dp->th_block = htons((uint16_t)block);
                if (!++block)
                    block = rollover_val;
            }

            timeout = rexmtval;
            (void)sigsetjmp(timeoutbuf, 1);
          resend_window:
            for (n = 0; n < (int)packet_count; n++) {
                dp = tftpio_reader_packet(io, (unsigned int)n);
                if (send(peer, dp, tftpio_reader_length(io, (unsigned int)n),
                         0) != tftpio_reader_length(io, (unsigned int)n)) {
                    tftpd_log(LOG_WARNING, "tftpd: write: %m");
                    goto abort;
                }
            }
            r_timeout = timeout;
            for (;;) {
                n = recv_time(peer, ackbuf, sizeof(ackbuf), 0, &r_timeout);
                if (n < 0) {
                    tftpd_log(LOG_WARNING, "tftpd: read(ack): %m");
                    goto abort;
                }
                ap = (struct tftphdr *)ackbuf;
                ap_opcode = ntohs((uint16_t)ap->th_opcode);
                ap_block = ntohs((uint16_t)ap->th_block);
                if (ap_opcode == ERROR)
                    goto abort;
                expected_ack = ntohs(tftpio_reader_packet(io, packet_count - 1)
                                     ->th_block);
                if (ap_opcode == ACK && ap_block == expected_ack)
                    break;
                if (ap_opcode == ACK)
                    (void)synchnet(peer);
                else if (ap_opcode == OACK)
                    goto resend_window;
            }
            tftpio_reader_release(io);
            if (final)
                break;
        }
    }
  abort:
    tftpio_stop(io);
    io = NULL;
    (void)fclose(file);
#else
    {
        size_t packetsize = ((size_t)segsize + 5) & ~(size_t)1;
        char *packets = xcalloc(windowsize, packetsize);
        int *lengths = xcalloc(windowsize, sizeof(*lengths));
        volatile int packet_count, final;
        uint16_t expected_ack;

        dp = r_init();
        for (;;) {
            packet_count = 0;
            final = 0;
            do {
                size = readit(file, &dp, pf->f_convert);
                if (size < 0) {
                    nak(-errno, NULL);
                    goto abort_packets;
                }
                dp->th_opcode = htons((uint16_t) DATA);
                dp->th_block = htons((uint16_t) block);
                memcpy(packets + (size_t)packet_count * packetsize, dp,
                       (size_t)size + 4);
                lengths[packet_count++] = size + 4;
                read_ahead(file, pf->f_convert);
                if (size != segsize)
                    final = 1;
                if (!++block)
                    block = rollover_val;
            } while (packet_count < (int)windowsize && !final);

            timeout = rexmtval;
            (void)sigsetjmp(timeoutbuf, 1);
          resend_window:
            for (n = 0; n < packet_count; n++) {
                dp = (struct tftphdr *)(packets + (size_t)n * packetsize);
                if (send(peer, dp, lengths[n], 0) != lengths[n]) {
                    tftpd_log(LOG_WARNING, "tftpd: write: %m");
                    goto abort_packets;
                }
            }
            r_timeout = timeout;
            for (;;) {
                n = recv_time(peer, ackbuf, sizeof(ackbuf), 0, &r_timeout);
                if (n < 0) {
                    tftpd_log(LOG_WARNING, "tftpd: read(ack): %m");
                    goto abort_packets;
                }
                ap = (struct tftphdr *)ackbuf;
                ap_opcode = ntohs((uint16_t) ap->th_opcode);
                ap_block = ntohs((uint16_t) ap->th_block);
                if (ap_opcode == ERROR)
                    goto abort_packets;
                expected_ack = ntohs(((struct tftphdr *)
                                      (packets + (size_t)(packet_count - 1) *
                                       packetsize))->th_block);
                if (ap_opcode == ACK && ap_block == expected_ack)
                    break;
                if (ap_opcode == ACK)
                    (void)synchnet(peer);
                else if (ap_opcode == OACK)
                    goto resend_window;
            }
            if (final)
                break;
        }
      abort_packets:
        xfree(packets);
        xfree(lengths);
    }
  abort:
    (void)fclose(file);
#endif
}

/*
 * Receive a file.
 */
static void tftp_recvfile(const struct formats *pf,
			  struct tftphdr *oack, int oacklen)
{
#ifdef HAVE_PTHREAD
    static struct tftphdr *dp;
    static struct tftpio *io;
#else
    struct tftphdr *dp;
#endif
    int n, size, restarted, final;
    /* These are "static" to avoid longjmp funnies */
    static struct tftphdr *oap;
    static struct tftphdr *ap;  /* ack buffer */
    static uint16_t block;
    static uint16_t last_acked;
    static int acksize;
    static unsigned int packets_in_window;
    uint16_t dp_opcode, dp_block;
    unsigned long r_timeout;

    oap = oack;
#ifdef HAVE_PTHREAD
    io = tftpio_writer_start(file, pf->f_convert, io_ring_slots(), segsize);
    if (!io) {
        nak(-errno, NULL);
        goto abort;
    }
    dp = tftpio_writer_reserve(io);
    if (!dp) {
        nak(-errno, NULL);
        goto abort;
    }
#else
    dp = w_init();
#endif
    block = 1;
    last_acked = 0;
    packets_in_window = 0;
    ap = (struct tftphdr *)ackbuf;
    acksize = oacklen;

    for (;;) {
        timeout = rexmtval;
        restarted = sigsetjmp(timeoutbuf, 1);
        if (oap || restarted) {
            if (!oap) {
                ap->th_opcode = htons((uint16_t) ACK);
                ap->th_block = htons(last_acked);
                acksize = 4;
            }
            /*
             * For a WRQ, an OACK replaces ACK 0.  Once DATA 1 has been
             * received, retransmissions use the most recent window ACK.
             */
            r_timeout = timeout;
            if (send(peer, ap, acksize, 0) != acksize) {
                tftpd_log(LOG_WARNING, "tftpd: write(ack): %m");
                goto abort;
            }
#ifndef HAVE_PTHREAD
            write_behind(file, pf->f_convert);
#endif
        }
        r_timeout = timeout;
        for (;;) {
            n = recv_time(peer, dp, PKTSIZE, 0, &r_timeout);
            if (n < 0) {
                tftpd_log(LOG_WARNING, "tftpd: read: %m");
                goto abort;
            }
            dp_opcode = ntohs((uint16_t) dp->th_opcode);
            dp_block = ntohs((uint16_t) dp->th_block);
            if (dp_opcode == ERROR)
                goto abort;
            if (dp_opcode == DATA) {
                if (dp_block == block)
                    break;
                if (dp_block == last_acked) {
                    ap->th_opcode = htons((uint16_t) ACK);
                    ap->th_block = htons(last_acked);
                    (void)send(peer, ap, 4, 0);
                }
            }
        }
        oap = NULL;
        if (n < 4 || n - 4 > segsize) {
            nak(EBADOP, "Data packet too large");
            goto abort;
        }
#ifdef HAVE_PTHREAD
        size = n - 4;
        tftpio_writer_publish(io, size);
#else
        size = writeit(file, &dp, n - 4, pf->f_convert);
        if (size != (n - 4)) {
            if (size < 0)
                nak(-errno, NULL);
            else
                nak(ENOSPACE, NULL);
            goto abort;
        }
#endif
        final = size != segsize;
        packets_in_window++;
        if (!++block)
            block = rollover_val;
        if (final || packets_in_window == windowsize) {
            last_acked = dp_block;
            packets_in_window = 0;
            ap->th_opcode = htons((uint16_t) ACK);
            ap->th_block = htons(last_acked);
            acksize = 4;
#ifdef HAVE_PTHREAD
            if (tftpio_writer_drain(io) < 0) {
                nak(-errno, NULL);
                goto abort;
            }
#endif
            if (send(peer, ap, acksize, 0) != acksize) {
                tftpd_log(LOG_WARNING, "tftpd: write(ack): %m");
                goto abort;
            }
#ifndef HAVE_PTHREAD
            write_behind(file, pf->f_convert);
#endif
            if (final)
                break;
        }
#ifdef HAVE_PTHREAD
        dp = tftpio_writer_reserve(io);
        if (!dp) {
            nak(-errno, NULL);
            goto abort;
        }
#endif
    }
#ifdef HAVE_PTHREAD
    tftpio_stop(io);
    io = NULL;
#else
    write_behind(file, pf->f_convert);
#endif
    (void)fclose(file);         /* close data file */
    file = NULL;

    timeout_quit = 1;           /* just quit on timeout */
    n = recv_time(peer, buf, sizeof(buf), 0, &timeout); /* normally times out and quits */
    timeout_quit = 0;

    if (n >= 4 &&               /* if read some data */
        ntohs(((struct tftphdr *)buf)->th_opcode) == DATA &&
        last_acked == ntohs(((struct tftphdr *)buf)->th_block)) {
        (void)send(peer, ackbuf, 4, 0); /* resend final ack */
    }
  abort:
#ifdef HAVE_PTHREAD
    tftpio_stop(io);
    io = NULL;
#endif
    if (file) {
        (void)fclose(file);
        file = NULL;
    }
    return;
}

static const char *const errmsgs[] = {
    "Undefined error code",     /* 0 - EUNDEF */
    "File not found",           /* 1 - ENOTFOUND */
    "Access denied",            /* 2 - EACCESS */
    "Disk full or allocation exceeded", /* 3 - ENOSPACE */
    "Illegal TFTP operation",   /* 4 - EBADOP */
    "Unknown transfer ID",      /* 5 - EBADID */
    "File already exists",      /* 6 - EEXISTS */
    "No such user",             /* 7 - ENOUSER */
    "Failure to negotiate RFC2347 options"      /* 8 - EOPTNEG */
};

#define ERR_CNT (sizeof(errmsgs)/sizeof(const char *))

/*
 * Send a nak packet (error message).
 * Error code passed in is one of the
 * standard TFTP codes, or a negative
 * errno.
 */
static void nak(int error, const char *msg)
{
    struct tftphdr *tp;
    int length;

    switch (error) {
    case -ENOENT:
    case -ENOTDIR:
    case -EPERM:
        error = ENOTFOUND;
        break;
    case -ENOSPC:
        error = ENOSPACE;
        break;
    case -EEXIST:
        error = EEXISTS;
        break;
    default:
        break;
    }

    if ((unsigned)error >= ERR_CNT) {
            error = EUNDEF;
            if (!msg && error < 0)
                msg = strerror(-error);
    } else if (!msg) {
        msg = errmsgs[error];
    }

    if (!msg)
        msg = "Request failed";

    tp = (struct tftphdr *)buf;
    tp->th_opcode = htons((uint16_t) ERROR);
    tp->th_code   = htons((uint16_t) error);

    length = strlen(msg) + 1;
    memcpy(tp->th_msg, msg, length);
    length += 4;                /* Add space for header */

    if (verbosity >= 2) {
        tmp_p = (char *)inet_ntop(from.sa.sa_family, SOCKADDR_P(&from),
                                  tmpbuf, INET6_ADDRSTRLEN);
        if (!tmp_p) {
            tmp_p = tmpbuf;
            strcpy(tmpbuf, "???");
        }
        tftpd_log(LOG_INFO, "sending NAK (%d, %s) to %s",
               error, tp->th_msg, tmp_p);
    }

    if (send(peer, buf, length, 0) != length)
        tftpd_log(LOG_WARNING, "nak: %m");
}
