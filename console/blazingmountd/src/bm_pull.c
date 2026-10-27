// SPDX-License-Identifier: GPL-3.0-or-later
// bm_pull.c - v2.9: the console PULLS a file from the host into <drive>/blz-stage/incoming/ (Markus 26 Oct 2026: "build nasbridge
// into blazing mount"). NASBridge's copier (J7mbo/nasbridge-ps5, GPL-3.0) is the teacher: a jailbroken payload reads the server
// itself and writes the drive itself. Our own code, and one step further: N Range connections at once, each piece written at its
// own offset, so a game no longer arrives on ONE FTP stream (ftpsrv 1.16 cannot take a second stream into one file and truncates
// at REST n: DOWNLOAD-TO-CONSOLE.md 1). mac1's download-to-console.py still plans, verifies (/api/sha256) and switches; only the
// bytes move this way when the payload answers GET /api/pull.
//
// #include'd ONCE into bm_api.c, after api_sha256 (it uses sha_path_ok, get_q, sb_*, connect_ms, read_conf and the route globals).
//
//   POST /api/pull?src=<host path>&port=<4451|4452>&dest=<console path>&size=<bytes>&conns=<1..16>   (header X-BM-Request: 1)
//        src  /raw/<image.ffpfsc> (imgserve: the whole image file) or /d/<folder>/<inner path> | /t/<TID>[@ver]/<inner> (dirserve)
//        dest a file under <drive>/blz-stage/incoming/ (the only place this ever writes; the same rule as /api/sha256)
//        The same src+dest again RESUMES (pieces already done are kept); another job while one runs = 409.
//   POST /api/pull?cancel=1      stop the running job (the bytes and the piece record stay, so a new POST resumes)
//   GET  /api/pull               {state, src, dest, size, done, rate, eta, conns, pieces, pieces_done, err}
//
// Resume record: <dest>.blzpull, one byte per 8 MiB piece ('1' = written and the connection closed cleanly with every byte).
// Pieces are taken in order, so on exFAT (which zero-fills a write past the end) the gap is never more than conns pieces.
// The server must answer each Range with 206 and a Content-Range whose total is the size asked for: a wrong file is never written.

#define PULL_PIECE     (8LL << 20)
#define PULL_MAX_CONNS 16
#define PULL_TRIES     5              // per piece, then the job fails (mac1 starts it again; the record keeps what was done)
#define PULL_TIMEOUT_S 30

typedef struct {
    char state[12];                   // idle | pulling | done | failed | cancelled
    char src[700], dest[700], err[200];
    int port, conns;
    long long size, done, pieces, pieces_done, next, rate;
    time_t started, finished;
    int cancel, fd, recfd, workers;
    unsigned char *rec;               // the piece record in memory (also in <dest>.blzpull)
    unsigned char *tries;
} pull_t;
static pull_t g_pull = { .state = "idle", .fd = -1, .recfd = -1 };
static pthread_mutex_t g_pull_mu = PTHREAD_MUTEX_INITIALIZER;

static int pull_src_ok(const char *s) {
    if (strncmp(s, "/raw/", 5) && strncmp(s, "/d/", 3) && strncmp(s, "/t/", 3) && strncmp(s, "/hb/", 4)) return 0;   // /hb/: the store (bm_store.c)
    if (strstr(s, "..") || strstr(s, "//")) return 0;
    for (const unsigned char *q = (const unsigned char *)s; *q; q++) if (*q < 0x20 || *q == 0x7f) return 0;
    return 1;
}

