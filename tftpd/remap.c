/* ----------------------------------------------------------------------- *
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 *   Copyright 2001-2025 H. Peter Anvin - All Rights Reserved
 *
 * ----------------------------------------------------------------------- */

/*
 * remap.c
 *
 * Perform regular-expression based filename remapping.
 */

#include "config.h"             /* Must be included first! */
#include "tftpd.h"
#include "remap.h"

#include <ctype.h>
#include <regex.h>

#define DEADMAN_MAX_STEPS	4096    /* Timeout after this many steps */

#define RULE_REWRITE	0x01    /* This is a rewrite rule */
#define RULE_GLOBAL	0x02    /* Global rule (repeat until no match) */
#define RULE_EXIT	0x04    /* Exit after matching this rule */
#define RULE_RESTART	0x08    /* Restart at the top after matching this rule */
#define RULE_ABORT	0x10    /* Terminate processing with an error */
#define RULE_INVERSE	0x20    /* Execute if regex *doesn't* match */
#define RULE_IPV4	0x40	/* IPv4 only */
#define RULE_IPV6	0x80	/* IPv6 only */

#define RULE_HASFILE	0x100	/* Valid if rule results in a valid filename */
#define RULE_RRQ	0x200	/* Get (read) only */
#define RULE_WRQ	0x400	/* Put (write) only */
#define RULE_SEDG	0x800   /* sed-style global */

#define RULE_JUMP	0x1000  /* Jump rule */
#define RULE_LABEL	0x2000  /* Label */
#define RULE_NOREGEX	0x4000  /* The rule has no regular expression */

#define RULE_HAS_REGEX(x) (!((x) & RULE_NOREGEX))

int deadman_max_steps = DEADMAN_MAX_STEPS;

#if defined(HAVE_WCHAR_H) && defined(HAVE_WCTYPE_H) && \
    defined(HAVE_MBRTOWC) && defined(HAVE_TOWLOWER)
# define WITH_MB 1
#else
# define WITH_MB 0
#endif

struct rule {
    struct rule *next;
    unsigned int line;
    unsigned int rule_flags;
    regex_t rx;
    const char *pattern;        /* Replacement pattern or label name */
};

#if WITH_MB
typedef wint_t xform_int;
#define xform_toupper towupper
#define xform_tolower towlower
#else
typedef int xform_int;
#define xform_toupper toupper
#define xform_tolower tolower
#endif

typedef xform_int (*xform_func)(xform_int xc);

struct xform_state {
    xform_func xform;
    char *out;
    size_t len;
    bool err;
#if WITH_MB
    mbstate_t ps;
#endif
};

static xform_int xform_null(xform_int xc)
{
    return xc;
}

static void xform_init(struct xform_state *xs, char *string)
{
    memset(xs, 0, sizeof *xs);
    xs->xform = xform_null;
    xs->out = string;
}

#if WITH_MB

static const char *xform_out(struct xform_state *xs, const char *p, size_t len)
{
    static char dummy_mb_buf[MB_LEN_MAX];
    mbstate_t ips;
    char *q = xs->out;

    memset(&ips, 0, sizeof ips);

    while (len && *p && *p != '\\') {
        wchar_t wc;
        ssize_t nb = mbrtowc(&wc, p, len, &ips);

        if (nb > 0) {
            len -= nb;
            p += nb;
            wc = xs->xform(wc);
            nb = wcrtomb(q ? q : dummy_mb_buf, wc, &xs->ps);
            if (nb > 0) {
                xs->len += nb;
                if (q)
                    q += nb;
            }
        } else {
            /* Conversion error */
            xs->err = true;
            break;
        }
    }

    if (q) {
        *q = '\0';
        xs->out = q;
    }
    return p;
}

#else

static const char *xform_out(struct xform_state *xs, const char *p, size_t len)
{
    char *q = xs->out;

    while (len && *p && *p != '\\') {
        if (q)
            q++ = xs->xform((unsigned char)*p);
        xs->len++;
        p++;
        len--;
    }

    if (q) {
        *q = '\0';
        xs->out = q;
    }
    return p;
}

#endif

/*
 * Do \-substitution.  Call with string == NULL to get length only.
 * "start" indicates an offset into the input buffer where the pattern
 * match was started; *nextp points to the first character after the
 * pattern expansion.
 *
 * If start is set to MATCHONLY == (size_t)-1 or the pmatch array indicates
 * that no match was found, then the before and after match contents of
 * the input string are discarded.
 */
#define MATCHONLY ((size_t)-1)

static size_t null_macrosub(char macro, const char **macrodata)
{
    (void)macro;
    (void)macrodata;
    return (size_t)-1;
}

