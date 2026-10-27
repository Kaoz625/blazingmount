// SPDX-License-Identifier: GPL-3.0-or-later
// bm_install.c - v2.10: BlazingMount INSTALLS a PKG itself, straight from the host (Markus 25+27 Oct 2026: "install from the PC with
// live %, ETA and cancel", PlayBridge/BGFT-style: the host serves the pkg, the console installs it, no copy on the console first).
//
// How: Sony's own installer (libSceAppInstUtil, sceAppInstUtilInstallByPackage - the call singleDPI, etaHEN DPI and PKG Manager's
// helper all make) is handed a 127.0.0.1 URL on THIS payload. A small proxy here answers the installer's Range reads from the host
// (imgserve /pkg/<path> on the drives - split X.pkg.N sets joined there - or any http://host:port/x.pkg a PC serves), so:
//   - progress is real: Sony's sceAppInstUtilGetInstallStatus (downloaded/total/remain) plus the bytes this proxy has served
//   - CANCEL works: the proxy stops answering, Sony's task fails, the job says cancelled
//   - the route is the daemon's: LAN, else the tailnet proxy (away from home)
// PKG Manager (his own tool, never replaced) stays the FALLBACK: when Sony's installer cannot be reached from this payload or refuses
// the URL, a drive pkg is handed to PKG Manager as smb://<host>/mac1-8TB/<path> (the /api/pm route, c86e9b2) and followed there.
//
// #include'd ONCE into bm_api.c after bm_store.c (uses pm_http, route_connect, connect_ms, read_conf, get_q, sb_*, url_path, logf_).
//
//   GET  /api/install                      the job: {state, via, name, src, title_id, content_id, total, done, served, rate, eta, ...}
//   GET  /api/install/pkgs                 the host's PKGs on the drives (imgserve /pkgs.json: path, name, size, parts)
//   POST /api/install?path=<pkgs.json path>[&name=]      install a drive pkg            (header X-BM-Request: 1; 409 while one runs)
//   POST /api/install?url=http://<ip>:<port>/<x>.pkg[&name=]   install what a PC serves (host/install-pkg.py serves + follows it)
//   POST /api/install/cancel               stop it
//
// The AppInstUtil layouts are public homebrew-scene knowledge (the same ones PKG Manager, itsPLK/ps5-pkg-manager GPL-3.0, documents in
// its ARCHITECTURE.md); the code here is ours. Host test builds (no __PROSPERO__) get a FAKE installer that reads the URL the way Sony's
// does (host/test_install.py), so every route, the proxy, progress and cancel run in the tests.

#ifndef BM_INST_PORT
#define BM_INST_PORT 8095
#endif
#ifndef INST_STALL_S
#define INST_STALL_S 300                  // nothing moved for this long (bytes, Sony's status) -> failed, like PKG Manager's 5 min
#endif
#define INST_PM_SHARE "mac1-8TB"          // the 8TB's SMB share (PKG Manager's mac1 source), for the fallback

typedef struct {
    char state[12];                       // idle | starting | installing | done | failed | cancelled
    char via[12];                         // sony | pkgmanager
    char src[900], name[200], err[300], step[200];
    char up_host[64], up_path[900]; int up_port, up_route;      // upstream: up_route = 1 -> the daemon's route to its host
    char content_id[64], title_id[16], sony_status[20];
    long long total, served, done, rate;
    int remain, sony_error, cancel;
    unsigned id;
    time_t started, finished, moved;
} inst_t;
static inst_t g_inst = { .state = "idle" };
static pthread_mutex_t g_inst_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_inst_listening;

static int inst_running_locked(void) { return !strcmp(g_inst.state, "starting") || !strcmp(g_inst.state, "installing"); }

static void inst_set(const char *state, const char *fmt, ...) {
    pthread_mutex_lock(&g_inst_mu);
    if (state) snprintf(g_inst.state, sizeof g_inst.state, "%s", state);
    if (fmt) {
        va_list ap; va_start(ap, fmt);
        vsnprintf(!strcmp(g_inst.state, "failed") ? g_inst.err : g_inst.step, sizeof g_inst.step, fmt, ap);
        va_end(ap);
    }
    if (state && strcmp(state, "starting") && strcmp(state, "installing")) g_inst.finished = time(NULL);
    pthread_mutex_unlock(&g_inst_mu);
}

// ---------------------------------------------------------------- Sony's installer (AppInstUtil)
typedef struct { char content_id[48]; int type, platform; } ai_pkg_info_t;
typedef struct { const char *uri, *ex_uri, *playgo_scenario_id, *content_id, *content_name, *icon_url; } ai_meta_t;
typedef struct { char languages[30][8]; char playgo_scenario_ids[64][3]; char content_ids[64][48]; unsigned char unknown[6480]; } ai_playgo_t;
typedef struct { int32_t error_code, version; char description[512]; char type[9]; } ai_err_t;
typedef struct {
    char status[16]; char src_type[8]; uint32_t remain_time;
    uint64_t downloaded_size, initial_chunk_size, total_size; uint32_t promote_progress;
    ai_err_t error_info; int32_t local_copy_percent; int8_t is_copy_only;
    char pad[2048];                       // whatever newer firmware appends
} ai_status_t;
#define AI_INVALID_SLOT 0x80B2116Fu       // PLAYGO_ERROR_CORE_INVALID_SLOT: the 2nd install of one AppInstUtil session (FW >= 9.60)

