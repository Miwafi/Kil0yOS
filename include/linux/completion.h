/* linux/completion.h - no wait-queue primitive exists; nothing uses it */
#ifndef _COMPAT_LINUX_COMPLETION_H
#define _COMPAT_LINUX_COMPLETION_H

struct completion { int done; };

#endif /* _COMPAT_LINUX_COMPLETION_H */
