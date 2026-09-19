/*
 * SPDX-License-Identifier: BSD-4-Clause-UC
 *
 * Copyright (c) 1983 Regents of the University of California.
 * Copyright (c) 1999-2009 H. Peter Anvin
 * Copyright (c) 2011-2014 Intel Corporation; author: H. Peter Anvin
 * Copyright (C) 2026 H. Peter Anvin <hpa@zytor.com>
 * All rights reserved.
 */

#include "config.h"             /* Must be included first */
#include "tftpd.h"
#include "path.h"
#include "strlist.h"
#include "options.h"
#include "common/clock.h"

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
#include "common/tftp-io.h"
#include "common/tftp-xfer.h"
#include "common/pollset.h"
#include "recvfrom.h"
#include "remap.h"

#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>          /* Necessary for FIONBIO on Solaris */
#endif

#if defined(HAVE_OPENAT2) && \
    defined(RESOLVE_IN_ROOT) && \
    defined(RESOLVE_NO_MAGICLINKS)
#define WITH_JAIL 1
#else
#define WITH_JAIL 0
#endif

#define	TIMEOUT 1000000         /* Default timeout (us) */
#define TRIES   6               /* Number of attempts to send each packet */
#define TIMEOUT_LIMIT ((1 << TRIES)-1)

/* Default daemon wait timeout when not running standalone */
#define DEFAULT_WAITTIME	(900*1000000)

static int peer;
static struct passwd *pw;                /* The user to run as */
static unsigned long timeout  = TIMEOUT;        /* Current timeout value */
static unsigned long rexmtval = TIMEOUT;       /* Basic timeout value */
static unsigned long maxtimeout = TIMEOUT_LIMIT * TIMEOUT;
static bool timeout_quit;
static sigjmp_buf timeoutbuf;
static sigjmp_buf *active_timeoutbuf = &timeoutbuf;
static uint16_t rollover_val = 0;
#define	PKTSIZE	(MAX_SEGSIZE + 4)
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
static unsigned int windowsize = 1;

static union sock_addr myaddr;  /* Local address */
static union sock_addr from;    /* Remote address */
static const char *from_str = "<client>";
enum tsize_mode {
    TSIZE_NAK,                  /* tsize not possible */
    TSIZE_READ,                 /* tsize valid read size from server */
    TSIZE_WRITE                 /* tsize write size from client */
};
static struct tsize {
    enum tsize_mode mode;
    off_t size;
} tsize;

struct daemon_options dopt = {
    .max_windowbytes = MAX_WINDOWBYTES,
    .rexmtval = TIMEOUT,
    .map_steps = DAEMON_DEFAULT_MAP_STEPS,
    .waittime = -1,
    .service = "tftp",
    .user = "nobody"
};

struct common_options xopt = {
#ifdef HAVE_IPV6
    .ai_fam = AF_UNSPEC,
#else
    .ai_fam = AF_INET,
#endif
    .max_blksize = MAX_SEGSIZE,
    .max_windowsize = MAX_WINDOWSIZE
};

#ifdef WITH_REGEX
static struct rule *rewrite_rules = NULL;
static size_t rewrite_macros(char macro, const char **output);
#else
#define rewrite_rules NULL
#define rewrite_macros NULL
#endif
static void rewrite_test(FILE *);

#if WITH_JAIL
static int jail_root = -1;
#endif

static char *normalize_path(char *name);

static FILE *file;

static noreturn void run_worker(struct tftphdr *tp, int n);
static noreturn void tftp(struct tftphdr *tp, int n);

static void nak(int, const char *);
static void timer(int);
static void parse_option(const char *, const char *);
static size_t negotiate_options(struct tftphdr **tpp);
static unsigned int io_ring_slots(void);
static bool io_is_threaded(void);

enum protocol_option_enum {
    PO_BLKSIZE,
    PO_BLKSIZE2,
    PO_ROLLOVER,
    PO_TIMEOUT,
    PO_TSIZE,
    PO_UTIMEOUT,
    PO_WINDOWSIZE,
    PO_NUM_OPTS
};

enum protocol_option_flags {
    POF_NONE   = 0,
    POF_REFUSE = 1,             /* Option to be refused */
    POF_REQ    = 2,             /* Option requested */
    POF_ACK    = 4              /* Option to be granted */
};

struct daemon_protocol_option {
    const char * const name;    /* Canonical name */
    const unsigned int nsize;   /* Size of the name string including NUL */
    enum protocol_option_flags flags;
    const char *client_name;    /* Name as used by client */
    uintmax_t val;		/* Requested/accepted value */
};

#define POPT(n) { n, sizeof(n), POF_NONE, NULL, 0 }

static struct daemon_protocol_option protocol_options[PO_NUM_OPTS] = {
    POPT("blksize"),
    POPT("blksize2"),
    POPT("rollover"),
    POPT("timeout"),
    POPT("tsize"),
    POPT("utimeout"),
    POPT("windowsize")
};

/* Signal handlers: just set a variable and return */
static volatile sig_atomic_t reload_signal = 0;
static volatile sig_atomic_t exit_signal = 0;

static void handle_exit(int sig)
{
    exit_signal = sig;
    pollset_notify_signal(sig);
}
static void handle_reload(int sig)
{
    reload_signal = sig;
    pollset_notify_signal(sig);
}

