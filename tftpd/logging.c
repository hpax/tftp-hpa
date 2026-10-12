/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1999-2009, 2026 H. Peter Anvin
 * Copyright (c) 2011-2014 Intel Corporation; author: H. Peter Anvin
 * All rights reserved.
 */

#include "config.h"             /* Must be included first */
#include "options.h"
#include "tftpd.h"

/*
 * Logging framework
 */

static void tftpd_log_file(int priority, const char *fmt, ...);
static void tftpd_log_file_tagged(int priority, const char *fmt, ...);
static void tftpd_reopenlog_file(void);
static void tftpd_reopenlog_syslog(void);
static void tftpd_rotatelog_file(void);

static const struct tftpd_log_ops log_ops_file = {
    .log	= tftpd_log_file,
    .reopen_log	= tftpd_reopenlog_file,
    .rotate_log = tftpd_rotatelog_file,
};
static const struct tftpd_log_ops log_ops_file_tagged = {
    .log	= tftpd_log_file_tagged,
    .reopen_log	= tftpd_reopenlog_file,
    .rotate_log = tftpd_rotatelog_file,
};
static const struct tftpd_log_ops log_ops_syslog = {
    .log	= syslog,
    .reopen_log	= tftpd_reopenlog_syslog,
    .rotate_log = tftpd_reopenlog_syslog,
};

const struct tftpd_log_ops *log_ops = &log_ops_file;
static FILE *log_filehandle;

static const char *prio_name(unsigned int priority)
{
    static const char * const log_markers[] = {
        /* Human readable */
        [LOG_EMERG]   = "emergency: ",
        [LOG_ALERT]   = "alert: ",
        [LOG_CRIT]    = "critical: ",
        [LOG_ERR]     = "error: ",
        [LOG_WARNING] = "warning: ",
        [LOG_NOTICE]  = "notice: ",
        [LOG_INFO]    = "info: ",
        [LOG_DEBUG]   = "debug: "
    };
    const char *tag;

    if (unlikely(priority >= ARRAY_SIZE(log_markers)))
        return "";

    tag = log_markers[priority];
    if (unlikely(!tag))
        return "";

    return tag;
}

static void tftpd_log_file(int priority, const char *fmt, ...)
{
    va_list ap;

    fprintf(log_filehandle, "%s[%lu]: %s",
            _progname ? _progname : "tftpd",
            (unsigned long)_progpid,
            prio_name(priority));

    va_start(ap, fmt);
    vfprintf(log_filehandle, fmt, ap);
    va_end(ap);
    putc('\n', log_filehandle);
}

static void tftpd_log_file_tagged(int priority, const char *fmt, ...)
{
    va_list ap;

    fprintf(log_filehandle, "<%d>", priority);

    va_start(ap, fmt);
    vfprintf(log_filehandle, fmt, ap);
    va_end(ap);
    putc('\n', log_filehandle);
}

/*
 * dup() stdout/stderr when used as log output so that closing the
 * standard descriptors doesn't break anything.
 */
static void tftpd_openlog_fd(int new_fd)
{
    FILE *old_fh = log_filehandle;
    int old_fd;

    if (new_fd <= 2) {
        int dup_fd = dup(new_fd);
        if (dup_fd < 0) {
            fprintf(stderr, "%s: dup(%d) for logging failed: %s\n",
                    _progname, new_fd, strerror(errno));
            exit(EX_OSERR);
        }
        new_fd = dup_fd;
    }

    fflush(NULL);
    old_fd = fileno(old_fh);

    if (old_fd != new_fd) {
        FILE *new_fh;

        new_fh = fdopen(new_fd, "at");
        if (new_fh) {
            if (old_fd > 2)
                fclose(old_fh);

            setvbuf(new_fh, NULL, _IOLBF, BUFSIZ);
            log_filehandle = new_fh;
            log_ops = dopt.log_tagged ? &log_ops_file_tagged : &log_ops_file;
        } else {
            if (new_fd > 2)
                close(new_fd);
            fprintf(stderr, "%s: fdopen for logging failed: %s\n",
                    _progname, strerror(errno));
            exit(EX_OSERR);
        }
    }
}

static void tftpd_openlog_file(bool rotate)
{
    const char * const path = dopt.logfile;
    int fd;

    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        tftpd_log(rotate ? LOG_ERR : LOG_CRIT,
                  "cannot %s log file '%s': %s",
                  rotate ? "reopen" : "open", path, strerror(errno));
        if (!rotate)
            exit(EX_CANTCREAT);
    } else {
        tftpd_openlog_fd(fd);
    }
}

static void tftpd_reopenlog_file(void)
{
    fflush(log_filehandle);
}

static void tftpd_rotatelog_file(void)
{
    if (dopt.log_type == LOG_FILE)
        tftpd_openlog_file(true);
}

static void tftpd_openlog_syslog(void)
{
    openlog(_progname, LOG_PID | LOG_NDELAY, LOG_DAEMON);
    log_ops = &log_ops_syslog;
}

static void tftpd_reopenlog_syslog(void)
{
    closelog();
    tftpd_openlog_syslog();
}

/* This should be run very early, after set_progname() */
void tftpd_initlog(void)
{
    /* Temporarily log directly to stderr */
    log_filehandle = stderr;
    log_ops = &log_ops_file;
}

void tftpd_openlog(void)
{
    switch (dopt.log_type) {
    case LOG_SYS:
        tftpd_openlog_syslog();
        break;
    case LOG_STDOUT:            /* == 1 */
    case LOG_STDERR:            /* == 2 */
        tftpd_openlog_fd(dopt.log_type);
        break;
    case LOG_FILE:
        tftpd_openlog_file(false);
        break;
    default:
        break;
    }
}
