/* AF_UNIX socket transport (P1.4). See net/unix_sock.h for the design.
 *
 * Concurrency model: all entry points run in syscall context under the
 * kernel's cli discipline (no SMP preemption inside syscall handlers),
 * so plain data structures are sufficient. Wake-ups go through
 * g_unix_wq; the syscall layer parks with WAIT_RESTART and re-executes.
 */
#include "net/unix_sock.h"
#include "core/file.h"
#include "core/process.h"
#include "sched/wait.h"
#include "lib/string.h"
#include "lib/stdlib.h"

#define L_EAGAIN     11
#define L_EINVAL     22
#define L_ENOMEM     12
#define L_EMSGSIZE   90
#define L_EPIPE      32
#define L_ECONNRESET 104
#define L_ECONNREFUSED 111
#define L_EADDRINUSE 98
#define L_EISCONN    106
#define L_ENOTCONN   107

#define UNIX_POLL_IN  0x001
#define UNIX_POLL_OUT 0x004
#define UNIX_POLL_HUP 0x008

/* framed-record header (dgram/seqpacket) */
typedef struct {
    uint32_t     len;
    uint32_t     pad;
    unix_sock_t* src;           /* scrubbed to NULL when sender closes */
} unix_rec_hdr_t;

typedef struct {
    uint64_t     off;           /* absolute stream position after msg  */
    struct file* files[UNIX_SCM_MAX];
    int          n;
} unix_scm_t;

struct unix_sock {
    int type;                   /* 1=stream 2=dgram 5=seqpacket */
    int state;
    struct unix_sock* peer;

    /* rx ring */
    uint8_t* rx;
    uint32_t rx_sz;
    uint32_t rx_head;           /* next byte to write */
    uint32_t rx_tail;           /* next byte to read */
    uint64_t abs_head;          /* absolute positions (SCM offsets)   */
    uint64_t abs_tail;
    int      msg_cnt;           /* framed mode: whole records in ring */

    /* listening: pending connections (pre-created server children) */
    unix_sock_t* pend[UNIX_BACKLOG];
    int pend_head, pend_cnt;
    int backlog;

    /* SCM_RIGHTS fifo */
    unix_scm_t scm[UNIX_SCM_QLEN];
    int scm_head, scm_cnt;

    /* bind name (abstract: raw bytes, may contain NULs) */
    char name[UNIX_PATH_MAX];
    int  name_len;              /* 0 = unbound */
    int  abstract;

    /* SO_PEERCRED, filled by unix_connect/socketpair */
    int cred_pid, cred_uid, cred_gid;
};

/* ---- bind table: pathname + abstract namespaces ---- */
#define UNIX_BIND_MAX 32
typedef struct {
    char path[UNIX_PATH_MAX];
    int  plen;
    int  abstract;
    unix_sock_t* s;
} unix_bind_node_t;

static unix_bind_node_t bind_tab[UNIX_BIND_MAX];

/* ---- ring helpers (stream + framed share one buffer) ---- */

static uint32_t ring_used(const unix_sock_t* s) {
    return (uint32_t)(s->abs_head - s->abs_tail);
}

static uint32_t ring_space(const unix_sock_t* s) {
    return s->rx_sz - ring_used(s);
}

static void ring_write(unix_sock_t* s, const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t h = s->rx_head;
    for (uint32_t i = 0; i < len; i++) {
        s->rx[h] = p[i];
        h = (h + 1) % s->rx_sz;
    }
    s->rx_head = h;
    s->abs_head += len;
}

static void ring_read(unix_sock_t* s, void* out, uint32_t len) {
    uint8_t* p = (uint8_t*)out;
    uint32_t t = s->rx_tail;
    for (uint32_t i = 0; i < len; i++) {
        p[i] = s->rx[t];
        t = (t + 1) % s->rx_sz;
    }
    s->rx_tail = t;
    s->abs_tail += len;
}

