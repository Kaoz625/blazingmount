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
//   GET  /api/library                every game: host image facts + this console's copies (starter / full / image / pkg / parked; the
//                                    active one, on_console) + choice + state + proof + its download request and mac1's progress
//   GET  /api/settings               mount.conf keys: value, default, help
//   GET  /api/log?lines=N            the tail of /data/blazing/blazingmountd.log
//   GET  /api/icon/<TID>             icon0.png (local starter, else cached, else fetched from the host once)
//   GET  /api/downloads              v2.8, docs/DOWNLOAD-TO-CONSOLE.md: the app's download requests + mac1's status + free space + route
//   GET  /api/sha256?path=&off=&len= v2.8: SHA-256 of up to 1 GiB of a file under <drive>/blz-stage/incoming/ (mac1 verifies a download)
//   GET  /api/pull, POST /api/pull?src=&port=&dest=&size=&conns= | ?cancel=1   v2.9 (bm_pull.c): the console pulls a host file into
//                                    incoming/ on several Range connections, resumable; mac1 verifies with /api/sha256 as before
//   GET  /api/pm/drives|packages|queue|status, POST /api/pm/install?path=|cancel?id=|refresh   v2.9: install a PKG from the drives
//   POST /api/smp/version | /api/smp/uninstall?tid=                       v2.9: calls ShadowMount+'s local API
//                                    with no copy first, through PKG Manager on this console (:8844)
//   GET  /api/store, POST /api/store/install?kind=app|payload|pkg&path=&name=&size=   v2.9 (bm_store.c): the homebrew store over the
//                                    host's /homebrew.json + /hb/: app folders into the homebrew dir, payloads into a payload slot
//   POST /api/action?do=<a>&tid=<TID>   a = install | remove | rebuild | hide | unhide | sync | auto&value=0|1 | set&key=K&value=V
//        | download | remove_local | cancel_dl (v2.8: one line per title in /data/blazing/downloads.tsv for mac1's worker)
//        needs header "X-BM-Request: 1" (a web page on another site cannot send it: no cross-site writes)
//   GET  /api/catalog                the host's /library.json: EVERY game Markus owns on every system (blazingmount host/library)
//   GET  /api/covers.json            where each cover sits in the sprite sheets (page, cell, colour)
//   GET  /api/cover/<thumb|full|sheet|dsheet>/<id>.jpg   a cover from the host (passed through, nothing kept on the console)
//   GET  /api/discover, /api/wishlist   DISCOVER: the host's /discover.json and /wishlist.json, passed through
//   POST /api/action?do=wish&id=<12 hex>&value=1|0   add / remove a Discover game on the ONE wishlist on mac1: forwarded at once to
//        the host's POST /wishlist (the only thing this API sends; no console file is touched). Each pass also turns the host's
//        Discover alerts into ONE PS5 notice each (bm_api_alerts, main thread, /data/blazing/alerts-seen.tsv capped at 200 lines)
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

