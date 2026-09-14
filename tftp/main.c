/*
 * SPDX-License-Identifier: BSD-4-Clause-UC
 *
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (C) 2026 H. Peter Anvin <hpa@zytor.com>
 */

#include "common/tftpsubs.h"
#include "options.h"

/* Many bug fixes are from Jim Guyton <guyton@rand-unix> */

/*
 * TFTP User Program -- Command Interface.
 */
#include <sys/file.h>
#include <ctype.h>
#ifdef WITH_READLINE
#include <readline/readline.h>
#ifdef HAVE_READLINE_HISTORY_H
#include <readline/history.h>
#endif
#endif

#include "extern.h"

#define	TIMEOUT		1       /* secs between rexmt's */
#define TRIES		6       /* Number of attempts to send each packet */
#define TIMEOUT_LIMIT	((1 << TRIES)-1)
#define	LBUFLEN		200     /* size of input buffer */

/* In theory up to 65535 is supported, but limit it for safety */
#define TFTP_MAX_WINDOWSIZE	32768

struct modes {
    const char *m_name;
    const char *m_mode;
    int m_openflags;
};

static const struct modes modes[] = {
    {"netascii", "netascii", O_TEXT},
    {"ascii", "netascii", O_TEXT},
    {"octet", "octet", O_BINARY},
    {"binary", "octet", O_BINARY},
    {"image", "octet", O_BINARY},
    {0, 0, 0}
};

#define MODE_OCTET    (&modes[2])
#define MODE_NETASCII (&modes[0])
#define MODE_DEFAULT  MODE_NETASCII

struct tftp_options copt = {
    .mode = MODE_DEFAULT,
    .rexmtval = TIMEOUT,
    .maxtimeout = TIMEOUT_LIMIT * TIMEOUT
};

struct common_options xopt = {
#ifdef HAVE_IPV6
    .ai_fam = AF_UNSPEC,
#else
    .ai_fam = AF_INET,
#endif
    .max_blksize = SEGSIZE
};

union sock_addr peeraddr;
int f = -1;
static uint16_t port;
static bool connected;
#ifdef WITH_READLINE
static char *line = NULL;
#else
static char line[LBUFLEN];
#endif
static int margc;
#define MARGVSIZE 20
static char *margv[MARGVSIZE];
static const char *const prompt = "tftp> ";
sigjmp_buf toplevel;
static void intr(int);
static const struct servent *sp;

#ifdef HAVE_IPV6
static int ai_fam_sock = AF_UNSPEC;
#else
static int ai_fam_sock = AF_INET;
#endif

static int get(int, char **);
static int help(int, char **);
static int modecmd(int, char **);
static int put(int, char **);
static int quit(int, char **);
static int setascii(int, char **);
static int setbinary(int, char **);
static int setblocksize(int, char **);
static int setpeer(int, char **);
static int setrexmt(int, char **);
static int settimeout(int, char **);
static int settrace(int, char **);
static int set_verbosity(const char *, bool);
static int setverbose(int, char **);
static int status(int, char **);
static int setliteral(int, char **);
static int setwindowsize(int, char **);

static void command(void);

static void getusage(const char *);
static int makeargv(char *, char **);
static bool parse_uint_range(const char *, unsigned int, unsigned int,
                             unsigned int *);
static void putusage(const char *);
static void settftpmode(const struct modes *);

#define HELPINDENT (sizeof("connect"))

struct cmd {
    const char *name;
    const char *help;
    int (*handler) (int, char **);
};

static const struct cmd cmdtab[] = {
    {"connect",
     "connect to remote tftp",
     setpeer},
    {"mode",
     "set file transfer mode",
     modecmd},
    {"put",
     "send file",
     put},
    {"get",
     "receive file",
     get},
    {"quit",
     "exit tftp",
     quit},
    {"verbose",
     "toggle verbose mode",
     setverbose},
    {"trace",
     "toggle packet tracing",
     settrace},
    {"literal",
     "toggle literal mode, ignore ':' in file name",
     setliteral},
    {"status",
     "show current status",
     status},
    {"binary",
     "set mode to octet",
     setbinary},
    {"ascii",
     "set mode to netascii",
     setascii},
    {"blocksize",
     "set the requested transfer block size",
     setblocksize},
    {"rexmt",
     "set per-packet transmission timeout",
     setrexmt},
    {"timeout",
     "set total retransmission timeout",
     settimeout},
    {"windowsize",
     "set the requested transfer window size",
     setwindowsize},
    {"?",
     "print help information",
     help},
    {"help",
     "print help information",
     help},
    {0, 0, 0}
};

