// SPDX-License-Identifier: GPL-3.0-or-later
// bm_api.c - blazingmountd's local API: what the BlazingMount app (a native PS5 title, its own tile) and a browser read and ask.
//
// #include'd ONCE into blazingmountd.c (one translation unit, so build.sh is unchanged), after sync_once's helpers. It uses the
// daemon's read_conf, logf_, P(), rm_rf, write_file, free_mb, tids_in. Design: docs/UI-DESIGN.md section 6.
//
// Threads. The API thread ONLY READS files (state, choices, mount.conf, the homebrew folders, the log) and the host's lists with
// its own HTTP client (the daemon's http_get has a static buffer). Every change the app asks for is QUEUED; the daemon's main
// loop is the one writer: bm_api_pass_begin() applies the queue at the start of each pass, bm_api_wait() replaces the poll sleep
// and returns early when something was queued or the host's game list changed. So the API can never race a starter build.
//
// The four hooks in blazingmountd.c (and nothing else):
//   #include "bm_api.c"                      after tsnet_setup()
//   bm_api_start();                           in main(), before the resident loop (not for "once")
//   bm_api_pass_begin(&c);                    in sync_once(), right after read_conf(&c)
//   bm_api_busy(g.tid, g.title) / bm_api_busy(NULL, NULL)   around build_game() in sync_once()
//   sleep(...) in main's loop -> bm_api_wait(seconds)
//
// HTTP/1.0 on 0.0.0.0:BM_API_PORT (8094), one request per connection, JSON out:
//   GET  /api/status                 version, gen, busy, route, free space, ShadowMount+, last pass, recent action results
//   GET  /api/library                every game: host image facts + this console's copy + choice + state + proof
//   GET  /api/settings               mount.conf keys: value, default, help
//   GET  /api/log?lines=N            the tail of /data/blazing/blazingmountd.log
//   GET  /api/icon/<TID>             icon0.png (local starter, else cached, else fetched from the host once)
//   POST /api/action?do=<a>&tid=<TID>   a = install | remove | rebuild | hide | unhide | sync | auto&value=0|1 | set&key=K&value=V
//        needs header "X-BM-Request: 1" (a web page on another site cannot send it: no cross-site writes)
//   GET  /api/catalog                the host's /library.json: EVERY game Markus owns on every system (blazingmount host/library)
//   GET  /api/covers.json            where each cover sits in the sprite sheets (page, cell, colour)
//   GET  /api/cover/<thumb|full|sheet>/<id>.jpg   a cover from the host (passed through, nothing kept on the console)
//   GET  /                           a small status page (browser fallback)

#include <pthread.h>

#ifndef BM_API_PORT
#define BM_API_PORT 8094
#endif
#define CHOICES_PATH "/data/blazing/choices.tsv"
#define ICON_DIR     "/data/blazing/icons"
#define API_MAXQ     64
#ifndef API_WATCH_S
#define API_WATCH_S  30            // the host's game list is re-read this often: a game added / rebuilt on the drives shows up by itself
#endif

// ---------------------------------------------------------------- shared state (mutex)
typedef struct { char what[16], tid[16], key[48], value[256]; } bm_act_t;
static pthread_mutex_t g_api_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_api_cv = PTHREAD_COND_INITIALIZER;
static bm_act_t g_q[API_MAXQ]; static int g_nq;
static int g_wake;                                  // a pass should run now
static unsigned long g_gen = 1;                     // bumps whenever the library may look different
static char g_busy_tid[16], g_busy_title[256]; static time_t g_busy_since;
static char g_recent[8][300]; static int g_nrecent; // the last action results, newest last
static time_t g_last_pass; static char g_host_sig[64];
static char g_catalog_etag[64];                   // the host's /library.json ETag (everything Markus owns): the app reloads when it moves
static int g_api_started;

static void api_bump(void) { pthread_mutex_lock(&g_api_mu); g_gen++; pthread_mutex_unlock(&g_api_mu); }

static void api_recent(const char *fmt, ...) {
    char m[300]; va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
    logf_("ui: %s", m);
    pthread_mutex_lock(&g_api_mu);
    if (g_nrecent == 8) { memmove(g_recent[0], g_recent[1], sizeof g_recent[0] * 7); g_nrecent = 7; }
    snprintf(g_recent[g_nrecent++], sizeof g_recent[0], "%s", m);
    g_gen++;
    pthread_mutex_unlock(&g_api_mu);
}

static void bm_api_busy(const char *tid, const char *title) {
    pthread_mutex_lock(&g_api_mu);
    snprintf(g_busy_tid, sizeof g_busy_tid, "%s", tid ? tid : "");
    snprintf(g_busy_title, sizeof g_busy_title, "%s", title ? title : "");
    g_busy_since = tid ? time(NULL) : 0;
    g_gen++;
    pthread_mutex_unlock(&g_api_mu);
}

// the main loop's sleep: returns early when the app queued something or the host's list changed
static void bm_api_wait(int secs) {
    if (!g_api_started) { sleep((unsigned)secs); return; }
    struct timeval now; gettimeofday(&now, NULL);
    struct timespec until = { now.tv_sec + secs, now.tv_usec * 1000 };
    pthread_mutex_lock(&g_api_mu);
    while (!g_wake && !g_nq) if (pthread_cond_timedwait(&g_api_cv, &g_api_mu, &until) != 0) break;
    g_wake = 0;
    pthread_mutex_unlock(&g_api_mu);
}

static void api_wake(void) { pthread_mutex_lock(&g_api_mu); g_wake = 1; pthread_cond_signal(&g_api_cv); pthread_mutex_unlock(&g_api_mu); }

// ---------------------------------------------------------------- small helpers
typedef struct { char *b; size_t n, cap; } sb_t;            // growing string
static void sb_put(sb_t *s, const char *t, size_t n) {
    if (s->n + n + 1 > s->cap) { size_t c = (s->n + n + 1) * 2; char *nb = realloc(s->b, c); if (!nb) return; s->b = nb; s->cap = c; }
    memcpy(s->b + s->n, t, n); s->n += n; s->b[s->n] = 0;
}
static void sb_puts(sb_t *s, const char *t) { sb_put(s, t, strlen(t)); }
static void sb_f(sb_t *s, const char *fmt, ...) {
    char tmp[4096]; va_list ap; va_start(ap, fmt); int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n > 0) sb_put(s, tmp, (size_t)(n < (int)sizeof tmp ? n : (int)sizeof tmp - 1));
}
static void sb_js(sb_t *s, const char *v) {                  // a JSON string, quoted and escaped
    sb_puts(s, "\"");
    for (const unsigned char *p = (const unsigned char *)(v ? v : ""); *p; p++) {
        if (*p == '"' || *p == '\\') { char e[2] = { '\\', (char)*p }; sb_put(s, e, 2); }
        else if (*p < 0x20) { char e[8]; snprintf(e, sizeof e, "\\u%04x", *p); sb_put(s, e, 6); }
        else sb_put(s, (const char *)p, 1);
    }
    sb_puts(s, "\"");
}

