/*
 * cap.h
 *
 * Capabilities handling
 */

#ifndef TFTPD_CAP_H
#define TFTPD_CAP_H 1

#include "config.h"

#define CAP_TYPE_NONE	0
#define CAP_TYPE_LINUX	1

#ifdef HAVE_CAP_SET_PROC
#ifdef __linux__
#define CAP_TYPE CAP_TYPE_LINUX
#endif
#endif

#ifndef CAP_TYPE
#define CAP_TYPE CAP_TYPE_NONE
#endif

#if CAP_TYPE

void cap_set_none(void);
void cap_set_before_initgroups(void);
void cap_set_after_initgroups(void);
void cap_set_before_listen(void);
void cap_set_before_socket_bind(void);
void cap_set_before_chroot(void);
void cap_set_before_setid(void);
void cap_set_drop_all(void);

#else

static inline void cap_set_none(void) { }
static inline void cap_set_after_initgroups(void) { }
static inline void cap_set_before_listen(void) { }
static inline void cap_set_after_listen(void) { }
static inline void cap_set_before_socket_bind(void) { }
static inline void cap_set_before_chroot(void) { }
static inline void cap_set_before_setid(void) { }
static inline void cap_set_drop_all(void) { }

#endif

#endif /* TFTPD_CAP_H */
