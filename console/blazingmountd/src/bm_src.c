// SPDX-License-Identifier: GPL-3.0-or-later
// bm_src.c - v2.12: NETWORK SOURCES. Markus (28 Oct 2026, PlayBridge-style): "add a network source (host, path, optional login) and
// browse/install PKGs from it, or stream a game image from it" - FTP, SMB and WebDAV. The PAYLOAD reads the bytes itself (no helper
// on the host, no copy on the console first): bm_install.c's proxy asks this file for a byte range of the source, Sony's installer
// reads the proxy at 127.0.0.1:8095 (so live %, MB/s, ETA, cancel, split .pkg.N all work as for the imgserve path).
//
// A source = name, kind (ftp | smb | webdav), host (IPv4), port, base path, optional user + password.
//   ftp     base = /dir                (PASV, REST, RETR; MLSD else LIST to browse)
//   smb     base = share[/dir]         (libsmb2, SMB2/3, NTLMSSP; "guest" when there is no login) - built in when -DBM_HAVE_SMB2
//   webdav  base = /dav                (HTTP Range GET + PROPFIND; Basic login; http only - the payload has no TLS)
// Logins live ONLY in /data/blazing/sources.conf (mode 0600, one tab-separated line per source): never in git, never in a log line, never
// in an API answer (the API says has_pass: true/false). The query string of a request is never logged.
//
//   GET  /api/src                          the sources (no passwords)
//   POST /api/src/add?name=&kind=&host=&port=&path=&user=&pass=      add or replace one        (header X-BM-Request: 1)
//   POST /api/src/del?name=                                                                    (header X-BM-Request: 1)
//   GET  /api/src/test?name=               connects, lists the base: {ok, ms, entries | error}
//   GET  /api/src/ls?name=&path=[&all=1]   one folder: dirs + .pkg (split .pkg.N sets collapse to one entry) + game images
//   POST /api/install?src=<name>&path=<rel to the base>[&name=]   install a pkg from it (bm_install.c)
//   GET  /src/<name>/<rel>                 the file's bytes, Range supported (a stream gateway: a game image from FTP/SMB/WebDAV)
//
// #include'd ONCE into bm_api.c after bm_pull.c, before bm_install.c (uses connect_ms, write_all, url_path, ends_with, logf_, sb_*, P()).

#include <pthread.h>
#ifdef BM_HAVE_SMB2
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#endif
#ifdef __PROSPERO__
#include <netdb.h>
#endif

#define SRC_PATH       "/data/blazing/sources.conf"
#define SRC_MAX        16
#define SRC_MAXPARTS   128
#define SRC_MAXENT     1500
#define SRC_IO_S       12                    // one network read/write may take this long (the API thread must never hang on a dead host)

typedef struct { char name[32], kind[8], host[64]; int port; char base[400]; char user[64], pass[96]; } src_t;
typedef struct { char name[256]; int dir; long long size; int parts; } sent_t;
// the file an install / gateway read is: one file, or the parts <path>.<first> .. <path>.<first+n-1> read as one
typedef struct { char path[512]; int n, first, multi; long long size[SRC_MAXPARTS]; long long total; } sparts_t;