/* peek at an absolute offset (within the buffered window) */
static void ring_peek(const unix_sock_t* s, uint64_t abs_off, void* out, uint32_t len) {
    uint8_t* p = (uint8_t*)out;
    uint32_t t = (uint32_t)((s->rx_tail + (uint32_t)(abs_off - s->abs_tail)) % s->rx_sz);
    for (uint32_t i = 0; i < len; i++) {
        p[i] = s->rx[t];
        t = (t + 1) % s->rx_sz;
    }
}

/* ---- helpers ---- */

static void unix_wake(void) { wait_wake(&g_unix_wq, -1); }

static int sock_readable(const unix_sock_t* s) {
    if (s->type == 1) return ring_used(s) > 0 || s->peer == NULL;
    return s->msg_cnt > 0;
}

static int sock_writable(const unix_sock_t* s) {
    if (s->type == 1) return ring_space(s) > 0;
    return ring_space(s) > sizeof(unix_rec_hdr_t) + 256;
}

/* Zero out every framed-record src pointer in 's' that refers to the
 * dying socket, so a later recvfrom cannot dereference freed memory. */
static void scrub_ring_src(unix_sock_t* s, unix_sock_t* dying) {
    if (s == NULL || s->type == 1 || s->msg_cnt == 0) return;
    uint64_t pos = s->abs_tail;
    for (int m = 0; m < s->msg_cnt; m++) {
        unix_rec_hdr_t h;
        ring_peek(s, pos, &h, sizeof(h));
        if (h.src == dying) {
            uint32_t off = (uint32_t)((s->rx_tail +
                                       (uint32_t)(pos - s->abs_tail)) % s->rx_sz);
            uint32_t src_off = (off + 8) % s->rx_sz;    /* &hdr->src */
            for (uint32_t b = 0; b < sizeof(unix_sock_t*); b++)
                s->rx[(src_off + b) % s->rx_sz] = 0;
        }
        pos += sizeof(unix_rec_hdr_t) + h.len;
    }
}

/* ---- lifecycle ---- */

unix_sock_t* unix_socket_create(int type) {
    unix_sock_t* s = (unix_sock_t*)kmalloc(sizeof(unix_sock_t));
    if (s == NULL) return NULL;
    memset(s, 0, sizeof(*s));
    s->rx = (uint8_t*)kmalloc(UNIX_RXBUF_SZ);
    if (s->rx == NULL) { kfree(s); return NULL; }
    s->rx_sz = UNIX_RXBUF_SZ;
    s->type = type;
    s->state = US_IDLE;
    return s;
}

void unix_sock_close(unix_sock_t* s) {
    if (s == NULL || s->state == US_CLOSED) return;

    /* detach from the bind table */
    for (int i = 0; i < UNIX_BIND_MAX; i++) {
        if (bind_tab[i].s == s) { bind_tab[i].s = NULL; bind_tab[i].plen = 0; }
    }

    /* connected: orphan the peer (reader sees EOF after draining) */
    if (s->peer != NULL) {
        s->peer->peer = NULL;
        scrub_ring_src(s->peer, s);
    }

    /* listening: refuse every pending connection, wake accept waiters */
    for (int i = 0; i < s->pend_cnt; i++) {
        unix_sock_t* child = s->pend[(s->pend_head + i) % UNIX_BACKLOG];
        if (child == NULL) continue;
        if (child->peer != NULL) {
            child->peer->peer = NULL;           /* client gets ECONNRESET */
        }
        child->state = US_CLOSED;
        kfree(child->rx);
        kfree(child);
    }

    /* undelivered SCM files: drop references */
    for (int i = 0; i < s->scm_cnt; i++) {
        unix_scm_t* r = &s->scm[(s->scm_head + i) % UNIX_SCM_QLEN];
        for (int j = 0; j < r->n; j++) file_close(r->files[j]);
    }

    s->state = US_CLOSED;
    unix_wake();
    kfree(s->rx);
    kfree(s);
}

