/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 H. Peter Anvin <hpa@zytor.com>
 */

/*
 * cap-linux.c
 *
 * Linux capability management
 */

#include "tftpd.h"
#include "options.h"

#include <pwd.h>

#include <sys/capability.h>

/* A bitmask of the privileges that need to be reserved or retained */
enum priv_mask {
    PRIV_NONE           =  0,
    PRIV_LISTEN		=  1,
    PRIV_PORTRANGE	=  2,
    PRIV_CHROOT		=  4,
    PRIV_SETUID         =  8,
    PRIV_SETGID         = 16,

    PRIV_ALL            = 31
};

struct need_priv {
    enum priv_mask when;
    cap_value_t cap;
};

static const struct need_priv need_privs[] =
{
    { PRIV_LISTEN,    CAP_NET_BIND_SERVICE },
    { PRIV_PORTRANGE, CAP_NET_BIND_SERVICE },
    { PRIV_CHROOT,    CAP_SYS_CHROOT },
    { PRIV_SETUID,    CAP_SETUID },
    { PRIV_SETGID,    CAP_SETGID },
};

/*
 * Set the current capability set to "now", unless already dropped;
 * permanently drop all in the "drop" set.
 *
 * If a bit is set in both the "now" and "drop" set, it will actually be
 * dropped on the *next* call.
 */
static void capset_privs(enum priv_mask now, enum priv_mask drop)
{
    static enum priv_mask effective;
    static enum priv_mask permitted;
    static enum priv_mask dropped;
    enum priv_mask keep;
    static cap_t ocap = NULL;
    static bool ocap_failed = false;
    cap_t ncap = NULL;
    const struct need_priv *np;
    int err;

    dropped |= drop;

    if (!ocap) {
        if (ocap_failed)
            return;

        ocap = cap_get_proc();
        if (!ocap) {
            if (errno != EPERM)
                goto fail;
            tftpd_log(LOG_WARNING,
                      "failed to query capabilities, cannot drop: %s",
                      strerror(errno));
            ocap_failed = true;
            return;
        }

        /*
         * Compute the effective and permitted set as per enum priv_mask
         */
        permitted = PRIV_ALL;
        ARRAY_FOREACH(np, need_privs) {
            cap_flag_value_t v;

            if (cap_get_flag(ocap, np->cap, CAP_PERMITTED, &v) || !v)
                permitted &= ~np->when;
        }
        dropped |= PRIV_ALL & ~permitted; /* In effect, already dropped */

        now  &= permitted;
        keep  = permitted & (now | ~dropped);

        /* Never bypass the cap setting the first time */
    } else {
        now  &= permitted;
        keep  = permitted & (now | ~dropped);

        if (now == effective && keep == permitted)
            return;                 /* Nothing to do */
    }

    ncap = cap_init();
    if (!ncap)
        goto fail;

    /*
     * ncap is empty at this point. Note that the inheritable
     * capability set is left as at all times, since tftpd should
     * never exec() anything.
     */

    ARRAY_FOREACH(np, need_privs) {
        if (np->when & keep) {
            cap_set_flag(ncap, CAP_PERMITTED, 1, &np->cap, CAP_SET);
            if (np->when & now)
                cap_set_flag(ncap, CAP_EFFECTIVE, 1, &np->cap, CAP_SET);
        }
    }

    if (cap_compare(ocap, ncap) && cap_set_proc(ncap))
        goto fail;

    effective = now;
    permitted = keep;
    ocap = ncap;
    ncap = NULL;

    err = 0;
    goto finish;
fail:
    err = errno;
finish:
    if (ncap)
        cap_free(ncap);

    if (err) {
        tftpd_log(LOG_CRIT, "failed to drop unwanted capabilities: %s",
                  strerror(err));
        exit(EX_NOPERM);
    }
}

static bool can_drop_setuid(uid_t uid)
{
    uid_t ruid, euid, suid;

    if (getresuid(&ruid, &euid, &suid))
        return errno == EPERM;

    return uid == ruid || uid == euid || uid == suid;
}

static bool can_drop_setgid(gid_t gid)
{
    gid_t rgid, egid, sgid;
    int ngroups, i;
    gid_t *list;
    bool ok;

    if (getresgid(&rgid, &egid, &sgid))
        return errno == EPERM;

    if (gid == rgid || gid == egid || gid == sgid)
        return true;

    ngroups = getgroups(0, NULL);
    if (ngroups < 0)
        return false;

    list = xmalloc(ngroups * sizeof(gid_t));
    ngroups = getgroups(ngroups, list);

    /* This implicitly handles ngroups < 0 */
    ok = false;
    for (i = 0; i < ngroups; i++) {
        if (gid == list[i]) {
            ok = true;
            break;
        }
    }
    xfree(list);
    return ok;
}

static uintmax_t read_uint_file(const char *file)
{
    uintmax_t val;
    FILE *f = fopen(file, "r");
    if (!f)
	return -1ULL;

    if (fscanf(f, "%"SCNuMAX, &val) < 1)
        val = -1ULL;

    fclose(f);
    return val;
}

static bool portrange_needs_priv_ports(void)
{
    uintmax_t unpriv;

    if (!xopt.portrange_from)
        return false;

    unpriv = read_uint_file("/proc/sys/net/ipv4/ip_unprivileged_port_start");
    return xopt.portrange_from < unpriv;
}

void capset_none(void)
{
    capset_privs(PRIV_NONE, PRIV_NONE);
}

void capset_before_initgroups(void)
{
    enum priv_mask drop = 0;

    if (!dopt.standalone)
        drop |= PRIV_LISTEN;

    if (!portrange_needs_priv_ports())
        drop |= PRIV_PORTRANGE;

    if (!dopt.secure)
        drop |= PRIV_CHROOT;

    if (can_drop_setuid(dopt.user.pw->pw_uid))
        drop |= PRIV_SETUID;

    capset_privs(PRIV_SETGID, drop);
}

void capset_after_initgroups(void)
{
    enum priv_mask drop = 0;

    if (can_drop_setgid(dopt.user.pw->pw_gid))
        drop |= PRIV_SETGID;

    capset_privs(PRIV_NONE, drop);
}

/* --- these two are called only if standalone --- */
void capset_before_listen(void)
{
    capset_privs(PRIV_LISTEN, PRIV_LISTEN);
}

void capset_before_socket_bind(void)
{
    capset_privs(PRIV_PORTRANGE, PRIV_PORTRANGE);
}

void capset_before_chroot(void)
{
    capset_privs(PRIV_CHROOT, PRIV_CHROOT);
}

void capset_before_setid(void)
{
    capset_privs(PRIV_SETUID|PRIV_SETGID, PRIV_ALL);
}

void capset_drop_all(void)
{
    capset_privs(PRIV_NONE, PRIV_ALL);
}

const char *capset_get_type(void)
{
    return "Linux";
}