static const struct cmd *getcmd(const char *, const char **errtype);
static char *tail(char *);

static noreturn void usage(int errcode)
{
    fprintf(errcode ? stderr : stdout,
            "Usage: %s [options] [host [port]] [-c command...]\n"
            "  Options:\n"
            "    -V, --version              print version number and exit\n"
            "    -h, --help                 print this help text and exit\n"
#ifdef HAVE_IPV6
            "    -4. --ipv4                 only use IPv4, no IPv6\n"
            "    -6. --ipv6                 only use IPv6, no IPv4\n"
#endif
            "    -v, --verbose              increase logging verbosity\n"
            "    -l, --literal              disable host:path syntax\n"
            "    -m, --mode mode            set the transfer mode (netascii, octet)\n"
            "    -a, --ascii                alias for --mode netascii\n"
            "        --netascii             alias for --mode netascii\n"
            "        --text                 alias for --mode netascii\n"
            "    -b, --binary               alias for --mode octet\n"
            "        --octet                alias for --mode octet\n"
            "    -R, --port-range min:max   use emhermeral ports in the given range\n"
            "    -B, --blocksize size       set the requested transfer block size\n"
            "    -W, --windowsize size      set the requested transfer window size\n"
            "    -c, --command command      execute \"command\", then exit (must be last)\n",
            _progname);

    exit(errcode);
}

static bool parse_uint_range(const char *arg, unsigned int minimum,
                             unsigned int maximum, unsigned int *value)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(arg, &end, 10);
    if (errno || *arg == '\0' || *end || parsed < minimum ||
        parsed > maximum)
        return false;

    *value = (unsigned int)parsed;
    return true;
}

static const struct option long_options[] = {
    { "ipv4",       no_argument,       NULL, '4' },
    { "ipv6",       no_argument,       NULL, '6' },
    { "verbose",    no_argument,       NULL, 'v' },
    { "version",    no_argument,       NULL, 'V' },
    { "literal",    no_argument,       NULL, 'l' },
    { "mode",       required_argument, NULL, 'm' },
    { "command",    no_argument,       NULL, 'c' },
    { "port-range", required_argument, NULL, 'R' },
    { "blocksize",  required_argument, NULL, 'B' },
    { "windowsize", required_argument, NULL, 'W' },
    { "ascii",      no_argument,       NULL, 'a' },
    { "text",       no_argument,       NULL, 'a' },
    { "netascii",   no_argument,       NULL, 'a' },
    { "binary",     no_argument,       NULL, 'b' },
    { "octet",      no_argument,       NULL, 'b' },
    { "help",       no_argument,       NULL, 'h' },
    { NULL,         0,                 NULL, 0 }
};

static const char short_options[] = "+46vVlm:cR:B:W:w:abh";