static int (*ai_init)(void), (*ai_term)(void);
static int (*ai_by_package)(ai_meta_t *, ai_pkg_info_t *, ai_playgo_t *);
static int (*ai_get_status)(const char *, ai_status_t *);
static int g_ai_state;                    // 0 not tried, 1 ready, -1 unavailable (then PKG Manager)
static char g_ai_why[160];

#ifdef __PROSPERO__
int sceKernelLoadStartModule(const char *, size_t, const void *, uint32_t, void *, int *);
static intptr_t ai_sym(const char *module, const char *sym) {   // the module's symbol, loading it from /system/common/lib when absent
    uint32_t h = 0;
    if (kernel_dynlib_handle(-1, module, &h) < 0) {
        char path[128]; snprintf(path, sizeof path, "/system/common/lib/%s", module);
        int rc = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
        if (rc > 0) h = (uint32_t)rc;
        else if (kernel_dynlib_handle(-1, module, &h) < 0) return 0;
    }
    return kernel_dynlib_dlsym(-1, h, sym);
}
static int ai_bring_up(void) {
    int (*sysmod)(unsigned) = (int (*)(unsigned))ai_sym("libSceSysmodule.sprx", "sceSysmoduleLoadModuleInternal");
    if (sysmod) { sysmod(0x8000001d); sysmod(0x80000011); sysmod(0x80000010); sysmod(0x80000014); }   // IPMI, UserService, SystemService, AppInstUtil
    ai_init = (int (*)(void))ai_sym("libSceAppInstUtil.sprx", "sceAppInstUtilInitialize");
    ai_term = (int (*)(void))ai_sym("libSceAppInstUtil.sprx", "sceAppInstUtilTerminate");
    ai_by_package = (int (*)(ai_meta_t *, ai_pkg_info_t *, ai_playgo_t *))ai_sym("libSceAppInstUtil.sprx", "sceAppInstUtilInstallByPackage");
    ai_get_status = (int (*)(const char *, ai_status_t *))ai_sym("libSceAppInstUtil.sprx", "sceAppInstUtilGetInstallStatus");
    if (!ai_init || !ai_by_package || !ai_get_status) { snprintf(g_ai_why, sizeof g_ai_why, "libSceAppInstUtil symbols not found"); return -1; }
    int rc = ai_init();
    if (rc) { snprintf(g_ai_why, sizeof g_ai_why, "sceAppInstUtilInitialize 0x%08x", (unsigned)rc); return -1; }
    return 0;
}
#else
// FAKE installer for the host tests: reads the URL like Sony's does (64 KiB header, then 1 MiB Range pieces on a keep-alive
// connection), reports downloading -> playable. BM_FAKE_AI=off (unavailable), refuse (rc 0x80B21104), stall (never finishes),
// BM_FAKE_AI_DELAY_MS between pieces (so a cancel lands mid-way).
static struct { char uri[200], status[16], cid[48]; long long got, total; int err, running; } g_fake;
static pthread_mutex_t g_fake_mu = PTHREAD_MUTEX_INITIALIZER;
static long long fake_range(int s, int port, const char *path, long long a, long long b, long long *total) {
    char req[400]; int rl = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nRange: bytes=%lld-%lld\r\n\r\n", path, port, a, b);
    if (write_all(s, req, (size_t)rl)) return -1;
    char h[2048]; size_t have = 0; char *e = NULL;
    while (!e) { ssize_t n = read(s, h + have, sizeof h - 1 - have); if (n <= 0) return -1; have += (size_t)n; h[have] = 0; e = strstr(h, "\r\n\r\n"); }
    if (atoi(h + 9) != 206) return -1;
    char *cl = strcasestr(h, "\nContent-Length: "), *cr = strcasestr(h, "\nContent-Range: bytes ");
    long long want = cl ? atoll(cl + 17) : -1, ra, rb; if (cr) sscanf(cr + 22, "%lld-%lld/%lld", &ra, &rb, total);
    long long got = (long long)(have - (size_t)(e + 4 - h)); char buf[65536];
    while (got < want) { ssize_t n = read(s, buf, sizeof buf); if (n <= 0) return -1; got += n; }
    return got;
}
static void *fake_sony(void *arg) {
    (void)arg;
    int port = 0; char path[160] = "";
    sscanf(g_fake.uri, "http://127.0.0.1:%d%159s", &port, path);
    int delay = getenv("BM_FAKE_AI_DELAY_MS") ? atoi(getenv("BM_FAKE_AI_DELAY_MS")) : 0;
    int s = connect_ms("127.0.0.1", port, 2000); long long total = -1;
    long long n = s < 0 ? -1 : fake_range(s, port, path, 0, 65535, &total);
    pthread_mutex_lock(&g_fake_mu); g_fake.total = total; g_fake.got = n > 0 ? n : 0; pthread_mutex_unlock(&g_fake_mu);
    for (long long a = 65536; n > 0 && a < total; a += 1 << 20) {
        if (delay) usleep((useconds_t)delay * 1000);
        long long b = a + (1 << 20) - 1; if (b >= total) b = total - 1;
        n = fake_range(s, port, path, a, b, &total);
        if (n > 0) { pthread_mutex_lock(&g_fake_mu); g_fake.got += n; pthread_mutex_unlock(&g_fake_mu); }
    }
    if (s >= 0) close(s);
    pthread_mutex_lock(&g_fake_mu);
    const char *mode = getenv("BM_FAKE_AI") ? getenv("BM_FAKE_AI") : "";
    if (n <= 0) { snprintf(g_fake.status, sizeof g_fake.status, "error"); g_fake.err = (int)0x80B2111Eu; }   // a read failed (a cancel)
    else if (strcmp(mode, "stall")) snprintf(g_fake.status, sizeof g_fake.status, "playable");
    g_fake.running = 0;
    pthread_mutex_unlock(&g_fake_mu);
    return NULL;
}
static int fake_init(void) { return 0; }
static int fake_term(void) { return 0; }
static int fake_by_package(ai_meta_t *m, ai_pkg_info_t *info, ai_playgo_t *pg) {
    (void)pg;
    const char *mode = getenv("BM_FAKE_AI") ? getenv("BM_FAKE_AI") : "";
    if (!strcmp(mode, "refuse")) return (int)0x80B21104u;
    pthread_mutex_lock(&g_fake_mu);
    snprintf(g_fake.uri, sizeof g_fake.uri, "%s", m->uri); snprintf(g_fake.status, sizeof g_fake.status, "downloading");
    snprintf(g_fake.cid, sizeof g_fake.cid, "IV9999-FAKE00001_00-BLAZINGMOUNTTEST"); g_fake.got = 0; g_fake.total = 0; g_fake.err = 0; g_fake.running = 1;
    pthread_mutex_unlock(&g_fake_mu);
    snprintf(info->content_id, sizeof info->content_id, "%s", g_fake.cid);
    pthread_t t; if (pthread_create(&t, NULL, fake_sony, NULL)) return -1;
    pthread_detach(t);
    return 0;
}
static int fake_get_status(const char *cid, ai_status_t *st) {
    (void)cid;
    pthread_mutex_lock(&g_fake_mu);
    snprintf(st->status, sizeof st->status, "%s", g_fake.status);
    st->downloaded_size = (uint64_t)g_fake.got; st->total_size = (uint64_t)(g_fake.total > 0 ? g_fake.total : 0);
    st->error_info.error_code = g_fake.err; st->remain_time = 7;
    pthread_mutex_unlock(&g_fake_mu);
    return 0;
}
static int ai_bring_up(void) {
    if (getenv("BM_FAKE_AI") && !strcmp(getenv("BM_FAKE_AI"), "off")) { snprintf(g_ai_why, sizeof g_ai_why, "fake: unavailable"); return -1; }
    ai_init = fake_init; ai_term = fake_term; ai_by_package = fake_by_package; ai_get_status = fake_get_status;
    return 0;
}
#endif