static size_t xsmemcpy(struct xform_state *xs, const char *from, size_t bytes)
{
    if (xs->out) {
        memcpy(xs->out, from, bytes);
        xs->out += bytes;
        *xs->out = '\0';         /* Enforce null termination */
    }
    xs->len += bytes;
    return xs->len;
}

static size_t
do_genmatchstring(char *string, const char *pattern,
                  const char *ibuf, const regmatch_t *pmatch,
                  match_pattern_callback macrosub,
                  size_t start, size_t *nextp)
{
    size_t endbytes;
    struct xform_state xs;
    const char *input;
    const char *pattern_end = strchr(pattern, '\0');

    if (!macrosub)
        macrosub = null_macrosub;

    xform_init(&xs, string);

    if (start == MATCHONLY) {
        input = ibuf;
        endbytes = 0;
    } else if (pmatch[0].rm_so == -1) {
        return xsmemcpy(&xs, ibuf, strlen(ibuf)+1);
    } else {
        input = ibuf + start;
        endbytes = strlen(input) - pmatch[0].rm_eo;
        xsmemcpy(&xs, ibuf, start + pmatch[0].rm_so);
    }

    /* Transform matched section */
    while (*pattern) {
        if (*pattern == '\\') {
            char macro = pattern[1];
            pattern += 2;

            switch (macro) {
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
            {
                const regmatch_t *pm = &pmatch[macro - '0'];
                if (pm->rm_so != -1) {
                    xform_out(&xs, input + pm->rm_so,
                              pm->rm_eo - pm->rm_so);
                }
                break;
            }

            case 'L':
                xs.xform = xform_tolower;
                break;

            case 'U':
                xs.xform = xform_toupper;
                break;

            case 'E':
                xs.xform = xform_null;
                break;

            case '\0':
                pattern--;
                /* fall through */
            case '\\':
                xs.len++;
                if (xs.out)
                    *xs.out++ = '\\';
                break;

            default:
            {
                const char *macrodata;
                size_t sublen = macrosub(macro, &macrodata);
                if (sublen != (size_t)-1) {
                    xform_out(&xs, macrodata, sublen);
                } else {
                    /* Ignore the backslash prefix */
                    pattern--;
                }
                break;
            }
            }
        } else {
            pattern = xform_out(&xs, pattern, pattern_end - pattern);
        }
    }

    /* Pointer to post-substitution tail */
    if (nextp)
        *nextp = xs.len;

    /* Copy section after match */
    xsmemcpy(&xs, input + pmatch[0].rm_eo, endbytes);

    return xs.err ? (size_t)-1 : xs.len;
}

/*
 * Ditto, but allocate the string in a new buffer
 */
static size_t
genmatchstring(char **string, const char *pattern,
               const char *ibuf, const regmatch_t *pmatch,
               match_pattern_callback macrosub,
               size_t start, size_t *nextp)
{
    size_t len;
    char *buf;

    len = do_genmatchstring(NULL, pattern, ibuf, pmatch,
                            macrosub, start, NULL);
    if (len == (size_t)-1) {
        *string = NULL;
        return len;
    }

    *string = buf = xmalloc(len + 1);
    return do_genmatchstring(buf, pattern, ibuf, pmatch,
                             macrosub, start, nextp);
}

/*
 * Extract a string terminated by non-escaped whitespace; ignoring
 * leading whitespace.  Consider an unescaped # to be a comment marker,
 * functionally \n.
 */
static size_t readescstring(char *buf, char **str)
{
    char *p = *str;
    bool wasbs = false;
    size_t len = 0;

    while (*p && isspace(*p))
        p++;

    if (!*p) {
        *buf = '\0';
        *str = p;
        return 0;
    }

    while (*p) {
        if (!wasbs && (isspace(*p) || *p == '#')) {
            *buf = '\0';
            *str = p;
            return len;
        }
        /* Important: two backslashes leave us in the !wasbs state! */
        wasbs = !wasbs && (*p == '\\');
        *buf++ = *p++;
        len++;
    }

    *buf = '\0';
    *str = p;
    return len;
}

/*
 * Parse a line into a set of instructions. Needs a work buffer
 * no shorter than the length of the line including final \0.
 */
