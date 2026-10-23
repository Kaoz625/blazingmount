// SPDX-License-Identifier: GPL-3.0-or-later
// blazingmountd - BlazingMount's self-contained on-console discovery/register daemon.
//
// REFERENCE IMPLEMENTATION. Build with ../build.sh (PS5 payload SDK). An on-console test
// is still pending; the host-assisted installer (install/blazingmount-install.py) is the
// proven path meanwhile. This file is deliberately small and POSIX-plain so other devs can
// build it, run it, and send fixes (see docs/REPORT-A-FIX.md).
//
// What it does, once at boot and then every POLL_SECS (resident, survives reboot via autoload):
//   1. read   /data/blazing/mount.conf          ->  one line: "<host-ip> <catalog-port>"
//   2. GET    http://<host>:<port>/catalog.txt   ->  lines "TID\tNAME\tBUNDLE_PATH\tCFG_PATH"
//   3. for each game NOT already at /data/homebrew/<NAME>/ :
//        GET  the bundle tar, extract it to /data/homebrew/<NAME>/ (ustar, regular files + dirs)
//        copy /data/blazing/fork/libSceAmpr.sprx -> the starter's fakelib/libSceAmpr.sprx
//        GET  the cfg -> /data/blazing/link/<TID>.cfg
//   ShadowMount Plus (already resident) then registers the tile; the fork streams on launch.
//
// It never fetches, cracks, signs, or decrypts anything: the host only serves files the user
// put there. Content-neutral. See the repo README.

#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define POLL_SECS      600
#define HOMEBREW_DIR   "/data/homebrew"
#define LINK_DIR       "/data/blazing/link"
#define FORK_SRC       "/data/blazing/fork/libSceAmpr.sprx"
#define CONF_PATH      "/data/blazing/mount.conf"
#define LOG_PATH       "/data/blazing/blazingmountd.log"

static void logln(const char *s) {
    int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { write(fd, s, strlen(s)); write(fd, "\n", 1); close(fd); }
}