// ---------------------------------------------------------------- upstream: one Range read of the source
static int inst_up_open(long long a, long long b, long long *total, char *pre, size_t *pre_n, char *why, size_t wn) {
    char host[64], path[900]; int port, route;
    pthread_mutex_lock(&g_inst_mu);
    snprintf(host, sizeof host, "%s", g_inst.up_host); snprintf(path, sizeof path, "%s", g_inst.up_path);
    port = g_inst.up_port; route = g_inst.up_route;
    pthread_mutex_unlock(&g_inst_mu);
    int px = 0, s = route ? route_connect(host, port, 20, &px) : connect_ms(host, port, 5000);
    if (s < 0) { snprintf(why, wn, "cannot reach %s:%d", host, port); return -1; }
    struct timeval tv = { 30, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char enc[2000], req[2400]; url_path(enc, sizeof enc, path);
    int rl = px ? snprintf(req, sizeof req, "GET http://%s:%d%s HTTP/1.0\r\nHost: %s:%d\r\nRange: bytes=%lld-%lld\r\nConnection: close\r\n\r\n",
                           g_ts_host, port, enc, g_ts_host, port, a, b)
                : snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s:%d\r\nRange: bytes=%lld-%lld\r\nConnection: close\r\n\r\n", enc, host, port, a, b);
    if (write_all(s, req, (size_t)rl)) { close(s); snprintf(why, wn, "request not sent"); return -1; }
    char h[4096]; size_t have = 0; char *e = NULL;
    while (!e) {
        if (have >= sizeof h - 1) { close(s); snprintf(why, wn, "headers too long"); return -1; }
        ssize_t n = read(s, h + have, sizeof h - 1 - have);
        if (n <= 0) { close(s); snprintf(why, wn, "no answer from %s:%d", host, port); return -1; }
        have += (size_t)n; h[have] = 0; e = strstr(h, "\r\n\r\n");
    }
    int st = (have > 12 && !strncmp(h, "HTTP/1.", 7)) ? atoi(h + 9) : 0;
    char *cr = strcasestr(h, "\nContent-Range: bytes ");
    long long ra = -1, rb = -1, rt = -1;
    if (cr) sscanf(cr + 22, "%lld-%lld/%lld", &ra, &rb, &rt);
    if (st != 206 || ra != a || rb != b || rt <= 0) { close(s); snprintf(why, wn, "%s:%d answered HTTP %d for bytes %lld-%lld", host, port, st, a, b); return -1; }
    *total = rt;
    *pre_n = have - (size_t)(e + 4 - h); memcpy(pre, e + 4, *pre_n);
    return s;
}

// ---------------------------------------------------------------- the proxy Sony's installer reads (127.0.0.1:BM_INST_PORT)
static int inst_live(unsigned id) {
    pthread_mutex_lock(&g_inst_mu);
    int ok = g_inst.id == id && inst_running_locked() && !g_inst.cancel;
    pthread_mutex_unlock(&g_inst_mu);
    return ok;
}
static void inst_simple(int s, int code, const char *what) {
    char h[200]; int n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", code, what);
    write_all(s, h, (size_t)n);
}
static void *inst_conn(void *arg) {
    int s = (int)(intptr_t)arg;
    struct timeval tv = { 60, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char *buf = malloc(1 << 20), req[4096]; size_t have = 0; req[0] = 0;
    if (!buf) { close(s); return NULL; }
    for (;;) {                                                      // keep-alive: Sony sends several Range requests on one connection
        char *e = NULL;
        while (!(e = strstr(req, "\r\n\r\n")) || !have) {
            if (have >= sizeof req - 1) goto out;
            ssize_t n = read(s, req + have, sizeof req - 1 - have);
            if (n <= 0) goto out;
            have += (size_t)n; req[have] = 0;
        }
        size_t hl = (size_t)(e + 4 - req);
        char method[8] = "", target[300] = ""; unsigned id = 0;
        sscanf(req, "%7s %299s", method, target);
        int keep = !strcasestr(req, "\nConnection: close") && strstr(req, "HTTP/1.1");
        char *rg = strcasestr(req, "\nRange: bytes=");
        long long a = 0, b = -1;
        if (rg) { char *d; a = strtoll(rg + 14, &d, 10); if (*d == '-' && d[1] >= '0' && d[1] <= '9') b = strtoll(d + 1, NULL, 10); }
        memmove(req, req + hl, have - hl); have -= hl; req[have] = 0;
        if (sscanf(target, "/i/%u.pkg", &id) != 1 || (strcmp(method, "GET") && strcmp(method, "HEAD")) || !inst_live(id)) {
            inst_simple(s, 404, "Not Found"); goto out;             // a .crc sidecar, an old job, a cancelled one
        }
        long long total = 0;
        pthread_mutex_lock(&g_inst_mu); total = g_inst.total; pthread_mutex_unlock(&g_inst_mu);
        if (b < 0 || b >= total) b = total - 1;
        if (a < 0 || a > b) { char h[200]; int n = snprintf(h, sizeof h, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%lld\r\nContent-Length: 0\r\n\r\n", total); write_all(s, h, (size_t)n); if (!keep) goto out; continue; }
        char hdr[400];
        int n = rg ? snprintf(hdr, sizeof hdr, "HTTP/1.1 206 Partial Content\r\nContent-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\n"
                              "Content-Range: bytes %lld-%lld/%lld\r\nContent-Length: %lld\r\nConnection: %s\r\n\r\n", a, b, total, b - a + 1, keep ? "keep-alive" : "close")
                   : snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\n"
                              "Content-Length: %lld\r\nConnection: %s\r\n\r\n", total, keep ? "keep-alive" : "close");
        if (!strcmp(method, "HEAD")) { if (write_all(s, hdr, (size_t)n) || !keep) goto out; continue; }
        char why[200] = ""; size_t pn = 0; long long t2 = 0;
        int u = inst_up_open(a, b, &t2, buf, &pn, why, sizeof why);
        if (u < 0 || t2 != total) {
            if (u >= 0) close(u);
            logf_("install: proxy %s", u < 0 ? why : "the source changed size");
            inst_simple(s, 502, "Bad Gateway"); goto out;
        }
        if (write_all(s, hdr, (size_t)n)) { close(u); goto out; }
        long long pos = a, left = b - a + 1; size_t k = pn;
        while (left > 0) {
            if (k) {
                if ((long long)k > left) k = (size_t)left;
                if (!inst_live(id) || write_all(s, buf, k)) { close(u); goto out; }   // cancel: the installer's read just ends
                pos += (long long)k; left -= (long long)k;
                pthread_mutex_lock(&g_inst_mu); g_inst.served += (long long)k; g_inst.moved = time(NULL); pthread_mutex_unlock(&g_inst_mu);
            }
            if (left <= 0) break;
            ssize_t r = read(u, buf, 1 << 20);
            if (r <= 0) { logf_("install: the source ended at %lld of bytes %lld-%lld", pos, a, b); close(u); goto out; }
            k = (size_t)r;
        }
        close(u);
        if (!keep) break;
    }
out:
    free(buf); close(s);
    return NULL;
}
static void *inst_listen(void *arg) {
    int ls = (int)(intptr_t)arg;
    for (;;) {
        int s = accept(ls, NULL, NULL);
        if (s < 0) { if (errno == EINTR) continue; sleep(1); continue; }
        pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256 * 1024);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, inst_conn, (void *)(intptr_t)s)) close(s);
        pthread_attr_destroy(&at);
    }
    return NULL;
}
static int inst_listen_start(void) {                                // once, on the first install (no extra port until then)
    if (g_inst_listening) return 0;
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(BM_INST_PORT); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // the installer only
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 16)) { if (ls >= 0) close(ls); return -1; }
    pthread_t t;
    if (pthread_create(&t, NULL, inst_listen, (void *)(intptr_t)ls)) { close(ls); return -1; }
    pthread_detach(t); g_inst_listening = 1;
    return 0;
}