/* ---- bind table ---- */

static unix_bind_node_t* bind_find(const char* path, int plen, int abstract) {
    for (int i = 0; i < UNIX_BIND_MAX; i++) {
        unix_bind_node_t* n = &bind_tab[i];
        if (n->s != NULL && n->abstract == abstract && n->plen == plen &&
            memcmp(n->path, path, (size_t)plen) == 0)
            return n;
    }
    return NULL;
}

int unix_bind(unix_sock_t* s, const char* path, int pathlen) {
    if (s == NULL || path == NULL) return -L_EINVAL;
    if (s->name_len != 0) return -L_EINVAL;             /* already bound */
    if (pathlen == 0) return -L_EINVAL;
    int abstract = (pathlen < 0);
    int plen = abstract ? -pathlen : pathlen;
    if (plen > UNIX_PATH_MAX) return -L_EINVAL;
    if (bind_find(path, plen, abstract) != NULL) return -L_EADDRINUSE;

    unix_bind_node_t* slot = NULL;
    for (int i = 0; i < UNIX_BIND_MAX; i++) {
        if (bind_tab[i].s == NULL) { slot = &bind_tab[i]; break; }
    }
    if (slot == NULL) return -L_EADDRINUSE;
    memcpy(slot->path, path, (size_t)plen);
    slot->plen = plen;
    slot->abstract = abstract;
    slot->s = s;

    memcpy(s->name, path, (size_t)plen);
    s->name_len = plen;
    s->abstract = abstract;
    return 0;
}

int unix_unlink_path(const char* path) {
    int plen = (int)strlen(path);
    if (plen <= 0 || plen > UNIX_PATH_MAX) return -L_EINVAL;
    for (int i = 0; i < UNIX_BIND_MAX; i++) {
        unix_bind_node_t* n = &bind_tab[i];
        if (n->s != NULL && !n->abstract && n->plen == plen &&
            memcmp(n->path, path, (size_t)plen) == 0) {
            n->s = NULL;
            n->plen = 0;
            return 0;
        }
    }
    return -2;   /* not a unix socket node */
}

int unix_has_node(const char* path) {
    int plen = (int)strlen(path);
    if (plen <= 0 || plen > UNIX_PATH_MAX) return 0;
    for (int i = 0; i < UNIX_BIND_MAX; i++) {
        unix_bind_node_t* n = &bind_tab[i];
        if (n->s != NULL && !n->abstract && n->plen == plen &&
            memcmp(n->path, path, (size_t)plen) == 0)
            return 1;
    }
    return 0;
}

/* ---- listen / connect / accept ---- */

int unix_listen(unix_sock_t* s, int backlog) {
    if (s == NULL || (s->type != 1 && s->type != 5)) return -L_EINVAL;
    if (s->name_len == 0) return -L_EINVAL;             /* must bind first */
    if (s->state != US_IDLE) return -L_EISCONN;
    s->backlog = backlog > 0 ? backlog : 1;
    if (s->backlog > UNIX_BACKLOG) s->backlog = UNIX_BACKLOG;
    s->state = US_LISTENING;
    return 0;
}

/* SO_PEERCRED: cred of the connecting process. */
static void fill_cred(unix_sock_t* s) {
    process_t* p = process_get_current();
    s->cred_pid = p ? p->tgid : 0;
    s->cred_uid = 0;
    s->cred_gid = 0;
}