// ---- tiny blocking HTTP/1.0 GET into a malloc'd buffer (body only) ----
// Returns body length, *out = malloc'd body (caller frees), or -1 on error.
static long http_get(const char *host, int port, const char *path, char **out) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { close(s); return -1; }
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return -1; }
    char req[1024];
    int rl = snprintf(req, sizeof req,
        "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    if (write(s, req, rl) != rl) { close(s); return -1; }

    size_t cap = 1 << 16, len = 0; char *buf = malloc(cap);
    for (;;) {
        if (len + 4096 > cap) { cap *= 2; buf = realloc(buf, cap); }
        ssize_t n = read(s, buf + len, cap - len);
        if (n <= 0) break;
        len += n;
    }
    close(s);
    // split headers / body, require a 2xx status (manual scan: the SDK libc has no memmem)
    char *hdr_end = 0;
    for (size_t i = 0; i + 4 <= len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') { hdr_end = buf + i; break; }
    if (!hdr_end || strncmp(buf, "HTTP/1.", 7) || (buf[9] != '2')) { free(buf); return -1; }
    size_t body_off = (hdr_end - buf) + 4, body_len = len - body_off;
    memmove(buf, buf + body_off, body_len);
    *out = buf;
    return (long)body_len;
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void makedirs(const char *path) {
    char tmp[1024]; strncpy(tmp, path, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

static void write_file(const char *path, const char *data, long n) {
    char dir[1700]; strncpy(dir, path, sizeof dir - 1); dir[sizeof dir - 1] = 0;
    char *slash = strrchr(dir, '/'); if (slash) { *slash = 0; makedirs(dir); }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { write(fd, data, n); close(fd); }
}

// ---- minimal tar extraction into dest_root ----
// Names longer than 100 chars arrive three ways: the ustar "prefix" field (offset 345), a pax 'x' header ("path=..."), or a GNU 'L'
// entry. All three are honoured (the old reader took only the first 100 chars, so long paths were cut and collided).
static int untar(const char *dest_root, const char *tar, long tlen) {
    long off = 0;
    char longname[1024]; longname[0] = 0;                       // pending name from a pax 'x' / GNU 'L' entry
    while (off + 512 <= tlen) {
        const char *h = tar + off; off += 512;
        if (h[0] == 0) break;                                   // end of archive
        char szoct[13]; memcpy(szoct, h + 124, 12); szoct[12] = 0;
        long fsz = strtol(szoct, NULL, 8);
        char type = h[156];
        if (type == 'x' || type == 'L') {                       // long-name carrier: body follows
            if (off + fsz > tlen) return -1;
            const char *b = tar + off;
            if (type == 'L') {
                long n = fsz < 1023 ? fsz : 1023; memcpy(longname, b, n); longname[n] = 0;
            } else {                                            // pax: "<len> path=<name>\n" records
                long p = 0;
                while (p < fsz) {
                    long rl = strtol(b + p, NULL, 10); if (rl <= 0 || p + rl > fsz) break;
                    const char *eq = memchr(b + p, '=', rl);
                    if (eq && eq - (b + p) >= 5 && memcmp(eq - 4, "path", 4) == 0 && eq[-5] == ' ') {
                        long n = (b + p + rl - 1) - (eq + 1); if (n > 1023) n = 1023;
                        memcpy(longname, eq + 1, n); longname[n] = 0;
                    }
                    p += rl;
                }
            }
            off += (fsz + 511) & ~511L;
            continue;
        }
        char name[1024];
        if (longname[0]) { strncpy(name, longname, sizeof name - 1); name[sizeof name - 1] = 0; longname[0] = 0; }
        else {
            char nm[101], pre[156]; memcpy(nm, h, 100); nm[100] = 0;
            memcpy(pre, h + 345, 155); pre[155] = 0;
            if (memcmp(h + 257, "ustar", 5) == 0 && pre[0]) snprintf(name, sizeof name, "%s/%s", pre, nm);
            else snprintf(name, sizeof name, "%s", nm);
        }
        char full[1700]; snprintf(full, sizeof full, "%s/%s", dest_root, name);
        if (type == '5') {                                      // directory
            makedirs(full);
        } else if (type == '0' || type == 0) {                  // regular file
            if (off + fsz > tlen) return -1;
            write_file(full, tar + off, fsz);
        }
        off += (fsz + 511) & ~511L;                             // next 512-block
    }
    return 0;
}

static void copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY); if (in < 0) return;
    char dir[1024]; strncpy(dir, dst, sizeof dir - 1); dir[sizeof dir - 1] = 0;
    char *slash = strrchr(dir, '/'); if (slash) { *slash = 0; makedirs(dir); }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return; }
    char b[1 << 16]; ssize_t n;
    while ((n = read(in, b, sizeof b)) > 0) write(out, b, n);
    close(in); close(out);
}

// install one catalog line: TID \t NAME \t BUNDLE \t CFG
static void install_game(const char *host, int port, char *line) {
    char *tid = strtok(line, "\t"), *name = strtok(NULL, "\t");
    char *bundle = strtok(NULL, "\t"), *cfg = strtok(NULL, "\t\r\n");
    if (!tid || !name || !bundle || !cfg) return;

    char dest[1024]; snprintf(dest, sizeof dest, "%s/%s", HOMEBREW_DIR, name);
    if (exists(dest)) return;                                    // already installed

    char msg[1200];
    snprintf(msg, sizeof msg, "installing %s (%s)", name, tid); logln(msg);

    char path[1024], *body;
    snprintf(path, sizeof path, "/%s", bundle);
    long n = http_get(host, port, path, &body);
    if (n < 0) { logln("  bundle fetch failed"); return; }

    const char *stage = "/data/blz-stage/bm";     // the bundle tar carries the top-level <name>/ dir
    makedirs(stage);
    if (untar(stage, body, n) != 0) { free(body); logln("  untar failed"); return; }
    free(body);

    // drop the fork so the game loads our libSceAmpr
    char fk[1200]; snprintf(fk, sizeof fk, "%s/%s/fakelib/libSceAmpr.sprx", stage, name);
    copy_file(FORK_SRC, fk);

    // atomic-ish move into the scanned homebrew dir
    char inner[1200]; snprintf(inner, sizeof inner, "%s/%s", stage, name);
    rename(inner, dest);

    // link config
    snprintf(path, sizeof path, "/%s", cfg);
    n = http_get(host, port, path, &body);
    if (n >= 0) {
        char cfgdst[1024]; snprintf(cfgdst, sizeof cfgdst, "%s/%s.cfg", LINK_DIR, tid);
        makedirs(LINK_DIR);
        write_file(cfgdst, body, n);
        free(body);
    }
    snprintf(msg, sizeof msg, "  done: %s -> tile (ShadowMount) + link %s/%s.cfg", name, LINK_DIR, tid);
    logln(msg);
}

static void sync_once(void) {
    char conf[128] = {0};
    int fd = open(CONF_PATH, O_RDONLY); if (fd < 0) { logln("no mount.conf"); return; }
    read(fd, conf, sizeof conf - 1); close(fd);
    char host[64]; int port = 0;
    if (sscanf(conf, "%63s %d", host, &port) != 2) { logln("bad mount.conf"); return; }

    char *cat; long n = http_get(host, port, "/catalog.txt", &cat);
    if (n < 0) { logln("catalog fetch failed"); return; }
    char *save, *line = strtok_r(cat, "\n", &save);
    while (line) {
        char copy[1200]; strncpy(copy, line, sizeof copy - 1); copy[sizeof copy - 1] = 0;
        if (copy[0] && copy[0] != '#') install_game(host, port, copy);
        line = strtok_r(NULL, "\n", &save);
    }
    free(cat);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "once") == 0) {   // one-shot: sync from the catalog, then exit (manual runs / tests)
        logln("blazingmountd once");
        sync_once();
        return 0;
    }
    logln("blazingmountd start");                      // resident (autoload): sync now and every POLL_SECS
    for (;;) {
        sync_once();
        sleep(POLL_SECS);
    }
    return 0;
}