// ---------------------------------------------------------------- PKG Manager fallback (a drive pkg only)
static const char *js_val(const char *j, const char *key) {          // the value after "key": in a flat JSON answer (spaces skipped)
    char k[64]; snprintf(k, sizeof k, "\"%s\":", key);
    const char *p = j ? strstr(j, k) : NULL;
    if (!p) return NULL;
    p += strlen(k); while (*p == ' ') p++;
    return p;
}
static long long js_num(const char *j, const char *key) { const char *p = js_val(j, key); return p ? strtoll(p, NULL, 10) : -1; }
static int js_true(const char *j, const char *key) { const char *p = js_val(j, key); return p && !strncmp(p, "true", 4); }
static void inst_via_pm(const char *rel) {
    conf_t *c = malloc(sizeof *c); if (!c) { inst_set("failed", "out of memory"); return; }
    read_conf(c);
    sb_t j = { 0 }, b = { 0 };
    char smb[1100]; snprintf(smb, sizeof smb, "smb://%s/%s/%s", c->host, INST_PM_SHARE, rel); free(c);
    sb_puts(&j, "{\"path\":"); sb_js(&j, smb); sb_puts(&j, "}");
    int st = pm_http("POST", "/api/install", j.b ? j.b : "{}", &b);
    logf_("install: fallback PKG Manager %s -> HTTP %d", smb, st);
    free(j.b);
    if (st != 200 || !js_true(b.b, "success")) {
        inst_set("failed", "PKG Manager: %s", st < 0 ? "not running on this console (:8844)" : b.b ? b.b : "refused"); free(b.b); return;
    }
    free(b.b);
    pthread_mutex_lock(&g_inst_mu); snprintf(g_inst.via, sizeof g_inst.via, "pkgmanager"); pthread_mutex_unlock(&g_inst_mu);
    inst_set("installing", "PKG Manager is installing it");
    long long last = -1; time_t t0 = time(NULL);
    for (;;) {
        sleep(1);
        int cancel; pthread_mutex_lock(&g_inst_mu); cancel = g_inst.cancel; pthread_mutex_unlock(&g_inst_mu);
        sb_t q = { 0 };
        if (cancel) {
            if (pm_http("GET", "/api/queue", NULL, &q) == 200) {
                long long id = js_num(q.b, "active_id"); char body[64]; sb_t r = { 0 };
                snprintf(body, sizeof body, "{\"id\":%lld}", id > 0 ? id : 0); pm_http("POST", "/api/queue/cancel", body, &r); free(r.b);
            }
            free(q.b); inst_set("cancelled", "cancelled (PKG Manager)"); return;
        }
        if (pm_http("GET", "/api/status", NULL, &q) != 200) { free(q.b); if (time(NULL) - t0 > 60) { inst_set("failed", "PKG Manager stopped answering"); return; } continue; }
        long long d = js_num(q.b, "downloaded_bytes"), t = js_num(q.b, "total_bytes");
        pthread_mutex_lock(&g_inst_mu);
        if (t > 0) g_inst.total = t;
        if (d >= 0) { if (d != last) g_inst.moved = time(NULL); g_inst.done = d; }
        pthread_mutex_unlock(&g_inst_mu);
        last = d;
        if (js_true(q.b, "completed")) { free(q.b); inst_set("done", "installed (PKG Manager)"); return; }
        if (js_true(q.b, "failed")) {
            char msg[200] = ""; const char *p = js_val(q.b, "prompt_message");
            if (p && *p == '"') js_str(p, msg, sizeof msg);
            free(q.b); inst_set("failed", "PKG Manager: %s", msg[0] ? msg : "install failed"); return;
        }
        free(q.b);
        t0 = time(NULL);
    }
}