int main(int argc, char *argv[])
{
    union sock_addr sa;
    int optc, ret;
    static int pargc, peerargc;
    static char **pargv;
    char *peerargv[3];

    set_progname(argv[0]);
    random_init();

    copt.mode = MODE_DEFAULT;

    peerargv[0] = argv[0];
    peerargc = 1;

    while (!copt.iscmd) {
        optc = getopt_long(argc, argv, short_options, long_options,
                           NULL);
        if (optc == -1) {
            if (optind == argc)
                break;
            if (peerargc >= 3)
                usage(EX_USAGE);
            peerargv[peerargc++] = argv[optind++];
            continue;
        }

        switch (optc) {
        case '4':
            xopt.ai_fam = AF_INET;
            break;
        case '6':
#ifdef HAVE_IPV6
            xopt.ai_fam = AF_INET6;
#else
            fprintf(stderr, "%s: this version compiled without IPv6 support\n",
                    _progname);
            exit(EX_UNAVAILABLE);
#endif
            break;
        case 'v':
            if (optarg && *optarg) {
                set_verbosity(optarg, true);
            } else {
                copt.verbose++;
            }
            break;
        case 'V':
            /* Print version and configuration to stdout and exit */
            printf("%s\n", TFTP_CONFIG_STR);
            exit(0);
        case 'b':
            settftpmode(MODE_OCTET);
            break;
        case 'a':
            settftpmode(MODE_NETASCII);
            break;
        case 'l':
            copt.literal = true;
            break;
        case 'm':
        {
            const struct modes *p;

            for (p = modes; p->m_name; p++) {
                if (!strcmp(optarg, p->m_name))
                    break;
            }
            if (p->m_name) {
                settftpmode(p);
            } else {
                fprintf(stderr, "%s: invalid mode: %s\n", argv[0], optarg);
                exit(EX_USAGE);
            }
            break;
        }
        case 'c':
            copt.iscmd = true;
            break;
        case 'R':
            if (sscanf(optarg, "%u:%u", &xopt.portrange_from,
                       &xopt.portrange_to) != 2 ||
                !xopt.portrange_from ||
                xopt.portrange_from > xopt.portrange_to ||
                xopt.portrange_to > 65535) {
                fprintf(stderr, "Bad port range: %s\n", optarg);
                exit(EX_USAGE);
            }
            break;
        case 'B':
            if (!parse_uint_range(optarg, 8, MAX_SEGSIZE,
                                  &xopt.max_blksize)) {
                fprintf(stderr, "Bad block size: %s (8-%d)\n", optarg,
                        MAX_SEGSIZE);
                exit(EX_USAGE);
            }
            break;
        case 'W':
        case 'w':
            if (!parse_uint_range(optarg, 1, TFTP_MAX_WINDOWSIZE,
                                  &xopt.max_windowsize)) {
                fprintf(stderr, "Bad window size: %s (valid range is 1-%u)\n",
                        optarg, TFTP_MAX_WINDOWSIZE);
                exit(EX_USAGE);
            }
            break;
        case 'h':
            usage(0);
            break;
        default:
            usage(EX_USAGE);
        }
    }

    ai_fam_sock = xopt.ai_fam;

    pargv = argv + optind;
    pargc = argc - optind;

    sp = getservbyname("tftp", "udp");
    if (sp == 0) {
        struct servent *fallback_sp;

        /* Use canned values */
        if (copt.verbose)
            fprintf(stderr,
                    "tftp: tftp/udp: unknown service, faking it...\n");
        fallback_sp = xmalloc(sizeof(*fallback_sp));
        fallback_sp->s_name = (char *)"tftp";
        fallback_sp->s_aliases = NULL;
        fallback_sp->s_port = htons(IPPORT_TFTP);
        fallback_sp->s_proto = (char *)"udp";
        sp = fallback_sp;
    }

    /* Allow SIGINT in non-interactive mode to terminate the program */
    if (!copt.iscmd)
        tftp_signal(SIGINT, intr, 0);

    if (peerargc > 1) {
        /* Set peer */
        if (sigsetjmp(toplevel, 1) != 0)
            exit(EX_NOHOST);
        (void)setpeer(peerargc, peerargv);
    }

    if (ai_fam_sock == AF_UNSPEC)
        ai_fam_sock = AF_INET;

    f = socket(ai_fam_sock, SOCK_DGRAM, 0);
    if (f < 0) {
        perror("tftp: socket");
        exit(EX_OSERR);
    }
    bzero(&sa, sizeof(sa));
    sa.sa.sa_family = ai_fam_sock;
    if (pick_port_bind(f, &sa)) {
        perror("tftp: bind");
        exit(EX_OSERR);
    }

    if (copt.iscmd) {
        /* -c specified; execute command and exit */
        const struct cmd *c;
        const char *errtype;
        static char *splitbuf = NULL;

        if (pargc == 1) {
            /* Only one string, see if it should be split */
            splitbuf = xstrdup(pargv[0]);
            pargc = makeargv(splitbuf, pargv = margv);
        }

        if (!pargc || !pargv[0] || !*pargv[0]) {
            fprintf(stderr, "%s: missing command after -c\n", _progname);
            exit(EX_USAGE);
        }

        c = getcmd(pargv[0], &errtype);
        if (!c) {
            fprintf(stderr, "%s: %s command: %s\n",
                    _progname, errtype, pargv[0]);
            exit(EX_USAGE);
        }

        ret = sigsetjmp(toplevel, 1);
        if (!ret) {
            ret = (*c->handler) (pargc, pargv);
            xfree(splitbuf);
        }
        exit(ret);
    }
#ifdef WITH_READLINE
#ifdef HAVE_READLINE_HISTORY_H
    using_history();
#endif
#endif

    if (sigsetjmp(toplevel, 1) != 0)
        (void)putchar('\n');
    command();

    return 0;                   /* Never reached */
}