// ---------------------------------------------------------------- DISCOVER (docs/UI-DESIGN.md 10.5): the wishlist press + release alerts
static const char *js_str(const char *p, char *out, size_t on);   // below (the host's games)
// One request to the host with a method, an optional extra header line, and the answer's status, ETag and body (cap 16 MB). LAN first,
// then the tsnet proxy (absolute-form), like api_get. Used by the wishlist forward (API thread) and the alert check (main thread):
// it has no shared buffers, so both may call it.
static int api_http(const conf_t *c, const char *method, const char *path_query, const char *extra, char **body, size_t *blen,
                    char *etag, size_t en, int timeout_s) {
    *body = NULL; if (blen) *blen = 0; if (etag && en) etag[0] = 0;
    int s = connect_ms(c->host, c->port, LAN_CONNECT_MS), px = 0;
    if (s < 0 && g_px_port && g_ts_port) { s = connect_ms(g_px_host, g_px_port, timeout_s * 1000); px = 1; }
    if (s < 0) return -1;
    struct timeval tv = { timeout_s, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char req[1600];
    int rl = px ? snprintf(req, sizeof req, "%s http://%s:%d%s HTTP/1.0\r\nHost: %s:%d\r\n%s%sConnection: close\r\n\r\n", method, g_ts_host, g_ts_port,
                           path_query, g_ts_host, g_ts_port, extra ? extra : "", strcmp(method, "POST") ? "" : "Content-Length: 0\r\n")
                : snprintf(req, sizeof req, "%s %s HTTP/1.0\r\nHost: %s\r\n%s%sConnection: close\r\n\r\n", method, path_query, c->host,
                           extra ? extra : "", strcmp(method, "POST") ? "" : "Content-Length: 0\r\n");
    if (rl <= 0 || rl >= (int)sizeof req || write_all(s, req, (size_t)rl) != 0) { close(s); return -1; }
    sb_t b = { 0 }; char chunk[16384];
    for (;;) { ssize_t n = read(s, chunk, sizeof chunk); if (n <= 0) break; sb_put(&b, chunk, (size_t)n); if (b.n > (16u << 20)) break; }
    close(s);
    if (!b.b) return -1;
    int status = (b.n > 12 && !strncmp(b.b, "HTTP/1.", 7)) ? atoi(b.b + 9) : 0;
    char *hend = strstr(b.b, "\r\n\r\n");
    if (!hend) { free(b.b); return status ? status : -1; }
    *hend = 0;
    if (etag && en) { char *e = strcasestr(b.b, "\nETag: \""); if (e) sscanf(e + 8, "%63[^\"]", etag); if (en < 64) etag[en - 1] = 0; }
    size_t bl = b.n - (size_t)(hend + 4 - b.b);
    memmove(b.b, hend + 4, bl); b.b[bl] = 0;
    *body = b.b; if (blen) *blen = bl;
    return status;
}

static int valid_did(const char *t) {                           // a Discover game id: 12 lower-case hex (sha1 of "igdb:<n>")
    if (strlen(t) != 12) return 0;
    for (int i = 0; i < 12; i++) if (!((t[i] >= '0' && t[i] <= '9') || (t[i] >= 'a' && t[i] <= 'f'))) return 0;
    return 1;
}

// the end of the JSON value that starts at p ('{' or '['), strings and escapes respected; NULL when it does not end
static const char *js_end(const char *p) {
    int depth = 0, str = 0;
    for (; *p; p++) {
        if (str) { if (*p == '\\' && p[1]) p++; else if (*p == '"') str = 0; continue; }
        if (*p == '"') str = 1;
        else if (*p == '{' || *p == '[') depth++;
        else if ((*p == '}' || *p == ']') && --depth == 0) return p;
    }
    return NULL;
}
// a string field of ONE flat object [obj, end]: "name":"value" (decoded); 1 when found
static int js_field(const char *obj, const char *end, const char *name, char *out, size_t on) {
    char pat[48]; snprintf(pat, sizeof pat, "\"%s\":", name);
    out[0] = 0;
    for (const char *p = obj; p && p < end; ) {
        const char *q = strstr(p, pat);
        if (!q || q >= end) return 0;
        q += strlen(pat); while (*q == ' ') q++;
        if (*q == '"') { js_str(q, out, on); return 1; }
        p = q;
    }
    return 0;
}

// PS5 notice (the TV toast). On the host build it is only logged, so the fake-console test can count them.
#ifdef __PROSPERO__
typedef struct { char useless1[45]; char message[3075]; } bm_notify_t;
int sceKernelSendNotificationRequest(int, bm_notify_t *, size_t, int);
#endif
static void bm_notice(const char *msg) {
#ifdef __PROSPERO__
    bm_notify_t n; memset(&n, 0, sizeof n); snprintf(n.message, sizeof n.message, "%s", msg);
    sceKernelSendNotificationRequest(0, &n, sizeof n, 0);
#endif
    logf_("notice: %s", msg);
}

#define ALERTS_SEEN "/data/blazing/alerts-seen.tsv"
#define ALERTS_KEEP 200
static char g_disc_etag[64];                         // the host's /discover.json ETag (main thread writes, under g_api_mu)

// main thread, every pass: the host's Discover alerts (a wishlisted game is out, or its date moved) -> ONE PS5 notice each, ever.
// A conditional GET: the file is downloaded only when its ETag moved (once a day, or when the wishlist changed).
static void bm_api_alerts(const conf_t *c) {
    char cur[64], hdr[120] = "", etag[64] = "";
    pthread_mutex_lock(&g_api_mu); snprintf(cur, sizeof cur, "%s", g_disc_etag); pthread_mutex_unlock(&g_api_mu);
    if (cur[0]) snprintf(hdr, sizeof hdr, "If-None-Match: \"%s\"\r\n", cur);
    char *m = NULL; size_t n = 0;
    int st = api_http(c, "GET", "/discover.json", hdr, &m, &n, etag, sizeof etag, 15);
    if (st != 200 || !m) { free(m); return; }                    // 304 (unchanged), 404 (not made yet), or the host is away
    const char *a = strstr(m, "\"alerts\":");                     // "alerts": [ ... ] (any spacing)
    if (a) { a += 9; while (*a == ' ' || *a == '\t' || *a == '\n' || *a == '\r') a++; if (*a != '[') a = NULL; }
    const char *ae = a ? js_end(a) : NULL;
    char sp[600]; P(sp, sizeof sp, ALERTS_SEEN);
    size_t sl = 0; char *seen = api_slurp(sp, 256 * 1024, &sl);
    sb_t add = { 0 }; int fresh = 0;
    for (const char *p = a ? a + 1 : NULL; p && ae && p < ae; ) {
        const char *o = strchr(p, '{'); if (!o || o >= ae) break;
        const char *oe = js_end(o); if (!oe || oe > ae) break;
        char key[96], kind[16], title[300], date[48];
        js_field(o, oe, "key", key, sizeof key); js_field(o, oe, "kind", kind, sizeof kind);
        js_field(o, oe, "title", title, sizeof title); js_field(o, oe, "date_text", date, sizeof date);
        p = oe + 1;
        if (!key[0] || strchr(key, '\n') || strchr(key, '\t')) continue;
        char line[100]; snprintf(line, sizeof line, "%s\n", key);
        int known = 0;
        if (seen) for (const char *q = seen; (q = strstr(q, key)); q += strlen(key)) if ((q == seen || q[-1] == '\n') && q[strlen(key)] == '\n') { known = 1; break; }
        if (!known && add.b) for (const char *q = add.b; (q = strstr(q, key)); q += strlen(key)) if ((q == add.b || q[-1] == '\n') && q[strlen(key)] == '\n') { known = 1; break; }
        if (known) continue;
        char msg[420];
        if (!strcmp(kind, "out")) snprintf(msg, sizeof msg, "Out now: %s (on your wishlist)", title);
        else if (!strcmp(kind, "moved")) snprintf(msg, sizeof msg, "New date for %s: %s (wishlist)", title, date);
        else continue;
        bm_notice(msg);
        sb_puts(&add, line); fresh++;
    }
    if (fresh) {                                                   // seen = old + new, only the last ALERTS_KEEP lines (it never grows)
        sb_t all = { 0 };
        if (seen) sb_puts(&all, seen);
        sb_puts(&all, add.b);
        int lines = 0; for (size_t i = 0; i < all.n; i++) lines += all.b[i] == '\n';
        const char *from = all.b;
        while (lines > ALERTS_KEEP) { from = strchr(from, '\n') + 1; lines--; }
        char dp[600], t[640]; makedirs(P(dp, sizeof dp, "/data/blazing")); snprintf(t, sizeof t, "%s.tmp", sp);
        if (write_file(t, from, strlen(from)) == 0) rename(t, sp);
        free(all.b);
    }
    free(add.b); free(seen); free(m);
    if (etag[0]) { pthread_mutex_lock(&g_api_mu); if (strcmp(g_disc_etag, etag)) { snprintf(g_disc_etag, sizeof g_disc_etag, "%s", etag); g_gen++; } pthread_mutex_unlock(&g_api_mu); }
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

// ---------------------------------------------------------------- what this console has: one row per copy found
// v2.8 (docs/DOWNLOAD-TO-CONSOLE.md 4.1): besides the homebrew folders, a console can hold a game as a real pkg install
// (/user/app/<TID>/app.pkg: Wolverine's 101.6 GB sat under its starter on PHAT5) or as a parked copy (blz-stage/parked).
// The ACTIVE copy is the one ShadowMount launches: a homebrew entry beats a pkg install; a parked copy is never active.
typedef struct {
    char tid[16], kind[10];   // kind: starter | full | image | pkg | parked
    char path[700];           // console path (no BM_ROOT prefix)
    char key[48];             // starter: the image key its cfg streams from ("base /t/<key>")
    char fmt[12];             // image: the file's extension
    long long bytes;          // image / parked file: file size; pkg: app.pkg size
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
            if (S_ISDIR(st.st_mode)) {                                // a game folder has an eboot.bin; a DLC pkg drop
                char eb[1500]; snprintf(eb, sizeof eb, "%s/eboot.bin", full);   // ("FC 27 DLC (PPSA34015)") or a backport
                if (!exists(eb)) continue;                            // overlay ("PPSA25642-8xx") is no copy of the game
            }
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
    // real pkg installs: <root>/<TID>/app.pkg with bytes in it (ShadowMount's own titles keep only mount.lnk + sce_sys there)
    static const char *pkgs[] = { "/user/app", "/mnt/ext0/user/app", "/mnt/ext1/user/app", 0 };
    for (int r = 0; pkgs[r] && nl < max; r++) {
        char rp[600]; DIR *d = opendir(P(rp, sizeof rp, pkgs[r]));
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) && nl < max) {
            if (!valid_tid(e->d_name)) continue;
            char ap[1400]; snprintf(ap, sizeof ap, "%s/%s/app.pkg", rp, e->d_name);
            struct stat st; if (stat(ap, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
            local_t *L = &out[nl++]; memset(L, 0, sizeof *L);
            snprintf(L->tid, sizeof L->tid, "%s", e->d_name); snprintf(L->kind, sizeof L->kind, "pkg");
            snprintf(L->path, sizeof L->path, "%s/%s", pkgs[r], e->d_name); snprintf(L->fmt, sizeof L->fmt, "pkg");
            L->bytes = (long long)st.st_size;
        }
        closedir(d);
    }
    // parked copies (phat5-games convert, a download's undo): on the console, never launched
    static const char *parked[] = { "/data/blz-stage/parked", "/mnt/ext0/blz-stage/parked", "/mnt/ext1/blz-stage/parked", "/mnt/usb0/blz-stage/parked",
                                    "/mnt/usb1/blz-stage/parked", "/data/blz-parked", 0 };
    for (int r = 0; parked[r] && nl < max; r++) {
        char rp[600]; DIR *d = opendir(P(rp, sizeof rp, parked[r]));
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) && nl < max) {
            char tid[16] = "";
            if (e->d_name[0] == '.' || !first_tid(e->d_name, tid)) continue;
            char full[1400]; snprintf(full, sizeof full, "%s/%s", rp, e->d_name);
            struct stat st; if (stat(full, &st) != 0) continue;
            local_t *L = &out[nl++]; memset(L, 0, sizeof *L);
            snprintf(L->tid, sizeof L->tid, "%s", tid); snprintf(L->kind, sizeof L->kind, "parked");
            snprintf(L->path, sizeof L->path, "%s/%s", parked[r], e->d_name);
            if (S_ISREG(st.st_mode)) { L->bytes = (long long)st.st_size; snprintf(L->fmt, sizeof L->fmt, "%s", ext_of(e->d_name)); }
        }
        closedir(d);
    }
    return nl;
}