// one Range request; the socket is left at the first body byte (the header bytes already read past it go to *pre). -1 = error.
static int pull_open(const conf_t *c, int port, const char *src, long long a, long long b, long long want_total,
                     char *pre, size_t *pre_n, char *why, size_t wn) {
    int via_px = 0, s = route_connect(c->host, port, PULL_TIMEOUT_S, &via_px);    // the daemon's route, LAN tried again every minute
    if (s < 0) { snprintf(why, wn, "connect %s:%d failed (LAN and tailnet)", c->host, port); return -1; }
    struct timeval tv = { PULL_TIMEOUT_S, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char enc[2200], req[2600];
    url_path(enc, sizeof enc, src);
    int rl = via_px
        ? snprintf(req, sizeof req, "GET http://%s:%d%s HTTP/1.0\r\nHost: %s:%d\r\nRange: bytes=%lld-%lld\r\nConnection: close\r\n\r\n",
                   g_ts_host, port, enc, g_ts_host, port, a, b)
        : snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nRange: bytes=%lld-%lld\r\nConnection: close\r\n\r\n", enc, c->host, a, b);
    if (write_all(s, req, (size_t)rl) != 0) { close(s); snprintf(why, wn, "request not sent"); return -1; }
    char h[4096]; size_t have = 0; long body = -1;
    while (body < 0) {
        if (have >= sizeof h - 1) { close(s); snprintf(why, wn, "headers too long"); return -1; }
        ssize_t n = read(s, h + have, sizeof h - 1 - have);
        if (n <= 0) { close(s); snprintf(why, wn, "no answer"); return -1; }
        have += (size_t)n; h[have] = 0;
        char *e = strstr(h, "\r\n\r\n"); if (e) body = (long)(e - h) + 4;
    }
    int st = (have > 12 && !strncmp(h, "HTTP/1.", 7)) ? atoi(h + 9) : 0;
    char *cr = strcasestr(h, "\nContent-Range: bytes ");
    long long ra = -1, rb = -1, rt = -1;
    if (cr) sscanf(cr + 22, "%lld-%lld/%lld", &ra, &rb, &rt);
    if (st != 206 || ra != a || rb != b || rt != want_total) {
        close(s);
        if (st == 206) snprintf(why, wn, "server sent bytes %lld-%lld of %lld, wanted %lld-%lld of %lld", ra, rb, rt, a, b, want_total);
        else snprintf(why, wn, "HTTP %d (not 206)", st);
        return -1;
    }
    *pre_n = have - (size_t)body; memcpy(pre, h + body, *pre_n);
    return s;
}

static void pull_err(const char *fmt, ...) {        // the first error of a job (caller holds g_pull_mu)
    if (g_pull.err[0]) return;
    va_list ap; va_start(ap, fmt); vsnprintf(g_pull.err, sizeof g_pull.err, fmt, ap); va_end(ap);
}

static void *pull_worker(void *arg) {
    (void)arg;
    conf_t *c = malloc(sizeof *c); unsigned char *buf = malloc(1 << 20);
    if (!c || !buf) { free(c); free(buf); pthread_mutex_lock(&g_pull_mu); pull_err("out of memory"); g_pull.workers--; pthread_mutex_unlock(&g_pull_mu); return NULL; }
    read_conf(c);
    for (;;) {
        pthread_mutex_lock(&g_pull_mu);
        long long k = -1;
        while (!g_pull.cancel && !g_pull.err[0] && g_pull.next < g_pull.pieces) {
            long long i = g_pull.next++;
            if (g_pull.rec[i] != '1') { k = i; break; }
        }
        pthread_mutex_unlock(&g_pull_mu);
        if (k < 0) break;
        long long a = k * PULL_PIECE, b = a + PULL_PIECE - 1; if (b >= g_pull.size) b = g_pull.size - 1;
        int ok = 0;
        while (!ok) {
            char why[200] = ""; size_t pn = 0;
            int s = pull_open(c, g_pull.port, g_pull.src, a, b, g_pull.size, (char *)buf, &pn, why, sizeof why);
            long long pos = a;
            if (s >= 0) {
                size_t n = pn;
                for (;;) {
                    if (n) {
                        if ((long long)n > b + 1 - pos) n = (size_t)(b + 1 - pos);      // never past this piece
                        if (pwrite(g_pull.fd, buf, n, (off_t)pos) != (ssize_t)n) { snprintf(why, sizeof why, "write failed (errno %d)", errno); break; }
                        pos += (long long)n;
                        pthread_mutex_lock(&g_pull_mu); g_pull.done += (long long)n; pthread_mutex_unlock(&g_pull_mu);
                    }
                    if (pos > b || g_pull.cancel) break;
                    ssize_t r = read(s, buf, 1 << 20);
                    if (r <= 0) { snprintf(why, sizeof why, "connection ended at %lld of piece %lld", pos - a, k); break; }
                    n = (size_t)r;
                }
                close(s);
                ok = pos > b;
            }
            pthread_mutex_lock(&g_pull_mu);
            if (ok) {
                g_pull.rec[k] = '1'; g_pull.pieces_done++;
                if (pwrite(g_pull.recfd, "1", 1, (off_t)k) != 1) pull_err("piece record not written (errno %d)", errno);
            } else {
                g_pull.done -= pos - a;                                  // the piece is fetched again from its start
                if (g_pull.cancel) { pthread_mutex_unlock(&g_pull_mu); break; }
                if (++g_pull.tries[k] >= PULL_TRIES) pull_err("piece %lld: %s (%d tries)", k, why, PULL_TRIES);
                else logf_("pull: piece %lld: %s - again", k, why);
            }
            int stop = g_pull.err[0] != 0;
            pthread_mutex_unlock(&g_pull_mu);
            if (stop) break;
            if (!ok) sleep(2);
        }
        if (!ok) break;
    }
    free(c); free(buf);
    pthread_mutex_lock(&g_pull_mu); g_pull.workers--; pthread_mutex_unlock(&g_pull_mu);
    return NULL;
}

static void *pull_thread(void *arg) {               // runs the workers, keeps the rate, closes the job
    (void)arg;
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256u << 10);
    pthread_mutex_lock(&g_pull_mu);
    int want = g_pull.conns;
    for (int i = 0; i < want; i++) {
        pthread_t t;
        if (pthread_create(&t, &at, pull_worker, NULL) == 0) { pthread_detach(t); g_pull.workers++; }
    }
    if (!g_pull.workers) pull_err("no worker thread started (errno %d)", errno);
    pthread_mutex_unlock(&g_pull_mu);
    pthread_attr_destroy(&at);
    long long last = -1; time_t lt = time(NULL);
    for (;;) {
        sleep(1);
        pthread_mutex_lock(&g_pull_mu);
        time_t now = time(NULL);
        if (last < 0) { last = g_pull.done; lt = now; }
        else if (now - lt >= 5) { g_pull.rate = (g_pull.done - last) / (now - lt); last = g_pull.done; lt = now; }
        int left = g_pull.workers;
        pthread_mutex_unlock(&g_pull_mu);
        if (!left) break;
    }
    pthread_mutex_lock(&g_pull_mu);
    fsync(g_pull.fd); close(g_pull.fd); g_pull.fd = -1;
    fsync(g_pull.recfd); close(g_pull.recfd); g_pull.recfd = -1;
    g_pull.finished = time(NULL);
    const char *res = g_pull.pieces_done == g_pull.pieces ? "done" : g_pull.cancel ? "cancelled" : "failed";
    if (!strcmp(res, "failed")) pull_err("stopped with %lld of %lld pieces", g_pull.pieces_done, g_pull.pieces);
    snprintf(g_pull.state, sizeof g_pull.state, "%s", res);
    logf_("pull: %s %s -> %s: %lld of %lld B in %lld s%s%s", res, g_pull.src, g_pull.dest, g_pull.done, g_pull.size,
          (long long)(g_pull.finished - g_pull.started), g_pull.err[0] ? " - " : "", g_pull.err);
    free(g_pull.rec); g_pull.rec = NULL; free(g_pull.tries); g_pull.tries = NULL;
    pthread_mutex_unlock(&g_pull_mu);
    return NULL;
}