static int parseline(char *line, struct rule *r, unsigned int lineno,
                     char *buffer)
{
    char *p;
    int rv;
    int rxflags = REG_EXTENDED;
    struct badcombo {
        unsigned int flags;
        char name[4];
    };
    static const struct badcombo badcombos[] = {
        { RULE_REWRITE | RULE_INVERSE, "r~" },
        { RULE_REWRITE | RULE_ABORT,   "ra" },
        { RULE_REWRITE | RULE_JUMP,    "rj" },
        { RULE_ABORT | RULE_JUMP,      "aj" },
        { RULE_ABORT | RULE_RESTART,   "as" },
        { RULE_EXIT | RULE_JUMP,       "ej" },
        { RULE_EXIT | RULE_ABORT,      "ae" },
        { RULE_EXIT | RULE_RESTART,    "es" },
        { RULE_EXIT | RULE_HASFILE,    "eE" },
        { RULE_HASFILE | RULE_JUMP,    "Ej" },
        { RULE_HASFILE | RULE_ABORT,   "aE" },
        { RULE_HASFILE | RULE_RESTART, "Es" },
        { 0, "" }
    };
    const struct badcombo *bc;

    memset(r, 0, sizeof *r);
    r->line = lineno;

    if (!readescstring(buffer, &line))
        return 0;               /* No rule found */

    p = buffer;
    if (*buffer == ':') {
        /* It is a label */
        r->rule_flags = RULE_LABEL | RULE_NOREGEX;
        p++;
        if (*p) {
            r->pattern = xstrdup(p);
            return 1;
        }
    }

    for (; *p; p++) {
        switch (*p) {
        case 'r':
            r->rule_flags |= RULE_REWRITE;
            break;
        case 'g':
            if (r->rule_flags & RULE_GLOBAL)
                r->rule_flags |= RULE_SEDG;
            else
                r->rule_flags |= RULE_GLOBAL;
            break;
        case 'e':
            r->rule_flags |= RULE_EXIT;
            break;
        case 'E':
            r->rule_flags |= RULE_HASFILE;
            break;
        case 's':
            r->rule_flags |= RULE_RESTART;
            break;
        case 'a':
            r->rule_flags |= RULE_ABORT;
            break;
        case 'j':
            r->rule_flags |= RULE_JUMP;
            break;
        case 'i':
            rxflags |= REG_ICASE;
            break;
        case '~':
            r->rule_flags |= RULE_INVERSE;
            rxflags |= REG_NOSUB;
            break;
        case '4':
            r->rule_flags |= RULE_IPV4;
            break;
        case '6':
            r->rule_flags |= RULE_IPV6;
            break;
        case 'G':
            r->rule_flags |= RULE_RRQ;
            break;
        case 'P':
            r->rule_flags |= RULE_WRQ;
            break;
        default:
            tftpd_log(LOG_ERR,
                      "remap: line %u: invalid operation flag \"%c\"",
                      *p, lineno);
            return -1;          /* Error */
            break;
        }
    }

    for (bc = badcombos; bc->flags; bc++) {
        if ((r->rule_flags & bc->flags) == bc->flags) {
            tftpd_log(LOG_ERR, "remap: line %u: rule flags %c and %c cannot be combined",
                      lineno, bc->name[0], bc->name[1]);
            return -1;
        }
    }

    if (!(r->rule_flags & RULE_REWRITE)) {
        /* RULE_GLOBAL and RULE_SEDG are meaningless without RULE_REWRITE */
        r->rule_flags &= ~(RULE_GLOBAL|RULE_SEDG);
    }

    if (RULE_HAS_REGEX(r->rule_flags)) {
        /* Read and compile the regex */
        if (!readescstring(buffer, &line)) {
            tftpd_log(LOG_ERR, "remap: line %u: regex missing", lineno);
            return -1;              /* Error */
        }

        if ((rv = regcomp(&r->rx, buffer, rxflags)) != 0) {
            char *errbuf = xmalloc(BUFSIZ);
            regerror(rv, &r->rx, errbuf, BUFSIZ);
            tftpd_log(LOG_ERR, "regex: line %u: bad regex: %s",
                   lineno, errbuf);
            return -1;              /* Error */
        }
    }

    /* Read the rewrite pattern, if any */
    if (readescstring(buffer, &line))
        r->pattern = xstrdup(buffer);
    else
        r->pattern = xstrdup("");

    return 1;                   /* Valid rule found */
}

#define MIN_LINE	64      /* Minimum size of allocated buffer */