// the copy ShadowMount launches for tid: a homebrew entry (starter / full / image) first, else a pkg install; parked never
static local_t *active_copy(local_t *lo, int n, const char *tid) {
    local_t *pkg = NULL;
    for (int j = 0; j < n; j++) {
        if (strcmp(lo[j].tid, tid)) continue;
        if (!strcmp(lo[j].kind, "pkg")) { if (!pkg) pkg = &lo[j]; }
        else if (strcmp(lo[j].kind, "parked")) return &lo[j];
    }
    return pkg;
}
static int plays_offline(const local_t *L) { return L && (!strcmp(L->kind, "full") || !strcmp(L->kind, "image") || !strcmp(L->kind, "pkg")); }

// ---------------------------------------------------------------- downloads (docs/DOWNLOAD-TO-CONSOLE.md 3): two small TSV files
// downloads.tsv = the app's requests, written ONLY by the main loop (bm_api_pass_begin). dl-status.tsv = mac1's progress, written
// ONLY by mac1's download-to-console.py over FTP; the payload never writes it. Both are read here by the API thread.
#define DL_REQ_PATH    "/data/blazing/downloads.tsv"
#define DL_STATUS_PATH "/data/blazing/dl-status.tsv"
#define DL_REQ_MAX     256
#define DL_FIELDS      8
typedef struct { char state[16]; long long done, total, eta, at; char msg[256]; } dljob_t;

static int tsv_split(const char *l, size_t L, char f[][256], int maxf) {   // one line -> up to maxf tab fields (each cut at 255)
    int k = 0; size_t o = 0;
    if (maxf < 1) return 0;
    f[0][0] = 0;
    for (size_t i = 0; i < L; i++) {
        if (l[i] == '\t') { f[k][o] = 0; if (++k >= maxf) return k; o = 0; f[k][0] = 0; continue; }
        if (l[i] == '\r') continue;
        if (o < 255) f[k][o++] = l[i];
    }
    f[k][o] = 0;
    return k + 1;
}

// the LAST line of m whose field 0 is w0 (NULL = any) and field 1 is tid (NULL = any); 0 when none, else its field count
static int tsv_last(const char *m, const char *w0, const char *tid, char f[][256]) {
    int got = 0; char t[DL_FIELDS][256];
    for (const char *l = m; l && *l; ) {
        const char *nl = strchr(l, '\n'); size_t L = nl ? (size_t)(nl - l) : strlen(l);
        if (L && l[0] != '#') {
            int k = tsv_split(l, L, t, DL_FIELDS);
            if (k >= 2 && (!w0 || !strcmp(t[0], w0)) && (!tid || !strcmp(t[1], tid))) { memcpy(f, t, sizeof t); got = k; }
        }
        l = nl ? nl + 1 : NULL;
    }
    return got;
}

static int dl_job(const char *st, const char *tid, dljob_t *j) {     // this title's "job" line in dl-status.tsv
    memset(j, 0, sizeof *j);
    char f[DL_FIELDS][256];
    if (!st || tsv_last(st, "job", tid, f) < 8) return 0;
    snprintf(j->state, sizeof j->state, "%s", f[2]);
    j->done = atoll(f[3]); j->total = atoll(f[4]); j->eta = atoll(f[5]); j->at = atoll(f[6]);
    snprintf(j->msg, sizeof j->msg, "%s", f[7]);
    return 1;
}

static int dl_job_active(const char *state) {
    static const char *on[] = { "queued", "waiting", "sending", "verifying", "switching", "removing", 0 };
    for (int i = 0; on[i]; i++) if (!strcmp(state, on[i])) return 1;
    return 0;
}

static void dl_request(const char *rq, const char *tid, char *w, size_t wn, long long *at) {   // the app's newest request for tid
    char f[DL_FIELDS][256];
    w[0] = 0; *at = 0;
    if (rq && tsv_last(rq, NULL, tid, f) >= 3) { snprintf(w, wn, "%s", f[0]); *at = atoll(f[2]); }
}