int unix_connect(unix_sock_t* s, const char* path, int pathlen) {
    if (s == NULL || path == NULL) return -L_EINVAL;
    if (s->state == US_CONNECTED) return -L_EISCONN;
    int abstract = (pathlen < 0);
    int plen = abstract ? -pathlen : pathlen;
    if (plen <= 0 || plen > UNIX_PATH_MAX) return -L_EINVAL;

    unix_bind_node_t* n = bind_find(path, plen, abstract);
    if (n == NULL || n->s == NULL || n->s->state != US_LISTENING)
        return -L_ECONNREFUSED;
    unix_sock_t* lsn = n->s;
    if (lsn->pend_cnt >= lsn->backlog) return -L_EAGAIN;    /* park & retry */

    /* Pre-create the server-side child, Linux-style: data the client
     * pushes before accept(2) is visible to the accepted connection. */
    unix_sock_t* child = unix_socket_create(lsn->type);
    if (child == NULL) return -L_ENOMEM;
    child->state = US_CONNECTED;
    child->peer = s;
    fill_cred(child);                       /* cred of the client */
    /* the client's getpeername(2) reports the listener's bound address */
    if (lsn->name_len != 0) {
        memcpy(child->name, lsn->name, sizeof(child->name));
        child->name_len = lsn->name_len;
        child->abstract = lsn->abstract;
    }

    s->peer = child;
    s->state = US_CONNECTED;                /* client socket stays unnamed */

    lsn->pend[(lsn->pend_head + lsn->pend_cnt) % UNIX_BACKLOG] = child;
    lsn->pend_cnt++;
    unix_wake();
    return 0;
}

unix_sock_t* unix_accept(unix_sock_t* s) {
    if (s == NULL || s->state != US_LISTENING) return NULL;
    if (s->pend_cnt == 0) return NULL;
    unix_sock_t* child = s->pend[s->pend_head];
    s->pend[s->pend_head] = NULL;
    s->pend_head = (s->pend_head + 1) % UNIX_BACKLOG;
    s->pend_cnt--;
    return child;
}

int unix_socketpair(unix_sock_t* a, unix_sock_t* b) {
    if (a == NULL || b == NULL) return -L_EINVAL;
    a->peer = b;
    b->peer = a;
    a->state = US_CONNECTED;
    b->state = US_CONNECTED;
    fill_cred(a);
    b->cred_pid = a->cred_pid;
    b->cred_uid = a->cred_uid;
    b->cred_gid = a->cred_gid;
    return 0;
}

/* ---- data path: stream ---- */

int unix_send(unix_sock_t* s, const void* buf, int len) {
    if (s == NULL || s->state != US_CONNECTED) return -L_ENOTCONN;
    if (s->peer == NULL) return -L_EPIPE;               /* peer closed */
    if (ring_space(s->peer) < (uint32_t)len) return -L_EAGAIN;
    ring_write(s->peer, buf, (uint32_t)len);
    unix_wake();
    return len;
}

int unix_recv(unix_sock_t* s, void* buf, int len) {
    if (s == NULL || s->type != 1) return -L_EINVAL;
    uint32_t used = ring_used(s);
    if (used == 0) {
        if (s->peer == NULL) return 0;                  /* EOF */
        return -L_EAGAIN;
    }
    uint32_t n = used < (uint32_t)len ? used : (uint32_t)len;
    ring_read(s, buf, n);

    /* deliver SCM records whose message boundary has been consumed */
    while (s->scm_cnt > 0) {
        unix_scm_t* r = &s->scm[s->scm_head];
        if (r->off > s->abs_tail) break;
        /* no recvmsg consumer here: drop the file references so they do
         * not leak (recvmsg delivers them instead) */
        for (int j = 0; j < r->n; j++) file_close(r->files[j]);
        r->n = 0;
        s->scm_head = (s->scm_head + 1) % UNIX_SCM_QLEN;
        s->scm_cnt--;
    }
    return (int)n;
}

/* ---- data path: framed (dgram/seqpacket) ---- */