/* Read a line into an allocated buffer; drops \n \r \0 */
static size_t read_line(FILE *f, char **bufp, size_t *bufsizep)
{
    char *buf = *bufp;
    size_t bufsize = *bufsizep;
    size_t len = 0;

    while (1) {
        int c = 0;

        while (len+1 < bufsize) {
            c = getc(f);

            if (c == EOF) {
                buf[len] = '\0';
                return len ? len : (size_t)-1;
            } else if (c == '\n') {
                buf[len] = '\0';
                return len;
            } else if (c != '\0' && c != '\r') {
                buf[len++] = c;
            }
        }

        if (bufsize < MIN_LINE)
            bufsize = MIN_LINE;
        else
            bufsize <<= 1;
        *bufsizep = bufsize;
        *bufp = buf = xrealloc(buf, bufsize);
    }
}

/* Read a rule file */
struct rule *parserulefile(FILE * f)
{
    char *line = NULL;
    char *parsebuf = NULL;
    size_t linesize = 0;
    size_t parsebufsize = 0;
    struct rule *first_rule = NULL;
    struct rule **last_rule = &first_rule;
    struct rule *this_rule = xmalloc(sizeof(struct rule));
    int rv;
    unsigned int lineno = 0;
    size_t len;
    bool err = false;

    while ((len = read_line(f, &line, &linesize)) != (size_t)-1) {
        lineno++;
        if (parsebufsize < linesize)
            parsebuf = xrealloc(parsebuf, parsebufsize = linesize);
        rv = parseline(line, this_rule, lineno, parsebuf);
        if (rv < 0)
            err = true;
        if (rv > 0) {
            *last_rule = this_rule;
            last_rule = &this_rule->next;
            this_rule = xmalloc(sizeof(struct rule));
        }
    }

    xfree(this_rule);          /* Last one is always unused */
    xfree(parsebuf);
    xfree(line);               /* Free buffer */

    if (err) {
        /* Bail on error, we have already logged an error message */
        exit(EX_CONFIG);
    }

    return first_rule;
}

/* Destroy a rule file data structure */
void freerules(struct rule *r)
{
    struct rule *next;

    while (r) {
        next = r->next;

        if (RULE_HAS_REGEX(r->rule_flags))
            regfree(&r->rx);

        xfree((void *)r->pattern);
        xfree(r);

        r = next;
    }
}

static void clear_pmatch(int pmatches, regmatch_t *pmatch)
{
    int i;
    for (i = 0; i < pmatches; i++)
        pmatch[i].rm_so = pmatch[i].rm_eo = -1;
}