typedef struct { long long rate, rate_at, worker_at; char route[16]; int active; } dlsum_t;
static void dl_summary(const char *st, dlsum_t *s) {
    memset(s, 0, sizeof *s);
    if (!st) return;
    char f[DL_FIELDS][256];
    if (tsv_last(st, "rate", NULL, f) >= 4) { s->rate = atoll(f[1]); s->rate_at = atoll(f[2]); snprintf(s->route, sizeof s->route, "%s", f[3]); }
    if (tsv_last(st, "worker", NULL, f) >= 2) s->worker_at = atoll(f[1]);
    for (const char *l = st; l && *l; ) {                          // each title once: its LAST job line is its state
        const char *nl = strchr(l, '\n'); size_t L = nl ? (size_t)(nl - l) : strlen(l);
        if (L > 4 && !strncmp(l, "job\t", 4) && tsv_split(l, L, f, DL_FIELDS) >= 2) {
            char pre[24]; snprintf(pre, sizeof pre, "job\t%.9s\t", f[1]);
            const char *first = strstr(st, pre);                   // count a title at its first line only
            while (first && first != st && first[-1] != '\n') first = strstr(first + 1, pre);
            dljob_t j;
            if (first == l && dl_job(st, f[1], &j) && dl_job_active(j.state)) s->active++;
        }
        l = nl ? nl + 1 : NULL;
    }
}

static char *dl_slurp(const char *path) { char p[600]; return api_slurp(P(p, sizeof p, path), 64 * 1024, NULL); }

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

// v2.8 fields of one library row (docs/DOWNLOAD-TO-CONSOLE.md 4.2): where it plays from, every copy, the app's request, mac1's progress
static void api_row_dl(sb_t *o, const char *tid, const local_t *L, const char *rq, const char *st) {
    sb_f(o, ",\"on_console\":%s,\"local_bytes\":%lld,\"copies\":[", plays_offline(L) ? "true" : "false", L ? L->bytes : 0LL);
    int n = 0;
    for (int j = 0; j < g_nloc; j++) {
        if (strcmp(g_loc[j].tid, tid)) continue;
        sb_puts(o, n++ ? ",{\"kind\":" : "{\"kind\":"); sb_js(o, g_loc[j].kind);
        sb_puts(o, ",\"path\":"); sb_js(o, g_loc[j].path); sb_f(o, ",\"bytes\":%lld}", g_loc[j].bytes);
    }
    char w[16]; long long at; dl_request(rq, tid, w, sizeof w, &at);
    sb_puts(o, "],\"request\":"); sb_js(o, w); sb_f(o, ",\"request_at\":%lld", at);
    dljob_t j; dl_job(st, tid, &j);
    sb_puts(o, ",\"dl_state\":"); sb_js(o, j.state);
    sb_f(o, ",\"dl_done\":%lld,\"dl_total\":%lld,\"dl_eta\":%lld,\"dl_at\":%lld,\"dl_msg\":", j.done, j.total, j.eta, j.at); sb_js(o, j.msg);
}

static void pkg_title(const char *tid, char *out, size_t on) {   // a pkg install's name from /user/app/<TID>/sce_sys/param.json, else ""
    out[0] = 0;
    char rp[600], pj[700]; snprintf(pj, sizeof pj, "%s/%s/sce_sys/param.json", P(rp, sizeof rp, "/user/app"), tid);
    char *m = api_slurp(pj, 1 << 18, NULL); if (!m) return;
    char *t = strstr(m, "\"titleName\""); if (t) { t = strchr(t + 11, '"'); if (t) js_str(t, out, on); }
    free(m);
}

static void api_library(sb_t *o) {
    conf_t c; read_conf(&c);
    int host_ok = host_games(&c, 10) == 0;
    host_proven(&c);
    g_nloc = scan_local(&c, g_loc, MAX_LOCAL);
    char *rq = dl_slurp(DL_REQ_PATH), *dst = dl_slurp(DL_STATUS_PATH);
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
        for (int j = 0; j < g_nloc; j++) if (!strcmp(g_loc[j].tid, g->tid)) seen[j] = 1;
        local_t *L = active_copy(g_loc, g_nloc, g->tid);
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
        sb_f(o, ",\"pinned\":%s", L && L->pinned ? "true" : "false");
        api_row_dl(o, g->tid, L, rq, dst);
        sb_puts(o, "}");
    }
    for (int j = 0; j < g_nloc; j++) {                            // on this console, not (or no longer) on the drives
        if (seen[j]) continue;
        int dupe = 0; for (int k = 0; k < j; k++) if (!strcmp(g_loc[k].tid, g_loc[j].tid)) dupe = 1;
        if (dupe) continue;
        local_t *L = active_copy(g_loc, g_nloc, g_loc[j].tid);
        if (!L) continue;                                         // only parked: no row of its own (it shows in a row's copies)
        const char *nm = strrchr(L->path, '/'); nm = nm ? nm + 1 : L->path;
        char title[300]; snprintf(title, sizeof title, "%s", nm);
        if (!strcmp(L->kind, "pkg")) pkg_title(L->tid, title, sizeof title);     // its own name; the folder name is only the title id
        if (!title[0]) snprintf(title, sizeof title, "%s", L->tid);
        else if (strcmp(L->kind, "pkg")) {
            char *paren = strstr(title, " ("); if (paren) *paren = 0;
            char *dot = strrchr(title, '.'); if (dot && !strcmp(L->kind, "image")) *dot = 0;
        }
        char proof[300]; proven_line(L->tid, proof, sizeof proof);
        if (!first) sb_puts(o, ","); first = 0;
        sb_puts(o, "{\"tid\":"); sb_js(o, L->tid); sb_puts(o, ",\"title\":"); sb_js(o, title);
        sb_f(o, ",\"platform\":\"%s\",\"key\":\"\",\"version\":\"\",\"fw\":\"\",\"size\":%lld,\"format\":", !strncmp(L->tid, "CUSA", 4) ? "PS4" : "PS5", L->bytes);
        sb_js(o, !strcmp(L->kind, "image") || !strcmp(L->kind, "pkg") ? L->fmt : "folder");
        sb_f(o, ",\"compressed\":%s,\"on_drives\":false,\"file\":\"\",\"local\":", !strcasecmp(L->fmt, "ffpfsc") ? "true" : "false");
        sb_js(o, L->kind); sb_puts(o, ",\"local_path\":"); sb_js(o, L->path);
        // a starter whose image left the drives cannot stream any more; a full copy or image plays on its own
        sb_puts(o, ",\"state\":"); sb_js(o, !strcmp(L->kind, "starter") ? "orphan" : "installed");
        sb_puts(o, ",\"choice\":"); sb_js(o, choice_of(ch, L->tid));
        sb_puts(o, ",\"build\":\"\",\"tries\":0,\"reason\":\"\",\"proof\":"); sb_js(o, proof);
        sb_f(o, ",\"pinned\":%s", L->pinned ? "true" : "false");
        api_row_dl(o, L->tid, L, rq, dst);
        sb_puts(o, "}");
    }
    sb_puts(o, "]}");
    free(stm); free(ch); free(rq); free(dst);
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
    pthread_mutex_lock(&g_api_mu); sb_puts(o, ",\"discover_etag\":"); sb_js(o, g_disc_etag); pthread_mutex_unlock(&g_api_mu);
    sb_f(o, ",\"free_mb\":{\"data\":%lld,\"ext0\":%lld,\"ext1\":%lld,\"usb0\":%lld},\"min_free_mb\":%lld,\"fork\":\"%.16s\"",
         dir_free("/data"), dir_free("/mnt/ext0"), dir_free("/mnt/ext1"), dir_free("/mnt/usb0"), c.min_free_mb, BM_FORK_SHA);
    char *st = dl_slurp(DL_STATUS_PATH); dlsum_t d; dl_summary(st, &d); free(st);   // mac1's download helper (DOWNLOAD-TO-CONSOLE.md 4.3)
    sb_f(o, ",\"dl_rate\":%lld,\"dl_rate_at\":%lld,\"dl_worker_at\":%lld,\"dl_active\":%d}", d.rate, d.rate_at, d.worker_at, d.active);
}

