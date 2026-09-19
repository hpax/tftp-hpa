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
    .maxtimeout = TIMEOUT_LIMIT * TIMEOUT,
    .tsize = true
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

static void help_init(void);
static int print_cmd_help(const char *);

static int get(int, char **);
static int getfiles(int, char **, const char *);
static int help(int, char **);
static int mget(int, char **);
static int modecmd(int, char **);
static int mput(int, char **);
static int put(int, char **);
static int putfiles(int, char **, bool);
static int quit(int, char **);
static int setascii(int, char **);
static int setbinary(int, char **);
static int setblocksize(int, char **);
static int setpeer(int, char **);
static int setrexmt(int, char **);
static int set_transfer_host(char *, const char *);
static int settimeout(int, char **);
static int settrace(int, char **);
static int set_verbosity(const char *, bool);
static int setverbose(int, char **);
static int status(int, char **);
static int setliteral(int, char **);
static int setwindowsize(int, char **);
static int settsize(int, char **);

static void command(void);

static void getusage(const char *);
static int makeargv(char *, char **);
static bool parse_uint_range(const char *, unsigned int, unsigned int,
                             unsigned int *);
static void putusage(const char *);
static void settftpmode(const struct modes *);

struct cmd {
    const char *name;
    int (*handler) (int, char **);
    const char *shorthelp;
    const char *longhelp;
};