static char *hostname;

/* Called when a command is incomplete; modifies
   the global variable "line" */
static bool getmoreargs(const char *partial, const char *mprompt)
{
    if (copt.iscmd)
        return false;

#ifdef WITH_READLINE
    char *eline;
    int len, elen;

    len = strlen(partial);
    eline = readline(mprompt);
    if (!eline)
        exit(0);                /* EOF */

    elen = strlen(eline);

    if (line) {
        free(line);
        line = NULL;
    }
    line = xmalloc(len + elen + 1);
    strcpy(line, partial);
    strcpy(line + len, eline);
    free(eline);

#ifdef HAVE_READLINE_HISTORY_H
    add_history(line);
#endif
#else
    int len = strlen(partial);

    strcpy(line, partial);
    fputs(mprompt, stdout);
    if (fgets(line + len, LBUFLEN - len, stdin) == 0)
        if (feof(stdin))
            exit(0);            /* EOF */
#endif
    return true;
}

static int setpeer(int argc, char *argv[])
{
    int err;

    if (argc < 2) {
        if (!getmoreargs("connect ", "(to) "))
            return EX_USAGE;
        margc = makeargv(line, margv);
        argc = margc;
        argv = margv;
    }
    if ((argc < 2) || (argc > 3)) {
        printf("usage: %s host-name [port]\n", argv[0]);
        return EX_USAGE;
    }

    peeraddr.sa.sa_family = xopt.ai_fam;
    err = set_sock_addr(argv[1], &peeraddr, &hostname, false);
    if (err) {
        printf("Error: %s\n", gai_strerror(err));
        printf("%s: unknown host\n", argv[1]);
        connected = false;
        return EX_NOHOST;
    }
    xopt.ai_fam = peeraddr.sa.sa_family;
    if (f == -1) { /* socket not open */
        ai_fam_sock = xopt.ai_fam;
    } else { /* socket was already open */
        if (ai_fam_sock != xopt.ai_fam) { /* need reopen socken for new family */
            union sock_addr sa;

            close(f);
            ai_fam_sock = xopt.ai_fam;
            f = socket(ai_fam_sock, SOCK_DGRAM, 0);
            if (f < 0) {
                perror("tftp: socket");
                exit(EX_OSERR);
            }
            bzero((char *)&sa, sizeof (sa));
            sa.sa.sa_family = ai_fam_sock;
            if (pick_port_bind(f, &sa)) {
                perror("tftp: bind");
                exit(EX_OSERR);
            }
        }
    }
    port = sp->s_port;
    if (argc == 3) {
        const struct servent *usp;
        usp = getservbyname(argv[2], "udp");
        if (usp) {
            port = usp->s_port;
        } else {
            unsigned long myport;
            char *ep;
            myport = strtoul(argv[2], &ep, 10);
            if (*ep || myport > 65535UL) {
                printf("%s: bad port number\n", argv[2]);
                connected = false;
                return EX_USAGE;
            }
            port = htons((uint16_t) myport);
        }
    }

    if (copt.verbose) {
        char tmp[INET6_ADDRSTRLEN];
        const char *tp;
        tp = inet_ntop(peeraddr.sa.sa_family, SOCKADDR_P(&peeraddr),
                       tmp, INET6_ADDRSTRLEN);
        if (!tp)
            tp = "???";
        printf("Connected to %s (%s), port %u\n",
               hostname, tp, (unsigned int)ntohs(port));
    }
    connected = true;
    return 0;
}