/* Handle timeout signal or timeout event */
static void timer(int sig)
{
    (void)sig;                  /* Suppress unused warning */
    timeout <<= 1;
    if (timeout >= maxtimeout || timeout_quit)
        exit(0);
    siglongjmp(*active_timeoutbuf, 1);
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

    fprintf(stderr, "%s[%lu]: %s",
            _progname ? _progname : "tftpd",
            (unsigned long)_progpid,
            prio_name(priority));

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
        tftpd_log(LOG_ERR, "Cannot open map file: %s: %s",
                  rulefile, strerror(errno));
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
static int lock_file(int fd, bool lock_write)
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

#ifndef MSG_TRUNC
#define MSG_TRUNC 0
#endif

static int recv_time(int s, void *rbuf, int len, unsigned int flags,
                     unsigned long *timeout_us_p)
{
    int rv = tftp_recv_time(s, rbuf, len, flags, NULL, NULL, timeout_us_p);

    if (rv < 0 && errno == ETIMEDOUT)
        timer(0);               /* Should not return */

    return rv;
}

struct daemon_xfer_context {
    unsigned long timeout;
};

static int daemon_xfer_send(void *vctx, const void *packet, int length)
{
    (void)vctx;
    return send(peer, packet, length, 0) == length ? 0 : -1;
}

static int daemon_xfer_recv(void *vctx, void *packet, int length)
{
    struct daemon_xfer_context *ctx = vctx;
    return recv_time(peer, packet, length, MSG_TRUNC, &ctx->timeout);
}

static void daemon_xfer_received(void *vctx, const struct tftphdr *packet,
                                 int length)
{
    (void)vctx;
    (void)packet;
    (void)length;
}

static void daemon_xfer_retry_enter(void *vctx, sigjmp_buf *retrybuf,
                                    bool restarted)
{
    (void)vctx;
    active_timeoutbuf = retrybuf;
    if (!restarted)
        timeout = rexmtval;
}

static void daemon_xfer_retry_leave(void *vctx)
{
    (void)vctx;
    active_timeoutbuf = &timeoutbuf;
}

static void daemon_xfer_wait_begin(void *vctx)
{
    struct daemon_xfer_context *ctx = vctx;

    ctx->timeout = timeout;
}

static void daemon_xfer_dally(uint16_t last_acked)
{
    int n;
    struct tftphdr hdr;

    timeout_quit = true;
    n = recv_time(peer, &hdr, 4, MSG_TRUNC, &timeout);
    timeout_quit = false;

    if (n >= 4 && hdr.th_opcode == htons(DATA) &&
        hdr.th_block  == htons(last_acked)) {
        hdr.th_opcode = htons(ACK);
        (void)send(peer, &hdr, 4, 0);
    }
}

static const struct tftp_xfer_ops daemon_xfer_ops = {
    daemon_xfer_send,
    daemon_xfer_recv,
    daemon_xfer_received,
    daemon_xfer_retry_enter,
    daemon_xfer_retry_leave,
    daemon_xfer_wait_begin
};

static void tftpd_out_of_memory(void)
{
    tftpd_log(LOG_ERR, "fatal error: %s", strerror(errno));
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

static enum normalizations parse_normalize(const char *str)
{
    if (!str)
        return NORM_PATH;

    switch (*str) {
    case '\0':
        return NORM_PATH;

    case 'a':
        if (!strcmp(str, "all"))
            return NORM_PATH;
        break;

    case 'p':
        if (!strcmp(str, "path"))
            return NORM_PATH;
        break;

    case 'n':
        if (!strcmp(str, "no") || !strcmp(str, "none"))
            return NORM_NONE;
        break;

    case 'y':
        if (!strcmp(str, "yes"))
            return NORM_PATH;
        break;

    case 'r':
        if (!strcmp(str, "root"))
            return NORM_ROOT;
        break;

    case '/':
        if (!str[1])
            return NORM_ROOT;
        break;

    default:
        break;
    }

    tftpd_log(LOG_ERR, "invalid --normalize option: %s\n", str);
    exit(EX_USAGE);
}

enum long_only_options {
    OPT_VERBOSITY	= 256,
    OPT_STDERR,
    OPT_MAP_TEST,
    OPT_MAP_STEPS,
    OPT_SYSTEMD,
    OPT_WINDOW_BYTES,
    OPT_REJECT_ALL,
    OPT_PATH_PREFIX,
    OPT_NORMALIZE
};

static const struct option long_options[] = {
    { "ipv4",        0, NULL, '4' },
    { "ipv6",        0, NULL, '6' },
    { "create",      0, NULL, 'c' },
    { "secure",      0, NULL, 's' },
    { "chroot",      0, NULL, 's' },
    { "jail",        0, NULL, 'j' },
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
    { "reject",      1, NULL, 'r' },
    { "refuse-all",  0, NULL, OPT_REJECT_ALL },
    { "reject-all",  0, NULL, OPT_REJECT_ALL },
    { "timeout",     1, NULL, 't' },
    { "retransmit",  1, NULL, 'T' },
    { "port-range",  1, NULL, 'R' },
    { "ports",       1, NULL, 'R' },
    { "service",     1, NULL, 'S' },
    { "path-prefix", 1, NULL, OPT_PATH_PREFIX },
    { "port",        1, NULL, 'S' },
    { "map-file",    1, NULL, 'm' },
    { "map-steps",   1, NULL, OPT_MAP_STEPS },
    { "pidfile",     1, NULL, 'P' },
    { "stderr",      0, NULL, OPT_STDERR },
    { "map-test",    1, NULL, OPT_MAP_TEST },
    { "systemd",     0, NULL, OPT_SYSTEMD },
    { "normalize",   2, NULL, OPT_NORMALIZE },
    { NULL, 0, NULL, 0 }
};
static const char short_options[] = "46csjpvVlLa:B:W:u:U:r:t:T:R:S:m:P:";

static struct pollset *listen_set;

static void close_listen_set(void)
{
    if (listen_set)
        pollset_close(&listen_set);
}

static bool is_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0)
        return false;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    struct tftphdr *tp;
    uint16_t th_opcode;
    int n;
    int fd = -1;
    pid_t pid;
    int c;
    char *ep;
    bool patherr;
    pollset_cursor cursor;
    int nullfd;

#ifdef HAVE_LOCALE_H
    setlocale(LC_CTYPE, "");     /* For to(w)(lower|upper)() */
#endif

    set_progname(argv[0]);
    out_of_memory = tftpd_out_of_memory;

    /* Randomness is used for port (transfer ID) assignment */
    random_init();

    /* Initialize the list and pollset of listen addresses */
    listen_set = pollset_new();
    atexit(close_listen_set);

    strlist_init(&dopt.listen_addrs);

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
            xopt.ai_fam = AF_INET;
            break;
#ifdef HAVE_IPV6
        case '6':
            xopt.ai_fam = AF_INET6;
            break;
#endif
        case 'c':
            dopt.cancreate = true;
            break;
        case 's':
            dopt.secure = true;
            break;
        case 'j':
#if WITH_JAIL
            dopt.jail = true;
#else
            tftpd_log(LOG_ERR, "--jail not supported by this OS or build");
            exit(EX_USAGE);
#endif
            break;
        case 'p':
            dopt.unixperms = true;
            break;
        case 'l':
            dopt.standalone = true;
            break;
        case 'L':
            dopt.standalone = true;
            dopt.nodaemon = true;
            break;
        case 'a':
            dopt.standalone = true;
            strlist_add(&dopt.listen_addrs, optarg);
            break;
        case 't':
            dopt.waittime = strtoul(optarg, NULL, 10) * (intmax_t)1000000;
            break;
        case 'S':
            if (!optarg || !*optarg) {
                tftpd_log(LOG_ERR, "Missing service name");
                exit(EX_USAGE);
            }
            dopt.service = optarg;
            break;
        case 'B':
            {
                char *vp;
                xopt.max_blksize = (unsigned int)strtoul(optarg, &vp, 10);
                if (xopt.max_blksize < 512 ||
                    xopt.max_blksize > MAX_SEGSIZE || *vp) {
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
                xopt.max_windowsize = (unsigned int)value;
            }
            break;
        case OPT_WINDOW_BYTES:
            {
                char *vp;

                errno = 0;
                dopt.max_windowbytes = strtoumax(optarg, &vp, 10);
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
                dopt.rexmtval = tov;
            }
            break;
        case 'R':
            if (sscanf(optarg, "%u:%u", &xopt.portrange_from,
                       &xopt.portrange_to) != 2 ||
                xopt.portrange_from > xopt.portrange_to ||
                xopt.portrange_to >= 65535) {
                tftpd_log(LOG_ERR, "Bad port range: %s", optarg);
                exit(EX_USAGE);
            }
            break;
        case 'u':
            dopt.user = optarg;
            break;
        case 'U':
            dopt.my_umask = strtoul(optarg, &ep, 8);
            if (*ep) {
                tftpd_log(LOG_ERR, "Invalid umask: %s", optarg);
                exit(EX_USAGE);
            }
            dopt.spec_umask = true;
            break;
        case 'r':
        {
            struct daemon_protocol_option *po;
            ARRAY_FOREACH(po, protocol_options) {
                if (ascii_strcaseeq(optarg, po->name)) {
                    po->flags |= POF_REFUSE;
                    goto done_refuse;
                }
            }
            tftpd_log(LOG_ERR, "Unknown option: %s", optarg);
            exit(EX_USAGE);
        done_refuse:
            break;
        }
        case OPT_REJECT_ALL:
            dopt.reject_all_options = true;
            break;
        case 'm':
#ifdef WITH_REGEX
            if (dopt.rewrite_file) {
                tftpd_log(LOG_ERR, "Multiple -m options");
                exit(EX_USAGE);
            }
            dopt.rewrite_file = optarg;
#else
            tftpd_log(LOG_ERR, "--map-file not supported by this build");
            exit(EX_USAGE);
#endif
            break;
        case OPT_MAP_STEPS:
        {
            unsigned long steps = strtoul(optarg, &ep, 0);
            if (*optarg && !*ep && steps > 0 && steps <= INT_MAX) {
                dopt.map_steps = steps;
            } else {
                tftpd_log(LOG_ERR, "Bad --map-steps option: %s", optarg);
                exit(EX_USAGE);
            }
            break;
        }
        case OPT_MAP_TEST:
            dopt.map_test_file = optarg;
            dopt.use_stderr = true;
            break;
        case 'v':
            dopt.verbosity++;
            break;
        case OPT_VERBOSITY:
            dopt.verbosity = atoi(optarg);
            break;
        case OPT_STDERR:
            dopt.use_stderr = true;
            break;
        case OPT_SYSTEMD:
            dopt.nodaemon = true;
            dopt.systemd = true;
            break;
        case OPT_PATH_PREFIX:
            dopt.path_prefix = *optarg ? optarg : NULL;
            break;
        case 'V':
            /* Print configuration to stdout and exit */
            printf("%s\n", TFTPD_CONFIG_STR);
            exit(0);
            break;
        case 'P':
            dopt.pidfile = optarg;
            break;
        case OPT_NORMALIZE:
            dopt.normalize = parse_normalize(optarg);
            break;
        default:
            tftpd_log(LOG_ERR, "Unknown option: '%c'", optopt);
            break;
        }

    rexmtval = timeout = dopt.rexmtval;
    maxtimeout = rexmtval * TIMEOUT_LIMIT;

    if (!dopt.use_stderr)
        tftpd_openlog();

#ifdef WITH_REGEX
    if (dopt.rewrite_file)
        rewrite_rules = read_remap_rules(dopt.rewrite_file);
#endif

    if (dopt.map_test_file) {
        FILE *tf = fopen(dopt.map_test_file, "r");
        if (!tf) {
            tftpd_log(LOG_ERR, "%s: cannot open map test file: %s",
                      dopt.map_test_file, strerror(errno));
            exit(EX_NOINPUT);
        }
        rewrite_test(tf);
        fclose(tf);
        exit(0);
    }

    if (dopt.path_prefix) {
        if (!is_directory(dopt.path_prefix)) {
            tftpd_log(LOG_ERR, "%s: invalid path prefix: %s",
                      dopt.path_prefix, strerror(errno));
            exit(EX_DATAERR);
        }
    }

    dopt.dirs = xmalloc((argc - optind + 1) * sizeof(char *));
    patherr = false;
    for (dopt.ndirs = 0; optind != argc; optind++) {
        const char *path = argv[optind];
        const char * const *pathlist =
            parse_path(path, !dopt.secure && !dopt.jail);
        if (!pathlist) {
            tftpd_log(LOG_ERR, "invalid directory path: %s", path);
            patherr = true;
        } else if (!pathlist[0] && !dopt.path_prefix) {
            tftpd_log(LOG_ERR, "/ as directory path requires --path-prefix");
            patherr = true;
        }
        dopt.dirs[dopt.ndirs++] = pathlist;
    }
    dopt.dirs[dopt.ndirs] = NULL;

    if (patherr)
        exit(EX_DATAERR);

    if (!dopt.ndirs) {
        tftpd_log(LOG_ERR, "directory list not specified");
        exit(EX_USAGE);
    }

    if (dopt.secure && dopt.jail) {
        tftpd_log(LOG_ERR, "--chroot and --jail are mutually exclusive");
        exit(EX_USAGE);
    }

    if (dopt.secure || dopt.jail) {
        if (dopt.ndirs != 1) {
            tftpd_log(LOG_ERR, "--chroot or --jail require exactly one directory");
            exit(EX_USAGE);
        }

        char *securepath = build_path(dopt.path_prefix, dopt.dirs[0]);
        if (dopt.secure) {
            if (chdir(securepath)) {
                tftpd_log(LOG_ERR, "%s: %s", securepath, strerror(errno));
                exit(EX_NOINPUT);
            }
        } else
#if WITH_JAIL
        if (dopt.jail) {
            /* This uses openat2() partly as a test */
            static const struct open_how how = {
                .flags   = O_RDONLY | O_DIRECTORY,
                .resolve = RESOLVE_NO_MAGICLINKS
            };
            jail_root = openat2(AT_FDCWD, securepath, &how, sizeof how);
            if (jail_root < 0) {
                if (errno == ENOSYS || errno == EINVAL) {
                    tftpd_log(LOG_ERR, "--jail not supported on this system");
                    exit(EX_OSERR);
                } else {
                    tftpd_log(LOG_ERR, "%s: %s", securepath, strerror(errno));
                    exit(EX_NOINPUT);
                }
            }
        } else
#endif
            exit(EX_SOFTWARE);  /* Should never happen */

        free(securepath);
    }

    pw = getpwnam(dopt.user);
    if (!pw) {
        tftpd_log(LOG_ERR, "no user %s: %s", dopt.user, strerror(errno));
        exit(EX_NOUSER);
    }

    if (dopt.pidfile && !dopt.standalone) {
        tftpd_log(LOG_WARNING, "not in standalone mode, ignoring pid file");
        dopt.pidfile = NULL;
    }

    /*
     * If no wait time is specified, set it to infinite if
     * standalone, otherwise to DEFAULT_WAITTIME.
     *
     * If a wait time of 0 is specified, set it to infinite.
     */
    if (dopt.waittime < 0)
        dopt.waittime = dopt.standalone ? -1 : DEFAULT_WAITTIME;
    else if (!dopt.waittime)
        dopt.waittime = -1;

    /*
     * If we're running standalone, open the listening sockets,
     * daemonize the process and add a pid file if requested.
     */
    if (dopt.standalone || dopt.systemd) {
        struct liststr *ls;

        if (dopt.systemd) {
            int startfd = 3;    /* Fixed by systemd protocol */
            long nfds = getenv_ulong("LISTEN_FDS");
            long listen_pid = getenv_ulong("LISTEN_PID");

            if (dopt.standalone) {
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
            if (strlist_isempty(&dopt.listen_addrs))
                strlist_add(&dopt.listen_addrs, ":");

            for (ls = dopt.listen_addrs.list; ls; ls = ls->next)
                listen_to(listen_set, ls->str, xopt.ai_fam);
        }

        strlist_free(&dopt.listen_addrs);

        if (pollset_isempty(listen_set)) {
            tftpd_log(LOG_ERR, "no listen addresses available");
            exit(EX_NOINPUT);
        }

        /* Daemonize this process */
        /* Note: when running in secure mode (-s), we must not chdir, since
           we are already in the proper directory. */
        if (!dopt.nodaemon && daemon(dopt.secure, true) < 0) {
            tftpd_log(LOG_ERR, "cannot daemonize: %s", strerror(errno));
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
    if (!dopt.use_stderr)
        dup2(nullfd, 2);

    if (dopt.pidfile) {
        FILE *pf = fopen(dopt.pidfile, "w");
        if (!pf) {
            tftpd_log(LOG_ERR,
                      "cannot open pid file '%s' for writing: %s",
                      dopt.pidfile, strerror(errno));
            dopt.pidfile = NULL;
        } else {
            bool err = fprintf(pf, "%d\n", getpid()) < 0;
            bool write_error = !!ferror(pf);
            bool close_error = fclose(pf) != 0;
            err = err || write_error || close_error;
            if (err)
                tftpd_log(LOG_ERR, "error writing pid file '%s': %s",
                          dopt.pidfile, strerror(errno));
        }
    }

    cursor = 0;
    while ((fd = pollset_next(listen_set, &cursor, NULL)) >= 0) {
        tftpd_config_socket(fd, false);
        cygwin_set_socket_nonblock(fd, false);
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
    set_signal(SIGHUP,  dopt.standalone ? handle_reload : handle_exit, 0);
    set_signal(SIGTERM, handle_exit, 0);
    set_signal(SIGINT,  handle_exit, 0);

    if (dopt.spec_umask || !dopt.unixperms)
        umask(dopt.my_umask);

    tp = xmalloc(PKTSIZE);

    while (1) {
        int what;
        int rv;

        if (exit_signal) {
            if (dopt.pidfile && unlink(dopt.pidfile)) {
                tftpd_log(LOG_WARNING, "error removing pid file '%s': %s",
                          dopt.pidfile, strerror(errno));
                exit(EX_OSERR);
            } else {
                exit(0);
            }
	}

        if (reload_signal) {
            reload_signal = 0;
#ifdef WITH_REGEX
            if (dopt.rewrite_file) {
                freerules(rewrite_rules);
                rewrite_rules = read_remap_rules(dopt.rewrite_file);
            }
#endif
        }

        rv = pollset_poll(listen_set, POLLSET_IN, dopt.waittime);
        if (rv == -1 && errno == EINTR)
            continue;           /* Signal caught, reloop */

        if (rv == -1) {
            tftpd_log(LOG_ERR, "listen loop: %s", strerror(errno));
            exit(EX_IOERR);
        } else if (rv == 0) {
            exit(0);            /* Timeout, return to inetd */
        }

        cursor = 0;
        what = POLLSET_IN;
        fd = pollset_next(listen_set, &cursor, &what);
        if (fd <= 0)
            continue;

        cygwin_set_socket_nonblock(fd, true);
        n = myrecvfrom(fd, tp, PKTSIZE, 0, &from, &myaddr);
        cygwin_set_socket_nonblock(fd, false);

        if (n < 0) {
            int error = errno;

            if (E_WOULD_BLOCK(error) || error == EINTR) {
                continue;       /* Again, from the top */
            } else {
                tftpd_log(LOG_ERR, "recvfrom: %s", strerror(error));
                exit(EX_IOERR);
            }
        }

#ifdef HAVE_IPV6
        if ((from.sa.sa_family != AF_INET) && (from.sa.sa_family != AF_INET6)) {
            tftpd_log(LOG_ERR, "received address was not AF_INET/AF_INET6,"
                   " please check your inetd config");
            exit(EX_PROTOCOL);
        }
#else
        if (from.sa.sa_family != AF_INET) {
            tftpd_log(LOG_ERR, "received address was not AF_INET,"
                   " please check your inetd config");
            exit(EX_PROTOCOL);
        }
#endif

        /*
         * Ignore the packet out of hand if it isn't a valid request packet
         */
        if (n < (int)sizeof(tp->th_opcode))
            continue;           /* Packet too short */
        th_opcode = ntohs(tp->th_opcode);
        if (th_opcode != RRQ && th_opcode != WRQ)
            continue;

        if (dopt.standalone) {
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
            tftpd_log(LOG_ERR, "fork: %s", strerror(errno));
            exit(EX_OSERR);     /* Return to inetd, just in case */
        } else if (pid == 0) {
            run_worker(tp, n);
            abort();            /* It should not be possible to get here */
        }

        random_post_fork_parent();
    }
}

static noreturn void run_worker(struct tftphdr *tp, int n)
{
    int setrv;
    int die;

    /* Child process: handle the actual request here */
    post_fork();
    random_post_fork_child();

    /* Ignore SIGHUP; make SIGTERM and SIGINT kill the process */
    set_signal(SIGHUP,  SIG_IGN, 0);
    set_signal(SIGTERM, SIG_DFL, 0);
    set_signal(SIGINT,  SIG_DFL, 0);

    /* Make sure the log socket is still connected.  This has to be
       done before the chroot, while /dev/log is still accessible,
       so depending on the automatic re-opening by syslog() is unsafe. */
    if (dopt.secure)
        tftpd_reopenlog();

    /*
     * Trim the part of the request packet not actually used. Most of the
     * time the request packet is *much* smaller, so freeing this memory
     * early helps memory reuse.
     */
    tp = xrealloc(tp, n);

    /* Close file descriptors we don't need */
    close_listen_set();

    /* Convert the client address to a string */
    from_str = net_address(&from.sa, sizeof from);

    /* Get a socket.  This has to be done before the chroot(), since
       some systems require access to /dev to create a socket. */

    peer = socket(myaddr.sa.sa_family, SOCK_DGRAM, 0);
    if (peer < 0) {
        tftpd_log(LOG_ERR, "socket: %s", strerror(errno));
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
    setrv = initgroups(dopt.user, pw->pw_gid);
    if (!setrv) {
	die = 0;
    } else if (errno != EPERM) {
        tftpd_log(LOG_ERR, "cannot set groups for user %s", dopt.user);
	die = EX_OSERR;
    }
#endif
    if (die)
	exit(die);

    /* Chroot and drop privileges */
    if (dopt.secure) {
        if (chroot(".") || chdir("/")) {
            tftpd_log(LOG_ERR, "chroot: %s", strerror(errno));
            exit(EX_OSERR);
        }
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
        tftpd_log(LOG_ERR, "cannot drop privileges: %s", strerror(errno));
        exit(EX_OSERR);
    }

    /* Process the request... */
    if (pick_port_bind(peer, &myaddr)) {
        tftpd_log(LOG_ERR, "bind: %s", strerror(errno));
        exit(EX_IOERR);
    }

    if (connect(peer, &from.sa, sizeof from) < 0) {
        tftpd_log(LOG_ERR, "connect: %s", strerror(errno));
        exit(EX_IOERR);
    }

    tftpd_config_socket(peer, true);

    tftp(tp, n);
    abort();                    /* It should not be possible to get here */
}

static const char *rewrite_access(const struct formats *,
                                  const char *, int, int, const char **);
static int validate_access(const char *, int, const struct formats *,
                           const char **);
static void tftp_sendfile(const struct formats *, struct tftphdr *, int,
                          const char *);
static void tftp_recvfile(const struct formats *, struct tftphdr *, int,
                          const char *);

static const struct formats formats[] = {
    { "netascii", rewrite_access, validate_access,
      tftp_sendfile, tftp_recvfile, true},
    { "octet", rewrite_access, validate_access,
      tftp_sendfile, tftp_recvfile, false },
};

/*
 * Handle initial connection protocol.
 */
static noreturn void tftp(struct tftphdr *tp, int size)
{
    char *cp, *end;
    int argn, ecode;
    const struct formats *pf = NULL;
    char *origfilename, *request_filename = NULL;
    const char *filename;
    char *mode = NULL;
    const char *errmsgptr;
    uint16_t tp_opcode = ntohs(tp->th_opcode);
    char *val = NULL, *opt = NULL;
    struct tftphdr *oack;
    size_t oacklen;

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
            ARRAY_FOREACH(pf, formats) {
                if (ascii_strcaseeq(pf->f_mode, mode))
                    goto found_format;
            }
            nak(EBADOP, "Unknown mode");
            exit(0);

        found_format:
	    file = NULL;
            request_filename = xstrdup(origfilename);
            if (!(filename = (*pf->f_rewrite)
		  (pf, origfilename, tp_opcode, from.sa.sa_family, &errmsgptr))) {
                nak(EACCESS, errmsgptr);        /* File denied by mapping rule */
                exit(0);
            }
            if (dopt.verbosity >= 1) {
                if (!strcmp(filename, origfilename)) {
                    tftpd_log(LOG_NOTICE, "%s from %s filename %s",
                              tp_opcode == WRQ ? "WRQ" : "RRQ",
                              from_str, filename);
                } else {
                    tftpd_log(LOG_NOTICE,
                           "%s from %s filename %s remapped to %s",
                              tp_opcode == WRQ ? "WRQ" : "RRQ",
                              from_str, origfilename,
                              filename);
                }
            }
	    /*
	     * If "file" is already set, then a file was already validated
	     * and opened during remap processing.
	     */
	    if (!file) {
		ecode =
		    (*pf->f_validate) (filename, tp_opcode, pf, &errmsgptr);
		if (ecode == ENOTFOUND)
		    tftpd_log(LOG_NOTICE, "%s: file not found: %s",
			      from_str, filename);
		if (ecode) {
		    nak(ecode, errmsgptr);
		    exit(0);
		}
	    }
            opt = ++cp;
        } else if (argn & 1) {
            val = ++cp;
        } else {
            parse_option(opt, val);
            opt = ++cp;
        }
    }

    if (!pf) {
        nak(EBADOP, "Missing mode");
        exit(0);
    }

    oacklen = negotiate_options(&oack);

    /* There are no more uses of the request packet after this point */
    xfree(tp);

    tftp_set_socket_buffers(peer, segsize, windowsize, tp_opcode == RRQ);

    if (tp_opcode == RRQ)
        (*pf->f_send) (pf, oack, oacklen, request_filename);
    else
        (*pf->f_recv) (pf, oack, oacklen, request_filename);

    exit(0);                    /* Request completed */
}

/*
 * Keep at least one full window for retransmission, then for
 * asynchronous I/O allow for the largest of:
 * 1. one full TFTP window;
 * 2. two full TFTP blocks;
 * 3. IO_RING_MIN_BYTES (256 KiB by default).
 */
static unsigned int io_ring_slots(void)
{
#ifdef HAVE_PTHREAD
    unsigned int io_slots;

    io_slots = (segsize + IO_RING_MIN_BYTES - 1) / segsize;
    if (io_slots < windowsize)
        io_slots = windowsize;
    if (io_slots < 2)
        io_slots = 2;

    return windowsize + io_slots;
#else
    return windowsize;
#endif
}

static bool io_is_threaded(void)
{
#ifdef HAVE_PTHREAD
    return true;
#else
    return false;
#endif
}

/*
 * Parse an RFC2347 option; we limit the arguments to positive
 * integers which matches all our current options.
 */
static void parse_option(const char *opt, const char *val)
{
    char *vend;
    uintmax_t v;
    struct daemon_protocol_option *po;

    /* Global option-parsing variables initialization */
    if (!*opt || !*val)
        return;

    errno = 0;
    v = strtoumax(val, &vend, 10);
    if (*vend || errno == ERANGE)
	return;                 /* Invalid string */

    ARRAY_FOREACH(po, protocol_options) {
        if (ascii_strcaseeq(po->name, opt)) {
            po->flags |= POF_REQ;
            po->val = v;
            /*
             * Save the name in the same case as the client sent;
             * it's not supposed to matter but broken TFTP clients abound.
             */
            po->client_name = opt;
            break;
        }
    }
}

/*
 * Returns a pointer to the option structure if it was requested
 * by the client and not configured to be refused.
 */
static inline struct daemon_protocol_option *
opt_requested(enum protocol_option_enum opt)
{
    struct daemon_protocol_option *po = &protocol_options[opt];

    if ((po->flags & (POF_REQ|POF_REFUSE)) == POF_REQ)
        return po;
    else
        return NULL;
}

static inline bool opt_granted(const struct daemon_protocol_option *po)
{
    /* The options must have been both requested and acknowledgeable */
    return (po->flags & (POF_ACK|POF_REQ|POF_REFUSE)) == (POF_ACK|POF_REQ);
}

/*
 * Select the options to acknowledge; return true if at least one
 * option should be acknowledged.
 */
static void negotiate_blksize(void)
{
    struct daemon_protocol_option *blk  = opt_requested(PO_BLKSIZE);
    struct daemon_protocol_option *blk2 = opt_requested(PO_BLKSIZE2);
    unsigned int blksize = 0;

    if (blk2) {
        if (blk2->val < MIN_SEGSIZE) {
            blk2 = NULL;
        } else {
            unsigned int lb;

            if (blk2->val > xopt.max_blksize)
                blksize = xopt.max_blksize;
            else
                blksize = blk2->val;

            /* Round down to a power of 2 */
            while ((lb = blksize & (blksize - 1)))
                blksize = lb;
        }
    }
    if (blk) {
        if (blk->val < MIN_SEGSIZE) {
            blk = NULL;
        } else {
            if (blk->val > xopt.max_blksize) {
                blksize = xopt.max_blksize;
            } else if (blksize > blk->val) {
                blk = NULL;		/* blksize2 wins, reject blksize */
            } else {
                blksize = blk->val;
            }
            if (blksize & (blksize - 1))
                blk2 = NULL;	/* Reject blksize2, not power of 2 */
        }
    }
    if (blk) {
        blk->flags |= POF_ACK;
        blk->val    = blksize;
    }
    if (blk2) {
        blk2->flags |= POF_ACK;
        blk2->val    = blksize;
    }

    segsize = blksize ? blksize : SEGSIZE;
}

static void negotiate_windowsize(void)
{
    struct daemon_protocol_option *ws = opt_requested(PO_WINDOWSIZE);
    unsigned int window = 1;

    if (ws && ws->val > 1) {
        if (ws->val > xopt.max_windowsize)
            window = xopt.max_windowsize;
        else
            window = ws->val;

        if ((uintmax_t)window * segsize > dopt.max_windowbytes)
            window = dopt.max_windowbytes / (uintmax_t)segsize;

        if (window > 1) {
            ws->flags |= POF_ACK;
            ws->val    = window;
        } else {
            window = 1;
        }
    }

    windowsize = window;
}

static void negotiate_tsize(void)
{
    struct daemon_protocol_option *ts = opt_requested(PO_TSIZE);

    if (!ts)
        return;

    switch (tsize.mode) {
    case TSIZE_NAK:
        /* RRQ, but the size is not available (netascii) */
        break;
    case TSIZE_READ:
        /*
         * RRQ, and the size is available (octet). However, RFC 2349
         * requires that the option value is 0, so refuse the option
         * if the value is anything else.
         */
        if (!ts->val) {
            ts->val = tsize.size;
            ts->flags |= POF_ACK;
        }
        break;
    case TSIZE_WRITE:
        /* WRQ: echo back the tsize specified. */
        tsize.size = ts->val;
        ts->flags |= POF_ACK;
        break;
    default:
        abort();            /* Impossible */
    }
}

#define MIN_TIMEOUT     (10000UL)
#define MAX_TIMEOUT     (255UL * USEC_PER_SEC)
#define MIN_TIMEOUT_SEC ((MIN_TIMEOUT + USEC_PER_SEC - 1)/USEC_PER_SEC)
#define MAX_TIMEOUT_SEC (MAX_TIMEOUT / USEC_PER_SEC)

static void negotiate_timeout(void)
{
    struct daemon_protocol_option *tos = opt_requested(PO_BLKSIZE);
    struct daemon_protocol_option *tou = opt_requested(PO_BLKSIZE2);
    unsigned long to = 0;
    unsigned long tos_us = 0;

    if (tos) {
        if (tos->val < MIN_TIMEOUT_SEC || tos->val > MAX_TIMEOUT_SEC) {
            tos = NULL;
        } else {
            to = tos_us = tos->val * USEC_PER_SEC;
        }
    }
    if (tou) {
        if (tou->val < MIN_TIMEOUT || tou->val > MAX_TIMEOUT) {
            tou = NULL;
        } else {
            /*
             * utimeout takes priority over timeout, ack
             * timeout if and only if it is the same value as
             * utimeout.
             */
            to = tou->val;
            if (tos_us != to)
                tos = NULL;
        }
    }
    if (tos)
        tos->flags |= POF_ACK;
    if (tou)
        tou->flags |= POF_ACK;
}

static void negotiate_rollover(void)
{
    struct daemon_protocol_option *ro = opt_requested(PO_ROLLOVER);

    if (ro && ro->val <= 65535) {
        rollover_val = ro->val;
        ro->flags |= POF_ACK;
    } else {
        rollover_val = 0;
    }
}

/*
 * Build an OACK packet in a new buffer and return the size. If there
 * are NO options agreed upon, then return 0 and do not allocate a
 * buffer.
 */
static size_t build_oack(struct tftphdr **tpp)
{
    const struct daemon_protocol_option *po;
    struct tftphdr *tp;
    char *p;
    size_t bufsize = 0;

    ARRAY_FOREACH(po, protocol_options) {
        if (opt_granted(po))
            bufsize += po->nsize + DIGIT_SPACE(po->val) + 1;
    }

    if (!bufsize) {
        /* No granted options, no OACK phase */
        *tpp = NULL;
        return 0;
    }

    *tpp = tp = xmalloc(bufsize + 2);
    tp->th_opcode = htons(OACK);
    p = tp->th_stuff;

    ARRAY_FOREACH(po, protocol_options) {
        if (opt_granted(po)) {
            p = mempcpy(p, po->client_name, po->nsize);
            p += snprintf(p, DIGIT_SPACE(po->val)+1, "%"PRIuMAX, po->val);
            p++;
        }
    }

    return p - (char *)tp;
}

static size_t negotiate_options(struct tftphdr **tpp)
{
    if (dopt.reject_all_options) {
        *tpp = NULL;
        return 0;
    }

    negotiate_blksize();
    negotiate_windowsize();
    negotiate_tsize();
    negotiate_timeout();
    negotiate_rollover();

    return build_oack(tpp);
}

/*
 * Normalize a filename string according to the dopt.normalize
 * setting. This string MUST have been allocated in heap storage, and
 * may be changed.
 */
static char *normalize_path(char *fn)
{
    if (!fn)
        return fn;

    if (dopt.normalize >= NORM_ROOT) {
        /* Add a leading slash if missing */
        if (fn[0] != '/') {
            size_t len = strlen(fn);
            fn = xrealloc(fn, len+2);
            memmove(fn+1, fn, len+1);
            fn[0] = '/';
        }
    }

    if (dopt.normalize >= NORM_PATH) {
        /* Handle // /./ /../ -- the first character is already / */
        const char *p = fn;
        char *q = fn;
        char c;

        while ((c = *p++)) {
            *q++ = c;
            while (c == '/') {
                while ((c = *p) && c == '/')
                    p++;

                if (c == '.') {
                    if (!p[1] || p[1] == '/') {
                        /* Skip . path component */
                        p += 1;
                    } else if (p[1] == '.' && (!p[2] || p[2] == '/')) {
                        /* .. path component; drop one level if there is one */
                        if (q > fn+1) {
                            q--;        /* Drop immediately previous slash */
                            while (q > fn && q[-1] != '/')
                                q--;
                        }
                        p += 2;
                    }
                    c = *p;
                }
            }
        }
        *q = '\0';
    }

    return fn;
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

static size_t rewrite_macros(char macro, const char **output)
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
#endif

static int test_validate_fail(const char *filename, int mode,
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
        { "dummy", NULL, test_validate_fail, NULL, NULL, false };
#ifdef HAVE_IPV6
    /* Dummy addresses from netblocks assigned for documentation */
    static const char phony_ip6_addr[16] =
        { 0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
          0xfe, 0xed, 0xfa, 0xce, 0xde, 0xad, 0xbe, 0xef };
#endif
    static const char phony_ip4_addr[4] = { 192, 0, 2, 34 };
    char *line = xmalloc(MAX_SEGSIZE + 1);
    int mode = dopt.cancreate ? WRQ : RRQ;
    sa_family_t af = xopt.ai_fam;

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
    from_str = net_address(&from.sa, sizeof from);

    while (fgets(line, MAX_SEGSIZE + 1, tf)) {
        const char *msg;
        char *out;
        char *nl = strchr(line, '\n');
        if (!nl)
            continue;

        *nl = '\0';
        out = rewrite_string(&test_dummy_format, line, rewrite_rules,
                             mode, af, rewrite_macros, &msg);
        out = normalize_path(out);

        if (out) {
            printf("%s\n", out);
            xfree(out);
        } else {
            printf("ERROR: %s\n", msg);
        }
    }
    xfree(line);
}

/*
 * Modify the filename, if applicable.  If it returns NULL, deny the access.
 * Returns either filename or a newly malloc'd buffer.
 */
static const char *rewrite_access(const struct formats *pf,
                                  const char *filename,
                                  int mode, int af, const char **msg)
{
    char *fn;

    fn = rewrite_string(pf, filename, rewrite_rules, mode,
                        af, rewrite_macros, msg);
    fn = normalize_path(fn);

    return fn;
}

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
 *
 * If "secure" or "jail" is set the file path is used as-is, as the
 * kernel is expected to enforce any namespace restrictions.
 */
static int validate_access(const char *filename, int mode,
			   const struct formats *pf, const char **errmsg)
{
    struct stat stbuf;
    int fd, omode, wmode, rmode;
    char *fnbuf = NULL;
    const char * const **dirp;
    char stdio_mode[3];

    tsize.mode = TSIZE_NAK;
    *errmsg = NULL;

    if (!dopt.secure && !dopt.jail) {
        const char **pathlist = parse_path(filename, true);

        if (!pathlist) {
            *errmsg = "Invalid pathname specified";
            return (EACCESS);
        }

        for (dirp = dopt.dirs; *dirp; dirp++) {
            if (compare_paths(pathlist, *dirp) & 2)
                break;
        }
        if (!*dirp) {
            *errmsg = "Forbidden directory";
            return (EACCESS);
        }

        filename = fnbuf = build_path(dopt.path_prefix, pathlist);
        free(pathlist);
    }

    /*
     * We use different a different permissions scheme if `cancreate' is
     * set.
     */
    wmode = O_WRONLY | (dopt.cancreate ? O_CREAT : 0) |
        (pf->f_convert ? O_TEXT : O_BINARY);
    rmode = O_RDONLY | (pf->f_convert ? O_TEXT : O_BINARY);
#ifndef HAVE_FTRUNCATE
    wmode |= O_TRUNC;		/* This really sucks on a dupe */
#endif

    omode = mode == WRQ ? wmode : rmode;

    tftpd_log(LOG_DEBUG, "%s: final filename: %s", from_str, filename);

#if WITH_JAIL
    if (jail_root >= 0) {
        struct open_how how;
        memset(&how, 0, sizeof how);
        how.flags   = omode;
        how.mode    = (omode & O_CREAT) ? 0666 : 0;
        how.resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS;
        fd = openat2(jail_root, filename, &how, sizeof how);
    } else
#endif
    {
        fd = open(filename, omode, 0666);
    }
    if (fd < 0)
        fd = -errno;
    if (fnbuf)
        free(fnbuf);
    if (fd < 0)
        return fd;

    if (fstat(fd, &stbuf) < 0)
        exit(EX_OSERR);         /* This shouldn't happen */

    /* A duplicate RRQ or (worse!) WRQ packet could really cause havoc... */
    if (lock_file(fd, mode != RRQ))
	exit(0);                /* Assume a transfer is already underway */

    if (mode == RRQ) {
        if (!dopt.unixperms && (stbuf.st_mode & (S_IREAD >> 6)) == 0) {
            close(fd);
            *errmsg = "File must have global read permissions";
            return (EACCESS);
        }
        tsize.size = stbuf.st_size;
        /* We don't know the tsize if conversion is needed */
        tsize.mode = pf->f_convert ? TSIZE_NAK : TSIZE_READ;
    } else {
        if (!dopt.unixperms) {
            if ((stbuf.st_mode & (S_IWRITE >> 6)) == 0) {
                close(fd);
                *errmsg = "File must have global write permissions";
                return (EACCESS);
            }
        }

#ifdef HAVE_FTRUNCATE
	/* We didn't get to truncate the file at open() time */
	if (ftruncate(fd, (off_t) 0)) {
	  close(fd);
	  *errmsg = "Cannot reset file size";
	  return (EACCESS);
	}
#endif
        tsize.mode = TSIZE_WRITE;
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
static void tftp_sendfile(const struct formats *pf, struct tftphdr *oap,
                          int oacklen, const char *filename)
{
    struct tftphdr ack;         /* ack packet */
    uint16_t ap_opcode, ap_block;
    unsigned long r_timeout;
    int n;
    struct daemon_xfer_context context;
    struct tftp_xfer xfer;
    struct tftp_xfer_result result;
    struct tftp_io * volatile io = NULL;

    if (oap) {
        timeout = rexmtval;
        (void)sigsetjmp(timeoutbuf, 1);
    send_oack:
        r_timeout = timeout;
        if (send(peer, oap, oacklen, 0) != oacklen) {
            tftpd_log(LOG_WARNING, "tftpd: oack: %s\n", strerror(errno));
            goto out;
        }
        for (;;) {
            n = recv_time(peer, &ack, sizeof ack, 0, &r_timeout);
            if (n < 0) {
                tftpd_log(LOG_WARNING, "tftpd: read: %s", strerror(errno));
                goto out;
            }

            if (n < 2)
                continue;

            ap_opcode = ntohs(ack.th_opcode);
            ap_block  = ntohs(ack.th_block);

            if (ap_opcode == ERROR) {
                tftpd_log(LOG_WARNING,
                          "%s: client rejected negotiated options", from_str);
                goto out;
            } else if (n >= 4 && ap_opcode == ACK) {
                if (ap_block == 0)
                    break;
                else
                    goto send_oack;
            }
        }
    }

    xfree(oap);

    io = tftp_io_reader_start(file, pf->f_convert, windowsize,
                              io_ring_slots(), segsize, io_is_threaded());
    if (!io) {
        nak(-errno, NULL);
        goto out;
    }

    xfer.blocksize = segsize;
    xfer.windowsize = windowsize;
    xfer.rollover = rollover_val;
    xfer.resend_oack = false;
    xfer.control = &ack;
    xfer.control_size = sizeof ack;
    xfer.context = &context;
    xfer.ops = &daemon_xfer_ops;
    xfer.io_context = io;
    xfer.io_ops = &tftp_io_xfer_ops;
    tftp_xfer_send(&xfer, &result);

    switch (result.status) {
    case TFTP_XFER_READ_ERROR:
        nak(-result.error, NULL);
        break;
    case TFTP_XFER_SEND_ERROR:
        errno = result.error;
        tftpd_log(LOG_WARNING, "%s: write: %s", from_str, strerror(errno));
        break;
    case TFTP_XFER_RECV_ERROR:
        errno = result.error;
        tftpd_log(LOG_WARNING, "%s: read(ack): %s", from_str, strerror(errno));
        break;
    case TFTP_XFER_OK:
        tftpd_log(LOG_NOTICE, "%s: read completed: %s", from_str, filename);
        break;
    default:
        break;
    }

  out:
    tftp_io_stop(io);
    io = NULL;
    (void)fclose(file);
    file = NULL;
}

/*
 * Receive a file.
 */
static void tftp_recvfile(const struct formats *pf,
			  struct tftphdr *oack, int oacklen,
                          const char *filename)
{
    struct daemon_xfer_context context;
    struct tftp_xfer xfer;
    struct tftp_xfer_result result;
    struct tftphdr *initial_reply;
    int initial_reply_len;
    struct tftp_io * volatile io = NULL;
    struct tftphdr *datapkt;
    size_t datapktsize;

    if (oack) {
        initial_reply = oack;
        initial_reply_len = oacklen;
    } else {
        /* No OACK */
        initial_reply = xmalloc(4);
        initial_reply->th_opcode = htons(ACK);
        initial_reply->th_block = 0;
        initial_reply_len = 4;
    }

    io = tftp_io_writer_start(file, pf->f_convert, io_ring_slots(), segsize,
                              io_is_threaded());
    if (!io) {
        nak(-errno, NULL);
        goto out;
    }

    xfer.blocksize = segsize;
    xfer.windowsize = windowsize;
    xfer.rollover = rollover_val;
    xfer.resend_oack = false;
    xfer.control = NULL;
    xfer.control_size = 0;
    xfer.context = &context;
    xfer.ops = &daemon_xfer_ops;
    xfer.io_context = io;
    xfer.io_ops = &tftp_io_xfer_ops;

    datapktsize = xfer.blocksize + 4;
    datapkt = xmalloc(datapktsize);

    tftp_xfer_recv(&xfer, datapkt, datapktsize,
                   &initial_reply, initial_reply_len, NULL, 0, &result);

    xfree(datapkt);

    switch (result.status) {
    case TFTP_XFER_BAD_DATA:
        nak(EBADOP, "Data packet too large");
        break;
    case TFTP_XFER_WRITE_ERROR:
        nak(-result.error, NULL);
        break;
    case TFTP_XFER_SEND_ERROR:
        errno = result.error;
        tftpd_log(LOG_WARNING, "%s: write(ack): %s", from_str, strerror(errno));
        break;
    case TFTP_XFER_RECV_ERROR:
        errno = result.error;
        tftpd_log(LOG_WARNING, "%s: read: %s", from_str, strerror(errno));
        break;
    case TFTP_XFER_OK:
        tftpd_log(LOG_NOTICE, "%s: write completed: %s", from_str, filename);
        break;
    default:
        break;
    }

    tftp_io_stop(io);
    io = NULL;
    if (result.status != TFTP_XFER_OK)
        goto out;

    (void)fclose(file);
    file = NULL;
    daemon_xfer_dally(result.last_block);
  out:
    tftp_io_stop(io);
    if (file) {
        (void)fclose(file);
        file = NULL;
    }
    return;
}

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

    msg = error_msg(error);
    length = strlen(msg) + 1;
    tp = xmalloc(length + 4);

    /* Convert negative errno to TFTP error codes */
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
        if (error < 0 || !*msg)
            error = EUNDEF;
        break;
    }

    tp->th_opcode = htons(ERROR);
    tp->th_code   = htons(error);
    memcpy(tp->th_msg, msg, length);
    length += 4;                /* Add space for header */

    if (dopt.verbosity >= 2) {
        tftpd_log(LOG_INFO, "%s: sending ERROR %d: %s)",
                  from_str, error, tp->th_msg);
    }

    if (send(peer, tp, length, 0) != length)
        tftpd_log(LOG_WARNING, "%s: sending ERROR failed: %s",
                  from_str, strerror(errno));

    xfree(tp);
}