// GET /api/downloads: what mac1's worker polls (every 20 s per console, so it stays small): the app's requests, mac1's own last
// status as this console has it, and the space and route the worker plans with
static void api_downloads(sb_t *o) {
    conf_t c; read_conf(&c);
    char *rq = dl_slurp(DL_REQ_PATH), *st = dl_slurp(DL_STATUS_PATH);
    sb_f(o, "{\"version\":\"%s\",\"now\":%lld,\"requests\":[", BM_VERSION, (long long)time(NULL));
    int n = 0; char f[DL_FIELDS][256];
    for (const char *l = rq; l && *l; ) {
        const char *nl = strchr(l, '\n'); size_t L = nl ? (size_t)(nl - l) : strlen(l);
        if (L && l[0] != '#' && tsv_split(l, L, f, DL_FIELDS) >= 3 && valid_tid(f[1])) {
            sb_puts(o, n++ ? ",{\"what\":" : "{\"what\":"); sb_js(o, f[0]); sb_puts(o, ",\"tid\":"); sb_js(o, f[1]);
            sb_f(o, ",\"at\":%lld}", atoll(f[2]));
        }
        l = nl ? nl + 1 : NULL;
    }
    dlsum_t d; dl_summary(st, &d);
    sb_f(o, "],\"worker_at\":%lld,\"rate\":%lld,\"rate_at\":%lld,\"rate_route\":", d.worker_at, d.rate, d.rate_at); sb_js(o, d.route);
    sb_puts(o, ",\"jobs\":[");
    n = 0;
    for (const char *l = st; l && *l; ) {                          // each title's last job line
        const char *nl = strchr(l, '\n'); size_t L = nl ? (size_t)(nl - l) : strlen(l);
        if (L > 4 && !strncmp(l, "job\t", 4) && tsv_split(l, L, f, DL_FIELDS) >= 2) {
            char pre[24]; snprintf(pre, sizeof pre, "job\t%.9s\t", f[1]);
            const char *first = strstr(st, pre);
            while (first && first != st && first[-1] != '\n') first = strstr(first + 1, pre);
            dljob_t j;
            if (first == l && dl_job(st, f[1], &j)) {
                sb_puts(o, n++ ? ",{\"tid\":" : "{\"tid\":"); sb_js(o, f[1]); sb_puts(o, ",\"state\":"); sb_js(o, j.state);
                sb_f(o, ",\"done\":%lld,\"total\":%lld,\"eta\":%lld,\"at\":%lld,\"msg\":", j.done, j.total, j.eta, j.at); sb_js(o, j.msg);
                sb_puts(o, "}");
            }
        }
        l = nl ? nl + 1 : NULL;
    }
    sb_f(o, "],\"free_mb\":{\"data\":%lld,\"ext0\":%lld,\"ext1\":%lld,\"usb0\":%lld},\"min_free_mb\":%lld,\"homebrew_dir\":",
         dir_free("/data"), dir_free("/mnt/ext0"), dir_free("/mnt/ext1"), dir_free("/mnt/usb0"), c.min_free_mb);
    sb_js(o, c.homebrew_dir);
    sb_f(o, ",\"route\":\"%s\"}", g_route ? "TAILNET" : "LAN");
    free(rq); free(st);
}

// ---------------------------------------------------------------- GET /api/sha256: hash a range of a STAGED file on the console
// mac1 sends a game image in 1 GiB pieces and compares each against this, so a hotspot never carries the image twice
// (DOWNLOAD-TO-CONSOLE.md 5.3). Only files under <drive>/blz-stage/incoming/ can be read, at most 1 GiB per call.
typedef struct { uint32_t h[8]; uint64_t n; unsigned char b[64]; size_t bl; } sha256_t;
static const uint32_t k_sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
#define ROR32(x, r) (((x) >> (r)) | ((x) << (32 - (r))))
static void sha256_block(sha256_t *s, const unsigned char *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + k_sha_k[i] + w[i];
        uint32_t t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void sha256_init(sha256_t *s) {
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof iv); s->n = 0; s->bl = 0;
}
static void sha256_update(sha256_t *s, const unsigned char *p, size_t n) {
    s->n += n;
    if (s->bl) { size_t t = 64 - s->bl < n ? 64 - s->bl : n; memcpy(s->b + s->bl, p, t); s->bl += t; p += t; n -= t; if (s->bl == 64) { sha256_block(s, s->b); s->bl = 0; } }
    while (n >= 64) { sha256_block(s, p); p += 64; n -= 64; }
    if (n) { memcpy(s->b, p, n); s->bl = n; }
}
static void sha256_hex(sha256_t *s, char out[65]) {
    uint64_t bits = s->n * 8; unsigned char pad[72] = { 0x80 };
    size_t pl = (s->bl < 56 ? 56 : 120) - s->bl;
    for (int i = 0; i < 8; i++) pad[pl + i] = (unsigned char)(bits >> (56 - 8 * i));
    uint64_t keep = s->n; sha256_update(s, pad, pl + 8); s->n = keep;
    for (int i = 0; i < 8; i++) snprintf(out + 8 * i, 9, "%08x", s->h[i]);
}