static char *api_slurp(const char *path, size_t max, size_t *len) {   // whole file (up to max), NUL-terminated, or NULL
    int fd = open(path, O_RDONLY); if (fd < 0) return NULL;
    struct stat sb; if (fstat(fd, &sb) != 0) { close(fd); return NULL; }
    size_t want = (size_t)sb.st_size, off = 0;
    if (want > max) { lseek(fd, (off_t)(want - max), SEEK_SET); want = max; }
    char *m = malloc(want + 1); if (!m) { close(fd); return NULL; }
    while (off < want) { ssize_t n = read(fd, m + off, want - off); if (n <= 0) break; off += (size_t)n; }
    close(fd); m[off] = 0; if (len) *len = off;
    return m;
}

// The API's own GET (the daemon's http_get is not thread-safe). LAN host first, then the tsnet proxy when the daemon found one.
static long long api_get(const conf_t *c, const char *path, char **out, int timeout_s) {
    *out = NULL;
    int s = connect_ms(c->host, c->port, LAN_CONNECT_MS), px = 0;
    if (s < 0 && g_px_port && g_ts_port) { s = connect_ms(g_px_host, g_px_port, timeout_s * 1000); px = 1; }
    if (s < 0) return -1;
    struct timeval tv = { timeout_s, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char enc[1200], req[1500]; url_path(enc, sizeof enc, path);
    int rl = px ? snprintf(req, sizeof req, "GET http://%s:%d%s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", g_ts_host, g_ts_port, enc, g_ts_host, g_ts_port)
                : snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", enc, c->host);
    if (write_all(s, req, (size_t)rl) != 0) { close(s); return -1; }
    sb_t b = { 0 }; char chunk[16384];
    for (;;) { ssize_t n = read(s, chunk, sizeof chunk); if (n <= 0) break; sb_put(&b, chunk, (size_t)n); if (b.n > (32u << 20)) break; }
    close(s);
    if (!b.b) return -1;
    char *body = strstr(b.b, "\r\n\r\n");
    int status = (b.n > 12 && !strncmp(b.b, "HTTP/1.", 7)) ? atoi(b.b + 9) : 0;
    if (!body || status < 200 || status > 299) { free(b.b); return -2; }
    body += 4;
    size_t bl = b.n - (size_t)(body - b.b);
    memmove(b.b, body, bl); b.b[bl] = 0;
    *out = b.b;
    return (long long)bl;
}

static int get_q(const char *query, const char *name, char *out, size_t on) {   // query parameter, %-decoded; 1 when found
    out[0] = 0; if (!query) return 0;
    size_t nl = strlen(name);
    for (const char *p = query; p && *p; ) {
        if (!strncmp(p, name, nl) && p[nl] == '=') {
            const char *v = p + nl + 1; size_t o = 0;
            while (*v && *v != '&' && o + 1 < on) {
                if (*v == '%' && v[1] && v[2]) { char h[3] = { v[1], v[2], 0 }; out[o++] = (char)strtol(h, NULL, 16); v += 3; }
                else { out[o++] = *v == '+' ? ' ' : *v; v++; }
            }
            out[o] = 0; return 1;
        }
        p = strchr(p, '&'); if (p) p++;
    }
    return 0;
}

static int valid_tid(const char *t) {
    if (strlen(t) != 9 || (strncmp(t, "PPSA", 4) && strncmp(t, "CUSA", 4))) return 0;
    for (int i = 4; i < 9; i++) if (t[i] < '0' || t[i] > '9') return 0;
    return 1;
}

// ---------------------------------------------------------------- choices: /data/blazing/choices.tsv  ("auto\t1", "skip\tTID", "want\tTID")
// auto 1 (default) = every game on the drives is built (today's behaviour) unless skipped; auto 0 = only the games marked want.
#define MAX_CH 1024
typedef struct { int autob; int n; char tid[MAX_CH][16]; char what[MAX_CH][6]; } choices_t;

static void choices_load(choices_t *ch) {
    memset(ch, 0, sizeof *ch); ch->autob = 1;
    char p[600]; char *m = api_slurp(P(p, sizeof p, CHOICES_PATH), 1 << 20, NULL);
    if (!m) return;
    char *save = NULL;
    for (char *l = strtok_r(m, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        char a[16] = "", b[64] = "";
        if (l[0] == '#' || sscanf(l, "%15s %63s", a, b) < 2) continue;
        if (!strcmp(a, "auto")) ch->autob = atoi(b) != 0;
        else if ((!strcmp(a, "skip") || !strcmp(a, "want")) && valid_tid(b) && ch->n < MAX_CH) {
            snprintf(ch->tid[ch->n], 16, "%s", b); snprintf(ch->what[ch->n], 6, "%s", a); ch->n++;
        }
    }
    free(m);
}
static const char *choice_of(const choices_t *ch, const char *tid) {
    for (int i = ch->n - 1; i >= 0; i--) if (!strcmp(ch->tid[i], tid)) return ch->what[i];
    return "";
}
static void choices_save(const choices_t *ch) {                   // main thread only
    sb_t s = { 0 };
    sb_f(&s, "# BlazingMount choices (written by the app; the daemon reads it every pass)\nauto\t%d\n", ch->autob);
    for (int i = 0; i < ch->n; i++) sb_f(&s, "%s\t%s\n", ch->what[i], ch->tid[i]);
    char p[600], t[640]; P(p, sizeof p, CHOICES_PATH); snprintf(t, sizeof t, "%s.tmp", p);
    if (write_file(t, s.b ? s.b : "", s.n) == 0) rename(t, p);
    free(s.b);
}
static void choice_set(choices_t *ch, const char *tid, const char *what) {   // what "" = forget
    int o = 0;
    for (int i = 0; i < ch->n; i++) if (strcmp(ch->tid[i], tid)) { if (o != i) { memcpy(ch->tid[o], ch->tid[i], 16); memcpy(ch->what[o], ch->what[i], 6); } o++; }
    ch->n = o;
    if (what[0] && ch->n < MAX_CH) { snprintf(ch->tid[ch->n], 16, "%s", tid); snprintf(ch->what[ch->n], 6, "%s", what); ch->n++; }
}

// ---------------------------------------------------------------- what this console has: one row per title id found
typedef struct {
    char tid[16], kind[10];   // kind: starter | full | image
    char path[700];           // console path (no BM_ROOT prefix)
    char key[48];             // starter: the image key its cfg streams from ("base /t/<key>")
    char fmt[12];             // image: the file's extension
    long long bytes;          // image: file size
    int pinned;               // folder holds .blazingmount-pin: auto-update leaves it alone
} local_t;
#define MAX_LOCAL 1024
static local_t g_loc[MAX_LOCAL]; static int g_nloc;           // API thread only (the main thread scans into its own array)

static int first_tid(const char *s, char *out) {              // the first PPSA12345 / CUSA12345 in s (own copy: g_present is the main thread's)
    for (const char *q = s; q[0]; q++)
        if (!strncmp(q, "PPSA", 4) || !strncmp(q, "CUSA", 4)) {
            int ok = 1; for (int i = 4; i < 9; i++) if (q[i] < '0' || q[i] > '9') { ok = 0; break; }
            if (ok) { memcpy(out, q, 9); out[9] = 0; return 1; }
        }
    return 0;
}

static const char *ext_of(const char *n) { const char *e = strrchr(n, '.'); return e ? e + 1 : ""; }

static int scan_local(const conf_t *c, local_t *out, int max) {
    int nl = 0;
    const char *roots[16]; int nr = 0;
    roots[nr++] = c->homebrew_dir;
    static const char *more[] = { "/data/homebrew", "/mnt/ext0/homebrew", "/mnt/ext1/homebrew", "/mnt/usb0/homebrew", "/mnt/usb1/homebrew", 0 };
    for (int i = 0; more[i]; i++) if (strcmp(more[i], c->homebrew_dir)) roots[nr++] = more[i];
    for (int r = 0; r < nr; r++) {
        char rp[600]; DIR *d = opendir(P(rp, sizeof rp, roots[r]));
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) && nl < max) {
            if (e->d_name[0] == '.' || e->d_name[0] == '_') continue;
            char tid[16] = "";
            first_tid(e->d_name, tid);
            char full[1400]; snprintf(full, sizeof full, "%s/%s", rp, e->d_name);
            struct stat st; if (stat(full, &st) != 0) continue;
            if (!tid[0] && S_ISDIR(st.st_mode)) {                     // a folder named otherwise: its param.json
                char pj[1500]; snprintf(pj, sizeof pj, "%s/sce_sys/param.json", full);
                char *m = api_slurp(pj, 65536, NULL);
                if (m) { char *t = strstr(m, "\"titleId\""); if (t) { char one[64]; snprintf(one, sizeof one, "%.40s", t); first_tid(one, tid); } free(m); }
            }
            if (!tid[0]) continue;
            local_t *L = &out[nl]; memset(L, 0, sizeof *L);
            snprintf(L->tid, sizeof L->tid, "%s", tid);
            snprintf(L->path, sizeof L->path, "%s/%s", roots[r], e->d_name);
            if (S_ISDIR(st.st_mode)) {
                char pin[1500]; snprintf(pin, sizeof pin, "%s/.blazingmount-pin", full); L->pinned = exists(pin);
                char cfg[700], lp[600]; snprintf(cfg, sizeof cfg, "%s/%s.cfg", P(lp, sizeof lp, LINK_DIR), tid);
                char fk[1500]; snprintf(fk, sizeof fk, "%s/fakelib/libSceAmpr.sprx", full);
                char *m = exists(fk) ? api_slurp(cfg, 1 << 16, NULL) : NULL;   // the head of the cfg holds "base"
                if (m) {
                    snprintf(L->kind, sizeof L->kind, "starter");
                    char *b = strstr(m, "\nbase /t/"); if (b) sscanf(b + 9, "%47s", L->key);
                    free(m);
                } else snprintf(L->kind, sizeof L->kind, "full");
            } else {
                snprintf(L->kind, sizeof L->kind, "image");
                snprintf(L->fmt, sizeof L->fmt, "%s", ext_of(e->d_name));
                L->bytes = (long long)st.st_size;
            }
            nl++;
        }
        closedir(d);
    }
    return nl;
}