// ---------------------------------------------------------------- the job
static int ai_ready(void) {
    if (g_ai_state == 0) {
        g_ai_state = ai_bring_up() == 0 ? 1 : -1;
        logf_("install: Sony's installer from this payload: %s", g_ai_state == 1 ? "ready" : g_ai_why);
    }
    return g_ai_state == 1;
}
static void *inst_run(void *arg) {
    (void)arg;
    char rel[900] = ""; unsigned id; int route;
    pthread_mutex_lock(&g_inst_mu);
    id = g_inst.id; route = g_inst.up_route;
    if (route) snprintf(rel, sizeof rel, "%s", g_inst.up_path + 5);    // "/pkg/<rel>"
    pthread_mutex_unlock(&g_inst_mu);

    char why[200] = "", pre[4096]; size_t pn = 0; long long total = 0;
    int s = inst_up_open(0, 0, &total, pre, &pn, why, sizeof why);   // the size, and the source answers Range
    if (s < 0) { inst_set("failed", "the source: %s", why); return NULL; }
    close(s);
    pthread_mutex_lock(&g_inst_mu); g_inst.total = total; g_inst.moved = time(NULL); pthread_mutex_unlock(&g_inst_mu);

    if (!ai_ready() || inst_listen_start() != 0) {
        if (route) { logf_("install: %s -> PKG Manager", g_ai_state == 1 ? "proxy port busy" : g_ai_why); inst_via_pm(rel); }
        else inst_set("failed", "Sony's installer is not reachable from this payload (%s)", g_ai_state == 1 ? "proxy port busy" : g_ai_why);
        return NULL;
    }
    char uri[120], name[200];
    snprintf(uri, sizeof uri, "http://127.0.0.1:%d/i/%u.pkg", BM_INST_PORT, id);
    pthread_mutex_lock(&g_inst_mu); snprintf(name, sizeof name, "%s", g_inst.name); snprintf(g_inst.via, sizeof g_inst.via, "sony"); pthread_mutex_unlock(&g_inst_mu);
    inst_set("installing", "handing it to the PS5 installer");
    ai_meta_t m = { uri, "", "", "", name, "" };
    ai_pkg_info_t info; ai_playgo_t *pg = calloc(1, sizeof *pg);
    if (!pg) { inst_set("failed", "out of memory"); return NULL; }
    int rc = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        memset(&info, 0, sizeof info); memset(pg, 0, sizeof *pg);
        rc = ai_by_package(&m, &info, pg);
        logf_("install: sceAppInstUtilInstallByPackage %s (%s) -> 0x%08x cid %s", uri, name, (unsigned)rc, info.content_id);
        if ((unsigned)rc != AI_INVALID_SLOT || !ai_term) break;
        ai_term(); if (ai_init()) break;                              // a fresh AppInstUtil session, then once more
    }
    free(pg);
    if (rc) {
        if (route) { logf_("install: the PS5 installer refused it (0x%08x) -> PKG Manager", (unsigned)rc); inst_via_pm(rel); }
        else inst_set("failed", "the PS5 installer refused it (0x%08x)", (unsigned)rc);
        return NULL;
    }
    pthread_mutex_lock(&g_inst_mu);
    snprintf(g_inst.content_id, sizeof g_inst.content_id, "%.48s", info.content_id);
    if (strlen(g_inst.content_id) >= 16) snprintf(g_inst.title_id, sizeof g_inst.title_id, "%.9s", g_inst.content_id + 7);
    pthread_mutex_unlock(&g_inst_mu);
    inst_set(NULL, "the PS5 is installing it");

    ai_status_t *st = calloc(1, sizeof *st);
    long long prev = 0; time_t tprev = time(NULL); char last_status[20] = "";
    for (;;) {
        sleep(1);
        int cancel; char cid[64];
        pthread_mutex_lock(&g_inst_mu); cancel = g_inst.cancel; snprintf(cid, sizeof cid, "%s", g_inst.content_id); pthread_mutex_unlock(&g_inst_mu);
        int grc = st ? (memset(st, 0, sizeof *st), ai_get_status(cid, st)) : -1;
        pthread_mutex_lock(&g_inst_mu);
        if (grc == 0) {
            snprintf(g_inst.sony_status, sizeof g_inst.sony_status, "%.15s", st->status);
            g_inst.sony_error = st->error_info.error_code; g_inst.remain = (int)st->remain_time;
            if (strcmp(last_status, g_inst.sony_status)) { snprintf(last_status, sizeof last_status, "%s", g_inst.sony_status); g_inst.moved = time(NULL); }
        }
        long long d = grc == 0 && st->downloaded_size ? (long long)st->downloaded_size : 0;
        if (d > g_inst.served) d = g_inst.served;                       // Sony counts what it asked for; never more than really arrived
        if (d < g_inst.done) d = g_inst.done;
        if (!d) d = g_inst.served < g_inst.total ? g_inst.served : g_inst.total;
        g_inst.done = d;
        time_t now = time(NULL);
        if (now > tprev) { long long r = (d - prev) / (now - tprev); g_inst.rate = g_inst.rate ? (g_inst.rate * 2 + r) / 3 : r; prev = d; tprev = now; }
        int sony_done = grc == 0 && (!strcmp(st->status, "playable") || !strcmp(st->status, "completed"));
        int sony_err = grc == 0 && st->error_info.error_code != 0;
        int stalled = now - g_inst.moved > INST_STALL_S;
        char sstat[20]; snprintf(sstat, sizeof sstat, "%s", g_inst.sony_status); int serr = g_inst.sony_error;
        pthread_mutex_unlock(&g_inst_mu);
        if (cancel) { inst_set("cancelled", "cancelled: the PS5 stops reading it (its Downloads list may show it as failed - delete it there)"); break; }
        if (sony_done) { pthread_mutex_lock(&g_inst_mu); g_inst.done = g_inst.total; pthread_mutex_unlock(&g_inst_mu); inst_set("done", "installed"); break; }
        if (sony_err) { inst_set("failed", "the PS5 installer: error 0x%08x (status %s)", (unsigned)serr, sstat); break; }
        if (stalled) { inst_set("failed", "nothing moved for %d s (PS5 status '%s') - check the PS5's Downloads", INST_STALL_S, sstat); break; }
    }
    free(st);
    char fin[16], tid[16]; long long sv, tt;
    pthread_mutex_lock(&g_inst_mu); snprintf(fin, sizeof fin, "%s", g_inst.state); snprintf(tid, sizeof tid, "%s", g_inst.title_id);
    sv = g_inst.served; tt = g_inst.total; pthread_mutex_unlock(&g_inst_mu);
    logf_("install: %s %s (%s) - served %lld of %lld bytes", tid, fin, name, sv, tt);
    return NULL;
}

