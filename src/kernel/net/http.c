/* Phase 4.4: minimal synchronous HTTP GET over the TCP stack. */

#include "net/http.h"
#include "net/tcp.h"
#include "net/dns.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "fs/fs.h"
#include "mm/memory.h"
#include "timer/pit.h"
#include "drivers/vga.h"

#define HTTP_TIMEOUT_MS 20000
#define HTTP_MAX_BODY   (8 * 1024 * 1024)

static uint32_t parse_ip_str(const char* s) {
    uint32_t ip = 0;
    int part = 0;
    int parts[4] = {0, 0, 0, 0};
    while (*s) {
        if (*s == '.') { part++; if (part > 3) return 0; }
        else if (*s >= '0' && *s <= '9') {
            parts[part] = parts[part] * 10 + (*s - '0');
            if (parts[part] > 255) return 0;
        } else return 0;
        s++;
    }
    if (part != 3) return 0;
    ip = ((uint32_t)parts[0] << 24) | ((uint32_t)parts[1] << 16) |
         ((uint32_t)parts[2] << 8) | (uint32_t)parts[3];
    return ip;
}

/* Resolve a hostname through /etc/hosts ("10.0.2.2 kil0yos"). */
static uint32_t resolve_hosts(const char* name) {
    fs_entry_t* f = fs_resolve_path("/etc/hosts");
    if (f == NULL || f->type != FS_TYPE_FILE) return 0;
    uint8_t* buf = (uint8_t*)kmalloc(f->size + 1);
    if (buf == NULL) return 0;
    int got = fs_read_file(f, buf, f->size);
    if (got <= 0) { kfree(buf); return 0; }
    buf[got] = '\0';

    uint32_t ip = 0;
    char* p = (char*)buf;
    while (p && *p) {
        char* nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        /* line: <ip> <name> [aliases...]  (skip comments) */
        if (*p != '#') {
            char* sp = strchr(p, ' ');
            if (sp) {
                *sp = '\0';
                char* rest = sp + 1;
                uint32_t cand = parse_ip_str(p);
                if (cand) {
                    char* save = rest;
                    /* first token = canonical name, rest = aliases */
                    for (char* tok = strtok(save, " \t"); tok;
                         tok = strtok(NULL, " \t")) {
                        if (strcmp(tok, name) == 0) { ip = cand; break; }
                    }
                }
            }
        }
        if (ip) break;
        p = nl ? nl + 1 : NULL;
    }
    kfree(buf);
    return ip;
}