// ---------------------------------------------------------------- the host's games (/games.json is flat objects: parse just that)
typedef struct { char key[48], tid[16], title[256], version[32], fw[16], sdk[16], name[300]; long long size; } hg_t;
#define MAX_HG 2048
static hg_t g_hg[MAX_HG]; static int g_nhg; static time_t g_hg_at; static int g_hg_ok;

static const char *js_str(const char *p, char *out, size_t on) {   // p at the opening quote; decodes \uXXXX (BMP) to UTF-8
    size_t o = 0; p++;
    while (*p && *p != '"') {
        unsigned cp = (unsigned char)*p++;
        if (cp == '\\' && *p) {
            char e = *p++;
            if (e == 'u') { char h[5] = { 0 }; for (int i = 0; i < 4 && *p; i++) h[i] = *p++; cp = (unsigned)strtol(h, NULL, 16); }
            else cp = e == 'n' ? '\n' : e == 't' ? '\t' : (unsigned char)e;
            if (cp >= 0x80) {
                char u[3]; int k = 0;
                if (cp < 0x800) { u[k++] = (char)(0xC0 | (cp >> 6)); u[k++] = (char)(0x80 | (cp & 63)); }
                else { u[k++] = (char)(0xE0 | (cp >> 12)); u[k++] = (char)(0x80 | ((cp >> 6) & 63)); u[k++] = (char)(0x80 | (cp & 63)); }
                for (int i = 0; i < k && o + 1 < on; i++) out[o++] = u[i];
                continue;
            }
        }
        if (o + 1 < on) out[o++] = (char)cp;
    }
    out[o] = 0;
    return *p ? p + 1 : p;
}

static int host_games(const conf_t *c, int max_age) {
    if (g_hg_ok && time(NULL) - g_hg_at < max_age) return 0;
    char *m = NULL;
    if (api_get(c, "/games.json", &m, 8) < 0) { g_hg_ok = 0; return -1; }
    g_nhg = 0;
    for (char *p = strchr(m, '{'); p && g_nhg < MAX_HG; p = strchr(p, '{')) {
        hg_t *g = &g_hg[g_nhg]; memset(g, 0, sizeof *g);
        char *end = strchr(p, '}'); if (!end) break;
        while (p < end) {
            char *q = strchr(p, '"'); if (!q || q > end) break;
            char k[32]; p = (char *)js_str(q, k, sizeof k);
            while (*p == ' ' || *p == ':') p++;
            if (*p == '"') {
                char v[400]; p = (char *)js_str(p, v, sizeof v);
                if (!strcmp(k, "key")) snprintf(g->key, sizeof g->key, "%s", v);
                else if (!strcmp(k, "tid")) snprintf(g->tid, sizeof g->tid, "%s", v);
                else if (!strcmp(k, "title")) snprintf(g->title, sizeof g->title, "%s", v);
                else if (!strcmp(k, "version")) snprintf(g->version, sizeof g->version, "%s", v);
                else if (!strcmp(k, "fw")) snprintf(g->fw, sizeof g->fw, "%s", v);
                else if (!strcmp(k, "sdk")) snprintf(g->sdk, sizeof g->sdk, "%s", v);
                else if (!strcmp(k, "name")) snprintf(g->name, sizeof g->name, "%s", v);
            } else {
                if (!strcmp(k, "size")) g->size = atoll(p);
                while (p < end && *p != ',') p++;
            }
            if (*p == ',') p++;
        }
        if (g->tid[0]) g_nhg++;
        p = end + 1;
    }
    free(m);
    g_hg_ok = 1; g_hg_at = time(NULL);
    return 0;
}