static int modecmd(int argc, char *argv[])
{
    const struct modes *p;
    const char *sep;

    if (argc < 2) {
        printf("Using %s mode to transfer files.\n", copt.mode->m_mode);
        return 0;
    }
    if (argc == 2) {
        for (p = modes; p->m_name; p++)
            if (strcmp(argv[1], p->m_name) == 0)
                break;
        if (p->m_name) {
            settftpmode(p);
            return 0;
        }
        printf("%s: unknown mode\n", argv[1]);
        /* drop through and print usage message */
    }

    printf("usage: %s [", argv[0]);
    sep = " ";
    for (p = modes; p->m_name; p++) {
        printf("%s%s", sep, p->m_name);
        if (*sep == ' ')
            sep = " | ";
    }
    printf(" ]\n");
    return EX_USAGE;
}

static int setbinary(int argc, char *argv[])
{
    (void)argc;
    (void)argv;                 /* Quiet unused warning */
    settftpmode(MODE_OCTET);
    return 0;
}

static int setascii(int argc, char *argv[])
{
    (void)argc;
    (void)argv;                 /* Quiet unused warning */
    settftpmode(MODE_NETASCII);
    return 0;
}

static void settftpmode(const struct modes *newmode)
{
    copt.mode = newmode;
    if (copt.verbose)
        printf("mode set to %s\n", copt.mode->m_mode);
}

/*
 * Send file(s).
 */
static int put(int argc, char *argv[])
{
    int fd;
    int n, err;
    char *cp;
    char *targ;

    if (argc < 2) {
        if (!getmoreargs("send ", "(file) "))
            return EX_USAGE;
        margc = makeargv(line, margv);
        argc = margc;
        argv = margv;
    }
    if (argc < 2) {
        putusage(argv[0]);
        return EX_USAGE;
    }
    targ = argv[argc - 1];
    if (!copt.literal && strchr(argv[argc - 1], ':')) {
        for (n = 1; n < argc - 1; n++)
            if (strchr(argv[n], ':')) {
                putusage(argv[0]);
                return EX_USAGE;
            }
        cp = argv[argc - 1];
        targ = strchr(cp, ':');
        *targ++ = 0;
        peeraddr.sa.sa_family = xopt.ai_fam;
        err = set_sock_addr(cp, &peeraddr, &hostname, false);
        if (err) {
            printf("Error: %s\n", gai_strerror(err));
            printf("%s: unknown host\n", argv[1]);
            connected = false;
            return EX_NOHOST;
        }
        xopt.ai_fam = peeraddr.sa.sa_family;
        connected = true;
    }
    if (!connected) {
        printf("No target machine specified.\n");
        return EX_USAGE;
    }
    if (argc < 4) {
        cp = argc == 2 ? tail(targ) : argv[1];
        fd = open(cp, O_RDONLY | copt.mode->m_openflags);
        if (fd < 0) {
            fprintf(stderr, "tftp: ");
            perror(cp);
            return EX_OSERR;
        }
        if (copt.verbose)
            printf("putting %s to %s:%s [%s]\n",
                   cp, hostname, targ, copt.mode->m_mode);
        sa_set_port(&peeraddr, port);
        return tftp_sendfile(fd, targ, copt.mode->m_mode,
                             xopt.max_windowsize);
    }
    /* this assumes the target is a directory */
    /* on a remote unix system.  hmmmm.  */
    err = 0;
    for (n = 1; n < argc - 1; n++) {
        const char *base = tail(argv[n]);
        char *remotepath = xmalloc(strlen(targ) + 1 + strlen(base) + 1);

        sprintf(remotepath, "%s/%s", targ, base);
        fd = open(argv[n], O_RDONLY | copt.mode->m_openflags);
        if (fd < 0) {
            fprintf(stderr, "tftp: ");
            perror(argv[n]);
            free(remotepath);
            if (!err)
                err = EX_OSERR;
            continue;
        }
        if (copt.verbose)
            printf("putting %s to %s:%s [%s]\n",
                   argv[n], hostname, remotepath, copt.mode->m_mode);
        sa_set_port(&peeraddr, port);
        n = tftp_sendfile(fd, remotepath, copt.mode->m_mode,
                          xopt.max_windowsize);
        if (!err)
            err = n;
        free(remotepath);
    }
    return err;
}