static pthread_mutex_t g_src_mu = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------- the sources file
static int src_name_ok(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 31) return 0;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '.' && *s != '-' && *s != '_') return 0;
    return 1;
}
static int src_text_ok(const char *s) {                          // no control characters (the file is tab + newline separated)
    for (; *s; s++) if ((unsigned char)*s < 0x20 || *s == 0x7f) return 0;
    return 1;
}
static int src_load(src_t *out, int max) {                       // how many sources
    char p[600]; char *m = api_slurp(P(p, sizeof p, SRC_PATH), 1 << 16, NULL);
    int n = 0;
    if (!m) return 0;
    for (char *line = strtok(m, "\n"); line && n < max; line = strtok(NULL, "\n")) {
        char *f[7] = { 0 }; int k = 0;
        for (char *q = line; k < 7; ) { f[k++] = q; char *t = strchr(q, '\t'); if (!t) break; *t = 0; q = t + 1; }
        if (k < 5 || !src_name_ok(f[0])) continue;
        src_t *s = &out[n]; memset(s, 0, sizeof *s);
        snprintf(s->name, sizeof s->name, "%s", f[0]); snprintf(s->kind, sizeof s->kind, "%s", f[1]); snprintf(s->host, sizeof s->host, "%s", f[2]);
        s->port = atoi(f[3]); snprintf(s->base, sizeof s->base, "%s", f[4]);
        if (k > 5) snprintf(s->user, sizeof s->user, "%s", f[5]);
        if (k > 6) snprintf(s->pass, sizeof s->pass, "%s", f[6]);
        n++;
    }
    free(m);
    return n;
}
static int src_find(const char *name, src_t *out) {
    src_t *all = calloc(SRC_MAX, sizeof *all); if (!all) return 0;
    pthread_mutex_lock(&g_src_mu);
    int n = src_load(all, SRC_MAX), found = 0;
    pthread_mutex_unlock(&g_src_mu);
    for (int i = 0; i < n; i++) if (!strcmp(all[i].name, name)) { *out = all[i]; found = 1; break; }
    free(all);
    return found;
}
static int src_save(const src_t *all, int n) {                   // 0600, written aside and renamed in
    char p[600], t[640]; P(p, sizeof p, SRC_PATH); snprintf(t, sizeof t, "%s.tmp", p);
    parent_dirs(p);
    int fd = open(t, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    fchmod(fd, 0600);
    for (int i = 0; i < n; i++) {
        char line[1300]; int l = snprintf(line, sizeof line, "%s\t%s\t%s\t%d\t%s\t%s\t%s\n", all[i].name, all[i].kind, all[i].host, all[i].port,
                                          all[i].base, all[i].user, all[i].pass);
        if (l <= 0 || write_all(fd, line, (size_t)l)) { close(fd); unlink(t); return -1; }
    }
    close(fd);
    return rename(t, p);
}
static const char *src_add(const src_t *s) {                     // NULL = saved, else why not
    if (!src_name_ok(s->name)) return "name: letters, digits . - _ only (max 31)";
    if (strcmp(s->kind, "ftp") && strcmp(s->kind, "smb") && strcmp(s->kind, "webdav")) return "kind must be ftp, smb or webdav";
    struct in_addr ia; if (inet_pton(AF_INET, s->host, &ia) != 1) return "host must be an IPv4 address";
    if (s->port < 1 || s->port > 65535) return "bad port";
    if (!src_text_ok(s->base) || !src_text_ok(s->user) || !src_text_ok(s->pass)) return "no control characters in the path or the login";
    if (!strcmp(s->kind, "smb") && !s->base[0]) return "smb: the path starts with the share name (share/folder)";
#ifndef BM_HAVE_SMB2
    if (!strcmp(s->kind, "smb")) return "this payload was built without SMB support";
#endif
    src_t *all = calloc(SRC_MAX, sizeof *all); if (!all) return "out of memory";
    pthread_mutex_lock(&g_src_mu);
    int n = src_load(all, SRC_MAX), at = -1;
    for (int i = 0; i < n; i++) if (!strcmp(all[i].name, s->name)) at = i;
    const char *err = NULL;
    if (at < 0 && n >= SRC_MAX) err = "too many sources";
    else { if (at < 0) at = n++; all[at] = *s; if (src_save(all, n)) err = "could not write /data/blazing/sources.conf"; }
    pthread_mutex_unlock(&g_src_mu);
    free(all);
    return err;
}
static int src_del(const char *name) {
    src_t *all = calloc(SRC_MAX, sizeof *all); if (!all) return 0;
    pthread_mutex_lock(&g_src_mu);
    int n = src_load(all, SRC_MAX), k = 0, hit = 0;
    for (int i = 0; i < n; i++) { if (!strcmp(all[i].name, name)) { hit = 1; continue; } all[k++] = all[i]; }
    if (hit) src_save(all, k);
    pthread_mutex_unlock(&g_src_mu);
    free(all);
    return hit;
}
static void src_json_one(sb_t *b, const src_t *s) {              // never the password
    sb_puts(b, "{\"name\":"); sb_js(b, s->name); sb_puts(b, ",\"kind\":"); sb_js(b, s->kind); sb_puts(b, ",\"host\":"); sb_js(b, s->host);
    sb_f(b, ",\"port\":%d,\"path\":", s->port); sb_js(b, s->base); sb_puts(b, ",\"user\":"); sb_js(b, s->user);
    sb_f(b, ",\"has_pass\":%s}", s->pass[0] ? "true" : "false");
}

// ---------------------------------------------------------------- a session = one live connection to a source
enum { SK_FTP = 1, SK_SMB = 2, SK_DAV = 3 };
typedef struct {
    src_t s; int kind;
    char why[260];
    int cs;                                  // ftp: control socket
    int ds;                                  // ftp data socket / webdav response socket
    long long left;                          // bytes still wanted from the open stream
    char pre[4096]; size_t pre_n, pre_o;     // webdav: body bytes that came with the headers
    long long dav_total;                     // webdav: the file size from the last Content-Range
#ifdef BM_HAVE_SMB2
    struct smb2_context *smb; struct smb2fh *fh; long long fh_off;
#endif
    int smb_up;
    sparts_t parts; int has_parts;           // read mode
    long long pos, end; int cur; int stream_open;   // read mode: the next byte, one past the last wanted, the part index of the open stream
} sess_t;

static void b64(const char *in, char *out, size_t on) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(in), o = 0;
    for (size_t i = 0; i < n && o + 5 < on; i += 3) {
        unsigned v = (unsigned)(unsigned char)in[i] << 16 | (i + 1 < n ? (unsigned)(unsigned char)in[i + 1] << 8 : 0) | (i + 2 < n ? (unsigned char)in[i + 2] : 0u);
        out[o++] = T[v >> 18 & 63]; out[o++] = T[v >> 12 & 63]; out[o++] = i + 1 < n ? T[v >> 6 & 63] : '='; out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = 0;
}
static void src_join(char *out, size_t n, const char *base, const char *rel) {   // "/" + base + "/" + rel, single slashes, no leading ./
    char t[900]; snprintf(t, sizeof t, "/%s/%s", base, rel);
    size_t o = 0; for (const char *p = t; *p && o + 1 < n; p++) { if (*p == '/' && o && out[o - 1] == '/') continue; out[o++] = *p; }
    while (o > 1 && out[o - 1] == '/') o--;
    out[o] = 0;
}
static int rel_ok(const char *s) {                                // a path under the base: no .., no controls
    if (strstr(s, "..")) return 0;
    for (const unsigned char *q = (const unsigned char *)s; *q; q++) if (*q < 0x20 || *q == 0x7f) return 0;
    return 1;
}
static void ss_why(sess_t *ss, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(ss->why, sizeof ss->why, fmt, ap); va_end(ap);
}
static int sock_to(int s, int secs) {
    struct timeval tv = { secs, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return s;
}

// ------------------------------------------------ FTP
static int ftp_reply(sess_t *ss, char *text, size_t tn) {          // the final reply's code; text = its last line
    char line[600]; int code = -1; if (tn) text[0] = 0;
    for (int guard = 0; guard < 200; guard++) {
        size_t n = 0; char c;
        for (;;) {
            ssize_t r = read(ss->cs, &c, 1);
            if (r <= 0) return -1;
            if (c == '\n') break;
            if (c != '\r' && n + 1 < sizeof line) line[n++] = c;
        }
        line[n] = 0;
        if (n >= 4 && isdigit((unsigned char)line[0]) && isdigit((unsigned char)line[1]) && isdigit((unsigned char)line[2])) {
            if (line[3] == ' ') { code = atoi(line); if (tn) snprintf(text, tn, "%s", line + 4); return code; }
        }
    }
    return code;
}
static int ftp_cmd(sess_t *ss, char *text, size_t tn, const char *fmt, ...) {
    char c[900]; va_list ap; va_start(ap, fmt); int l = vsnprintf(c, sizeof c - 2, fmt, ap); va_end(ap);
    if (l <= 0 || strpbrk(c, "\r\n")) return -1;
    c[l++] = '\r'; c[l++] = '\n';
    if (write_all(ss->cs, c, (size_t)l)) return -1;
    return ftp_reply(ss, text, tn);
}
static void ftp_drop(sess_t *ss) { if (ss->ds >= 0) close(ss->ds); ss->ds = -1; if (ss->cs >= 0) close(ss->cs); ss->cs = -1; ss->stream_open = 0; }
static int ftp_up(sess_t *ss) {                                    // control connection logged in, binary mode
    if (ss->cs >= 0) return 0;
    int s = connect_ms(ss->s.host, ss->s.port, 5000);
    if (s < 0) { ss_why(ss, "cannot connect to %s:%d", ss->s.host, ss->s.port); return -1; }
    ss->cs = sock_to(s, SRC_IO_S);
    char t[300]; int code = ftp_reply(ss, t, sizeof t);
    if (code != 220) { ss_why(ss, "no FTP greeting from %s:%d", ss->s.host, ss->s.port); ftp_drop(ss); return -1; }
    const char *u = ss->s.user[0] ? ss->s.user : "anonymous";
    code = ftp_cmd(ss, t, sizeof t, "USER %s", u);
    if (code == 331 || code == 332) code = ftp_cmd(ss, t, sizeof t, "PASS %s", ss->s.user[0] ? ss->s.pass : "blazingmount@");
    if (code != 230 && code != 202) { ss_why(ss, "FTP login refused (%d)", code); ftp_drop(ss); return -1; }
    if (ftp_cmd(ss, t, sizeof t, "TYPE I") != 200) { ss_why(ss, "FTP binary mode refused"); ftp_drop(ss); return -1; }
    return 0;
}
static int ftp_pasv(sess_t *ss) {                                  // a connected data socket, or -1
    char t[300]; int code = ftp_cmd(ss, t, sizeof t, "PASV");
    if (code != 227) { ss_why(ss, "FTP PASV refused (%d)", code); return -1; }
    int a, b, c, d, e, f; const char *p = strchr(t, '(');
    if (!p || sscanf(p + 1, "%d,%d,%d,%d,%d,%d", &a, &b, &c, &d, &e, &f) != 6) { ss_why(ss, "FTP PASV answer not understood"); return -1; }
    int s = connect_ms(ss->s.host, e * 256 + f, 5000);               // the control host, not the PASV address (NAT, 0.0.0.0)
    if (s < 0) { ss_why(ss, "FTP data connection refused"); return -1; }
    return sock_to(s, SRC_IO_S);
}
static void ftp_end_data(sess_t *ss) {                             // after our side closed the data socket: the server's closing reply
    if (ss->ds >= 0) { close(ss->ds); ss->ds = -1; }
    if (ss->cs < 0 || !ss->stream_open) return;
    ss->stream_open = 0;
    struct timeval tv = { 3, 0 }; setsockopt(ss->cs, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int code = ftp_reply(ss, NULL, 0);
    tv.tv_sec = SRC_IO_S; setsockopt(ss->cs, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (code < 0) ftp_drop(ss);                                      // it kept sending / never answered: start over next time
}
static int ftp_stream_open(sess_t *ss, const char *path, long long off, long long len) {
    if (ftp_up(ss)) return -1;
    int d = ftp_pasv(ss); if (d < 0) { ftp_drop(ss); return -1; }
    char t[300]; int code;
    if (off > 0 && (code = ftp_cmd(ss, t, sizeof t, "REST %lld", off)) != 350) { close(d); ss_why(ss, "FTP REST refused (%d): the server cannot resume", code); return -1; }
    code = ftp_cmd(ss, t, sizeof t, "RETR %s", path);
    if (code != 150 && code != 125) { close(d); ss_why(ss, "FTP RETR %s refused (%d)", path, code); return -1; }
    ss->ds = d; ss->left = len; ss->stream_open = 1;
    return 0;
}
static int ftp_size(sess_t *ss, const char *path, long long *size) {
    if (ftp_up(ss)) return -1;
    char t[300]; int code = ftp_cmd(ss, t, sizeof t, "SIZE %s", path);
    if (code != 213) { ss_why(ss, "FTP SIZE %s: %d %s", path, code, t); return -1; }
    *size = atoll(t); return 0;
}
static int read_all_fd(int fd, char **out, size_t *n, size_t max) {  // to EOF
    size_t cap = 8192, len = 0; char *m = malloc(cap);
    if (!m) return -1;
    for (;;) {
        if (len + 4096 + 1 > cap) { if (cap >= max) break; cap *= 2; char *nm = realloc(m, cap); if (!nm) { free(m); return -1; } m = nm; }
        ssize_t r = read(fd, m + len, cap - len - 1);
        if (r <= 0) break;
        len += (size_t)r;
    }
    m[len] = 0; *out = m; *n = len;
    return 0;
}
static void ent_add(sent_t *e, int *n, const char *name, int dir, long long size) {
    if (*n >= SRC_MAXENT || !name[0] || !strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/')) return;
    sent_t *x = &e[(*n)++]; memset(x, 0, sizeof *x); snprintf(x->name, sizeof x->name, "%s", name); x->dir = dir; x->size = size; x->parts = 1;
}
static int ftp_list(sess_t *ss, const char *dir, sent_t *e, int *n) {
    if (ftp_up(ss)) return -1;
    for (int mlsd = 1; mlsd >= 0; mlsd--) {
        int d = ftp_pasv(ss); if (d < 0) return -1;
        char t[300]; int code = mlsd ? ftp_cmd(ss, t, sizeof t, "MLSD %s", dir) : ftp_cmd(ss, t, sizeof t, "LIST %s", dir);
        if (code != 150 && code != 125) { close(d); if (mlsd) continue; ss_why(ss, "FTP cannot list %s (%d)", dir, code); return -1; }
        char *m = NULL; size_t ml = 0; read_all_fd(d, &m, &ml, 4 << 20); close(d);
        ftp_reply(ss, t, sizeof t);
        if (!m) return -1;
        *n = 0;
        for (char *line = strtok(m, "\n"); line; line = strtok(NULL, "\n")) {
            size_t L = strlen(line); while (L && (line[L - 1] == '\r' || line[L - 1] == ' ')) line[--L] = 0;
            if (mlsd) {
                char *sp = strstr(line, "; "); if (!sp) continue;
                int isdir = 0; long long sz = 0; char *f = line;
                while (f < sp + 1) {
                    char *sc = strchr(f, ';'); if (!sc) break;
                    *sc = 0;
                    if (!strncasecmp(f, "type=", 5)) isdir = !strcasecmp(f + 5, "dir") || !strcasecmp(f + 5, "cdir") || !strcasecmp(f + 5, "pdir");
                    else if (!strncasecmp(f, "size=", 5)) sz = atoll(f + 5);
                    f = sc + 1;
                }
                if (!strncasecmp(line, "type=cdir", 9) || !strncasecmp(line, "type=pdir", 9)) continue;
                ent_add(e, n, sp + 2, isdir, sz);
            } else {                                                 // unix ls -l: perms links owner group size month day time|year name
                char *p = line; if (!(*p == 'd' || *p == '-' || *p == 'l')) continue;
                int isdir = *p == 'd'; long long sz = 0; int fld = 0;
                for (; *p && fld < 8; fld++) {
                    while (*p == ' ') p++;
                    if (fld == 4) sz = atoll(p);
                    while (*p && *p != ' ') p++;
                }
                while (*p == ' ') p++;
                char *arrow = strstr(p, " -> "); if (arrow) *arrow = 0;
                ent_add(e, n, p, isdir, sz);
            }
        }
        free(m);
        return 0;
    }
    return -1;
}

// ------------------------------------------------ WebDAV (plain HTTP)
static int dav_auth(sess_t *ss, char *out, size_t on) {
    out[0] = 0; if (!ss->s.user[0]) return 0;
    char up[200], e[300]; snprintf(up, sizeof up, "%s:%s", ss->s.user, ss->s.pass); b64(up, e, sizeof e);
    snprintf(out, on, "Authorization: Basic %s\r\n", e);
    return 0;
}
// send a request, read the status line + headers; *st = HTTP status, the rest of the headers' buffer in h (NUL-terminated), body bytes after them in pre
static int dav_request(sess_t *ss, const char *method, const char *path, const char *extra, const char *body, int *st, char *h, size_t hn, int *sock) {
    int s = connect_ms(ss->s.host, ss->s.port, 5000);
    if (s < 0) { ss_why(ss, "cannot connect to %s:%d", ss->s.host, ss->s.port); return -1; }
    sock_to(s, SRC_IO_S);
    char enc[1200], auth[400], req[3000]; url_path(enc, sizeof enc, path); dav_auth(ss, auth, sizeof auth);
    int rl = snprintf(req, sizeof req, "%s %s HTTP/1.0\r\nHost: %s:%d\r\n%s%sContent-Length: %zu\r\nConnection: close\r\n\r\n%s", method, enc, ss->s.host,
                      ss->s.port, auth, extra ? extra : "", body ? strlen(body) : (size_t)0, body ? body : "");
    if (rl <= 0 || (size_t)rl >= sizeof req || write_all(s, req, (size_t)rl)) { close(s); ss_why(ss, "request not sent"); return -1; }
    size_t have = 0; char *e = NULL;
    while (!e) {
        if (have >= hn - 1) { close(s); ss_why(ss, "headers too long"); return -1; }
        ssize_t n = read(s, h + have, hn - 1 - have);
        if (n <= 0) { close(s); ss_why(ss, "no answer from %s:%d", ss->s.host, ss->s.port); return -1; }
        have += (size_t)n; h[have] = 0; e = strstr(h, "\r\n\r\n");
    }
    *st = (have > 12 && !strncmp(h, "HTTP/1.", 7)) ? atoi(h + 9) : 0;
    size_t hl = (size_t)(e + 4 - h);
    ss->pre_n = have - hl; if (ss->pre_n > sizeof ss->pre) ss->pre_n = sizeof ss->pre;
    memcpy(ss->pre, h + hl, ss->pre_n); ss->pre_o = 0; h[hl] = 0;
    *sock = s;
    return 0;
}
static int dav_stream_open(sess_t *ss, const char *path, long long off, long long len) {
    char h[4096], rg[120]; int st = 0, s = -1;
    snprintf(rg, sizeof rg, "Range: bytes=%lld-%lld\r\n", off, off + len - 1);
    if (dav_request(ss, "GET", path, rg, NULL, &st, h, sizeof h, &s)) return -1;
    char *cr = strcasestr(h, "\nContent-Range: bytes ");
    long long ra = -1, rb = -1, rt = -1;
    if (cr) sscanf(cr + 22, "%lld-%lld/%lld", &ra, &rb, &rt);
    if (st == 206 && cr) ss->dav_total = rt;
    else if (st == 200 && off == 0) { char *cl = strcasestr(h, "\nContent-Length:"); ss->dav_total = cl ? atoll(cl + 16) : -1; }
    else {
        close(s);
        if (st == 401) ss_why(ss, "WebDAV login refused (401)");
        else if (st == 404) ss_why(ss, "WebDAV: not found: %s", path);
        else ss_why(ss, "WebDAV answered HTTP %d for bytes %lld-%lld of %s", st, off, off + len - 1, path);
        return -1;
    }
    ss->ds = s; ss->left = len; ss->stream_open = 1;
    return 0;
}
static void xml_unent(char *s) {
    char *o = s;
    for (char *p = s; *p; ) {
        if (*p == '&') {
            if (!strncmp(p, "&amp;", 5)) { *o++ = '&'; p += 5; continue; }
            if (!strncmp(p, "&lt;", 4)) { *o++ = '<'; p += 4; continue; }
            if (!strncmp(p, "&gt;", 4)) { *o++ = '>'; p += 4; continue; }
            if (!strncmp(p, "&quot;", 6)) { *o++ = '"'; p += 6; continue; }
            if (!strncmp(p, "&apos;", 6)) { *o++ = '\''; p += 6; continue; }
        }
        *o++ = *p++;
    }
    *o = 0;
}
// the text of the first element whose LOCAL name (after any "ns:") is `local`, inside [blk, end); 0 = none
static int xml_text(const char *blk, const char *end, const char *local, char *out, size_t on) {
    for (const char *p = blk; p < end; ) {
        const char *lt = memchr(p, '<', (size_t)(end - p)); if (!lt) return 0;
        const char *q = lt + 1; if (*q == '/' || *q == '?' || *q == '!') { p = q; continue; }
        const char *nm = q; while (q < end && *q != '>' && *q != ' ' && *q != '/' && *q != '\r' && *q != '\n') q++;
        const char *colon = NULL; for (const char *c = nm; c < q; c++) if (*c == ':') colon = c;
        const char *ln = colon ? colon + 1 : nm;
        if ((size_t)(q - ln) == strlen(local) && !strncasecmp(ln, local, strlen(local))) {
            const char *gt = memchr(q, '>', (size_t)(end - q)); if (!gt) return 0;
            if (gt[-1] == '/') { out[0] = 0; return 1; }              // <D:collection/>
            const char *tx = gt + 1, *cl = memchr(tx, '<', (size_t)(end - tx));
            size_t L = cl ? (size_t)(cl - tx) : 0; if (L >= on) L = on - 1;
            memcpy(out, tx, L); out[L] = 0;
            return 1;
        }
        p = q;
    }
    return 0;
}
static char *dechunk(char *b, size_t *n) {                         // in place; b is the whole body
    if (!(*n)) return b;
    char *o = b, *p = b, *end = b + *n;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p)); if (!nl) break;
        long sz = strtol(p, NULL, 16); p = nl + 1;
        if (sz <= 0) break;
        if (p + sz > end) sz = (long)(end - p);
        memmove(o, p, (size_t)sz); o += sz; p += sz;
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;
    }
    *o = 0; *n = (size_t)(o - b);
    return b;
}
static int dav_list(sess_t *ss, const char *dir, sent_t *e, int *n) {
    char h[8192]; int st = 0, s = -1;
    char path[1000]; snprintf(path, sizeof path, "%s%s", dir, strcmp(dir, "/") && dir[strlen(dir) - 1] != '/' ? "/" : "");
    const char *body = "<?xml version=\"1.0\"?><propfind xmlns=\"DAV:\"><prop><resourcetype/><getcontentlength/></prop></propfind>";
    if (dav_request(ss, "PROPFIND", path, "Depth: 1\r\nContent-Type: application/xml\r\n", body, &st, h, sizeof h, &s)) return -1;
    if (st != 207 && st != 200) { close(s); if (st == 401) ss_why(ss, "WebDAV login refused (401)"); else ss_why(ss, "WebDAV PROPFIND answered HTTP %d", st); return -1; }
    char *rest = NULL; size_t rn = 0; read_all_fd(s, &rest, &rn, 8 << 20); close(s);
    if (!rest) return -1;
    char *m = malloc(ss->pre_n + rn + 1); if (!m) { free(rest); return -1; }
    memcpy(m, ss->pre, ss->pre_n); memcpy(m + ss->pre_n, rest, rn); size_t ml = ss->pre_n + rn; m[ml] = 0; free(rest);
    if (strcasestr(h, "Transfer-Encoding: chunked")) dechunk(m, &ml);
    *n = 0;
    char self[1000]; snprintf(self, sizeof self, "%s", path);
    const char *p = m, *end = m + ml;
    for (;;) {
        const char *rs = NULL;
        for (const char *q = p; q < end; q++) if (*q == '<') {                      // next <..response>
            const char *r = q + 1; if (*r == '/') continue;
            const char *gt = memchr(r, '>', (size_t)(end - r)); if (!gt) break;
            const char *c = memchr(r, ':', (size_t)(gt - r)); const char *ln = c ? c + 1 : r;
            if ((size_t)(gt - ln) >= 8 && !strncasecmp(ln, "response", 8) && (gt - ln == 8 || ln[8] == ' ')) { rs = q; break; }
        }
        if (!rs) break;
        const char *re = rs + 1; const char *nx = NULL;
        for (const char *q = re; q + 1 < end; q++) if (q[0] == '<' && q[1] == '/') {
            const char *gt = memchr(q, '>', (size_t)(end - q)); if (!gt) break;
            const char *c = memchr(q, ':', (size_t)(gt - q)); const char *ln = c ? c + 1 : q + 2;
            if ((size_t)(gt - ln) == 8 && !strncasecmp(ln, "response", 8)) { nx = gt + 1; break; }
        }
        if (!nx) break;
        char href[1000] = "", cl[40] = "", tmp[8]; int col = 0;
        xml_text(rs, nx, "href", href, sizeof href); xml_unent(href);
        xml_text(rs, nx, "getcontentlength", cl, sizeof cl);
        col = xml_text(rs, nx, "collection", tmp, sizeof tmp);
        char dec[1000]; size_t o = 0;                                                // percent-decode, drop a scheme+host
        const char *hp = href; if (!strncmp(hp, "http://", 7)) { hp = strchr(hp + 7, '/'); if (!hp) hp = ""; }
        for (const char *c = hp; *c && o + 1 < sizeof dec; ) {
            if (*c == '%' && c[1] && c[2]) { char x[3] = { c[1], c[2], 0 }; dec[o++] = (char)strtol(x, NULL, 16); c += 3; } else dec[o++] = *c++;
        }
        dec[o] = 0;
        while (o > 1 && dec[o - 1] == '/') dec[--o] = 0;
        char sdec[1000]; snprintf(sdec, sizeof sdec, "%s", self); size_t sl = strlen(sdec); while (sl > 1 && sdec[sl - 1] == '/') sdec[--sl] = 0;
        if (strcmp(dec, sdec)) { const char *slash = strrchr(dec, '/'); ent_add(e, n, slash ? slash + 1 : dec, col, atoll(cl)); }
        p = nx;
    }
    free(m);
    return 0;
}

// ------------------------------------------------ SMB (libsmb2)
#if defined(__PROSPERO__) && defined(BM_HAVE_SMB2)
// the PS5 libc's resolver answers nothing for a payload-linked library: libsmb2 is built asking these (tools/build-libsmb2.sh); IPv4 literals only
int bm_smb_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    (void)hints;
    struct in_addr ia; if (!node || inet_pton(AF_INET, node, &ia) != 1) return EAI_NONAME;
    struct addrinfo *ai = calloc(1, sizeof *ai + sizeof(struct sockaddr_in)); if (!ai) return EAI_MEMORY;
    struct sockaddr_in *sa = (struct sockaddr_in *)(ai + 1);
    sa->sin_family = AF_INET; sa->sin_addr = ia; sa->sin_port = htons((unsigned short)(service ? atoi(service) : 0));
    ai->ai_family = AF_INET; ai->ai_socktype = SOCK_STREAM; ai->ai_protocol = IPPROTO_TCP; ai->ai_addrlen = sizeof *sa; ai->ai_addr = (struct sockaddr *)sa;
    *res = ai; return 0;
}
void bm_smb_freeaddrinfo(struct addrinfo *ai) { free(ai); }
#endif
#ifdef BM_HAVE_SMB2
static int smb_split(const sess_t *ss, const char *rel, char *share, size_t sn, char *path, size_t pn) {   // base = share[/dir]; path inside the share
    char all[900]; snprintf(all, sizeof all, "%s/%s", ss->s.base, rel);
    const char *p = all; while (*p == '/') p++;
    const char *sl = strchr(p, '/'); size_t L = sl ? (size_t)(sl - p) : strlen(p);
    if (!L || L >= sn) return -1;
    memcpy(share, p, L); share[L] = 0;
    char *o = path; size_t k = 0; for (const char *q = sl ? sl : ""; *q && k + 1 < pn; q++) { if (*q == '/' && (k == 0 || o[k - 1] == '/')) continue; o[k++] = *q; }
    while (k && o[k - 1] == '/') k--;
    o[k] = 0;
    return 0;
}
static void smb_drop(sess_t *ss) {
    if (ss->smb) { if (ss->fh) smb2_close(ss->smb, ss->fh); ss->fh = NULL; if (ss->smb_up) smb2_disconnect_share(ss->smb); smb2_destroy_context(ss->smb); }
    ss->smb = NULL; ss->smb_up = 0; ss->stream_open = 0;
}
static int smb_up(sess_t *ss) {
    if (ss->smb_up) return 0;
    char share[200], p[900]; if (smb_split(ss, "", share, sizeof share, p, sizeof p)) { ss_why(ss, "smb: the path starts with the share name"); return -1; }
    ss->smb = smb2_init_context();
    if (!ss->smb) { ss_why(ss, "smb: out of memory"); return -1; }
    smb2_set_timeout(ss->smb, SRC_IO_S);
    smb2_set_security_mode(ss->smb, SMB2_NEGOTIATE_SIGNING_ENABLED);
    smb2_set_user(ss->smb, ss->s.user[0] ? ss->s.user : "guest");
    smb2_set_password(ss->smb, ss->s.user[0] ? ss->s.pass : "");
    char srv[100]; snprintf(srv, sizeof srv, "%s:%d", ss->s.host, ss->s.port);               // libsmb2 takes "host:port"
    if (smb2_connect_share(ss->smb, srv, share, ss->s.user[0] ? ss->s.user : "guest") < 0) {
        ss_why(ss, "SMB %s:%d share %s: %s", ss->s.host, ss->s.port, share, smb2_get_error(ss->smb)); smb_drop(ss); return -1;
    }
    ss->smb_up = 1;
    return 0;
}
static int smb_size(sess_t *ss, const char *rel, long long *size) {
    if (smb_up(ss)) return -1;
    char share[200], p[900]; smb_split(ss, rel, share, sizeof share, p, sizeof p);
    struct smb2_stat_64 st; if (smb2_stat(ss->smb, p, &st) < 0) { ss_why(ss, "SMB stat %s: %s", p, smb2_get_error(ss->smb)); return -1; }
    *size = (long long)st.smb2_size; return 0;
}
static int smb_stream_open(sess_t *ss, const char *rel, long long off, long long len) {
    if (smb_up(ss)) return -1;
    char share[200], p[900]; smb_split(ss, rel, share, sizeof share, p, sizeof p);
    if (ss->fh) { smb2_close(ss->smb, ss->fh); ss->fh = NULL; }
    ss->fh = smb2_open(ss->smb, p, O_RDONLY);
    if (!ss->fh) { ss_why(ss, "SMB open %s: %s", p, smb2_get_error(ss->smb)); return -1; }
    ss->fh_off = off; ss->left = len; ss->stream_open = 1;
    return 0;
}
static int smb_list(sess_t *ss, const char *rel, sent_t *e, int *n) {
    if (smb_up(ss)) return -1;
    char share[200], p[900]; smb_split(ss, rel, share, sizeof share, p, sizeof p);
    struct smb2dir *d = smb2_opendir(ss->smb, p);
    if (!d) { ss_why(ss, "SMB list %s: %s", p[0] ? p : "/", smb2_get_error(ss->smb)); return -1; }
    *n = 0;
    struct smb2dirent *de;
    while ((de = smb2_readdir(ss->smb, d))) ent_add(e, n, de->name, de->st.smb2_type == SMB2_TYPE_DIRECTORY, (long long)de->st.smb2_size);
    smb2_closedir(ss->smb, d);
    return 0;
}
#endif

// ------------------------------------------------ one interface over the three
static sess_t *ss_new(const src_t *s) {
    sess_t *ss = calloc(1, sizeof *ss); if (!ss) return NULL;
    ss->s = *s; ss->cs = ss->ds = -1;
    ss->kind = !strcmp(s->kind, "ftp") ? SK_FTP : !strcmp(s->kind, "smb") ? SK_SMB : SK_DAV;
    return ss;
}
static void ss_stream_close(sess_t *ss) {
    if (!ss->stream_open && ss->ds < 0) return;
    if (ss->kind == SK_FTP) ftp_end_data(ss);
    else { if (ss->ds >= 0) close(ss->ds); ss->ds = -1; ss->stream_open = 0; }
}
static void ss_free(sess_t *ss) {
    if (!ss) return;
    if (ss->kind == SK_FTP) { if (ss->stream_open && ss->ds >= 0) { close(ss->ds); ss->ds = -1; } if (ss->cs >= 0) { char t[100]; ftp_cmd(ss, t, sizeof t, "QUIT"); } ftp_drop(ss); }
    else if (ss->ds >= 0) close(ss->ds);
#ifdef BM_HAVE_SMB2
    smb_drop(ss);
#endif
    free(ss);
}
static char *ss_abs(const sess_t *ss, const char *rel, char *out, size_t on) {     // the path the protocol wants for base + rel
    if (ss->kind == SK_SMB) snprintf(out, on, "%s", rel);                           // smb_split joins base itself
    else src_join(out, on, ss->s.base, rel);
    return out;
}
static int ss_size(sess_t *ss, const char *rel, long long *size) {                  // one file's size
    char p[1000]; ss_abs(ss, rel, p, sizeof p);
    if (ss->kind == SK_FTP) return ftp_size(ss, p, size);
#ifdef BM_HAVE_SMB2
    if (ss->kind == SK_SMB) return smb_size(ss, rel, size);
#else
    if (ss->kind == SK_SMB) { ss_why(ss, "this payload was built without SMB support"); return -1; }
#endif
    if (dav_stream_open(ss, p, 0, 1)) return -1;                                    // GET bytes 0-0: Content-Range names the size
    *size = ss->dav_total; ss_stream_close(ss);
    return *size >= 0 ? 0 : -1;
}
static int ss_stream_open(sess_t *ss, const char *rel, long long off, long long len) {
    char p[1000]; ss_abs(ss, rel, p, sizeof p);
    if (ss->kind == SK_FTP) return ftp_stream_open(ss, p, off, len);
#ifdef BM_HAVE_SMB2
    if (ss->kind == SK_SMB) return smb_stream_open(ss, rel, off, len);
#endif
    if (ss->kind == SK_SMB) { ss_why(ss, "this payload was built without SMB support"); return -1; }
    return dav_stream_open(ss, p, off, len);
}
static ssize_t ss_stream_read(sess_t *ss, void *buf, size_t n) {                    // bytes, 0 at the end of the stream, -1 error
    if (ss->left <= 0) return 0;
    if ((long long)n > ss->left) n = (size_t)ss->left;
    ssize_t r = -1;
    if (ss->kind == SK_FTP) r = read(ss->ds, buf, n);
    else if (ss->kind == SK_DAV) {
        if (ss->pre_o < ss->pre_n) { size_t k = ss->pre_n - ss->pre_o; if (k > n) k = n; memcpy(buf, ss->pre + ss->pre_o, k); ss->pre_o += k; r = (ssize_t)k; }
        else r = read(ss->ds, buf, n);
    }
#ifdef BM_HAVE_SMB2
    else {
        uint32_t mx = smb2_get_max_read_size(ss->smb); if (mx > (1u << 20)) mx = 1u << 20; if (n > mx) n = mx;
        int k = smb2_pread(ss->smb, ss->fh, buf, (uint32_t)n, (uint64_t)ss->fh_off);
        if (k < 0) { ss_why(ss, "SMB read: %s", smb2_get_error(ss->smb)); return -1; }
        ss->fh_off += k; r = k;
    }
#endif
    if (r > 0) ss->left -= r;
    if (r < 0) ss_why(ss, "the source stopped sending");
    return r;
}
static int ss_list(sess_t *ss, const char *rel, sent_t *e, int *n) {
    char p[1000]; ss_abs(ss, rel, p, sizeof p);
    if (ss->kind == SK_FTP) return ftp_list(ss, p, e, n);
#ifdef BM_HAVE_SMB2
    if (ss->kind == SK_SMB) return smb_list(ss, rel, e, n);
#endif
    if (ss->kind == SK_SMB) { ss_why(ss, "this payload was built without SMB support"); return -1; }
    return dav_list(ss, p, e, n);
}

// ------------------------------------------------ a file that may be split: X.pkg.0 X.pkg.1 ... read as one
static int split_num(const char *name, char *stem, size_t sn) {      // "X.pkg.12" -> 12 and stem "X.pkg"; -1 when it is not a part
    const char *dot = strrchr(name, '.'); if (!dot || !dot[1]) return -1;
    for (const char *q = dot + 1; *q; q++) if (!isdigit((unsigned char)*q)) return -1;
    size_t L = (size_t)(dot - name); if (L < 5 || strncasecmp(name + L - 4, ".pkg", 4) || L >= sn) return -1;
    memcpy(stem, name, L); stem[L] = 0;
    return atoi(dot + 1);
}
static int cmp_ent(const void *a, const void *b) {
    const sent_t *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}
// folds a folder's .pkg.N parts into one entry named X.pkg (size = the sum, parts = how many)
static void ents_fold(sent_t *e, int *n) {
    for (int i = 0; i < *n; i++) {
        char stem[256]; if (e[i].dir || split_num(e[i].name, stem, sizeof stem) < 0) continue;
        int idx = -1; for (int j = 0; j < i; j++) if (!e[j].dir && !strcmp(e[j].name, stem)) { idx = j; break; }
        if (idx < 0) { snprintf(e[i].name, sizeof e[i].name, "%s", stem); e[i].parts = 1; continue; }
        e[idx].size += e[i].size; e[idx].parts++;
        e[i] = e[*n - 1]; (*n)--; i--;
    }
}
static int sp_resolve(sess_t *ss, const char *rel, sparts_t *sp) {   // fills sp for the file `rel` (X.pkg, X.pkg.N or any file)
    memset(sp, 0, sizeof *sp);
    char dir[512] = "", name[256]; const char *sl = strrchr(rel, '/');
    if (sl) { snprintf(dir, sizeof dir, "%.*s", (int)(sl - rel), rel); snprintf(name, sizeof name, "%s", sl + 1); } else snprintf(name, sizeof name, "%s", rel);
    char stem[256]; int pn = split_num(name, stem, sizeof stem);
    if (pn < 0) snprintf(stem, sizeof stem, "%s", name);
    snprintf(sp->path, sizeof sp->path, "%s%s%s", dir, dir[0] ? "/" : "", stem);          // the base name without the part number
    long long sz;
    if (pn < 0 && ss_size(ss, sp->path, &sz) == 0) { sp->n = 1; sp->size[0] = sz; sp->total = sz; return 0; }   // one plain file
    if (pn < 0 && ss->why[0] && strstr(ss->why, "login")) return -1;
    sent_t *e = calloc(SRC_MAXENT, sizeof *e); int n = 0; if (!e) { ss_why(ss, "out of memory"); return -1; }
    if (ss_list(ss, dir, e, &n)) { free(e); return -1; }
    long long by[SRC_MAXPARTS + 1]; int have[SRC_MAXPARTS + 1]; memset(have, 0, sizeof have); int lo = 1 << 30, hi = -1;
    for (int i = 0; i < n; i++) {
        char st2[256]; int k = split_num(e[i].name, st2, sizeof st2);
        if (k < 0 || k > SRC_MAXPARTS || strcmp(st2, stem) || e[i].dir) continue;
        have[k] = 1; by[k] = e[i].size; if (k < lo) lo = k; if (k > hi) hi = k;
    }
    free(e);
    if (hi < 0) { ss_why(ss, "%s: no such file (and no %s.N parts)", name, stem); return -1; }
    for (int k = lo; k <= hi; k++) if (!have[k]) { ss_why(ss, "%s: part %d is missing", stem, k); return -1; }
    sp->multi = 1; sp->first = lo; sp->n = hi - lo + 1;
    for (int k = lo; k <= hi; k++) { sp->size[k - lo] = by[k]; sp->total += by[k]; }
    return 0;
}
static void sp_part_path(const sparts_t *sp, int i, char *out, size_t on) {
    if (sp->multi) snprintf(out, on, "%s.%d", sp->path, sp->first + i); else snprintf(out, on, "%s", sp->path);
}
// position the session to deliver bytes a..b of the whole file (sp), then ss_read until it returns 0
static int ss_range(sess_t *ss, const sparts_t *sp, long long a, long long b) {
    if (a < 0 || b < a || b >= sp->total) { ss_why(ss, "range %lld-%lld outside the file (%lld bytes)", a, b, sp->total); return -1; }
    ss_stream_close(ss);
    ss->parts = *sp; ss->has_parts = 1; ss->pos = a; ss->end = b + 1; ss->cur = -1;
    return 0;
}
static ssize_t ss_read(sess_t *ss, void *buf, size_t n) {
    for (;;) {
        if (ss->pos >= ss->end) { ss_stream_close(ss); return 0; }
        if (ss->cur < 0 || ss->left <= 0) {                           // open (or move to) the part holding ss->pos
            ss_stream_close(ss);
            long long off = 0; int i = 0;
            while (i < ss->parts.n - 1 && ss->pos >= off + ss->parts.size[i]) { off += ss->parts.size[i]; i++; }
            long long inpart = ss->pos - off, want = ss->parts.size[i] - inpart;
            if (want > ss->end - ss->pos) want = ss->end - ss->pos;
            char pp[600]; sp_part_path(&ss->parts, i, pp, sizeof pp);
            if (ss_stream_open(ss, pp, inpart, want)) return -1;
            ss->cur = i;
        }
        ssize_t r = ss_stream_read(ss, buf, n);
        if (r < 0) return -1;
        if (r == 0) { ss_why(ss, "the source ended early at byte %lld", ss->pos); return -1; }
        ss->pos += r;
        if (ss->left <= 0) { ss_stream_close(ss); ss->cur = -1; }
        return r;
    }
}

// ------------------------------------------------ API: list / test / add / delete / gateway (each runs in its own thread: a dead host must not hang :8094)
typedef struct { int s; char method[8], target[700], query[1500]; int hdr_ok; char range[80]; } srcjob_t;

static int is_image_name(const char *n) {
    return ends_with(n, ".ffpfsc", 1) || ends_with(n, ".exfat", 1) || ends_with(n, ".iso", 1) || ends_with(n, ".ffpkg", 1);
}
static void src_ls_json(sb_t *b, const src_t *s, const char *rel, int all) {
    sess_t *ss = ss_new(s); sent_t *e = calloc(SRC_MAXENT, sizeof *e); int n = 0;
    if (!ss || !e) { sb_puts(b, "{\"ok\":false,\"error\":\"out of memory\"}"); free(e); ss_free(ss); return; }
    time_t t0 = time(NULL);
    char dir[512]; snprintf(dir, sizeof dir, "%s", rel); size_t L = strlen(dir); while (L && dir[L - 1] == '/') dir[--L] = 0;
    if (ss_list(ss, dir, e, &n)) {
        sb_puts(b, "{\"ok\":false,\"error\":"); sb_js(b, ss->why[0] ? ss->why : "could not list"); sb_puts(b, "}");
        free(e); ss_free(ss); return;
    }
    ents_fold(e, &n);
    qsort(e, (size_t)n, sizeof *e, cmp_ent);
    sb_puts(b, "{\"ok\":true,\"src\":"); sb_js(b, s->name); sb_puts(b, ",\"path\":"); sb_js(b, dir);
    sb_f(b, ",\"ms\":%lld,\"entries\":[", (long long)(time(NULL) - t0) * 1000);
    int first = 1;
    for (int i = 0; i < n; i++) {
        int pkg = !e[i].dir && ends_with(e[i].name, ".pkg", 1), img = !e[i].dir && is_image_name(e[i].name);
        if (!all && !e[i].dir && !pkg && !img) continue;
        if (!first) sb_puts(b, ","); first = 0;
        sb_puts(b, "{\"name\":"); sb_js(b, e[i].name);
        sb_f(b, ",\"dir\":%s,\"size\":%lld,\"parts\":%d,\"pkg\":%s,\"image\":%s}", e[i].dir ? "true" : "false", e[i].size, e[i].parts, pkg ? "true" : "false", img ? "true" : "false");
    }
    sb_puts(b, "]}");
    free(e); ss_free(ss);
}
static void *src_job_thread(void *arg) {
    srcjob_t *j = arg; int s = j->s; sb_t b = { 0 };
    char name[64] = "", path[512] = "", v[8]; src_t sc;
    if (!strncmp(j->target, "/src/", 5)) {                            // the gateway: /src/<name>/<rel>
        char *sl = strchr(j->target + 5, '/'); char dec[400] = "";
        if (sl) { *sl = 0; snprintf(name, sizeof name, "%s", j->target + 5); snprintf(dec, sizeof dec, "%s", sl + 1); }
        char rel[400]; size_t o = 0;
        for (const char *c = dec; *c && o + 1 < sizeof rel; ) { if (*c == '%' && c[1] && c[2]) { char x[3] = { c[1], c[2], 0 }; rel[o++] = (char)strtol(x, NULL, 16); c += 3; } else rel[o++] = *c++; }
        rel[o] = 0;
        if (!sl || !src_find(name, &sc) || !rel_ok(rel) || !rel[0]) { sb_puts(&b, "{\"error\":\"no such source or file\"}"); reply_json(s, 404, &b); goto done; }
        sess_t *ss = ss_new(&sc); sparts_t *sp = calloc(1, sizeof *sp);
        if (!ss || !sp || sp_resolve(ss, rel, sp)) {
            sb_puts(&b, "{\"error\":"); sb_js(&b, ss ? ss->why : "out of memory"); sb_puts(&b, "}"); reply_json(s, 502, &b); free(sp); ss_free(ss); goto done;
        }
        long long a = 0, e2 = sp->total - 1; int has_range = 0;
        const char *rg = j->range[0] ? j->range : NULL;
        if (rg && !strncasecmp(rg, "bytes=", 6)) { char *d; has_range = 1; a = strtoll(rg + 6, &d, 10); if (*d == '-' && d[1]) e2 = strtoll(d + 1, NULL, 10); if (e2 >= sp->total) e2 = sp->total - 1; }
        if (a < 0 || a > e2 || sp->total <= 0) {
            char h[200]; int hl = snprintf(h, sizeof h, "HTTP/1.0 416 Range Not Satisfiable\r\nContent-Range: bytes */%lld\r\nContent-Length: 0\r\n\r\n", sp->total);
            write_all(s, h, (size_t)hl); free(sp); ss_free(ss); goto done;
        }
        char h[400]; int hl = has_range
            ? snprintf(h, sizeof h, "HTTP/1.0 206 Partial Content\r\nContent-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\nContent-Range: bytes %lld-%lld/%lld\r\nContent-Length: %lld\r\nConnection: close\r\n\r\n", a, e2, sp->total, e2 - a + 1)
            : snprintf(h, sizeof h, "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\nContent-Length: %lld\r\nConnection: close\r\n\r\n", sp->total);
        if (ss_range(ss, sp, a, e2) || write_all(s, h, (size_t)hl)) { free(sp); ss_free(ss); goto done; }
        char *buf = malloc(1 << 20);
        for (ssize_t r; buf && (r = ss_read(ss, buf, 1 << 20)) > 0; ) if (write_all(s, buf, (size_t)r)) break;
        free(buf); free(sp); ss_free(ss);
        goto done;
    }
    if (!get_q(j->query, "name", name, sizeof name) || !src_find(name, &sc)) { sb_puts(&b, "{\"ok\":false,\"error\":\"no such source\"}"); reply_json(s, 404, &b); goto done; }
    if (!strcmp(j->target, "/api/src/ls")) {
        get_q(j->query, "path", path, sizeof path);
        if (!rel_ok(path)) { sb_puts(&b, "{\"ok\":false,\"error\":\"bad path\"}"); reply_json(s, 400, &b); goto done; }
        src_ls_json(&b, &sc, path, get_q(j->query, "all", v, sizeof v) && v[0] == '1');
    } else {                                                         // /api/src/test
        src_ls_json(&b, &sc, "", 0);
        logf_("ui: source test %s (%s %s:%d): %s", sc.name, sc.kind, sc.host, sc.port, strstr(b.b ? b.b : "", "\"ok\":true") ? "ok" : "failed");
    }
    reply_json(s, 200, &b);
done:
    free(b.b); close(s); free(j);
    return NULL;
}
static void src_spawn(int s, const char *method, const char *target, const char *query, const char *req) {   // takes a dup of s
    srcjob_t *j = calloc(1, sizeof *j); sb_t b = { 0 };
    int d = dup(s);
    if (!j || d < 0) { free(j); if (d >= 0) close(d); sb_puts(&b, "{\"error\":\"busy\"}"); reply_json(s, 503, &b); free(b.b); return; }
    j->s = d; snprintf(j->method, sizeof j->method, "%s", method); snprintf(j->target, sizeof j->target, "%s", target); snprintf(j->query, sizeof j->query, "%s", query ? query : "");
    const char *rg = strcasestr(req, "\nRange: "); if (rg) { sscanf(rg + 8, "%79[^\r\n]", j->range); }
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 512 * 1024); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, src_job_thread, j)) { close(d); free(j); sb_puts(&b, "{\"error\":\"no thread\"}"); reply_json(s, 503, &b); }
    pthread_attr_destroy(&at);
    free(b.b);
}
static void api_src(int s, const char *method, const char *target, const char *query, const char *req) {
    sb_t b = { 0 };
    if (!strncmp(target, "/src/", 5)) { src_spawn(s, method, target, query, req); return; }       // the stream gateway
    if (!strcmp(method, "GET") && !strcmp(target, "/api/src")) {
        src_t *all = calloc(SRC_MAX, sizeof *all); int n = 0;
        if (all) { pthread_mutex_lock(&g_src_mu); n = src_load(all, SRC_MAX); pthread_mutex_unlock(&g_src_mu); }
        sb_puts(&b, "{\"smb\":");
#ifdef BM_HAVE_SMB2
        sb_puts(&b, "true");
#else
        sb_puts(&b, "false");
#endif
        sb_puts(&b, ",\"sources\":[");
        for (int i = 0; i < n; i++) { if (i) sb_puts(&b, ","); src_json_one(&b, &all[i]); }
        sb_puts(&b, "]}"); free(all); reply_json(s, 200, &b); free(b.b); return;
    }
    if (!strcmp(method, "GET") && (!strcmp(target, "/api/src/ls") || !strcmp(target, "/api/src/test"))) { src_spawn(s, method, target, query, req); return; }
    if (strcmp(method, "POST") || (strcmp(target, "/api/src/add") && strcmp(target, "/api/src/del"))) { sb_puts(&b, "{\"error\":\"not found\"}"); reply_json(s, 404, &b); free(b.b); return; }
    if (!strcasestr(req, "\nX-BM-Request: 1")) { sb_puts(&b, "{\"ok\":false,\"error\":\"missing X-BM-Request header\"}"); reply_json(s, 403, &b); free(b.b); return; }
    src_t n; memset(&n, 0, sizeof n); char v[16];
    get_q(query, "name", n.name, sizeof n.name);
    if (!strcmp(target, "/api/src/del")) {
        int hit = src_del(n.name);
        if (hit) logf_("ui: source %s removed", n.name);
        sb_f(&b, "{\"ok\":%s}", hit ? "true" : "false"); reply_json(s, hit ? 200 : 404, &b); free(b.b); return;
    }
    get_q(query, "kind", n.kind, sizeof n.kind); get_q(query, "host", n.host, sizeof n.host);
    get_q(query, "path", n.base, sizeof n.base); get_q(query, "user", n.user, sizeof n.user); get_q(query, "pass", n.pass, sizeof n.pass);
    n.port = get_q(query, "port", v, sizeof v) ? atoi(v) : (!strcmp(n.kind, "ftp") ? 21 : !strcmp(n.kind, "smb") ? 445 : 80);
    const char *err = src_add(&n);
    if (!err) logf_("ui: source %s saved (%s %s:%d%s)", n.name, n.kind, n.host, n.port, n.user[0] ? ", with a login" : "");   // never the login itself
    if (err) { sb_puts(&b, "{\"ok\":false,\"error\":"); sb_js(&b, err); sb_puts(&b, "}"); } else sb_puts(&b, "{\"ok\":true}");
    reply_json(s, err ? 400 : 200, &b);
    memset(&n, 0, sizeof n);
    free(b.b);
}
