/* AF_UNIX sockets (P1.4). Kernel-side transport for the Unix domain
 * socket family: the X11 socket and D-Bus buses are the consumers.
 *
 * Design notes:
 *  - SOCK_STREAM: byte ring with SCM_RIGHTS records keyed to absolute
 *    consume offsets (fd passing).
 *  - SOCK_DGRAM / SOCK_SEQPACKET: framed ring, one message per record
 *    (16-byte header + payload), sender recorded for recvfrom(2).
 *  - connect() pre-creates the server-side child socket and queues it on
 *    the listener, exactly like Linux: data sent before accept(2) is
 *    already visible to the accepted connection.
 *  - Blocking is coarse: the syscall layer parks the caller on
 *    g_unix_wq (WAIT_RESTART) and re-executes; kernel side only ever
 *    returns -EAGAIN when it would block.
 *
 * No dynamic registration with the VFS: pathname sockets live in a
 * kernel bind table (addr space separate from the memory fs). unlink(2)
 * is hooked in the syscall layer.
 */
#ifndef NET_UNIX_SOCK_H
#define NET_UNIX_SOCK_H

#include "lib/types.h"

#define UNIX_PATH_MAX   108
#define UNIX_RXBUF_SZ   (64 * 1024)
#define UNIX_BACKLOG    16
#define UNIX_SCM_MAX    8       /* fds per SCM_RIGHTS record */
#define UNIX_SCM_QLEN   8       /* undelivered records per socket */

/* socket states */
#define US_IDLE      0
#define US_LISTENING 1
#define US_CONNECTED 2
#define US_CLOSED    3

/* sockaddr_un layout (user side) */
struct lnx_sockaddr_un {
    uint16_t family;
    char     path[UNIX_PATH_MAX];
};

struct unix_iovec {
    void*  base;
    size_t len;
};

struct file;                /* SCM_RIGHTS payload (core/file.h) */

typedef struct unix_sock unix_sock_t;

/* lifecycle */
unix_sock_t* unix_socket_create(int type);   /* 1=stream 2=dgram 5=seqpacket */
void         unix_sock_close(unix_sock_t* s);

/* addressing: pathlen < 0 means abstract address of -pathlen bytes */
int          unix_bind(unix_sock_t* s, const char* path, int pathlen);
int          unix_listen(unix_sock_t* s, int backlog);
int          unix_connect(unix_sock_t* s, const char* path, int pathlen);
unix_sock_t* unix_accept(unix_sock_t* s);    /* NULL = nothing pending */
int          unix_socketpair(unix_sock_t* a, unix_sock_t* b);

/* data path: all return -L_EAGAIN when they would block */
int          unix_send(unix_sock_t* s, const void* buf, int len);
int          unix_sendto_dgram(unix_sock_t* s, const void* buf, int len,
                               const char* path, int pathlen);
int          unix_recv(unix_sock_t* s, void* buf, int len);   /* stream: 0=EOF */

int          unix_sendmsg(unix_sock_t* s, const struct unix_iovec* iv, int iovcnt,
                          struct file** fds, int nfds);
int          unix_recvmsg(unix_sock_t* s, const struct unix_iovec* iv, int iovcnt,
                          struct file** out_fds, int* out_nfds);

/* readiness: POLL_IN|POLL_OUT|POLL_ERR bits (core/file.h values) */
int          unix_poll_bits(unix_sock_t* s);

/* introspection for the syscall layer (type is opaque) */
int          unix_sock_type(unix_sock_t* s);    /* 1=stream 2=dgram 5=seqpacket */
int          unix_sock_state(unix_sock_t* s);   /* US_* */
int          unix_sock_has_peer(unix_sock_t* s);
unix_sock_t* unix_sock_peer(unix_sock_t* s);

/* name/cred reporting */
int          unix_getsockname(unix_sock_t* s, char* out /*UNIX_PATH_MAX*/);
int          unix_getpeername(unix_sock_t* s, char* out);
void         unix_peer_cred(unix_sock_t* s, int* pid, int* uid, int* gid);

/* pathname bind table management (sc_unlink / stat hooks) */
int          unix_unlink_path(const char* path);   /* 0 removed, -ENOENT */
int          unix_has_node(const char* path);

#endif /* NET_UNIX_SOCK_H */