static void putusage(const char *s)
{
    printf("usage: %s file ... host:target, or\n", s);
    printf("       %s file ... target (when already connected)\n", s);
}

/*
 * Receive file(s).
 */
static int get(int argc, char *argv[])
{
    int fd;
    int n, err;
    char *cp;
    char *src;

    if (argc < 2) {
        if (!getmoreargs("get ", "(files) "))
            return EX_USAGE;
        margc = makeargv(line, margv);
        argc = margc;
        argv = margv;
    }
    if (argc < 2) {
        getusage(argv[0]);
        return EX_USAGE;
    }
    if (!connected) {
        for (n = 1; n < argc; n++)
            if (copt.literal || strchr(argv[n], ':') == 0) {
                getusage(argv[0]);
                return EX_USAGE;
            }
    }
    err = 0;
    for (n = 1; n < argc; n++) {
        src = strchr(argv[n], ':');
        if (copt.literal || src == NULL)
            src = argv[n];
        else {
            int resolve_error;

            *src++ = 0;
            peeraddr.sa.sa_family = xopt.ai_fam;
            resolve_error = set_sock_addr(argv[n], &peeraddr, &hostname, false);
            if (resolve_error) {
                printf("Warning: %s\n", gai_strerror(resolve_error));
                printf("%s: unknown host\n", argv[1]);
                if (!err)
                    err = EX_NOHOST;
                continue;
            }
            xopt.ai_fam = peeraddr.sa.sa_family;
            connected = true;
        }
        if (argc < 4) {
            cp = argc == 3 ? argv[2] : tail(src);
            fd = open(cp, O_WRONLY | O_CREAT | O_TRUNC | copt.mode->m_openflags,
                      0666);
            if (fd < 0) {
                fprintf(stderr, "tftp: ");
                perror(cp);
                return EX_OSERR;
            }
            if (copt.verbose)
                printf("getting from %s:%s to %s [%s]\n",
                       hostname, src, cp, copt.mode->m_mode);
            sa_set_port(&peeraddr, port);
            err = tftp_recvfile(fd, src, copt.mode->m_mode,
                                xopt.max_windowsize);
            break;
        }
        cp = tail(src);         /* new .. jdg */
        fd = open(cp, O_WRONLY | O_CREAT | O_TRUNC | copt.mode->m_openflags,
                  0666);
        if (fd < 0) {
            fprintf(stderr, "tftp: ");
            perror(cp);
            if (!err)
                err = EX_OSERR;
            continue;
        }
        if (copt.verbose)
            printf("getting from %s:%s to %s [%s]\n",
                   hostname, src, cp, copt.mode->m_mode);
        sa_set_port(&peeraddr, port);
        n = tftp_recvfile(fd, src, copt.mode->m_mode,
                          xopt.max_windowsize);
        if (!err)
            err = n;
    }
    return err;
}

static void getusage(const char *s)
{
    printf("usage: %s host:file host:file ... file, or\n", s);
    printf("       %s file file ... file if connected\n", s);
}

static int setrexmt(int argc, char *argv[])
{
    int t;

    if (argc < 2) {
        if (!getmoreargs("rexmt-timeout ", "(value) "))
            return EX_USAGE;
        argc = margc = makeargv(line, margv);
        argv = margv;
    }
    if (argc != 2) {
        printf("usage: %s value\n", argv[0]);
        return EX_USAGE;
    }
    t = atoi(argv[1]);
    if (t < 1) {
        printf("%s: bad value\n", argv[1]);
        return EX_USAGE;
    } else {
        copt.rexmtval = t;
        copt.maxtimeout = copt.rexmtval * TIMEOUT_LIMIT;
    }
    return 0;
}