static int inst_rel_ok(const char *s) {
    if (!*s || s[0] == '/' || strstr(s, "..") || strstr(s, "//") || !ends_with(s, ".pkg", 1)) return 0;
    for (const unsigned char *q = (const unsigned char *)s; *q; q++) if (*q < 0x20 || *q == 0x7f) return 0;
    return 1;
}
static int inst_url_parse(const char *u, char *host, size_t hn, int *port, char *path, size_t pn) {   // http://<ipv4>:<port>/<...>.pkg
    if (strncmp(u, "http://", 7)) return 0;
    const char *h = u + 7, *sl = strchr(h, '/');
    if (!sl) return 0;
    char hp[80]; size_t L = (size_t)(sl - h); if (!L || L >= sizeof hp) return 0;
    memcpy(hp, h, L); hp[L] = 0;
    char *colon = strchr(hp, ':'); *port = 80;
    if (colon) { *colon = 0; *port = atoi(colon + 1); }
    struct in_addr ia; if (*port <= 0 || *port > 65535 || inet_pton(AF_INET, hp, &ia) != 1) return 0;
    snprintf(host, hn, "%s", hp);
    char raw[900]; size_t o = 0;                                       // stored decoded: inst_up_open percent-encodes it again
    for (const char *p = sl; *p && o + 1 < sizeof raw; ) {
        if (*p == '%' && p[1] && p[2]) { char x[3] = { p[1], p[2], 0 }; raw[o++] = (char)strtol(x, NULL, 16); p += 3; }
        else raw[o++] = *p++;
    }
    raw[o] = 0;
    for (const unsigned char *q = (const unsigned char *)raw; *q; q++) if (*q < 0x20 || *q == 0x7f) return 0;
    if (strstr(raw, "..") || !ends_with(raw, ".pkg", 1)) return 0;
    snprintf(path, pn, "%s", raw);
    return 1;
}