static int host_has_key(const char *tid, const char *key) {      // any image of tid on the drives has this key (a title may have two)
    for (int i = 0; i < g_nhg; i++) if (!strcmp(g_hg[i].tid, tid) && !strcmp(g_hg[i].key, key)) return 1;
    return 0;
}

// proofs: the host MAY serve /proven.tsv (TID \t console \t date \t evidence, = mac1 games-proven.tsv). Missing = "unknown", never "works".
static char *g_proven; static time_t g_proven_at;
static void host_proven(const conf_t *c) {
    if (time(NULL) - g_proven_at < 300) return;
    g_proven_at = time(NULL);
    char *m = NULL;
    if (api_get(c, "/proven.tsv", &m, 5) >= 0) { free(g_proven); g_proven = m; }
}
static int proven_line(const char *tid, char *out, size_t on) {   // the newest proof line for tid ("console date: evidence")
    out[0] = 0; if (!g_proven) return 0;
    for (const char *l = g_proven; l && *l; ) {
        const char *nl = strchr(l, '\n'); size_t L = nl ? (size_t)(nl - l) : strlen(l);
        if (L > 10 && !strncmp(l, tid, 9) && l[9] == '\t') {
            char row[600]; snprintf(row, sizeof row, "%.*s", (int)(L < 599 ? L : 599), l);
            char *f[4] = { 0 }; int k = 0; char *q = row; f[k++] = q;
            while (*q && k < 4) { if (*q == '\t') { *q = 0; f[k++] = q + 1; } q++; }
            if (k >= 4) snprintf(out, on, "%s %s: %.200s", f[1], f[2], f[3]);
        }
        l = nl ? nl + 1 : NULL;
    }
    return out[0] != 0;
}

// ---------------------------------------------------------------- GET /api/library
static void st_find(const char *m, const char *key, char *status, size_t sn, int *tries, char *reason, size_t rn) {
    status[0] = 0; reason[0] = 0; *tries = 0;
    if (!m) return;
    size_t kl = strlen(key);
    for (const char *l = m; l && *l; ) {
        const char *nl = strchr(l, '\n');
        if (!strncmp(l, key, kl) && l[kl] == '\t') {
            char row[400]; size_t L = nl ? (size_t)(nl - l) : strlen(l); snprintf(row, sizeof row, "%.*s", (int)(L < 399 ? L : 399), l);
            char *f[4] = { 0 }; int k = 0; char *q = row; f[k++] = q;
            while (*q && k < 4) { if (*q == '\t') { *q = 0; f[k++] = q + 1; } q++; }
            if (k >= 3) { snprintf(status, sn, "%s", f[1]); *tries = atoi(f[2]); if (k > 3) snprintf(reason, rn, "%s", f[3]); }
        }
        l = nl ? nl + 1 : NULL;
    }
}

static void api_library(sb_t *o) {
    conf_t c; read_conf(&c);
    int host_ok = host_games(&c, 10) == 0;
    host_proven(&c);
    g_nloc = scan_local(&c, g_loc, MAX_LOCAL);
    choices_t *ch = malloc(sizeof *ch); choices_load(ch);
    char sp[600]; char *stm = api_slurp(P(sp, sizeof sp, STATE_PATH), 1 << 20, NULL);
    int cfw = console_fw(&c);
    unsigned long gen; pthread_mutex_lock(&g_api_mu); gen = g_gen; char busy[16]; snprintf(busy, sizeof busy, "%s", g_busy_tid); pthread_mutex_unlock(&g_api_mu);
    sb_f(o, "{\"gen\":%lu,\"host\":", gen); sb_js(o, c.host); sb_f(o, ",\"host_ok\":%s,\"auto\":%d,\"console_fw\":\"%d.%02d\",\"games\":[",
         host_ok ? "true" : "false", ch->autob, cfw / 100, cfw % 100);
    static char seen[MAX_LOCAL]; memset(seen, 0, sizeof seen);
    int first = 1;
    for (int i = 0; i < (host_ok ? g_nhg : 0); i++) {
        hg_t *g = &g_hg[i];
        int dupe = 0; for (int j = 0; j < i; j++) if (!strcmp(g_hg[j].tid, g->tid)) dupe = 1;
        if (dupe) continue;                                       // one row per title (the host's first image wins, as a pass does)
        local_t *L = NULL; for (int j = 0; j < g_nloc; j++) if (!strcmp(g_loc[j].tid, g->tid)) { if (!L) L = &g_loc[j]; seen[j] = 1; }
        char status[16], reason[240]; int tries; st_find(stm, g->key, status, sizeof status, &tries, reason, sizeof reason);
        const char *choice = choice_of(ch, g->tid);
        int gfw = fw_hundredths(g->fw);
        const char *fmt = ext_of(g->name); if (!strchr(g->name, '.')) fmt = "folder";
        // one word the app can badge: playable here, being built, waiting, needs something, or off
        const char *state;
        if (L && strcmp(L->kind, "image") && game_running(g->tid)) state = "playing";
        else if (!strcmp(busy, g->tid)) state = "building";
        else if (L && !strcmp(L->kind, "starter") && L->key[0] && !host_has_key(g->tid, L->key)) state = "outdated";
        else if (L) state = "installed";
        else if (!strcmp(choice, "skip")) state = "hidden";
        else if (cfw && gfw > cfw) state = "needs_fw";
        else if (!strcmp(status, "refused")) state = "refused";
        else if (!strcmp(status, "failed")) state = tries >= MAX_TRIES ? "failed" : "queued";
        else if (!ch->autob && strcmp(choice, "want")) state = "available";
        else state = "queued";
        char proof[300]; proven_line(g->tid, proof, sizeof proof);
        if (!first) sb_puts(o, ","); first = 0;
        sb_puts(o, "{\"tid\":"); sb_js(o, g->tid); sb_puts(o, ",\"title\":"); sb_js(o, g->title[0] ? g->title : g->tid);
        sb_f(o, ",\"platform\":\"%s\",\"key\":", !strncmp(g->tid, "CUSA", 4) ? "PS4" : "PS5"); sb_js(o, g->key);
        sb_puts(o, ",\"version\":"); sb_js(o, g->version); sb_puts(o, ",\"fw\":"); sb_js(o, g->fw);
        sb_f(o, ",\"size\":%lld,\"format\":", g->size); sb_js(o, fmt);
        sb_f(o, ",\"compressed\":%s,\"on_drives\":true,\"file\":", !strcasecmp(fmt, "ffpfsc") ? "true" : "false"); sb_js(o, g->name);
        sb_puts(o, ",\"local\":"); sb_js(o, L ? L->kind : "none");
        sb_puts(o, ",\"local_path\":"); sb_js(o, L ? L->path : "");
        sb_puts(o, ",\"state\":"); sb_js(o, state);
        sb_puts(o, ",\"choice\":"); sb_js(o, choice);
        sb_puts(o, ",\"build\":"); sb_js(o, status); sb_f(o, ",\"tries\":%d,\"reason\":", tries); sb_js(o, reason);
        sb_puts(o, ",\"proof\":"); sb_js(o, proof);
        sb_f(o, ",\"pinned\":%s}", L && L->pinned ? "true" : "false");
    }
    for (int j = 0; j < g_nloc; j++) {                            // on this console, not (or no longer) on the drives
        if (seen[j]) continue;
        int dupe = 0; for (int k = 0; k < j; k++) if (!strcmp(g_loc[k].tid, g_loc[j].tid)) dupe = 1;
        if (dupe) continue;
        local_t *L = &g_loc[j];
        const char *nm = strrchr(L->path, '/'); nm = nm ? nm + 1 : L->path;
        char title[300]; snprintf(title, sizeof title, "%s", nm);
        char *paren = strstr(title, " ("); if (paren) *paren = 0;
        char *dot = strrchr(title, '.'); if (dot && !strcmp(L->kind, "image")) *dot = 0;
        char proof[300]; proven_line(L->tid, proof, sizeof proof);
        if (!first) sb_puts(o, ","); first = 0;
        sb_puts(o, "{\"tid\":"); sb_js(o, L->tid); sb_puts(o, ",\"title\":"); sb_js(o, title);
        sb_f(o, ",\"platform\":\"%s\",\"key\":\"\",\"version\":\"\",\"fw\":\"\",\"size\":%lld,\"format\":", !strncmp(L->tid, "CUSA", 4) ? "PS4" : "PS5", L->bytes);
        sb_js(o, !strcmp(L->kind, "image") ? L->fmt : "folder");
        sb_f(o, ",\"compressed\":%s,\"on_drives\":false,\"file\":\"\",\"local\":", !strcasecmp(L->fmt, "ffpfsc") ? "true" : "false");
        sb_js(o, L->kind); sb_puts(o, ",\"local_path\":"); sb_js(o, L->path);
        // a starter whose image left the drives cannot stream any more; a full copy or image plays on its own
        sb_puts(o, ",\"state\":"); sb_js(o, !strcmp(L->kind, "starter") ? "orphan" : "installed");
        sb_puts(o, ",\"choice\":"); sb_js(o, choice_of(ch, L->tid));
        sb_puts(o, ",\"build\":\"\",\"tries\":0,\"reason\":\"\",\"proof\":"); sb_js(o, proof);
        sb_f(o, ",\"pinned\":%s}", L->pinned ? "true" : "false");
    }
    sb_puts(o, "]}");
    free(stm); free(ch);
}

