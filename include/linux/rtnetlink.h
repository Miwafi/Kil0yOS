/* linux/rtnetlink.h - rtnl semaphore is a no-op on the uniprocessor compat layer */
#ifndef _COMPAT_LINUX_RTNETLINK_H
#define _COMPAT_LINUX_RTNETLINK_H

#include "linux/compiler.h"

static inline void rtnl_lock(void)   { barrier(); }
static inline void rtnl_unlock(void) { barrier(); }

#endif /* _COMPAT_LINUX_RTNETLINK_H */