// Start one pull (the API's POST and the store's install thread). The HTTP status; the JSON answer in o. port 0 = imgserve's.
static int pull_begin(sb_t *o, const char *src, int port, const char *dest, long long size, int conns) {
    conf_t *c = malloc(sizeof *c); if (!c) { sb_puts(o, "{\"error\":\"out of memory\"}"); return 500; }
    read_conf(c);
    if (port <= 0) port = c->port;
    int port_ok = port == c->port || port == c->port + 1;            // imgserve, or dirserve beside it (4451 + 1)
    free(c);
    if (!pull_src_ok(src)) { sb_puts(o, "{\"error\":\"src must be /raw/<image>, /d/<folder>/<file> or /t/<TID>/<file> on the host\"}"); return 400; }
    if (!sha_path_ok(dest)) { sb_puts(o, "{\"error\":\"dest must be a file under <drive>/blz-stage/incoming/\"}"); return 403; }
    if (size <= 0) { sb_puts(o, "{\"error\":\"size (bytes) please\"}"); return 400; }
    if (!port_ok) { sb_puts(o, "{\"error\":\"port must be imgserve's or dirserve's\"}"); return 400; }
    if (conns < 1) conns = 1;
    if (conns > PULL_MAX_CONNS) conns = PULL_MAX_CONNS;
    pthread_mutex_lock(&g_pull_mu);
    if (!strcmp(g_pull.state, "pulling")) {
        int same = !strcmp(g_pull.src, src) && !strcmp(g_pull.dest, dest);
        pthread_mutex_unlock(&g_pull_mu);
        sb_f(o, "{\"error\":\"%s\"}", same ? "already pulling this file" : "another pull is running (one at a time)");
        return 409;
    }
    char fp[800], rp[820]; P(fp, sizeof fp, dest); snprintf(rp, sizeof rp, "%s.blzpull", fp);
    parent_dirs(fp);
    long long pieces = (size + PULL_PIECE - 1) / PULL_PIECE;
    unsigned char *rec = calloc((size_t)pieces, 1), *tries = calloc((size_t)pieces, 1);
    int fd = open(fp, O_RDWR | O_CREAT, 0644), recfd = open(rp, O_RDWR | O_CREAT, 0644);
    struct stat st;
    const char *bad = !rec || !tries ? "out of memory" : fd < 0 ? "dest not opened" : recfd < 0 ? "piece record not opened" : NULL;
    if (!bad && fstat(fd, &st) == 0 && st.st_size > size) bad = "dest is bigger than size: not this file";
    if (bad) {
        pthread_mutex_unlock(&g_pull_mu);
        if (fd >= 0) close(fd);
        if (recfd >= 0) close(recfd);
        free(rec); free(tries);
        sb_f(o, "{\"error\":\"%s\"}", bad); return 500;
    }
    long long have = 0;                                              // a record from an earlier run of the same dest: keep its pieces
    ssize_t rn = pread(recfd, rec, (size_t)pieces, 0);
    for (long long i = 0; i < pieces; i++) {
        if (i >= rn || rec[i] != '1') rec[i] = '0';
        else have += (i == pieces - 1) ? size - i * PULL_PIECE : PULL_PIECE;
    }
    if (pwrite(recfd, rec, (size_t)pieces, 0) != (ssize_t)pieces) logf_("pull: piece record %s not written (errno %d)", rp, errno);
    long long pd = 0; for (long long i = 0; i < pieces; i++) pd += rec[i] == '1';
    snprintf(g_pull.src, sizeof g_pull.src, "%s", src); snprintf(g_pull.dest, sizeof g_pull.dest, "%s", dest);
    g_pull.port = port; g_pull.conns = conns; g_pull.size = size; g_pull.done = have; g_pull.pieces = pieces; g_pull.pieces_done = pd;
    g_pull.next = 0; g_pull.rate = 0; g_pull.started = time(NULL); g_pull.finished = 0; g_pull.cancel = 0; g_pull.err[0] = 0;
    g_pull.fd = fd; g_pull.recfd = recfd; g_pull.rec = rec; g_pull.tries = tries; g_pull.workers = 0;
    snprintf(g_pull.state, sizeof g_pull.state, "pulling");
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256u << 10);
    int r = pthread_create(&t, &at, pull_thread, NULL);
    pthread_attr_destroy(&at);
    if (r != 0) {
        snprintf(g_pull.state, sizeof g_pull.state, "failed"); pull_err("pull thread did not start (%d)", r);
        close(fd); close(recfd); g_pull.fd = g_pull.recfd = -1; free(rec); free(tries); g_pull.rec = g_pull.tries = NULL;
        pthread_mutex_unlock(&g_pull_mu);
        sb_puts(o, "{\"error\":\"pull thread did not start\"}"); return 500;
    }
    pthread_detach(t);
    logf_("pull: start %s (:%d) -> %s, %lld B, %d connections, %lld of %lld pieces already here", src, port, dest, size, conns, pd, pieces);
    pthread_mutex_unlock(&g_pull_mu);
    sb_f(o, "{\"ok\":true,\"pieces\":%lld,\"pieces_done\":%lld,\"done\":%lld}", pieces, pd, have);
    return 200;
}