static void inst_status(sb_t *b) {
    pthread_mutex_lock(&g_inst_mu);
    long long eta = -1;
    if (inst_running_locked() && g_inst.total > 0) eta = g_inst.rate > 0 ? (g_inst.total - g_inst.done) / g_inst.rate : (g_inst.remain > 0 ? g_inst.remain : -1);
    sb_f(b, "{\"state\":\"%s\",\"via\":\"%s\",\"id\":%u,\"name\":", g_inst.state, g_inst.via, g_inst.id); sb_js(b, g_inst.name);
    sb_puts(b, ",\"src\":"); sb_js(b, g_inst.src);
    sb_puts(b, ",\"title_id\":"); sb_js(b, g_inst.title_id); sb_puts(b, ",\"content_id\":"); sb_js(b, g_inst.content_id);
    sb_f(b, ",\"total\":%lld,\"done\":%lld,\"served\":%lld,\"progress\":%.1f,\"rate\":%lld,\"eta\":%lld,\"remain_time\":%d",
         g_inst.total, g_inst.done, g_inst.served, g_inst.total > 0 ? 100.0 * (double)g_inst.done / (double)g_inst.total : 0.0, g_inst.rate, eta, g_inst.remain);
    sb_puts(b, ",\"sony_status\":"); sb_js(b, g_inst.sony_status);
    sb_f(b, ",\"sony_error\":%d,\"step\":", g_inst.sony_error); sb_js(b, g_inst.step);
    sb_puts(b, ",\"error\":"); sb_js(b, g_inst.err);
    sb_f(b, ",\"started\":%lld,\"finished\":%lld,\"installer\":\"%s\"}", (long long)g_inst.started, (long long)g_inst.finished,
         g_ai_state == 1 ? "ready" : g_ai_state < 0 ? "pkgmanager" : "not tried");
    pthread_mutex_unlock(&g_inst_mu);
}