// ---------------------------------------------------------------- GET /api/status, /api/settings, /api/log
static long long dir_free(const char *d) { char p[600]; return free_mb(P(p, sizeof p, d)); }

static void api_status(sb_t *o) {
    conf_t c; read_conf(&c);
    char sp[600], b[512] = "", ver[64] = "";
    int fd = open(P(sp, sizeof sp, SMP_LOG), O_RDONLY);
    if (fd >= 0) { ssize_t n = read(fd, b, sizeof b - 1); close(fd); if (n > 0) { b[n] = 0; char *v = strstr(b, "ShadowMount+ v"); if (v) sscanf(v + 14, "%63s", ver); } }
    pthread_mutex_lock(&g_api_mu);
    sb_f(o, "{\"version\":\"%s\",\"now\":%lld,\"gen\":%lu,\"api_port\":%d,\"busy\":{\"tid\":", BM_VERSION, (long long)time(NULL), g_gen, BM_API_PORT);
    sb_js(o, g_busy_tid); sb_puts(o, ",\"title\":"); sb_js(o, g_busy_title);
    sb_f(o, ",\"since\":%lld},\"last_pass\":%lld,\"queued\":%d,\"recent\":[", (long long)g_busy_since, (long long)g_last_pass, g_nq);
    for (int i = 0; i < g_nrecent; i++) { if (i) sb_puts(o, ","); sb_js(o, g_recent[i]); }
    pthread_mutex_unlock(&g_api_mu);
    sb_puts(o, "],\"host\":"); sb_js(o, c.host); sb_f(o, ",\"port\":%d,\"route\":\"%s\",\"tsnet\":%s", c.port,
         g_route ? "TAILNET" : "LAN", (g_px_port && g_ts_port) ? "true" : "false");
    sb_puts(o, ",\"smp\":"); sb_js(o, ver); sb_puts(o, ",\"smp_expected\":"); sb_js(o, c.smp_version);
    sb_puts(o, ",\"homebrew_dir\":"); sb_js(o, c.homebrew_dir);
    sb_puts(o, ",\"catalog_etag\":"); sb_js(o, g_catalog_etag);
    sb_f(o, ",\"free_mb\":{\"data\":%lld,\"ext0\":%lld,\"ext1\":%lld,\"usb0\":%lld},\"min_free_mb\":%lld,\"fork\":\"%.16s\"}",
         dir_free("/data"), dir_free("/mnt/ext0"), dir_free("/mnt/ext1"), dir_free("/mnt/usb0"), c.min_free_mb, BM_FORK_SHA);
}

// the keys the app may show and change (= read_conf). type: n number, b 0/1, s text, l list (shown, edited as choices)
static const struct { const char *k, *type, *help; } k_keys[] = {
    { "host", "s", "The computer with your game drives (imgserve)" },
    { "port", "n", "imgserve port" },
    { "homebrew_dir", "s", "Where starters go (/data/homebrew, or /mnt/ext1/homebrew = the M.2)" },
    { "max_starter_mb", "n", "Largest starter built (MB)" },
    { "big_starter_mb", "n", "Largest starter when there is lots of room (MB)" },
    { "big_free_mb", "n", "\"Lots of room\" means this much free (MB)" },
    { "min_free_mb", "n", "Always leave this much free (MB)" },
    { "disk_cache_mb", "n", "Console disk cache per game, fixed cap (MB, 0 = off)" },
    { "cache_mb_ram", "n", "RAM cache per game (MB)" },
    { "conns", "n", "Connections per game" },
    { "readahead_kb", "n", "Read-ahead (KB)" },
    { "tsnet", "b", "Play away from home through the Tailscale payload" },
    { "auto_update", "b", "Keep starters current (new link, changed images rebuilt)" },
    { "poll_secs", "n", "Full check of the drives every (seconds)" },
    { "smp_version", "s", "The ShadowMount+ build starters are proven with" },
    { 0, 0, 0 } };