int unix_sendto_dgram(unix_sock_t* s, const void* buf, int len,
                      const char* path, int pathlen) {
    if (s == NULL || s->type == 1) return -L_EINVAL;
    unix_sock_t* dst = NULL;
    if (s->peer != NULL) {
        dst = s->peer;                                  /* connected */
    } else {
        if (path == NULL || pathlen <= 0) return -L_ENOTCONN;
        int abstract = (pathlen < 0);
        int plen = abstract ? -pathlen : pathlen;
        unix_bind_node_t* n = bind_find(path, plen, abstract);
        if (n == NULL) return -L_ECONNREFUSED;
        dst = n->s;
    }
    if (dst->state == US_CLOSED) return -L_ECONNRESET;
    uint32_t need = (uint32_t)len + sizeof(unix_rec_hdr_t);
    if (need > dst->rx_sz) return -L_EMSGSIZE;
    if (ring_space(dst) < need) return -L_EAGAIN;

    unix_rec_hdr_t h;
    h.len = (uint32_t)len;
    h.pad = 0;
    h.src = s;
    ring_write(dst, &h, sizeof(h));
    ring_write(dst, buf, (uint32_t)len);
    dst->msg_cnt++;
    unix_wake();
    return len;
}

static int framed_pop(unix_sock_t* s, void* buf, int len, unix_sock_t** src_out) {
    if (s->msg_cnt == 0) return -L_EAGAIN;
    unix_rec_hdr_t h;
    ring_read(s, &h, sizeof(h));
    s->msg_cnt--;
    uint32_t n = h.len < (uint32_t)len ? h.len : (uint32_t)len;
    ring_read(s, buf, n);
    /* discard any remainder of an overlong message */
    uint32_t drop = h.len - n;
    while (drop-- > 0) { uint8_t sink; ring_read(s, &sink, 1); }
    if (src_out != NULL) *src_out = h.src;
    return (int)n;
}

/* ---- sendmsg / recvmsg with SCM_RIGHTS ---- */

static int scm_push(unix_sock_t* peer, struct file** fds, int nfds) {
    if (peer->scm_cnt >= UNIX_SCM_QLEN) return -L_EAGAIN;
    unix_scm_t* r = &peer->scm[(peer->scm_head + peer->scm_cnt) % UNIX_SCM_QLEN];
    r->off = peer->abs_head;            /* boundary after the data */
    for (int i = 0; i < nfds; i++) r->files[i] = fds[i];
    r->n = nfds;
    peer->scm_cnt++;
    return 0;
}

int unix_sendmsg(unix_sock_t* s, const struct unix_iovec* iv, int iovcnt,
                 struct file** fds, int nfds) {
    if (s == NULL || s->state != US_CONNECTED) return -L_ENOTCONN;
    if (s->peer == NULL) return -L_EPIPE;
    unix_sock_t* peer = s->peer;

    size_t total = 0;
    for (int i = 0; i < iovcnt; i++) total += iv[i].len;
    if (total > UNIX_RXBUF_SZ - 16) return -L_EMSGSIZE;

    if (peer->type == 1) {
        if (ring_space(peer) < total) return -L_EAGAIN;
        for (int i = 0; i < iovcnt; i++)
            if (iv[i].len != 0) ring_write(peer, iv[i].base, (uint32_t)iv[i].len);
        if (nfds > 0 && scm_push(peer, fds, nfds) != 0) {
            /* receiver scm queue full: data went out, fds dropped */
            for (int i = 0; i < nfds; i++) file_close(fds[i]);
        }
    } else {
        /* framed peer: one record per sendmsg */
        uint32_t need = (uint32_t)total + sizeof(unix_rec_hdr_t);
        if (need > peer->rx_sz) return -L_EMSGSIZE;
        if (ring_space(peer) < need) return -L_EAGAIN;
        unix_rec_hdr_t h;
        h.len = (uint32_t)total;
        h.pad = 0;
        h.src = s;
        ring_write(peer, &h, sizeof(h));
        for (int i = 0; i < iovcnt; i++)
            if (iv[i].len != 0) ring_write(peer, iv[i].base, (uint32_t)iv[i].len);
        peer->msg_cnt++;
        (void)fds; (void)nfds;          /* cmsg over framed: unsupported */
    }
    unix_wake();
    return (int)total;
}