static const struct cmd cmdtab[] = {
    {
        "c[onnect]", setpeer,
        "connect to remote tftp server",
        "  connect host [port]\n"
        "    Set the host, and optionally port, for transfers. TFTP does not\n"
        "    maintain connections between transfers, so this command merely\n"
        "    remembers the host to use. A remote host can instead be specified\n"
        "    as part of an [m]get or [m]put command.\n"
    },
    {
        "m[ode]", modecmd,
        "set file transfer mode",
        "  mode {netascii|octet}\n"
        "    Specify the transfer mode. netascii converts line endings and\n"
        "    octet transfers binary data without conversion. The default is\n"
        "    netascii.\n"
    },
    {
        "p[ut]", put,
        "send (upload) file or files",
        "  put [local-file] [host:]remote-file\n"
        "    Upload a file to a remote file or directory. If 'local-file'\n"
        "    is not specified, it is assumed to be the same as 'remote-file'\n"
        "    with any directory portion removed. If 'remote-file' is\n"
        "    to be considered a directory, it needs to end in '/'.\n"
        "  put local-file local-file... [host:]remote-directory\n"
        "    Upload two or more files to a directory on the remote host.\n"
        "    See also the 'mput' command.\n"
        "\n"
        "  The remote host is assumed to use '/' as its directory separator.\n"
        "  The remote name may be of the form host:filename to specify\n"
        "  the remote host to connect to, as if the 'connect' command had\n"
        "  been used to specify this host.\n"
        "  Enable literal mode to prevent special treatment of ':' in filenames.\n"
    },
    {
        "mp[ut]", mput,
        "send (upload) multiple files",
        "  mput local-file... [host:]remote-directory\n"
        "    Upload one or more files to a directory on the remote host.\n"
        "\n"
        "  The remote host is assumed to use '/' as its directory separator.\n"
        "  The remote name may be of the form host:filename to specify\n"
        "  the remote host to connect to, as if the 'connect' command had\n"
        "  been used to specify this host.\n"
        "  Enable literal mode to prevent special treatment of ':' in filenames.\n"
    },
    {
        "g[et]", get,
        "receive (download) file or files",
        "  get [host:]remote-file [local-file]\n"
        "    Download a file from a remote file or directory. If 'local-file'\n"
        "    is not specified, it is assumed to be the same as 'remote-file'\n"
        "    with any directory portion removed.\n"
        "  get [host:]remote-file [host:]remote-file [host:]remote-file...\n"
        "    Download three or more files into the current directory on the\n"
        "   local system. See also the 'mget' command.\n"
        "\n"
        "  The remote host is assumed to use '/' as its directory separator.\n"
        "  The remote name may be of the form host:filename to specify\n"
        "  the remote host to connect to, as if the 'connect' command had\n"
        "  been used to specify this host.\n"
        "  Enable literal mode to prevent special treatment of ':' in filenames.\n"
    },
    {
        "mg[et]", mget,
        "receive (download) multiple files",
        "  mget [host:]remote-file... local-directory\n"
        "    Download one or more files from from a remote directory into\n"
        "    a specified directory on the local system.\n"
        "\n"
        "  The remote host is assumed to use '/' as its directory separator.\n"
        "  The remote name may be of the form host:filename to specify\n"
        "  the remote host to connect to, as if the 'connect' command had\n"
        "  been used to specify this host.\n"
        "  Enable literal mode to prevent special treatment of ':' in filenames.\n"
    },
    {
        "q[uit]", quit,
        "exit tftp",
        "  quit\n"
        "    Exit tftp. End-of-file also exits.\n"
    },
    {
        "v[erbose]", setverbose,
        "toggle or set message verbosity",
        "  verbose [level]\n"
        "    Toggle verbose mode or set the verbosity level.\n"
    },
    {
        "tr[ace]", settrace,
        "toggle packet tracing",
        "  trace\n"
        "    Toggle packet tracing, a debugging feature.\n"
    },
    {
        "l[iteral]", setliteral,
        "toggle literal mode, ignore ':' in file name",
        "  literal\n"
        "    Toggle literal mode. When enabled, this mode prevents special\n"
        "    treatment of ':' in filenames.\n"
    },
    {
        "st[atus]", status,
        "show current status",
        "  status\n"
        "    Show current status.\n"
    },
    {
        "b[inary]", setbinary,
        "set mode to octet",
        "  binary\n"
        "    Set the transfer mode to octet (shorthand for \"mode octet\".)\n"
    },
    {
        "a[scii]", setascii,
        "set mode to netascii",
        "  ascii\n"
        "    Set the transfer mode to netascii (shorthand for \"mode netascii\".)\n"
    },
    {
        "bl[ocksize]", setblocksize,
        "set the requested transfer block size",
        "  blocksize size\n"
        "    Request the RFC 2348 blksize option with 'size' bytes per data\n"
        "    block. Valid values are 8 through 65464.\n"
    },
    {
        "r[exmt]", setrexmt,
        "set per-packet transmission timeout",
        "  rexmt packet-timeout\n"
        "    Set the initial per-packet retransmission timeout in seconds. Each\n"
        "    subsequent retry doubles this interval.\n"
    },
    {
        "ti[meout]", settimeout,
        "set total retransmission timeout",
        "  timeout total-timeout\n"
        "    Set the maximum retransmission interval in seconds.\n"
    },
    {
        "tsi[ze]", settsize,
        "toggle sending tsize option",
        "  tsize\n"
        "    Toggle the sending of the TFTP tsize (transfer size) option.\n"
        "    The default is to send the tsize option.\n"
    },
    {
        "win[dowsize]", setwindowsize,
        "set the requested transfer window size",
        "  windowsize size\n"
        "    Request the RFC 7440 windowsize option with 'size' data\n"
        "    blocks per window. Valid values are 1 through 32768.\n"
    },
    {
        "?", help,
        "print help information",
        "  help [command ...]\n"
        "  ? [command ...]\n"
        "    Print help information. With command names, display detailed help\n"
        "    for each command.\n"
    },
    {
        "h[elp]", help,
        "print help information",
        "  help [command ...]\n"
        "  ? [command ...]\n"
        "    Print help information. With command names, display detailed help\n"
        "    for each command.\n"
    }
};

static const char *getcmd(const char *, const struct cmd **cmdp);
static char *tail(char *);

static void usage(int errcode)
{
    fprintf(errcode ? stderr : stdout,
            "Usage: %s [options] [host [port]] [-c command...]\n"
            "  Options:\n"
            "    -V, --version              print version number and exit\n"
            "    -h, --help                 print this help text and exit\n"
            "        --help=command         print the help text for \"command\" and exit\n"
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
            "    -T, --no-tsize             disable sending the tsize TFTP option\n"
            "    -c, --command command      execute \"command\", then exit (must be last)\n"
            "    -c, --command help         get a list of available commands\n"
            ,  _progname);

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
    { "no-tsize",   no_argument,       NULL, 'T' },
    { "help",       optional_argument, NULL, 'h' },
    { NULL,         0,                 NULL, 0 }
};

static const char short_options[] = "+46vVlm:cR:B:W:w:abTh";