int http_download(netif_t* iface, const char* host, uint16_t port,
                  const char* path, uint8_t** out, size_t* out_len) {
    (void)iface;
    *out = NULL;
    *out_len = 0;
    if (port == 0) port = 80;

    uint32_t ip = parse_ip_str(host);
    if (ip == 0) ip = resolve_hosts(host);
    if (ip == 0) ip = dns_resolve(&g_netif, host);
    if (ip == 0) {
        klog("[http] cannot resolve host: ");
        klog(host);
        klog("\n");
        return -1;
    }

    tcp_socket_t* sock = tcp_socket_create();
    if (sock == NULL) return -1;
    if (tcp_connect(sock, ip, port) != 0) {
        klog("[http] connect failed\n");
        tcp_socket_close(sock);
        return -1;
    }

    char req[512];
    ksprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\n"
             "Connection: close\r\nUser-Agent: kilget/1.0\r\n\r\n",
             path, host);
    /* tcp_send blocks until the (chunked) data is fully ACKed */
    if (tcp_send(sock, (const uint8_t*)req, (uint16_t)strlen(req)) < 0) {
        tcp_socket_close(sock);
        klog("[http] send failed\n");
        return -1;
    }

    /* receive the whole response (Connection: close) */
    heap_verify("http pre-rx");
    size_t cap = 64 * 1024;
    uint8_t* resp = (uint8_t*)kmalloc(cap);
    if (resp == NULL) {
        tcp_socket_close(sock);
        return -1;
    }
    size_t rlen = 0;
    uint64_t start_us = pit_uptime_us();
    uint64_t last_data_us = start_us;

    for (;;) {
        uint8_t chunk[2048];
        int n = tcp_recv(sock, chunk, sizeof(chunk), 300);
        if (n > 0) {
            if (rlen + (size_t)n > cap) {
                size_t ncap = cap * 2;
                if (ncap > HTTP_MAX_BODY) ncap = HTTP_MAX_BODY;
                if (rlen + (size_t)n > ncap) {
                    kfree(resp);
                    tcp_socket_close(sock);
                    klog("[http] response too large\n");
                    return -1;
                }
                uint8_t* nr = (uint8_t*)krealloc(resp, ncap);
                if (nr == NULL) {
                    kfree(resp);
                    tcp_socket_close(sock);
                    return -1;
                }
                resp = nr;
                cap = ncap;
            }
            memcpy(resp + rlen, chunk, (size_t)n);
            rlen += (size_t)n;
            last_data_us = pit_uptime_us();
        } else if (n == 0) {
            break;                    /* peer FIN: orderly EOF */
        } else {
            uint64_t now = pit_uptime_us();
            if (sock->state == TCP_CLOSED) break;
            if (now - last_data_us > 5000000ULL) break;      /* 5 s idle */
            if (now - start_us > HTTP_TIMEOUT_MS * 1000ULL) break;
            tcp_poll(sock);
        }
    }
    if (sock->state == TCP_ESTABLISHED) tcp_close(sock);
    tcp_socket_close(sock);
    heap_verify("http post-rx");

    /* split headers/body */
    const char* hdr_end = NULL;
    for (size_t i = 0; i + 3 < rlen; i++) {
        if (resp[i] == '\r' && resp[i+1] == '\n' &&
            resp[i+2] == '\r' && resp[i+3] == '\n') {
            hdr_end = (const char*)resp + i;
            break;
        }
    }
    if (hdr_end == NULL) {
        kfree(resp);
        klog("[http] bad response (no header end)\n");
        return -1;
    }
    size_t body_off = (size_t)(hdr_end - (const char*)resp) + 4;

    /* Content-Length completeness check: tcp_recv returning 0 also
     * happens on a RESET or slirp hiccup mid-transfer, and treating
     * that as orderly EOF silently truncated a 1.8 MB mirror index
     * (the gzip ISIZE then read as garbage from the cut stream). */
    {
        long long want_len = -1;
        const char* hp = (const char*)resp;
        const char* hend = (const char*)hdr_end;
        while (hp < hend) {
            const char* eol = strchr(hp, '\n');
            size_t linelen = (eol && eol <= hend) ? (size_t)(eol - hp)
                                                  : (size_t)(hend - hp);
            /* case-insensitive "content-length:" prefix match */
            if (linelen > 15) {
                const char* key = "content-length:";
                int match = 1;
                for (int i = 0; i < 15; i++) {
                    char a = hp[i];
                    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                    if (a != key[i]) { match = 0; break; }
                }
                if (match) {
                    const char* v = hp + 15;
                    while (v < hp + linelen && *v == ' ') v++;
                    long long w = 0;
                    while (v < hp + linelen && *v >= '0' && *v <= '9')
                        w = w * 10 + (*v++ - '0');
                    want_len = w;
                }
            }
            if (eol == NULL || eol > hend) break;
            hp = eol + 1;
        }
        size_t blen_now = rlen - body_off;
        if (want_len >= 0 && blen_now != (size_t)want_len) {
            klog("[http] body truncated: got ");
            char nb[16];
            itoa((int)blen_now, nb, 10, sizeof(nb));
            klog(nb);
            klog(" want ");
            itoa((int)want_len, nb, 10, sizeof(nb));
            klog(nb);
            klog("\n");
            kfree(resp);
            return -1;
        }
    }

    /* status code check ("HTTP/1.x NNN ...") */
    if (rlen < 13 || resp[8] != ' ') {
        kfree(resp);
        klog("[http] bad status line\n");
        return -1;
    }
    int code = atoi((const char*)resp + 9);
    if (code != 200) {
        klog("[http] HTTP status ");
        char nb[8];
        itoa(code, nb, 10, sizeof(nb));
        klog(nb);
        klog("\n");
        kfree(resp);
        return -1;
    }

    /* move body to a tight buffer */
    size_t blen = rlen - body_off;
    uint8_t* body = (uint8_t*)kmalloc(blen > 0 ? blen : 1);
    if (body == NULL) {
        kfree(resp);
        return -1;
    }
    memcpy(body, resp + body_off, blen);
    kfree(resp);
    *out = body;
    *out_len = blen;
    return 0;
}