int unix_recvmsg(unix_sock_t* s, const struct unix_iovec* iv, int iovcnt,
                 struct file** out_fds, int* out_nfds) {
    if (s == NULL || out_nfds == NULL) return -L_EINVAL;
    *out_nfds = 0;

    if (s->type == 1) {
        if (ring_used(s) == 0) {
            if (s->peer == NULL) return 0;              /* EOF */
            return -L_EAGAIN;
        }
        size_t total = 0;
        for (int i = 0; i < iovcnt; i++) {
            uint32_t want = ring_used(s);
            if (want > iv[i].len) want = (uint32_t)iv[i].len;
            if (want == 0) break;
            ring_read(s, iv[i].base, want);
            total += want;
        }
        /* SCM_RIGHTS: deliver every record whose boundary we consumed */
        while (s->scm_cnt > 0 && *out_nfds < UNIX_SCM_MAX) {
            unix_scm_t* r = &s->scm[s->scm_head];
            if (r->off > s->abs_tail) break;
            for (int j = 0; j < r->n && *out_nfds < UNIX_SCM_MAX; j++)
                out_fds[(*out_nfds)++] = r->files[j];
            r->n = 0;
            s->scm_head = (s->scm_head + 1) % UNIX_SCM_QLEN;
            s->scm_cnt--;
        }
        return (int)total;
    }

    /* framed: pop one message */
    if (s->msg_cnt == 0) return -L_EAGAIN;
    size_t total = 0;
    for (int i = 0; i < iovcnt; i++) total += iv[i].len;
    if (total == 0) return -L_EMSGSIZE;
    void* stage = kmalloc(total);
    if (stage == NULL) return -L_ENOMEM;
    unix_sock_t* src = NULL;
    int r = framed_pop(s, stage, (int)total, &src);
    if (r < 0) { kfree(stage); return r; }

    size_t left = (size_t)r, off = 0;
    const uint8_t* sb = (const uint8_t*)stage;
    for (int i = 0; i < iovcnt && left > 0; i++) {
        size_t n = iv[i].len < left ? iv[i].len : left;
        memcpy(iv[i].base, sb + off, n);
        off += n;
        left -= n;
    }
    kfree(stage);
    (void)src;
    return r;
}

/* ---- readiness / names / creds ---- */

int unix_poll_bits(unix_sock_t* s) {
    if (s == NULL) return 0;
    int bits = 0;
    if (s->state == US_LISTENING) {
        if (s->pend_cnt > 0) bits |= UNIX_POLL_IN;
        bits |= UNIX_POLL_OUT;
        return bits;
    }
    if (sock_readable(s)) bits |= UNIX_POLL_IN;
    if (sock_writable(s)) bits |= UNIX_POLL_OUT;
    if (s->peer == NULL && s->state == US_CONNECTED) bits |= UNIX_POLL_HUP;
    return bits;
}

int unix_sock_type(unix_sock_t* s)  { return s ? s->type : 0; }
int unix_sock_state(unix_sock_t* s) { return s ? s->state : US_CLOSED; }
int unix_sock_has_peer(unix_sock_t* s) { return s != NULL && s->peer != NULL; }
unix_sock_t* unix_sock_peer(unix_sock_t* s) { return s ? s->peer : NULL; }

int unix_getsockname(unix_sock_t* s, char* out) {
    if (s == NULL) return -L_EINVAL;
    if (s->name_len == 0) return 0;                     /* unnamed */
    memcpy(out, s->name, (size_t)(s->name_len < 0 ? -s->name_len : s->name_len));
    return s->name_len;
}

int unix_getpeername(unix_sock_t* s, char* out) {
    if (s == NULL || s->peer == NULL) return -L_ENOTCONN;
    return unix_getsockname(s->peer, out);
}

void unix_peer_cred(unix_sock_t* s, int* pid, int* uid, int* gid) {
    if (s == NULL) { *pid = *uid = *gid = 0; return; }
    *pid = s->cred_pid;
    *uid = s->cred_uid;
    *gid = s->cred_gid;
}