static void api_settings(sb_t *o) {
    conf_t c; read_conf(&c);
    char hd[64]; snprintf(hd, sizeof hd, "%s", c.homebrew_dir);
    char v[40][300]; int i = 0;
    snprintf(v[i++], 300, "%s", c.host); snprintf(v[i++], 300, "%d", c.port); snprintf(v[i++], 300, "%s", hd);
    snprintf(v[i++], 300, "%lld", c.max_starter_mb); snprintf(v[i++], 300, "%lld", c.big_starter_mb); snprintf(v[i++], 300, "%lld", c.big_free_mb);
    snprintf(v[i++], 300, "%lld", c.min_free_mb); snprintf(v[i++], 300, "%lld", c.disk_cache_mb); snprintf(v[i++], 300, "%d", c.cache_mb_ram);
    snprintf(v[i++], 300, "%d", c.conns); snprintf(v[i++], 300, "%d", c.readahead_kb); snprintf(v[i++], 300, "%d", c.tsnet); snprintf(v[i++], 300, "%d", c.auto_update);
    snprintf(v[i++], 300, "%d", c.poll_secs); snprintf(v[i++], 300, "%s", c.smp_version);
    choices_t *ch = malloc(sizeof *ch); choices_load(ch);
    sb_f(o, "{\"auto\":%d,\"keys\":[", ch->autob);
    free(ch);
    for (int k = 0; k_keys[k].k; k++) {
        if (k) sb_puts(o, ",");
        sb_puts(o, "{\"key\":"); sb_js(o, k_keys[k].k); sb_puts(o, ",\"type\":"); sb_js(o, k_keys[k].type);
        sb_puts(o, ",\"value\":"); sb_js(o, v[k]); sb_puts(o, ",\"help\":"); sb_js(o, k_keys[k].help); sb_puts(o, "}");
    }
    sb_puts(o, "]}");
}