#define SHA_MAX_LEN (1LL << 30)
static int sha_path_ok(const char *p) {
    static const char *ok[] = { "/data/blz-stage/incoming/", "/mnt/ext0/blz-stage/incoming/", "/mnt/ext1/blz-stage/incoming/",
                                "/mnt/usb0/blz-stage/incoming/", "/mnt/usb1/blz-stage/incoming/", 0 };
    int in = 0;
    for (int i = 0; ok[i]; i++) if (!strncmp(p, ok[i], strlen(ok[i])) && p[strlen(ok[i])]) in = 1;
    if (!in || strstr(p, "..") || strstr(p, "//") || strstr(p, "/./")) return 0;
    for (const unsigned char *q = (const unsigned char *)p; *q; q++) if (*q < 0x20 || *q == 0x7f) return 0;
    return 1;
}
static int api_sha256(sb_t *o, const char *query) {                 // the HTTP status; the JSON answer in o
    char path[700] = "", v[32];
    get_q(query, "path", path, sizeof path);
    long long off = get_q(query, "off", v, sizeof v) ? atoll(v) : 0, len = get_q(query, "len", v, sizeof v) ? atoll(v) : -1;
    if (!path[0] || off < 0 || (len < -1)) { sb_puts(o, "{\"error\":\"path, off and len (bytes) please\"}"); return 400; }
    if (!sha_path_ok(path)) { sb_puts(o, "{\"error\":\"only files under <drive>/blz-stage/incoming/ can be hashed\"}"); return 403; }
    char fp[800]; int fd = open(P(fp, sizeof fp, path), O_RDONLY);
    struct stat sb;
    if (fd < 0 || fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode)) { if (fd >= 0) close(fd); sb_puts(o, "{\"error\":\"no such file\"}"); return 404; }
    long long size = (long long)sb.st_size;
    if (off > size) off = size;
    if (len < 0 || len > size - off) len = size - off;
    if (len > SHA_MAX_LEN) len = SHA_MAX_LEN;
    size_t bufn = 1 << 20; unsigned char *buf = malloc(bufn);       // heap: the API thread's stack is 1 MB
    if (!buf) { close(fd); sb_puts(o, "{\"error\":\"out of memory\"}"); return 500; }
    sha256_t s; sha256_init(&s);
    long long done = 0;
    while (done < len) {
        size_t want = (size_t)(len - done < (long long)bufn ? len - done : (long long)bufn);
        ssize_t n = pread(fd, buf, want, (off_t)(off + done));
        if (n <= 0) break;
        sha256_update(&s, buf, (size_t)n); done += n;
    }
    free(buf); close(fd);
    if (done != len) { sb_puts(o, "{\"error\":\"read failed\"}"); return 500; }
    char hex[65]; sha256_hex(&s, hex);
    sb_puts(o, "{\"path\":"); sb_js(o, path);
    sb_f(o, ",\"off\":%lld,\"len\":%lld,\"size\":%lld,\"sha256\":\"%s\"}", off, len, size, hex);
    return 200;
}

#include "bm_pull.c"               // v2.9: the console pulls a file from the host itself (POST/GET /api/pull)