static int api_pull_start(sb_t *o, const char *query) {              // POST /api/pull: the HTTP status; the JSON answer in o
    char src[700] = "", dest[700] = "", v[32];
    get_q(query, "src", src, sizeof src); get_q(query, "dest", dest, sizeof dest);
    long long size = get_q(query, "size", v, sizeof v) ? atoll(v) : -1;
    int conns = get_q(query, "conns", v, sizeof v) ? atoi(v) : 4;
    int port = get_q(query, "port", v, sizeof v) ? atoi(v) : 0;
    return pull_begin(o, src, port, dest, size, conns);
}

static void api_pull_status(sb_t *o) {
    pthread_mutex_lock(&g_pull_mu);
    long long eta = g_pull.rate > 0 ? (g_pull.size - g_pull.done) / g_pull.rate : -1;
    sb_puts(o, "{\"state\":"); sb_js(o, g_pull.state);
    sb_puts(o, ",\"src\":"); sb_js(o, g_pull.src); sb_puts(o, ",\"dest\":"); sb_js(o, g_pull.dest);
    sb_f(o, ",\"port\":%d,\"size\":%lld,\"done\":%lld,\"rate\":%lld,\"eta\":%lld,\"conns\":%d,\"pieces\":%lld,\"pieces_done\":%lld,"
            "\"started\":%lld,\"finished\":%lld,\"err\":", g_pull.port, g_pull.size, g_pull.done, g_pull.rate, eta, g_pull.conns,
         g_pull.pieces, g_pull.pieces_done, (long long)g_pull.started, (long long)g_pull.finished);
    sb_js(o, g_pull.err); sb_puts(o, "}");
    pthread_mutex_unlock(&g_pull_mu);
}
