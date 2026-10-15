/* P1.4 acceptance probe: AF_UNIX end to end, musl static.
 * Covers socketpair echo, SCM_RIGHTS fd passing, pathname
 * bind/listen/connect/accept with SO_PEERCRED, and an abstract-namespace
 * listener (D-Bus style). Prints "ux: OK" when every check passes. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <fcntl.h>

static int fails = 0;

#define CHECK(cond, name) do { \
    if (cond) printf("ux: %s OK\n", name); \
    else { printf("ux: %s FAIL (errno=%d)\n", name, errno); fails++; } \
} while (0)

static int make_tmpfile(void) {
    int fd = open("/tmp/uxtest.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    if (write(fd, "PAYLOAD", 7) != 7) { close(fd); return -1; }
    return fd;
}

int main(void) {
    /* --- A: socketpair stream echo --- */
    int sv[2];
    int r = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    CHECK(r == 0, "socketpair");
    if (r == 0) {
        if (write(sv[0], "ping", 4) != 4) { printf("ux: sp-write FAIL\n"); fails++; }
        char buf[16] = {0};
        int n = (int)read(sv[1], buf, sizeof buf);
        CHECK(n == 4 && memcmp(buf, "ping", 4) == 0, "socketpair-echo");
        close(sv[0]);
        close(sv[1]);
    }

    /* --- B: SCM_RIGHTS over socketpair --- */
    r = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    if (r == 0) {
        int tf = make_tmpfile();
        CHECK(tf >= 0, "open-tmpfile");
        struct msghdr msg;
        struct iovec iov;
        char cbuf[CMSG_SPACE(sizeof(int))];
        char data[8] = "fdpass";
        memset(&msg, 0, sizeof msg);
        iov.iov_base = data;
        iov.iov_len = 6;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof cbuf;
        struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &tf, sizeof(int));
        r = (int)sendmsg(sv[0], &msg, 0);
        CHECK(r == 6, "sendmsg-scm");
        if (tf >= 0) close(tf);

        char rbuf[16];
        struct iovec riov = { rbuf, sizeof rbuf };
        char rcbuf[CMSG_SPACE(sizeof(int))];
        struct msghdr rmsg;
        memset(&rmsg, 0, sizeof rmsg);
        rmsg.msg_iov = &riov;
        rmsg.msg_iovlen = 1;
        rmsg.msg_control = rcbuf;
        rmsg.msg_controllen = sizeof rcbuf;
        r = (int)recvmsg(sv[1], &rmsg, 0);
        CHECK(r == 6, "recvmsg-scm");
        int rfd = -1;
        struct cmsghdr* rc = CMSG_FIRSTHDR(&rmsg);
        if (rc && rc->cmsg_level == SOL_SOCKET && rc->cmsg_type == SCM_RIGHTS)
            memcpy(&rfd, CMSG_DATA(rc), sizeof(int));
        CHECK(rfd >= 0, "scm-got-fd");
        if (rfd >= 0) {
            char pbuf[16] = {0};
            lseek(rfd, 0, SEEK_SET);
            ssize_t got = read(rfd, pbuf, 7);
            CHECK(got == 7 && memcmp(pbuf, "PAYLOAD", 7) == 0, "scm-fd-content");
            close(rfd);
        }
        close(sv[0]);
        close(sv[1]);
    }

    /* --- C: pathname server + SO_PEERCRED (real fork) --- */
    pid_t pid = fork();
    if (pid == 0) {
        int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (lfd < 0) _exit(10);
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        strcpy(sa.sun_path, "/tmp/ux.srv");
        unlink("/tmp/ux.srv");
        if (bind(lfd, (struct sockaddr*)&sa, sizeof sa) < 0) _exit(11);
        if (listen(lfd, 4) < 0) _exit(12);
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) _exit(13);
        char b[16] = {0};
        int n = (int)read(cfd, b, sizeof b);
        if (n != 5 || memcmp(b, "hello", 5) != 0) _exit(14);
        struct ucred cr;
        socklen_t cl = sizeof cr;
        if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cr, &cl) < 0) _exit(15);
        if (cr.pid != getppid()) _exit(16);
        if (write(cfd, "world", 5) != 5) _exit(17);
        _exit(0);
    }
    usleep(200000);                     /* let the child reach listen */
    int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(cfd >= 0, "client-socket");
    if (cfd >= 0) {
        struct sockaddr_un sa2;
        memset(&sa2, 0, sizeof sa2);
        sa2.sun_family = AF_UNIX;
        strcpy(sa2.sun_path, "/tmp/ux.srv");
        r = connect(cfd, (struct sockaddr*)&sa2, sizeof sa2);
        CHECK(r == 0, "pathname-connect");
        if (r == 0) {
            if (write(cfd, "hello", 5) != 5) { printf("ux: cli-write FAIL\n"); fails++; }
            char wb[16] = {0};
            int n = (int)read(cfd, wb, sizeof wb);
            CHECK(n == 5 && memcmp(wb, "world", 5) == 0, "accept-roundtrip");
        }
        close(cfd);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "server-exit-0");

    /* --- D: abstract namespace (D-Bus style) --- */
    pid = fork();
    if (pid == 0) {
        int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (lfd < 0) _exit(30);
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        sa.sun_path[0] = '\0';
        strcpy(sa.sun_path + 1, "uxabs");       /* @uxabs, len = 2+6 */
        socklen_t alen = (socklen_t)(2 + 1 + strlen("uxabs"));
        if (bind(lfd, (struct sockaddr*)&sa, alen) < 0) _exit(31);
        if (listen(lfd, 4) < 0) _exit(32);
        int cfd2 = accept(lfd, NULL, NULL);
        if (cfd2 < 0) _exit(33);
        char b[16] = {0};
        int n = (int)read(cfd2, b, sizeof b);
        if (n != 2 || memcmp(b, "ab", 2) != 0) _exit(34);
        _exit(0);
    }
    usleep(200000);
    int cfd3 = socket(AF_UNIX, SOCK_STREAM, 0);
    if (cfd3 >= 0) {
        struct sockaddr_un sa3;
        memset(&sa3, 0, sizeof sa3);
        sa3.sun_family = AF_UNIX;
        sa3.sun_path[0] = '\0';
        strcpy(sa3.sun_path + 1, "uxabs");
        socklen_t alen = (socklen_t)(2 + 1 + strlen("uxabs"));
        r = connect(cfd3, (struct sockaddr*)&sa3, alen);
        CHECK(r == 0, "abstract-connect");
        if (r == 0) {
            if (write(cfd3, "ab", 2) != 2) { printf("ux: abs-write FAIL\n"); fails++; }
        }
        close(cfd3);
    }
    st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "abstract-exit-0");

    if (fails == 0) printf("ux: OK\n");
    else printf("ux: %d FAILURES\n", fails);
    return fails != 0;
}