/* Execute a rule set on a string; returns a malloc'd new string. */
char *rewrite_string(const struct formats *pf,
                     const char *input, const struct rule *rules,
                     int mode, int af, match_pattern_callback macrosub,
                     const char **errmsg)
{
    char *current = xstrdup(input);
    char *newstr = current;
    const char *accerr;
    const struct rule *ruleptr = rules;
    regmatch_t pmatch[10];
    int deadman = deadman_max_steps;
    unsigned int bad_flags;

    /* Default error */
    *errmsg = "Remap table failure";

    if (verbosity >= 3) {
        tftpd_log(LOG_INFO, "remap: input: %s", current);
    }

    bad_flags = RULE_LABEL | RULE_NOREGEX;
    if (mode != RRQ)    bad_flags |= RULE_RRQ;
    if (mode != WRQ)    bad_flags |= RULE_WRQ;
    if (af != AF_INET)  bad_flags |= RULE_IPV4;
    if (af != AF_INET6) bad_flags |= RULE_IPV6;

    ruleptr = rules;
    while (ruleptr) {
        const bool inverse    = !!(ruleptr->rule_flags & RULE_INVERSE);
        const int pmatches   = inverse ? 0 : 10;
        const int matchsense = inverse ? REG_NOMATCH : 0;
        bool was_match;
        const char *whatami;
        const struct rule *next = ruleptr->next;

        if (ruleptr->rule_flags & bad_flags)
            goto nextrule;

        if (!deadman--)
            goto dead;

        newstr = current;

        /* Clear the pmatch[] array for good measure */
        clear_pmatch(10, pmatch);
        was_match = regexec(&ruleptr->rx, newstr, pmatches, pmatch, 0)
            == matchsense;
        if (!was_match)
            goto nextrule;      /* Rule did not match */

        if (verbosity >= 5) {
            tftpd_log(LOG_INFO, "remap: line %u: hit on %s%.*s%s, replacement: \"%s\"",
                      ruleptr->line,
                      inverse ? "~" : "\"",
                      pmatch[0].rm_eo - pmatch[0].rm_so,
                      inverse ? "" : newstr + pmatch[0].rm_so,
                      inverse ? "" : "\"",
                      ruleptr->pattern);
        }

        whatami = "match";

        if (ruleptr->rule_flags & RULE_REWRITE) {
            size_t ggoffset = 0;

            whatami = "rewrite";

            while (1) {
                char *newerstr;
                size_t len;
                size_t gg0 = ggoffset + pmatch[0].rm_so;

                len = genmatchstring(&newerstr, ruleptr->pattern, newstr,
                                     pmatch, macrosub, ggoffset, &ggoffset);

                if (len == (size_t)-1) {
                    xfree(newstr);
                    return NULL;
                }

                if (verbosity >= 4) {
                    tftpd_log(LOG_INFO, "remap: line %u: rewrite step: \"%.*s\" -> \"%.*s\" [%d]",
                              ruleptr->line,
                              pmatch[0].rm_eo - pmatch[0].rm_so, newstr + gg0,
                              (int)(ggoffset - gg0), newerstr + gg0,
                              (int)(ggoffset - gg0));
                }

                if (newstr != current)
                    xfree(newstr);
                newstr = newerstr;

                if (!(ruleptr->rule_flags & RULE_GLOBAL))
                    break;

                if (!(ruleptr->rule_flags & RULE_SEDG))
                    ggoffset = 0;
                else if (ggoffset >= len)
                    break;

                clear_pmatch(pmatches, pmatch);
                if (regexec(&ruleptr->rx, newstr + ggoffset, pmatches,
                            pmatch, ggoffset ? REG_NOTBOL : 0) != matchsense)
                    break;

                if (!deadman--)
                    goto dead;
            }
        }

        if ((ruleptr->rule_flags & RULE_HASFILE) &&
            pf->f_validate(newstr, mode, pf, &accerr)) {
            if (verbosity >= 3) {
                tftpd_log(LOG_INFO, "remap: line %u: ignoring %s (%s)",
                       ruleptr->line, whatami, accerr);
            }
            was_match = false;
            if (newstr != current) {
                xfree(newstr);
                newstr = current;
            }
        } else if (newstr != current) {
            xfree(current);
            current = newstr;
            if (verbosity >= 3) {
                tftpd_log(LOG_INFO, "remap: line %u: rewrite result: %s",
                       ruleptr->line, current);
            }
        }

        if (!was_match)
            goto nextrule;

        newstr = NULL;
        if (ruleptr->rule_flags & (RULE_ABORT|RULE_JUMP)) {
            if ((ssize_t)genmatchstring(&newstr, ruleptr->pattern, current,
                                        pmatch, macrosub, MATCHONLY, NULL)
                <= 0) {
                xfree(newstr);
                newstr = NULL;
            }
        }

        if (ruleptr->rule_flags & RULE_ABORT) {
            if (verbosity >= 3) {
                tftpd_log(LOG_INFO, "remap: line %u: abort: %s",
                       ruleptr->line, current);
            }

            *errmsg = newstr;
            newstr = NULL;
            goto quit;
        }

        if (ruleptr->rule_flags & (RULE_EXIT|RULE_HASFILE)) {
                if (verbosity >= 3) {
                    tftpd_log(LOG_INFO, "remap: line %u: exit",
                           ruleptr->line);
            }
            return current; /* Exit here, we're done */
        }

        if (ruleptr->rule_flags & RULE_RESTART) {
            next = rules;
        } else if (ruleptr->rule_flags & RULE_JUMP) {
            if (!newstr) {
                tftpd_log(LOG_ERR, "remap: line %u: no label in j rule",
                       ruleptr->line);
                goto quit;
            }

            for (next = rules; next; next = next->next) {
                if ((next->rule_flags & RULE_LABEL) &&
                    !strcmp(newstr, next->pattern))
                    break;
            }
            if (!next) {
                tftpd_log(LOG_ERR, "remap: line %u: label not found: %s",
                       ruleptr->line, newstr);
                goto quit;
            }
        }

        if (verbosity >= 3) {
            if (next != ruleptr->next) {
                if ((next->rule_flags & RULE_LABEL) && next->pattern[0]) {
                    tftpd_log(LOG_INFO, "remap: line %u: jump to %s",
                           ruleptr->line, next->pattern);
                } else {
                    tftpd_log(LOG_INFO, "remap: line %u: jump to line %u",
                           ruleptr->line, next->line);
                }
            }
        }

    nextrule:
        ruleptr = next;
    }

    if (verbosity >= 3) {
        tftpd_log(LOG_INFO, "remap: done: %s", current);
    }
    return current;

dead:                           /* Deadman expired */
    tftpd_log(LOG_ERR,
           "remap: Breaking loop after %d steps, input = %s, last = %s",
           deadman_max_steps, input, newstr);
quit:
    if (newstr != current)
        xfree(newstr);
    xfree(current);
    return NULL;        /* Did not terminate! */
}