static void api_log(sb_t *o, int lines) {
    char p[600]; size_t n = 0; char *m = api_slurp(P(p, sizeof p, LOG_PATH), 256 * 1024, &n);
    sb_puts(o, "{\"lines\":[");
    if (m) {
        int seen = 0; size_t i = n;
        while (i > 0 && seen <= lines) { i--; if (m[i] == '\n') seen++; }
        char *s = m + i; if (*s == '\n') s++;
        int first = 1; char *save = NULL;
        for (char *l = strtok_r(s, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) { if (!first) sb_puts(o, ","); first = 0; sb_js(o, l); }
        free(m);
    }
    sb_puts(o, "]}");
}

// ---------------------------------------------------------------- icons
static int api_icon(const char *tid, char **png, size_t *len) {
    if (!valid_tid(tid)) return -1;
    char p[600], ip[700]; snprintf(ip, sizeof ip, "%s/%s.png", P(p, sizeof p, ICON_DIR), tid);
    if ((*png = api_slurp(ip, 4 << 20, len))) return 0;
    conf_t c; read_conf(&c);
    g_nloc = scan_local(&c, g_loc, MAX_LOCAL);
    for (int j = 0; j < g_nloc; j++) if (!strcmp(g_loc[j].tid, tid) && strcmp(g_loc[j].kind, "image")) {
        char lp[1500], rp[800]; snprintf(lp, sizeof lp, "%s/sce_sys/icon0.png", P(rp, sizeof rp, g_loc[j].path));
        if ((*png = api_slurp(lp, 4 << 20, len))) return 0;
    }
    if (host_games(&c, 60) != 0) return -1;
    for (int i = 0; i < g_nhg; i++) if (!strcmp(g_hg[i].tid, tid)) {
        char url[200], *m = NULL; snprintf(url, sizeof url, "/t/%s/sce_sys/icon0.png", g_hg[i].key);
        long long n = api_get(&c, url, &m, 10);
        if (n <= 8 || memcmp(m, "\x89PNG", 4)) { free(m); return -1; }
        makedirs(P(p, sizeof p, ICON_DIR));
        char t[720]; snprintf(t, sizeof t, "%s.tmp", ip);
        if (write_file(t, m, (size_t)n) == 0) rename(t, ip);          // one small file per title, never refetched
        *png = m; *len = (size_t)n; return 0;
    }
    return -1;
}

// ---------------------------------------------------------------- actions (queued here, applied by the main loop)
static const char *api_enqueue(const char *what, const char *tid, const char *key, const char *value) {
    static const char *ok[] = { "install", "remove", "rebuild", "hide", "unhide", "sync", "auto", "set", 0 };
    int good = 0; for (int i = 0; ok[i]; i++) if (!strcmp(ok[i], what)) good = 1;
    if (!good) return "unknown action";
    int needs_tid = strcmp(what, "sync") && strcmp(what, "auto") && strcmp(what, "set");
    if (needs_tid && !valid_tid(tid)) return "bad title id";
    if (!strcmp(what, "set")) {
        int known = 0; for (int k = 0; k_keys[k].k; k++) if (!strcmp(k_keys[k].k, key)) known = 1;
        if (!known) return "unknown setting";
        for (const char *q = value; *q; q++) if (*q == '\n' || *q == '\r' || *q == '#') return "bad value";
        if (!value[0] || strlen(value) > 200) return "bad value";
    }
    pthread_mutex_lock(&g_api_mu);
    if (g_nq >= API_MAXQ) { pthread_mutex_unlock(&g_api_mu); return "queue full"; }
    bm_act_t *a = &g_q[g_nq++]; memset(a, 0, sizeof *a);
    snprintf(a->what, sizeof a->what, "%s", what); snprintf(a->tid, sizeof a->tid, "%s", tid ? tid : "");
    snprintf(a->key, sizeof a->key, "%s", key ? key : ""); snprintf(a->value, sizeof a->value, "%s", value ? value : "");
    g_gen++;
    pthread_cond_signal(&g_api_cv);
    pthread_mutex_unlock(&g_api_mu);
    return NULL;
}

static void state_forget_tid(const char *tid) {                   // main thread: every state line of this title (any image key)
    state_load();
    int o = 0;
    for (int i = 0; i < g_nst; i++) if (strncmp(g_st[i].key, tid, 9)) g_st[o++] = g_st[i];
    if (o != g_nst) { g_nst = o; state_save(); }
}

static int remove_starter(const conf_t *c, const char *tid, char *why, size_t wn) {   // ONLY our starters; 1 = removed, -1 = running
    if (game_running(tid)) { snprintf(why, wn, "%s is running: close it first, nothing changed", tid); return -1; }
    local_t *lo = malloc(sizeof *lo * MAX_LOCAL); if (!lo) return 0;
    int n = scan_local(c, lo, MAX_LOCAL), removed = 0, other = 0;
    for (int j = 0; j < n; j++) {
        if (strcmp(lo[j].tid, tid)) continue;
        if (strcmp(lo[j].kind, "starter")) { other++; continue; }
        char fp[800]; P(fp, sizeof fp, lo[j].path);
        rm_rf(fp); removed++;
    }
    free(lo);
    if (removed) { char lp[600], cfg[700]; snprintf(cfg, sizeof cfg, "%s/%s.cfg", P(lp, sizeof lp, LINK_DIR), tid); unlink(cfg); }
    if (other) snprintf(why, wn, "%d full copy/image of %s left alone (the app only removes starters it can rebuild)", other, tid);
    else if (!removed) snprintf(why, wn, "no starter of %s on this console", tid);
    return removed > 0;
}

static void conf_set_key(const char *key, const char *value) {  // main thread: replace "key ..." lines in mount.conf, else append
    char p[600]; char *m = api_slurp(P(p, sizeof p, CONF_PATH), 1 << 16, NULL);
    sb_t s = { 0 }; int done = 0; size_t kl = strlen(key);
    if (m) {
        char *save = NULL;
        for (char *l = strtok_r(m, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
            if (!strncmp(l, key, kl) && (l[kl] == ' ' || l[kl] == '\t')) { if (!done) sb_f(&s, "%s %s\n", key, value); done = 1; }
            else sb_f(&s, "%s\n", l);
        }
        free(m);
    }
    if (!done) sb_f(&s, "%s %s\n", key, value);
    char t[640]; snprintf(t, sizeof t, "%s.tmp", p);
    if (write_file(t, s.b, s.n) == 0) rename(t, p);
    free(s.b);
}

// main thread, start of every pass (after read_conf): apply the queue, then fold the choices into this pass's conf
static void bm_api_pass_begin(conf_t *c) {
    bm_act_t q[API_MAXQ]; int n;
    pthread_mutex_lock(&g_api_mu); n = g_nq; memcpy(q, g_q, sizeof q[0] * (size_t)n); g_nq = 0; pthread_mutex_unlock(&g_api_mu);
    choices_t *ch = malloc(sizeof *ch); choices_load(ch);
    int conf_changed = 0;
    for (int i = 0; i < n; i++) {
        bm_act_t *a = &q[i]; char why[300] = "";
        if (!strcmp(a->what, "install")) {
            choice_set(ch, a->tid, "want"); state_forget_tid(a->tid);
            api_recent("install %s: marked wanted, building this pass", a->tid);
        } else if (!strcmp(a->what, "unhide")) {
            choice_set(ch, a->tid, ""); api_recent("show %s again: back to the normal rule (%s)", a->tid, ch->autob ? "built with every game" : "built when picked");
        } else if (!strcmp(a->what, "hide")) {
            choice_set(ch, a->tid, "skip"); api_recent("hide %s: will not be built (its copy, if any, stays)", a->tid);
        } else if (!strcmp(a->what, "remove")) {
            int r = remove_starter(c, a->tid, why, sizeof why);
            if (r < 0) { api_recent("remove %s: %s", a->tid, why); continue; }
            choice_set(ch, a->tid, "skip"); state_forget_tid(a->tid);
            api_recent("remove %s: %s%s%s", a->tid, r ? "starter deleted, will not be built again" : "nothing deleted, will not be built",
                       why[0] ? "; " : "", why);
        } else if (!strcmp(a->what, "rebuild")) {
            int r = remove_starter(c, a->tid, why, sizeof why);
            if (r < 0) { api_recent("rebuild %s: %s", a->tid, why); continue; }
            choice_set(ch, a->tid, "want"); state_forget_tid(a->tid);
            api_recent("rebuild %s: %s", a->tid, r ? "old starter deleted, building again this pass" : (why[0] ? why : "building this pass"));
        } else if (!strcmp(a->what, "auto")) {
            ch->autob = atoi(a->value) != 0; api_recent("new games: %s", ch->autob ? "build every game on the drives" : "only the games I pick");
        } else if (!strcmp(a->what, "set")) {
            conf_set_key(a->key, a->value); conf_changed = 1; api_recent("setting %s = %s", a->key, a->value);
        } else if (!strcmp(a->what, "sync")) {
            api_recent("check the drives now");
        }
    }
    if (n) choices_save(ch);
    if (conf_changed) read_conf(c);
    // fold in: skips always; with auto off only the wanted games (an empty want list builds nothing)
    for (int i = 0; i < ch->n; i++) {
        if (!strcmp(ch->what[i], "skip") && c->n_skip < MAX_LIST) snprintf(c->skip[c->n_skip++], 40, "%s", ch->tid[i]);
        if (!strcmp(ch->what[i], "want") && c->n_first < MAX_LIST) snprintf(c->first[c->n_first++], 40, "%s", ch->tid[i]);   // wanted = first
    }
    if (!ch->autob) {
        int had = c->n_only;
        for (int i = 0; i < ch->n; i++) if (!strcmp(ch->what[i], "want") && c->n_only < MAX_LIST) snprintf(c->only[c->n_only++], 40, "%s", ch->tid[i]);
        if (c->n_only == had && c->n_only < MAX_LIST) snprintf(c->only[c->n_only++], 40, "NONE");
    }
    free(ch);
    pthread_mutex_lock(&g_api_mu); g_last_pass = time(NULL); g_gen++; pthread_mutex_unlock(&g_api_mu);
}

// ---------------------------------------------------------------- the server
static void reply(int s, int code, const char *ctype, const char *body, size_t n) {
    char h[400];
    int hl = snprintf(h, sizeof h, "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                      "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", code, code == 200 ? "OK" : code == 404 ? "Not Found" : "Error", ctype, n);
    write_all(s, h, (size_t)hl);
    if (n) write_all(s, body, n);
}
static void reply_json(int s, int code, sb_t *b) { reply(s, code, "application/json; charset=utf-8", b->b ? b->b : "{}", b->b ? b->n : 2); }

static const char k_page[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'><title>BlazingMount</title>"
    "<style>body{background:#0A0A0B;color:#fffc;font:18px system-ui;margin:16px}h1{color:#fff;font-size:28px}"
    "td{padding:6px 10px;border-bottom:2px solid #FF3D4733}.s{color:#fff9}button{background:#0E0A0C;color:#fff;border:2px solid #FF3D4733;"
    "border-radius:16px;padding:6px 12px}button:focus{border-color:#FF3D47}</style><h1>BlazingMount</h1><p class=s id=st></p><table id=t></table>"
    "<script>const A=(d,t)=>fetch('/api/action?do='+d+'&tid='+t,{method:'POST',headers:{'X-BM-Request':'1'}}).then(L);"
    "function L(){fetch('/api/status').then(r=>r.json()).then(s=>st.textContent=s.version+' | '+s.route+' | /data free '+s.free_mb.data+' MB'+(s.busy.tid?' | building '+s.busy.title:''));"
    "fetch('/api/library').then(r=>r.json()).then(j=>{t.innerHTML=j.games.map(g=>`<tr><td>${g.title}<div class=s>${g.tid} ${g.format} ${(g.size/1e9).toFixed(1)} GB</div>"
    "<td>${g.state}<td>${g.local}<td><button onclick=\"A('install','${g.tid}')\">Install</button> <button onclick=\"A('remove','${g.tid}')\">Remove</button>"
    " <button onclick=\"A('hide','${g.tid}')\">Hide</button>`).join('')})}L();setInterval(L,3000)</script>";

static void api_handle(int s) {
    struct timeval tv = { 3, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char req[4096]; size_t have = 0;
    while (have < sizeof req - 1) {
        ssize_t n = read(s, req + have, sizeof req - 1 - have);
        if (n <= 0) break;
        have += (size_t)n; req[have] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    req[have] = 0;
    char method[8] = "", target[1024] = "";
    if (sscanf(req, "%7s %1023s", method, target) != 2) return;
    char *query = strchr(target, '?'); if (query) *query++ = 0;
    sb_t b = { 0 };
    if (!strcmp(method, "GET") && !strcmp(target, "/api/status")) { api_status(&b); reply_json(s, 200, &b); }
    else if (!strcmp(method, "GET") && !strcmp(target, "/api/library")) { api_library(&b); reply_json(s, 200, &b); }
    else if (!strcmp(method, "GET") && !strcmp(target, "/api/settings")) { api_settings(&b); reply_json(s, 200, &b); }
    else if (!strcmp(method, "GET") && !strcmp(target, "/api/log")) {
        char v[16]; int n = get_q(query, "lines", v, sizeof v) ? atoi(v) : 200; if (n < 1) n = 1; if (n > 2000) n = 2000;
        api_log(&b, n); reply_json(s, 200, &b);
    }
    else if (!strcmp(method, "GET") && (!strcmp(target, "/api/catalog") || !strcmp(target, "/api/covers.json") || !strncmp(target, "/api/cover/", 11))) {
        // passed through from the host (LAN, else the tsnet proxy); a cover name is checked so nothing else on the host can be asked for
        char hp[200] = ""; const char *ct = "application/json";
        if (!strcmp(target, "/api/catalog")) snprintf(hp, sizeof hp, "/library.json");
        else if (!strcmp(target, "/api/covers.json")) snprintf(hp, sizeof hp, "/covers.json");
        else {
            char kind[8] = "", name[40] = ""; const char *q = target + 11;
            if (sscanf(q, "%7[a-z]/%39s", kind, name) == 2 && (!strcmp(kind, "thumb") || !strcmp(kind, "full") || !strcmp(kind, "sheet"))) {
                size_t L = strlen(name); int ok = L > 4 && L <= 16 && !strcmp(name + L - 4, ".jpg");
                for (size_t i = 0; ok && i + 4 < L; i++) if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) ok = 0;
                if (ok) { snprintf(hp, sizeof hp, "/covers/%s/%s", kind, name); ct = "image/jpeg"; }
            }
        }
        conf_t c; read_conf(&c);
        char *m = NULL; long long n = hp[0] ? api_get(&c, hp, &m, 20) : -2;
        if (n >= 0) reply(s, 200, ct, m, (size_t)n);
        else reply(s, 404, "text/plain", "not on the host", 15);
        free(m);
    }
    else if (!strcmp(method, "GET") && !strncmp(target, "/api/icon/", 10)) {
        char *png = NULL; size_t n = 0;
        if (api_icon(target + 10, &png, &n) == 0) reply(s, 200, "image/png", png, n);
        else reply(s, 404, "text/plain", "no icon", 7);
        free(png);
    }
    else if (!strcmp(method, "POST") && !strcmp(target, "/api/action")) {
        if (!strcasestr(req, "\nX-BM-Request: 1")) { sb_f(&b, "{\"ok\":false,\"error\":\"missing X-BM-Request header\"}"); reply_json(s, 403, &b); }
        else {
            char what[16], tid[16], key[48], value[256];
            get_q(query, "do", what, sizeof what); get_q(query, "tid", tid, sizeof tid);
            get_q(query, "key", key, sizeof key); get_q(query, "value", value, sizeof value);
            const char *err = api_enqueue(what, tid, key, value);
            if (err) { sb_f(&b, "{\"ok\":false,\"error\":"); sb_js(&b, err); sb_put(&b, "}", 1); reply_json(s, 400, &b); }
            else { sb_f(&b, "{\"ok\":true,\"queued\":"); sb_js(&b, what); sb_put(&b, "}", 1); reply_json(s, 200, &b); }
        }
    }
    else if (!strcmp(method, "GET") && !strcmp(target, "/")) reply(s, 200, "text/html; charset=utf-8", k_page, sizeof k_page - 1);
    else { sb_f(&b, "{\"error\":\"not found\"}"); reply_json(s, 404, &b); }
    free(b.b);
}

// the host's list changed (a game added, rebuilt or removed on the drives): bump gen + wake the main loop for a pass now
static void catalog_watch(const conf_t *c) {        // the ETag of the host's /library.json, from the response headers of a HEAD-like GET
    int s = connect_ms(c->host, c->port, LAN_CONNECT_MS);
    if (s < 0) return;
    struct timeval tv = { 5, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char req[300]; int rl = snprintf(req, sizeof req, "GET /library.json HTTP/1.0\r\nHost: %s\r\nIf-None-Match: \"%s\"\r\nConnection: close\r\n\r\n", c->host, g_catalog_etag);
    if (write_all(s, req, (size_t)rl) != 0) { close(s); return; }
    char h[2048]; ssize_t n = read(s, h, sizeof h - 1); close(s);   // the headers arrive in the first read; the body (if any) is dropped
    if (n <= 0) return;
    h[n] = 0;
    char *e = strcasestr(h, "\nETag: \"");
    if (!e) return;
    char tag[64] = ""; sscanf(e + 8, "%63[^\"]", tag);
    if (tag[0] && strcmp(tag, g_catalog_etag)) {
        pthread_mutex_lock(&g_api_mu); snprintf(g_catalog_etag, sizeof g_catalog_etag, "%s", tag); g_gen++; pthread_mutex_unlock(&g_api_mu);
        logf_("ui: the host's whole-library index is %s", tag);
    }
}

static void api_watch(void) {
    conf_t c; read_conf(&c);
    catalog_watch(&c);
    char *m = NULL;
    if (api_get(&c, "/games.txt", &m, 5) < 0) return;
    uint64_t h = fnv_key(m, strlen(m));                            // the daemon's FNV-1a (case-folded: fine for a change signal)
    free(m);
    char sig[64]; snprintf(sig, sizeof sig, "%016llx", (unsigned long long)h);
    if (!g_host_sig[0]) { snprintf(g_host_sig, sizeof g_host_sig, "%s", sig); return; }
    if (strcmp(sig, g_host_sig)) {
        snprintf(g_host_sig, sizeof g_host_sig, "%s", sig);
        g_hg_ok = 0;
        logf_("ui: the drives' game list changed: checking now");
        api_bump(); api_wake();
    }
}

static void *api_thread(void *arg) {
    (void)arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(BM_API_PORT); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 8) != 0) {
        logf_("ui: API could not listen on :%d (errno %d) - the app cannot reach this payload", BM_API_PORT, errno);
        if (ls >= 0) close(ls);
        return NULL;
    }
    logf_("ui: API listening on :%d (the BlazingMount app; a browser at http://<console>:%d/)", BM_API_PORT, BM_API_PORT);
    api_watch();                                                   // the baseline: the list as it is now
    time_t next_watch = time(NULL) + API_WATCH_S;
    for (;;) {
        struct pollfd pf = { ls, POLLIN, 0 };
        if (poll(&pf, 1, 1000) == 1) {
            int s = accept(ls, NULL, NULL);
            if (s >= 0) { api_handle(s); close(s); }
        }
        if (time(NULL) >= next_watch) { api_watch(); next_watch = time(NULL) + API_WATCH_S; }
    }
    return NULL;
}

static void bm_api_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, api_thread, NULL) == 0) { pthread_detach(t); g_api_started = 1; }
    else logf_("ui: API thread did not start (errno %d)", errno);
}