static void api_install(int s, const char *method, const char *target, const char *query, const char *req) {
    sb_t b = { 0 };
    if (!strcmp(method, "GET") && !strcmp(target, "/api/install")) { inst_status(&b); reply_json(s, 200, &b); free(b.b); return; }
    if (!strcmp(method, "GET") && !strcmp(target, "/api/install/pkgs")) {
        conf_t *c = malloc(sizeof *c); char *m = NULL; long long n = -1;
        if (c) { read_conf(c); n = api_get(c, "/pkgs.json", &m, 20); free(c); }
        if (n >= 0) reply(s, 200, "application/json", m, (size_t)n);
        else { sb_puts(&b, "{\"error\":\"the host does not list its pkgs (imgserve /pkgs.json)\"}"); reply_json(s, 502, &b); }
        free(m); free(b.b); return;
    }
    if (strcmp(method, "POST") || (strcmp(target, "/api/install") && strcmp(target, "/api/install/cancel"))) {
        sb_puts(&b, "{\"error\":\"not found\"}"); reply_json(s, 404, &b); free(b.b); return;
    }
    if (!strcasestr(req, "\nX-BM-Request: 1")) { sb_puts(&b, "{\"ok\":false,\"error\":\"missing X-BM-Request header\"}"); reply_json(s, 403, &b); free(b.b); return; }
    if (!strcmp(target, "/api/install/cancel")) {
        pthread_mutex_lock(&g_inst_mu); int on = inst_running_locked(); if (on) g_inst.cancel = 1; pthread_mutex_unlock(&g_inst_mu);
        if (on) logf_("ui: install cancelled (%s)", g_inst.name);
        sb_f(&b, "{\"ok\":%s}", on ? "true" : "false"); reply_json(s, on ? 200 : 409, &b); free(b.b); return;
    }
    char path[900] = "", url[1000] = "", name[200] = "", host[64] = "", up[900] = ""; int port = 0, route = 0;
    get_q(query, "path", path, sizeof path); get_q(query, "url", url, sizeof url); get_q(query, "name", name, sizeof name);
    if (path[0] && inst_rel_ok(path)) {
        conf_t *c = malloc(sizeof *c); if (!c) { reply_json(s, 500, &b); return; }
        read_conf(c); snprintf(host, sizeof host, "%s", c->host); port = c->port; free(c);
        snprintf(up, sizeof up, "/pkg/%s", path); route = 1;
    } else if (!(url[0] && inst_url_parse(url, host, sizeof host, &port, up, sizeof up))) {
        sb_puts(&b, "{\"ok\":false,\"error\":\"path=<a path from /api/install/pkgs> or url=http://<ip>:<port>/<file>.pkg\"}"); reply_json(s, 400, &b); free(b.b); return;
    }
    if (!name[0]) { const char *f = strrchr(up, '/'); snprintf(name, sizeof name, "%s", f ? f + 1 : up); char *dot = strrchr(name, '.'); if (dot) *dot = 0; }
    pthread_mutex_lock(&g_inst_mu);
    if (inst_running_locked()) {
        pthread_mutex_unlock(&g_inst_mu);
        sb_puts(&b, "{\"ok\":false,\"error\":\"an install is running (cancel it first)\"}"); reply_json(s, 409, &b); free(b.b); return;
    }
    unsigned id = g_inst.id + 1;
    memset(&g_inst, 0, sizeof g_inst);
    g_inst.id = id; snprintf(g_inst.state, sizeof g_inst.state, "starting"); snprintf(g_inst.via, sizeof g_inst.via, "sony");
    snprintf(g_inst.src, sizeof g_inst.src, "%s", path[0] && route ? path : url); snprintf(g_inst.name, sizeof g_inst.name, "%s", name);
    snprintf(g_inst.up_host, sizeof g_inst.up_host, "%s", host); g_inst.up_port = port; g_inst.up_route = route;
    snprintf(g_inst.up_path, sizeof g_inst.up_path, "%s", up);
    g_inst.started = g_inst.moved = time(NULL); snprintf(g_inst.step, sizeof g_inst.step, "asking the source for its size");
    pthread_mutex_unlock(&g_inst_mu);
    logf_("ui: install %s from %s:%d%s (job %u)", name, host, port, up, id);
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 512 * 1024);   // Sony's module loaders run here
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int pc = pthread_create(&t, &at, inst_run, NULL);
    pthread_attr_destroy(&at);
    if (pc) { inst_set("failed", "no thread"); sb_puts(&b, "{\"ok\":false,\"error\":\"no thread\"}"); reply_json(s, 500, &b); free(b.b); return; }
    sb_f(&b, "{\"ok\":true,\"id\":%u}", id); reply_json(s, 200, &b); free(b.b);
}
