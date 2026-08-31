/* Phase 3.3 TCP acceptance probe: freestanding Linux-ABI program (no
 * libc) that actively opens a TCP connection, sends a minimal HTTP GET
 * and dumps the response. Usage: nettest [port]  (default 8000).
 * Server side (WSL host): python3 -m http.server 8000  (10.0.2.2). */
#include <stdint.h>

static long sys3(long n, long a, long b, long c) {
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return ret;
}

static long sys6(long n, long a, long b, long c, long d, long e) {
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8v __asm__("r8") = e;
    register long r9v __asm__("r9") = 0;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8v),
                       "r"(r9v)
                     : "rcx", "r11", "memory");
    return ret;
}

#define SYS_write       1
#define SYS_socket      41
#define SYS_connect     42
#define SYS_sendto      44
#define SYS_recvfrom    45
#define SYS_close       3
#define SYS_exit_group  231

struct sa_in {
    uint16_t family;
    uint16_t port;
    uint32_t addr;
    uint64_t zero;
};

static uint16_t htons16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static uint32_t htonl32(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v >> 8) & 0xFF00) | (v >> 24);
}

static const char ok_conn[] = "nettest: established\n";
static const char ok_sent[] = "nettest: request sent\n";
static const char err_sock[] = "nettest: socket failed\n";
static const char err_conn[] = "nettest: connect failed\n";
static const char err_send[] = "nettest: send failed\n";
static const char prefix[] = "nettest: recv ";

static void wprint(const char* s, long n) { sys3(SYS_write, 1, (long)s, n); }
static void wstr(const char* s) {
    long n = 0;
    while (s[n]) n++;
    wprint(s, n);
}

static void wdec(long v) {
    char b[16];
    int i = (int)sizeof(b);
    if (v == 0) { wprint("0", 1); return; }
    while (v > 0) { b[--i] = (char)('0' + (v % 10)); v /= 10; }
    wprint(b + i, (long)(sizeof(b) - (long)i));
}

void _start(void) {
    /* optional argv[1]: port */
    long port = 8000;
    __asm__ volatile("" : : : "memory");
    {
        long* sp;
        __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
        long argc = sp[0];
        if (argc > 1) {
            char* a = (char*)sp[2];
            long v = 0;
            for (long i = 0; a[i] >= '0' && a[i] <= '9'; i++)
                v = v * 10 + (a[i] - '0');
            if (v > 0 && v < 65536) port = v;
        }
    }

    long fd = sys3(SYS_socket, 2 /*AF_INET*/, 1 /*SOCK_STREAM*/, 0);
    if (fd < 0) { wstr(err_sock); sys3(SYS_exit_group, 3, 0, 0); }

    struct sa_in sa;
    sa.family = 2;
    sa.port = htons16((uint16_t)port);
    sa.addr = htonl32(0x0A000202u);   /* 10.0.2.2 (QEMU host) */
    sa.zero = 0;

    if (sys3(SYS_connect, fd, (long)&sa, 16) != 0) {
        wstr(err_conn);
        sys3(SYS_exit_group, 4, 0, 0);
    }
    wstr(ok_conn);

    static const char req[] = "GET /nettest.txt HTTP/1.0\r\n\r\n";
    long n = sizeof(req) - 1;
    if (sys6(SYS_sendto, fd, (long)req, n, 0, 0) != n) {
        wstr(err_send);
        sys3(SYS_exit_group, 5, 0, 0);
    }
    wstr(ok_sent);

    static char buf[4096];
    long total = 0;
    for (;;) {
        long r = sys6(SYS_recvfrom, fd, (long)buf, (long)sizeof(buf), 0, 0);
        if (r <= 0) break;
        sys3(SYS_write, 1, (long)buf, r);
        total += r;
        if (total > 256 * 1024) break;
    }
    sys3(SYS_close, fd, 0, 0);

    wprint(prefix, (long)sizeof(prefix) - 1);
    wdec(total);
    wprint(" bytes\n", 7);
    sys3(SYS_exit_group, 0, 0, 0);
    for (;;) ;
}