int main(int argc, char *argv[])
{
    union sock_addr sa;
    int optc, ret;
    static int pargc, peerargc;
    static char **pargv;
    char *peerargv[3];

    set_progname(argv[0]);
    random_init();
    help_init();

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
        case 'T':
            copt.tsize = false;
            break;
        case 'h':
            if (optarg && *optarg)
                exit(print_cmd_help(optarg));
            else
                usage(0);
            break;
        default:
            usage(EX_USAGE);
            break;
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
        fallback_sp->s_port = htons(TFTP_IP_PORT);
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

        errtype = getcmd(pargv[0], &c);
        if (errtype) {
            fprintf(stderr, "%s: %s command: %s (did you mean %s?)\n",
                    _progname, errtype, pargv[0], c->name);
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

    err = set_transfer_host(argv[1], argc == 3 ? argv[2] : NULL);
    if (err) {
        if (err == EAI_SERVICE)
            printf("%s: bad port number\n", argv[2]);
        else {
            printf("Error: %s\n", gai_strerror(err));
            printf("%s: unknown host\n", argv[1]);
        }
        connected = false;
        return err == EAI_SERVICE ? EX_USAGE : EX_NOHOST;
    }
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
    if (argc == 2)
        port = sp->s_port;

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

static int set_transfer_host(char *host, const char *port_name)
{
    const struct servent *service;
    int err;

    peeraddr.sa.sa_family = xopt.ai_fam;
    err = set_sock_addr(host, &peeraddr, &hostname, false);
    if (err)
        return err;

    if (port_name) {
        service = getservbyname(port_name, "udp");
        if (service) {
            port = service->s_port;
        } else {
            char *ep;
            unsigned long port_number = strtoul(port_name, &ep, 10);

            if (*ep || port_number > 65535UL)
                return EAI_SERVICE;

            port = htons((uint16_t)port_number);
        }
    }

    xopt.ai_fam = peeraddr.sa.sa_family;
    connected = true;
    return 0;
}

/*
 * Is this a directory (ending in /)?
 */
static bool is_directory(const char *filename)
{
    const char *ep = strchr(filename, 0);
    return ep > filename && ep[-1] == '/';
}

/*
 * Send file(s).
 */
static int putfiles(int argc, char *argv[], bool target_is_directory)
{
    int fd;
    int n, err, result;
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
        err = set_transfer_host(cp, NULL);
        if (err) {
            printf("Error: %s\n", gai_strerror(err));
            printf("%s: unknown host\n", cp);
            connected = false;
            return EX_NOHOST;
        }
    }
    if (!connected) {
        printf("No target machine specified.\n");
        return EX_USAGE;
    }
    if (!target_is_directory &&
        (argc < 3 || (argc == 3 && !is_directory(targ)))) {
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
        result = tftp_sendfile(fd, remotepath, copt.mode->m_mode,
                               xopt.max_windowsize);
        if (!err)
            err = result;
        free(remotepath);
    }
    return err;
}

static int put(int argc, char *argv[])
{
    return putfiles(argc, argv, false);
}

static int mput(int argc, char *argv[])
{
    if (argc < 3) {
        putusage(argv[0]);
        return EX_USAGE;
    }
    return putfiles(argc, argv, true);
}

static void putusage(const char *s)
{
    printf("usage: %s file ... host:target, or\n", s);
    printf("       %s file ... target (when already connected)\n", s);
}

/*
 * Receive file(s).
 */
static int getfiles(int argc, char *argv[], const char *local_directory)
{
    int fd;
    int n, err, result;
    char *cp;
    char *src;

    if (argc < 2) {
        if (!getmoreargs("get ", "(files) "))
            return EX_USAGE;
        margc = makeargv(line, margv);
        argc = margc;
        argv = margv;
    }
    if (argc < 2 + !!local_directory) {
        getusage(argv[0]);
        return EX_USAGE;
    }
    if (!connected) {
        for (n = 1; n < argc - !!local_directory; n++)
            if (copt.literal || strchr(argv[n], ':') == 0) {
                getusage(argv[0]);
                return EX_USAGE;
            }
    }
    err = 0;
    for (n = 1; n < argc - !!local_directory; n++) {
        src = strchr(argv[n], ':');
        if (copt.literal || src == NULL)
            src = argv[n];
        else {
            int resolve_error;

            *src++ = 0;
            resolve_error = set_transfer_host(argv[n], NULL);
            if (resolve_error) {
                printf("Warning: %s\n", gai_strerror(resolve_error));
                printf("%s: unknown host\n", argv[n]);
                if (!err)
                    err = EX_NOHOST;
                continue;
            }
        }
        if (!local_directory &&
            (argc < 3 || (argc == 3 && !is_directory(argv[2])))) {
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
        if (local_directory) {
            const char *base = tail(src);

            cp = xmalloc(strlen(local_directory) + 1 + strlen(base) + 1);
            sprintf(cp, "%s/%s", local_directory, base);
        } else {
            cp = tail(src);     /* new .. jdg */
        }
        fd = open(cp, O_WRONLY | O_CREAT | O_TRUNC | copt.mode->m_openflags,
                  0666);
        if (fd < 0) {
            fprintf(stderr, "tftp: ");
            perror(cp);
            if (!err)
                err = EX_OSERR;
            if (local_directory)
                free(cp);
            continue;
        }
        if (copt.verbose)
            printf("getting from %s:%s to %s [%s]\n",
                   hostname, src, cp, copt.mode->m_mode);
        sa_set_port(&peeraddr, port);
        result = tftp_recvfile(fd, src, copt.mode->m_mode,
                               xopt.max_windowsize);
        if (!err)
            err = result;
        if (local_directory)
            free(cp);
    }
    return err;
}

static int get(int argc, char *argv[])
{
    return getfiles(argc, argv, NULL);
}

static int mget(int argc, char *argv[])
{
    if (argc < 3) {
        getusage(argv[0]);
        return EX_USAGE;
    }
    return getfiles(argc, argv, argv[argc - 1]);
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

static int settsize(int argc, char *argv[])
{
    (void)argc;
    (void)argv;                 /* Quiet unused warning */
    copt.tsize = !copt.tsize;
    printf("tsize option %s.\n", copt.tsize ? "on" : "off");
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
    printf("Blocksize: %u, windowsize: %u, tsize: %s\n", xopt.max_blksize,
           xopt.max_windowsize ? xopt.max_windowsize : 1,
           copt.tsize ? "on" : "off");
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

        errtype = getcmd(margv[0], &c);
        if (errtype) {
            printf("Error: %s command: %s (did you mean %s?)\n",
                   errtype, margv[0], c->name);
            continue;
        }
        (void)(*c->handler) (margc, margv);
    }
}

static const char *getcmd(const char *name, const struct cmd **cmdp)
{
    const char *p, *q;
    const struct cmd *cmd;
    const struct cmd *best = &cmdtab[0];
    int best_metric = -1;

    for (cmd = cmdtab; cmd < ARRAY_END(cmdtab); cmd++) {
        bool ok    = false;
        bool stop  = false;
        int metric = 0;
        for (p = name, q = cmd->name; !stop; q++) {
            unsigned char pc = *p;
            unsigned char qc = *q;
            switch (qc) {
            case '\0':
            case ']':
                /* Exact match if end of input, otherwise fail */
                ok = !pc;
                stop = true;
                break;

            case '[':
                ok = true;      /* Valid prefix, so far at least */
                /* Do not advance p here */
                break;

            default:
                if (!pc) {
                    /* ok if and only if it is a valid prefix */
                    stop = true;
                } else if (tolower(pc) != qc) {
                    /* Character mismatch */
                    ok   = false;
                    stop = true;
                } else {
                    metric++;
                    p++;
                }
                break;
            }
        }

        if (ok) {
            *cmdp = cmd;
            return NULL;
        } else if (metric > best_metric) {
            best = cmd;
            best_metric = metric;
        }
    }

    /* Failure, but suggest the best match */
    *cmdp = best;
    return "unknown";
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
static int helpindent = 0;

static void help_init(void)
{
    const struct cmd *c;

    for (c = cmdtab; c < ARRAY_END(cmdtab); c++) {
        int len = strlen(c->name);
        if (len > helpindent)
            helpindent = len;
    }
}

static int print_cmd_help(const char *cmd)
{
    const struct cmd *c;
    const char *errtype;

    errtype = getcmd(cmd, &c);
    if (errtype) {
        printf("help: %s command %s (did you mean %s?)\n",
               errtype, cmd, c->name);
        return EX_USAGE;
    } else {
        printf("%-*s  %s\n%s",
               helpindent, c->name, c->shorthelp, c->longhelp);
        return 0;
    }
}

static int help(int argc, char *argv[])
{
    int err = 0;

    if (argc == 1) {
        const struct cmd *c;

        printf("%s command list\n"
               "Commands may be abbreviated as indicated by [...]\n",
               VERSION);
        for (c = cmdtab; c < ARRAY_END(cmdtab); c++)
            printf("  %-*s  %s\n", helpindent, c->name, c->shorthelp);
    } else {
        int i;

        for (i = 1; i < argc; i++) {
            if (i > 1)
                putchar('\n');      /* Blank line between help texts */
            err |= print_cmd_help(argv[i]);
        }
    }

    return err;
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