// ---------------------------------------------------------------- v2.9: install a PKG straight from the drives (1x storage)
// Markus 26 Oct 2026: PlayBridge's "the console installs directly from your PC, no double storage". PKG Manager (our fork,
// pkgmgr-blazing, GPL-3.0) already does exactly that on both consoles: it hands Sony's installer a 127.0.0.1 stream URL and feeds
// it Range reads from an SMB share (mac1-8TB is one of its sources since 26 Oct), with live status, cancel, and split PKGs. So
// BlazingMount does not install anything itself: the app asks this API and it passes the call to PKG Manager on this console.
//   GET  /api/pm/drives | /api/pm/packages?drive=<id> | /api/pm/queue | /api/pm/status     -> PKG Manager's own answer
//   POST /api/pm/install?path=<smb://...pkg>  | /api/pm/cancel?id=<job>  | /api/pm/refresh   (header X-BM-Request: 1)
#ifndef BM_PM_PORT
#define BM_PM_PORT 8844
#endif
static int local_http(int port, const char *method, const char *path, const char *body, sb_t *out) {   // HTTP status, or -1
    int s = connect_ms("127.0.0.1", port, 2000);
    if (s < 0) return -1;
    struct timeval tv = { 20, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char h[1400]; size_t bl = body ? strlen(body) : 0;
    int hl = snprintf(h, sizeof h, "%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                      method, path, bl);
    if (hl >= (int)sizeof h || write_all(s, h, (size_t)hl) != 0 || (bl && write_all(s, body, bl) != 0)) { close(s); return -1; }
    sb_t r = { 0 }; char buf[16384]; ssize_t n;
    while ((n = read(s, buf, sizeof buf)) > 0) sb_put(&r, buf, (size_t)n);
    close(s);
    char *e = r.b ? strstr(r.b, "\r\n\r\n") : NULL;
    int st = (r.n > 12 && !strncmp(r.b, "HTTP/1.", 7)) ? atoi(r.b + 9) : -1;
    if (e && st > 0) sb_put(out, e + 4, r.n - (size_t)(e + 4 - r.b));
    free(r.b);
    return e ? st : -1;
}
static int pm_http(const char *method, const char *path, const char *body, sb_t *out) {   // PKG Manager's HTTP status, or -1
    return local_http(BM_PM_PORT, method, path, body, out);
}

static void reply_json(int s, int code, sb_t *b);
static void api_pm(int s, const char *method, const char *target, const char *query, const char *req) {
    sb_t b = { 0 }; int st = -1;
    const char *what = target + 8;                                   // after "/api/pm/"
    char v[700], body[1600], path[300];
    if (!strcmp(method, "GET") && (!strcmp(what, "drives") || !strcmp(what, "queue") || !strcmp(what, "status") || !strcmp(what, "packages"))) {
        if (!strcmp(what, "packages") && get_q(query, "drive", v, sizeof v)) {
            char enc[400]; url_path(enc, sizeof enc, v); snprintf(path, sizeof path, "/api/packages?drive=%s", enc);
        } else snprintf(path, sizeof path, "/api/%s", what);
        st = pm_http("GET", path, NULL, &b);
    } else if (!strcmp(method, "POST") && (!strcmp(what, "install") || !strcmp(what, "cancel") || !strcmp(what, "refresh"))) {
        if (!strcasestr(req, "\nX-BM-Request: 1")) { sb_puts(&b, "{\"ok\":false,\"error\":\"missing X-BM-Request header\"}"); reply_json(s, 403, &b); free(b.b); return; }
        if (!strcmp(what, "install")) {
            if (!get_q(query, "path", v, sizeof v) || strncmp(v, "smb://", 6) || !ends_with(v, ".pkg", 1)) {
                sb_puts(&b, "{\"ok\":false,\"error\":\"path must be an smb://.../<file>.pkg from /api/pm/packages\"}"); reply_json(s, 400, &b); free(b.b); return;
            }
            sb_t j = { 0 }; sb_puts(&j, "{\"path\":"); sb_js(&j, v); sb_puts(&j, "}");
            snprintf(body, sizeof body, "%s", j.b ? j.b : "{}"); free(j.b);
            st = pm_http("POST", "/api/install", body, &b);
            logf_("ui: install from the drives (PKG Manager) %s -> HTTP %d", v, st);
        } else if (!strcmp(what, "cancel")) {
            snprintf(body, sizeof body, "{\"id\":%llu}", get_q(query, "id", v, sizeof v) ? strtoull(v, NULL, 10) : 0ULL);
            st = pm_http("POST", "/api/queue/cancel", body, &b);
            logf_("ui: cancel install job %s (PKG Manager) -> HTTP %d", v, st);
        } else st = pm_http("POST", "/api/packages/refresh", "{}", &b);
    } else { sb_puts(&b, "{\"error\":\"not found\"}"); reply_json(s, 404, &b); free(b.b); return; }
    if (st < 0) { free(b.b); b.b = NULL; b.n = 0; sb_puts(&b, "{\"ok\":false,\"error\":\"PKG Manager is not running on this console (:8844)\"}"); reply_json(s, 502, &b); }
    else reply_json(s, st, &b);
    free(b.b);
}

// Markus 27 Oct 2026: BlazingMount calls his ShadowMount+, never replaces it. SMP's API listens on 127.0.0.1 only (:10101, every
// call a JSON POST), so the app and the Macs reach it through this payload:
//   POST /api/smp/version                    -> SMP /api/v1/version
//   POST /api/smp/uninstall?tid=<TITLE ID>   -> SMP /api/v1/games/uninstall {"title_id"} (sceAppInstUtilAppUnInstall; SMP refuses
//                                               a running game itself)                     (both need header X-BM-Request: 1)
#ifndef BM_SMP_PORT
#define BM_SMP_PORT 10101
#endif
static void api_smp(int s, const char *method, const char *target, const char *query, const char *req) {
    sb_t b = { 0 }; int st;
    const char *what = target + 9;                                   // after "/api/smp/"
    char tid[16] = "", body[64];
    if (strcmp(method, "POST") || (strcmp(what, "version") && strcmp(what, "uninstall"))) { sb_puts(&b, "{\"error\":\"not found\"}"); reply_json(s, 404, &b); free(b.b); return; }
    if (!strcasestr(req, "\nX-BM-Request: 1")) { sb_puts(&b, "{\"ok\":false,\"error\":\"missing X-BM-Request header\"}"); reply_json(s, 403, &b); free(b.b); return; }
    if (!strcmp(what, "version")) st = local_http(BM_SMP_PORT, "POST", "/api/v1/version", "{}", &b);
    else {
        if (!get_q(query, "tid", tid, sizeof tid) || !valid_tid(tid)) { sb_puts(&b, "{\"ok\":false,\"error\":\"bad title id\"}"); reply_json(s, 400, &b); free(b.b); return; }
        snprintf(body, sizeof body, "{\"title_id\":\"%s\"}", tid);
        st = local_http(BM_SMP_PORT, "POST", "/api/v1/games/uninstall", body, &b);
        logf_("ui: uninstall %s (ShadowMount+) -> HTTP %d %.200s", tid, st, b.b ? b.b : "");
    }
    if (st < 0) { free(b.b); b.b = NULL; b.n = 0; sb_puts(&b, "{\"ok\":false,\"error\":\"ShadowMount+ API is not answering on this console (:10101)\"}"); reply_json(s, 502, &b); }
    else reply_json(s, st, &b);
    free(b.b);
}

#include "bm_store.c"              // v2.9: the homebrew store (GET /api/store, POST /api/store/install)

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
    static const char *ok[] = { "install", "remove", "rebuild", "hide", "unhide", "sync", "auto", "set", "download", "remove_local", "cancel_dl", 0 };
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

// main thread: the app's download request for tid -> downloads.tsv, one line per title (the newest replaces the older), at most
// DL_REQ_MAX lines (the oldest go first). mac1's worker reads it through GET /api/downloads; nothing on the console acts on it.
static void downloads_set(const char *tid, const char *word) {
    char p[600]; char *m = api_slurp(P(p, sizeof p, DL_REQ_PATH), 64 * 1024, NULL);
    char (*keep)[300] = malloc(sizeof *keep * DL_REQ_MAX); int n = 0; char f[DL_FIELDS][256];
    if (!keep) { free(m); logf_("ui: downloads.tsv not written (out of memory)"); return; }
    for (const char *l = m; l && *l; ) {
        const char *nl = strchr(l, '\n'); size_t L = nl ? (size_t)(nl - l) : strlen(l);
        if (L && l[0] != '#' && tsv_split(l, L, f, DL_FIELDS) >= 3 && valid_tid(f[1]) && strcmp(f[1], tid)) {
            if (n == DL_REQ_MAX - 1) { memmove(keep[0], keep[1], sizeof keep[0] * (DL_REQ_MAX - 2)); n--; }
            snprintf(keep[n++], sizeof keep[0], "%.20s\t%s\t%lld", f[0], f[1], atoll(f[2]));
        }
        l = nl ? nl + 1 : NULL;
    }
    free(m);
    sb_t s = { 0 };
    sb_puts(&s, "# BlazingMount download requests (written by the payload; read by mac1's download-to-console.py)\n");
    for (int i = 0; i < n; i++) sb_f(&s, "%s\n", keep[i]);
    free(keep);
    sb_f(&s, "%s\t%s\t%lld\n", word, tid, (long long)time(NULL));
    char dp[600], t[640]; makedirs(P(dp, sizeof dp, "/data/blazing")); snprintf(t, sizeof t, "%s.tmp", p);
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
        } else if (!strcmp(a->what, "download") || !strcmp(a->what, "remove_local") || !strcmp(a->what, "cancel_dl")) {
            const char *word = !strcmp(a->what, "cancel_dl") ? "cancel" : a->what;
            downloads_set(a->tid, word);
            api_recent("%s %s: asked mac1's download helper", !strcmp(word, "download") ? "download" : !strcmp(word, "cancel") ? "cancel download of"
                       : "remove the local copy of", a->tid);
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
    bm_api_alerts(c);                                             // wishlist release alerts -> one PS5 notice each (DISCOVER)
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
    else if (!strcmp(method, "GET") && !strcmp(target, "/api/downloads")) { api_downloads(&b); reply_json(s, 200, &b); }
    else if (!strcmp(method, "GET") && !strcmp(target, "/api/sha256")) { int code = api_sha256(&b, query); reply_json(s, code, &b); }
    else if (!strcmp(method, "GET") && !strcmp(target, "/api/log")) {
        char v[16]; int n = get_q(query, "lines", v, sizeof v) ? atoi(v) : 200; if (n < 1) n = 1; if (n > 2000) n = 2000;
        api_log(&b, n); reply_json(s, 200, &b);
    }
    else if (!strcmp(method, "GET") && (!strcmp(target, "/api/catalog") || !strcmp(target, "/api/covers.json") || !strncmp(target, "/api/cover/", 11)
                                       || !strcmp(target, "/api/discover") || !strcmp(target, "/api/wishlist"))) {
        // passed through from the host (LAN, else the tsnet proxy); a cover name is checked so nothing else on the host can be asked for
        char hp[200] = ""; const char *ct = "application/json";
        if (!strcmp(target, "/api/catalog")) snprintf(hp, sizeof hp, "/library.json");
        else if (!strcmp(target, "/api/covers.json")) snprintf(hp, sizeof hp, "/covers.json");
        else if (!strcmp(target, "/api/discover")) snprintf(hp, sizeof hp, "/discover.json");
        else if (!strcmp(target, "/api/wishlist")) snprintf(hp, sizeof hp, "/wishlist.json");
        else {
            char kind[8] = "", name[40] = ""; const char *q = target + 11;
            if (sscanf(q, "%7[a-z]/%39s", kind, name) == 2 && (!strcmp(kind, "thumb") || !strcmp(kind, "full") || !strcmp(kind, "sheet") || !strcmp(kind, "dsheet"))) {
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
            if (!strcmp(what, "wish")) {
                char id[16] = ""; get_q(query, "id", id, sizeof id);
                if (!valid_did(id) || (strcmp(value, "0") && strcmp(value, "1"))) { sb_f(&b, "{\"ok\":false,\"error\":\"bad game id or value\"}"); reply_json(s, 400, &b); free(b.b); return; }
                conf_t c; read_conf(&c);
                char hp[80]; snprintf(hp, sizeof hp, "/wishlist?id=%s&on=%s", id, value);
                char *m = NULL; int st = api_http(&c, "POST", hp, "X-BM-Request: 1\r\n", &m, NULL, NULL, 0, 8);
                logf_("ui: wishlist %s %s -> %s", value[0] == '1' ? "add" : "remove", id, st == 200 ? "done" : st < 0 ? "mac1 did not answer" : "refused");
                if (st == 200) { sb_f(&b, "{\"ok\":true,\"wish\":\"%s\",\"on\":%s}", id, value[0] == '1' ? "true" : "false"); reply_json(s, 200, &b); }
                else { sb_f(&b, "{\"ok\":false,\"error\":"); sb_js(&b, st < 0 ? "mac1 did not answer" : st == 404 ? "not a Discover game on mac1" : "mac1 refused it"); sb_puts(&b, "}"); reply_json(s, 502, &b); }
                free(m); free(b.b); return;
            }
            const char *err = api_enqueue(what, tid, key, value);
            if (err) { sb_f(&b, "{\"ok\":false,\"error\":"); sb_js(&b, err); sb_put(&b, "}", 1); reply_json(s, 400, &b); }
            else { sb_f(&b, "{\"ok\":true,\"queued\":"); sb_js(&b, what); sb_put(&b, "}", 1); reply_json(s, 200, &b); }
        }
    }
    else if (!strncmp(target, "/api/pm/", 8)) api_pm(s, method, target, query, req);
    else if (!strncmp(target, "/api/smp/", 9)) api_smp(s, method, target, query, req);
    else if (!strcmp(target, "/api/store") || !strcmp(target, "/api/store/install")) api_store(s, method, target, query, req);
    else if (!strcmp(method, "GET") && !strcmp(target, "/api/pull")) { api_pull_status(&b); reply_json(s, 200, &b); }
    else if (!strcmp(method, "POST") && !strcmp(target, "/api/pull")) {
        char v[8];
        if (!strcasestr(req, "\nX-BM-Request: 1")) { sb_f(&b, "{\"ok\":false,\"error\":\"missing X-BM-Request header\"}"); reply_json(s, 403, &b); }
        else if (get_q(query, "cancel", v, sizeof v)) {
            pthread_mutex_lock(&g_pull_mu); int on = !strcmp(g_pull.state, "pulling"); if (on) g_pull.cancel = 1; pthread_mutex_unlock(&g_pull_mu);
            sb_f(&b, "{\"ok\":%s}", on ? "true" : "false"); reply_json(s, 200, &b);
        }
        else { int code = api_pull_start(&b, query); reply_json(s, code, &b); }
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

// The API thread's stack is set here, the same on the console and in the host tests. The console's default thread stack is small:
// v2.7-a7ce6b8 on PHAT5 (24 Oct 2026) died on its first GET /api/library with SIGSEGV "user write data, page not present" 8 bytes
// below rsp (api_handle -> api_library's 31 KB conf_t -> scan_local's path buffers ran off the end). A host build with
// -DBM_API_STACK=65536 dies the same way in test_api, so the host tests now run with the console's real stack size.
#ifndef BM_API_STACK
#define BM_API_STACK (1u << 20)
#endif
static void bm_api_start(void) {
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at);
    int sr = pthread_attr_setstacksize(&at, BM_API_STACK);
    if (sr) logf_("ui: API thread stack %u bytes refused (%d): the default is used", (unsigned)BM_API_STACK, sr);
    if (pthread_create(&t, &at, api_thread, NULL) == 0) { pthread_detach(t); g_api_started = 1; }
    else logf_("ui: API thread did not start (errno %d)", errno);
    pthread_attr_destroy(&at);
}