static int settimeout(int argc, char *argv[])
{
    int t;

    if (argc < 2) {
        if (!getmoreargs("maximum-timeout ", "(value) "))
            return EX_USAGE;
        argc = margc = makeargv(line, margv);
        argv = margv;
    }
    if (argc != 2) {
        printf("usage: %s value\n", argv[0]);
        return EX_USAGE;
    }
    t = atoi(argv[1]);
    if (t < 1) {
        printf("%s: bad value\n", argv[1]);
        return EX_USAGE;
    } else
        copt.maxtimeout = t;
    return 0;
}

static int setblocksize(int argc, char *argv[])
{
    if (argc < 2) {
        if (!getmoreargs("blocksize ", "(size) "))
            return EX_USAGE;
        argc = margc = makeargv(line, margv);
        argv = margv;
    }
    if (argc != 2) {
        printf("usage: %s size\n", argv[0]);
        return EX_USAGE;
    }
    if (!parse_uint_range(argv[1], 8, MAX_SEGSIZE, &xopt.max_blksize)) {
        printf("%s: bad block size (valid range is 8-%d)\n",
               argv[1], MAX_SEGSIZE);
        return EX_USAGE;
    }
    return 0;
}

static int setwindowsize(int argc, char *argv[])
{
    if (argc < 2) {
        if (!getmoreargs("windowsize ", "(size) "))
            return EX_USAGE;
        argc = margc = makeargv(line, margv);
        argv = margv;
    }
    if (argc != 2) {
        printf("usage: %s size\n", argv[0]);
        return EX_USAGE;
    }
    if (!parse_uint_range(argv[1], 1, TFTP_MAX_WINDOWSIZE,
                          &xopt.max_windowsize)) {
        printf("%s: bad window size (valid range is 1-%u)\n",
               argv[1], TFTP_MAX_WINDOWSIZE);
        return EX_USAGE;
    }
    return 0;
}

static int setliteral(int argc, char *argv[])
{
    (void)argc;
    (void)argv;                 /* Quiet unused warning */
    copt.literal = !copt.literal;
    printf("Literal mode %s.\n", copt.literal ? "on" : "off");
    return 0;
}

static int status(int argc, char *argv[])
{
    (void)argc;
    (void)argv;                 /* Quiet unused warning */
    if (connected)
        printf("Connected to %s.\n", hostname);
    else
        printf("Not connected.\n");
    printf("Mode: %s Verbose: %s Tracing: %s Literal: %s\n", copt.mode->m_mode,
           copt.verbose ? "on" : "off", copt.trace ? "on" : "off",
           copt.literal ? "on" : "off");
    printf("Rexmt-interval: %d seconds, Max-timeout: %d seconds\n",
           copt.rexmtval, copt.maxtimeout);
    printf("Blocksize: %u, windowsize: %u\n", xopt.max_blksize,
           xopt.max_windowsize ? xopt.max_windowsize : 1);
    return 0;
}

static void intr(int sig)
{
    int err;

    alarm(0);
    tftp_signal(SIGALRM, SIG_DFL, 0);
    if (sig == SIGALRM)
        err = EX_TEMPFAIL;
    else
        err = sig + 128;

    siglongjmp(toplevel, err);
}

static char *tail(char *filename)
{
    char *s;

    while (*filename) {
        s = strrchr(filename, '/');
        if (s == NULL)
            break;
        if (s[1])
            return (s + 1);
        *s = '\0';
    }
    return (filename);
}

/*
 * Command parser.
 */
static void command(void)
{
    const struct cmd *c;
    const char *errtype;

    for (;;) {
#ifdef WITH_READLINE
        if (line) {
            free(line);
            line = NULL;
        }
        line = readline(prompt);
        if (!line)
            exit(0);            /* EOF */
#else
        fputs(prompt, stdout);
        if (fgets(line, LBUFLEN, stdin) == 0) {
            if (feof(stdin)) {
                exit(0);
            } else {
                continue;
            }
        }
#endif
        if ((line[0] == 0) || (line[0] == '\n'))
            continue;
#ifdef WITH_READLINE
#ifdef HAVE_READLINE_HISTORY_H
        add_history(line);
#endif
#endif
        margc = makeargv(line, margv);
        if (margc == 0)
            continue;

        c = getcmd(margv[0], &errtype);
        if (!c) {
            printf("Error: %s command: %s\n", errtype, margv[0]);
            continue;
        }
        (void)(*c->handler) (margc, margv);
    }
}

static const struct cmd *getcmd(const char *name, const char **errtype)
{
    const char *p, *q;
    const struct cmd *c, *found;
    int nmatches, longest;

    *errtype = NULL;
    longest = 0;
    nmatches = 0;
    found = 0;
    for (c = cmdtab; (p = c->name) != NULL; c++) {
        for (q = name; *q == *p++; q++)
            if (*q == 0)        /* exact match? */
                return (c);
        if (!*q) {              /* the name was a prefix */
            if (q - name > longest) {
                longest = q - name;
                nmatches = 1;
                found = c;
            } else if (q - name == longest)
                nmatches++;
        }
    }
    if (nmatches > 1) {
        *errtype = "ambiguous";
        return NULL;
    } else if (!nmatches) {
        *errtype = "invalid";
        return NULL;
    }

    return (found);
}

/*
 * Slice a string up into argc/argv. Silently discards any words beyond
 * MARGVSIZE - 1 (leaving room for the NULL terminator) rather than
 * overflowing the fixed-size margv[] array.
 *
 * The string is modified in-place!
 *
 * XXX: handle quotes and escapes!
 */
static int makeargv(char *str, char **argp)
{
    char *cp;
    char ** const argpend = &argp[MARGVSIZE - 1];
    int argc = 0;

    margc = 0;
    for (cp = str; *cp;) {
        while (isspace(*cp))
            cp++;
        if (*cp == '\0')
            break;
        if (argp >= argpend)
            break;
        *argp++ = cp;
        argc++;
        while (*cp != '\0' && !isspace(*cp))
            cp++;
        if (*cp == '\0')
            break;
        *cp++ = '\0';
    }
    *argp++ = 0;
    return argc;
}

static int quit(int argc, char *argv[])
{
    (void)argc;
    (void)argv;                 /* Quiet unused warning */
    exit(0);
}

/*
 * Help command.
 */
static int help(int argc, char *argv[])
{
    const struct cmd *c;

    printf("%s\n", VERSION);

    if (argc == 1) {
        printf("Commands may be abbreviated.  Commands are:\n\n");
        for (c = cmdtab; c->name; c++)
            printf("%-*s\t%s\n", (int)HELPINDENT, c->name, c->help);
        return 0;
    }
    while (--argc > 0) {
        const char *errtype;
        char *arg;
        arg = *++argv;
        c = getcmd(arg, &errtype);
        if (!c)
            printf("help: %s command %s\n", errtype, arg);
        else
            printf("%s\n", c->help);
    }
    return 0;
}

static int settrace(int argc, char *argv[])
{
    (void)argc;
    (void)argv;                 /* Quiet unused warning */

    copt.trace = !copt.trace;
    printf("Packet tracing %s.\n", copt.trace ? "on" : "off");
    return 0;
}

static int set_verbosity(const char *to, bool startup)
{
    const char *name;

    if (to) {
        char *ep;
        long v = strtol(to, &ep, 0);
        if (*to && !*ep && v == (int)v) {
            copt.verbose = v;
        } else {
            if (startup) {
                fprintf(stderr, "%s: invalid verbosity level: %s\n",
                        _progname, to);
                exit(EX_USAGE);
            } else {
                printf("Invalid verbosity level: %s\n", to);
                return EX_USAGE;
            }
        }
    } else {
        copt.verbose = !copt.verbose;
    }

    switch (copt.verbose) {
    case 0:
        name = "off";
        break;
    case 1:
        name = "on";
        break;
    default:
        name = (copt.verbose < 0) ? "quiet" : "high";
        break;
    }

    if (!startup)
        printf("Verbosity set to level %d (%s).\n", copt.verbose, name);
    return 0;
}

static int setverbose(int argc, char *argv[])
{
    (void)argc;
    return set_verbosity(argv[1], false);
}
