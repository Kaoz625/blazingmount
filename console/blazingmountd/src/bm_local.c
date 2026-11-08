// SPDX-License-Identifier: GPL-3.0-or-later
// bm_local.c - v2.14 (8 Nov 2026): move a game between streaming and a local copy, ON the console, from the app or a phone.
// Markus, 8 Nov: "i need a way to move games from the external drives to the internal or the m.2 when i need to."
//
// #include'd ONCE into bm_api.c after bm_store.c (it uses pull_begin/g_pull, scan_local, host_games, local_http, choices_*, sb_*).
// No new payload and no new tool: BlazingMount pulls the image itself (bm_pull.c) and ShadowMount+ registers it the way it
// registers any image in a homebrew folder; we only call its API (POST /api/v1/scan) to make it look sooner.
//
//   POST /api/action?do=local&tid=T&to=m2|internal   streaming -> a local image in /mnt/ext1/homebrew (m2) or /data/homebrew (internal)
//                                                     a local image already on the other drive -> moved there (copied on the console)
//   POST /api/action?do=stream&tid=T                  local image -> streaming again (only when the drives still have the SAME image)
//   POST /api/action?do=lcancel&tid=T                 stop a copy / move; its staged bytes are deleted
//   GET  /api/local                                   every record below, with live progress
// Each is checked in the API thread first and refused at once with the reason (space, running, no M.2, ...); the main loop
// (the one writer) then does it. One job at a time.
//
// The steps (the proven order of mac1's download-to-console.py, docs/DOWNLOAD-TO-CONSOLE.md 5.4/5.5, now on the console):
//   copy   GET /raw/<image> on N Range connections -> <drive>/blz-stage/incoming/<name>.part (resumable; ShadowMount never scans
//          there). The starter keeps streaming the whole time. Done = every piece written, file size == the catalog's size.
//   switch only while the game is NOT running: each starter of the title -> <its drive>/blz-stage/stub/<starter>-off-<stamp>
//          (kept: the way back), /data/blazing/link/<TID>.cfg -> .cfg.off-<stamp>, then the image -> <drive>/homebrew/<name>.
//          Every rename stays on one drive (a cross-drive rename panics the kernel); any failure puts back what moved.
//   back   only while not running, and only when the host lists an image of the title with the SAME size: the image ->
//          <drive>/blz-stage/incoming/<name>.remove-<stamp>, wait for ShadowMount to let go of it ("source removed" or its
//          mount.lnk gone; it does nothing while a game runs, so neither does the clock), then the parked starter + cfg come back
//          (or, with none, the next pass builds a new starter), then the removed image is deleted. ShadowMount silent for
//          LOCAL_SMP_WAIT_S with no game open = the image is put back and still plays.
//   move   local image -> the other drive: copied on the console (no network), switched the same way, the old copy deleted once
//          ShadowMount let go of it.
// /data/blazing/local.tsv remembers each title we made local: while a title has a record, sync_once never builds or rebuilds
// a starter for it (local_guard), on top of scan_present seeing the image itself.

#define LOCAL_PATH "/data/blazing/local.tsv"
#define LOCAL_MAX  128
#ifndef LOCAL_TICK_S
#define LOCAL_TICK_S 5                // while a job runs the main loop looks at it this often
#endif
#ifndef LOCAL_SMP_WAIT_S
#define LOCAL_SMP_WAIT_S 600          // ShadowMount silent this long with no game open = give up waiting (tests: seconds)
#endif
#define LOCAL_M2  "/mnt/ext1"
#define LOCAL_INT "/data"
#define LOCAL_FIELDS 19

typedef struct {
    char tid[16], kind[8], state[12], to[10];   // kind local | move | stream; state below; to m2 | internal
    char name[300], key[48], title[200];        // the image's file name, the host key (TID@bytes), the title
    long long size;
    char image[700];          // the local image (state local), or the one being moved / sent back
    char part[700];           // the staging file while copying / moving
    char starter[700];        // where the parked starter lived
    char parked[700];         // the parked starter (the way back)
    char cfgoff[700];         // the link cfg we renamed off
    char old[700];            // a copy taken out of homebrew, deleted once ShadowMount let go of it
    long long smp_off, at, idle, had_link;
    char msg[240];
} lrec_t;
// states: copying | moving | ready (copied, waits until the game is closed) | local | releasing | cancelling | failed
static lrec_t g_lr[LOCAL_MAX]; static int g_nlr, g_lr_loaded;
static pthread_mutex_t g_lr_mu = PTHREAD_MUTEX_INITIALIZER;   // main thread writes (holding it), API thread reads (holding it)

static void state_forget_tid(const char *tid);                  // bm_api.c, below

// ---------------------------------------------------------------- the console-side copy for a move (one at a time)
typedef struct { char state[12], src[800], dst[800], err[200]; long long size, done, rate; int cancel; } lcopy_t;
static lcopy_t g_lc = { .state = "idle" };
static pthread_mutex_t g_lc_mu = PTHREAD_MUTEX_INITIALIZER;

static void *lcopy_thread(void *arg) {
    (void)arg;
    char s[800], d[800]; long long size;
    pthread_mutex_lock(&g_lc_mu); snprintf(s, sizeof s, "%s", g_lc.src); snprintf(d, sizeof d, "%s", g_lc.dst); size = g_lc.size; pthread_mutex_unlock(&g_lc_mu);
    char ps[900], pd[900]; P(ps, sizeof ps, s); P(pd, sizeof pd, d);
    parent_dirs(pd);
    int in = open(ps, O_RDONLY), out = open(pd, O_WRONLY | O_CREAT, 0644);
    const char *bad = in < 0 ? "the copy on this console cannot be read" : out < 0 ? "the target cannot be written" : NULL;
    unsigned char *buf = malloc(8u << 20);
    if (!bad && !buf) bad = "out of memory";
    long long off = 0;
    if (!bad) {
        struct stat st; if (fstat(out, &st) == 0) off = st.st_size;
        if (off > size) { if (ftruncate(out, 0) != 0) bad = "the target cannot be truncated"; off = 0; }   // not this file
    }
    long long last = off; time_t lt = time(NULL);
    while (!bad && off < size) {
        pthread_mutex_lock(&g_lc_mu); int stop = g_lc.cancel; g_lc.done = off; pthread_mutex_unlock(&g_lc_mu);
        if (stop) break;
        size_t want = (size_t)((size - off) < (8LL << 20) ? (size - off) : (8LL << 20));
        ssize_t n = pread(in, buf, want, (off_t)off);
        if (n <= 0) { bad = "read failed"; break; }
        if (pwrite(out, buf, (size_t)n, (off_t)off) != n) { bad = "write failed (drive full?)"; break; }
        off += n;
        time_t now = time(NULL);
        if (now - lt >= 3) { pthread_mutex_lock(&g_lc_mu); g_lc.rate = (off - last) / (now - lt); pthread_mutex_unlock(&g_lc_mu); last = off; lt = now; }
    }
    if (out >= 0) { fsync(out); close(out); }
    if (in >= 0) close(in);
    free(buf);
    pthread_mutex_lock(&g_lc_mu);
    g_lc.done = off;
    if (bad) { snprintf(g_lc.err, sizeof g_lc.err, "%s (errno %d)", bad, errno); snprintf(g_lc.state, sizeof g_lc.state, "failed"); }
    else snprintf(g_lc.state, sizeof g_lc.state, "%s", off >= size ? "done" : "cancelled");
    pthread_mutex_unlock(&g_lc_mu);
    logf_("local: console copy %s -> %s: %s, %lld of %lld B", s, d, g_lc.state, off, size);
    return NULL;
}

static int lcopy_start(const char *src, const char *dst, long long size) {
    pthread_mutex_lock(&g_lc_mu);
    if (!strcmp(g_lc.state, "copying")) { pthread_mutex_unlock(&g_lc_mu); return -1; }
    memset(&g_lc, 0, sizeof g_lc);
    snprintf(g_lc.state, sizeof g_lc.state, "copying"); snprintf(g_lc.src, sizeof g_lc.src, "%s", src); snprintf(g_lc.dst, sizeof g_lc.dst, "%s", dst);
    g_lc.size = size;
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256u << 10);
    int r = pthread_create(&t, &at, lcopy_thread, NULL);
    pthread_attr_destroy(&at);
    if (r) { snprintf(g_lc.state, sizeof g_lc.state, "failed"); snprintf(g_lc.err, sizeof g_lc.err, "copy thread did not start (%d)", r); }
    else pthread_detach(t);
    pthread_mutex_unlock(&g_lc_mu);
    return r ? -1 : 0;
}

// ---------------------------------------------------------------- records: /data/blazing/local.tsv (main thread writes)
static void lr_clean(char *s) { for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' '; }

static void local_save(void) {                                   // caller holds g_lr_mu
    sb_t s = { 0 };
    sb_puts(&s, "# BlazingMount local copies (blazingmountd 2.14 bm_local.c): tid kind state to name key title size image part starter parked cfgoff old smp_off at idle had_link msg\n");
    for (int i = 0; i < g_nlr; i++) {
        lrec_t *r = &g_lr[i];
        char *str[] = { r->tid, r->kind, r->state, r->to, r->name, r->key, r->title };
        for (int k = 0; k < 7; k++) { lr_clean(str[k]); sb_puts(&s, str[k]); sb_puts(&s, "\t"); }
        sb_f(&s, "%lld\t", r->size);
        char *pth[] = { r->image, r->part, r->starter, r->parked, r->cfgoff, r->old };
        for (int k = 0; k < 6; k++) { lr_clean(pth[k]); sb_puts(&s, pth[k]); sb_puts(&s, "\t"); }
        lr_clean(r->msg);
        sb_f(&s, "%lld\t%lld\t%lld\t%lld\t", r->smp_off, r->at, r->idle, r->had_link);
        sb_puts(&s, r->msg); sb_puts(&s, "\n");
    }
    char p[600], t[640]; P(p, sizeof p, LOCAL_PATH); snprintf(t, sizeof t, "%s.tmp", p);
    if (write_file(t, s.b ? s.b : "", s.n) == 0) rename(t, p);
    free(s.b);
}

static void local_load(void) {                                   // caller holds g_lr_mu
    g_lr_loaded = 1; g_nlr = 0;
    char p[600]; char *m = api_slurp(P(p, sizeof p, LOCAL_PATH), 4 << 20, NULL);
    if (!m) return;
    char *save = NULL;
    for (char *l = strtok_r(m, "\n", &save); l && g_nlr < LOCAL_MAX; l = strtok_r(NULL, "\n", &save)) {
        if (l[0] == '#') continue;
        char *f[LOCAL_FIELDS] = { 0 }; int k = 0; f[k++] = l;
        for (char *q = l; *q && k < LOCAL_FIELDS; q++) if (*q == '\t') { *q = 0; f[k++] = q + 1; }
        if (k < LOCAL_FIELDS || !valid_tid(f[0])) continue;
        lrec_t *r = &g_lr[g_nlr++]; memset(r, 0, sizeof *r);
        snprintf(r->tid, sizeof r->tid, "%s", f[0]); snprintf(r->kind, sizeof r->kind, "%s", f[1]); snprintf(r->state, sizeof r->state, "%s", f[2]);
        snprintf(r->to, sizeof r->to, "%s", f[3]); snprintf(r->name, sizeof r->name, "%s", f[4]); snprintf(r->key, sizeof r->key, "%s", f[5]);
        snprintf(r->title, sizeof r->title, "%s", f[6]); r->size = atoll(f[7]);
        snprintf(r->image, sizeof r->image, "%s", f[8]); snprintf(r->part, sizeof r->part, "%s", f[9]); snprintf(r->starter, sizeof r->starter, "%s", f[10]);
        snprintf(r->parked, sizeof r->parked, "%s", f[11]); snprintf(r->cfgoff, sizeof r->cfgoff, "%s", f[12]); snprintf(r->old, sizeof r->old, "%s", f[13]);
        r->smp_off = atoll(f[14]); r->at = atoll(f[15]); r->idle = atoll(f[16]); r->had_link = atoll(f[17]);
        snprintf(r->msg, sizeof r->msg, "%s", f[18]);
    }
    free(m);
}

static lrec_t *lr_get(const char *tid) { for (int i = 0; i < g_nlr; i++) if (!strcmp(g_lr[i].tid, tid)) return &g_lr[i]; return NULL; }

static int lr_active(const lrec_t *r) {                           // a job that needs the main loop to look at it
    static const char *on[] = { "copying", "moving", "ready", "releasing", "cancelling", 0 };
    for (int i = 0; on[i]; i++) if (!strcmp(r->state, on[i])) return 1;
    return !strcmp(r->state, "local") && (r->old[0] || r->smp_off > 0);
}
static int lr_job(const lrec_t *r) {                              // a job the user started that has not finished (one at a time)
    static const char *on[] = { "copying", "moving", "ready", "releasing", "cancelling", 0 };
    for (int i = 0; on[i]; i++) if (!strcmp(r->state, on[i])) return 1;
    return 0;
}

// main thread: sync_once never builds or rebuilds a starter for a title that has a record (a local copy, or one on its way)
static int local_guard(const char *tid) {
    pthread_mutex_lock(&g_lr_mu);
    if (!g_lr_loaded) local_load();
    int r = lr_get(tid) != NULL;
    pthread_mutex_unlock(&g_lr_mu);
    return r;
}
static int local_busy(void) {
    pthread_mutex_lock(&g_lr_mu);
    if (!g_lr_loaded) local_load();
    int b = 0; for (int i = 0; i < g_nlr; i++) if (lr_active(&g_lr[i])) b = 1;
    pthread_mutex_unlock(&g_lr_mu);
    return b;
}

// ---------------------------------------------------------------- small helpers
static int drive_of(const char *p, char *out, size_t on) {        // "/data/x" -> "/data", "/mnt/ext1/x" -> "/mnt/ext1"
    if (!strncmp(p, "/data/", 6)) { snprintf(out, on, "/data"); return 1; }
    if (!strncmp(p, "/mnt/", 5)) { const char *s = strchr(p + 5, '/'); if (!s) return 0; snprintf(out, on, "%.*s", (int)(s - p), p); return 1; }
    return 0;
}
static const char *where_of(const char *p) {
    if (!strncmp(p, "/mnt/ext1/", 10)) return "m2";
    if (!strncmp(p, "/data/", 6)) return "internal";
    if (!strncmp(p, "/mnt/ext0/", 10)) return "ext0";
    if (!strncmp(p, "/mnt/usb", 8)) return "usb";
    return "";
}
static const char *where_name(const char *w) { return !strcmp(w, "m2") ? "M.2" : !strcmp(w, "internal") ? "console storage" : w; }
static const char *drive_for(const char *to) { return !strcmp(to, "m2") ? LOCAL_M2 : LOCAL_INT; }
static const char *base_of(const char *p) { const char *b = strrchr(p, '/'); return b ? b + 1 : p; }
static void lstamp(char *o, size_t n) {
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    snprintf(o, n, "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}
static int lexists(const char *console_path) { char p[900]; return exists(P(p, sizeof p, console_path)); }
static long long lsize(const char *console_path) { char p[900]; struct stat st; return stat(P(p, sizeof p, console_path), &st) == 0 ? (long long)st.st_size : -1; }

// a rename between two console paths; never across drives (a cross-device rename panics the PS5 kernel)
static int lrename(const char *a, const char *b) {
    char da[64], db[64];
    if (!drive_of(a, da, sizeof da) || !drive_of(b, db, sizeof db) || strcmp(da, db)) {
        logf_("local: REFUSED a cross-drive rename %s -> %s", a, b); errno = EXDEV; return -1;
    }
    char pa[900], pb[900]; P(pa, sizeof pa, a); P(pb, sizeof pb, b);
    parent_dirs(pb);
    if (rename(pa, pb) != 0) { logf_("local: rename %s -> %s failed (errno %d)", a, b, errno); return -1; }
    logf_("local: %s -> %s", a, b);
    return 0;
}
// deletes ONLY a staged file of ours: under <drive>/blz-stage/incoming/ (a .part, its .blzpull, or a .remove-<stamp>)
static void lunlink_staged(const char *console_path) {
    if (!console_path[0] || !strstr(console_path, "/blz-stage/incoming/") || strstr(console_path, "..")) return;
    char p[900]; if (unlink(P(p, sizeof p, console_path)) == 0) logf_("local: deleted %s", console_path);
}

static long long smp_size(void) { char p[600]; struct stat st; return stat(P(p, sizeof p, SMP_LOG), &st) == 0 ? (long long)st.st_size : 0; }
// a ShadowMount log line after byte `off` that names tid and w1 (or w2)
static int smp_seen(long long off, const char *tid, const char *w1, const char *w2) {
    char p[600]; int fd = open(P(p, sizeof p, SMP_LOG), O_RDONLY);
    if (fd < 0) return 0;
    struct stat st; if (fstat(fd, &st) != 0) { close(fd); return 0; }
    if (st.st_size < off) off = 0;                                   // a new log (ShadowMount restarted): all of it is new
    if (st.st_size - off > (4LL << 20)) off = st.st_size - (4LL << 20);
    size_t n = (size_t)(st.st_size - off), got = 0;
    char *m = malloc(n + 1); if (!m) { close(fd); return 0; }
    while (got < n) { ssize_t r = pread(fd, m + got, n - got, (off_t)(off + (long long)got)); if (r <= 0) break; got += (size_t)r; }
    close(fd); m[got] = 0;
    int hit = 0; char *save = NULL;
    for (char *l = strtok_r(m, "\n", &save); l && !hit; l = strtok_r(NULL, "\n", &save))
        if (strstr(l, tid) && (strstr(l, w1) || (w2 && strstr(l, w2)))) hit = 1;
    free(m);
    return hit;
}
static void smp_scan(void) {                                      // ShadowMount+'s own API: look now (it still waits while a game runs)
    sb_t b = { 0 }; int st = local_http(BM_SMP_PORT, "POST", "/api/v1/scan", "{}", &b);
    logf_("local: asked ShadowMount+ to scan: %s", st < 0 ? "its API does not answer (it scans every 15 s anyway)" : "queued");
    free(b.b);
}
static int any_game_running(void) {                              // ShadowMount does nothing while any title runs
    char p[600]; DIR *d = opendir(P(p, sizeof p, "/mnt/sandbox"));
    if (!d) return 0;
    int n = 0; struct dirent *e;
    while ((e = readdir(d))) if ((!strncmp(e->d_name, "PPSA", 4) || !strncmp(e->d_name, "CUSA", 4)) && strstr(e->d_name, "_000")) n = 1;
    closedir(d);
    return n;
}
static long long keep_mb(const conf_t *c, const char *to) { return !strcmp(to, "internal") ? c->local_keep_internal_mb : c->local_keep_m2_mb; }
static int has_backport_in(const char *drive, const char *tid) {  // <drive>/homebrew/backports/<TID>[suffix] (SMP's overlay for this root)
    conf_t *t = malloc(sizeof *t); if (!t) return 0;
    memset(t, 0, sizeof *t); snprintf(t->homebrew_dir, sizeof t->homebrew_dir, "%s/homebrew", drive);
    char bp[1000]; int r = backport_for(t, tid, bp, sizeof bp);
    free(t);
    return r;
}

// ---------------------------------------------------------------- API thread: check, then queue (refused at once with the reason)
static char g_lerr[400];
static const char *lfail(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vsnprintf(g_lerr, sizeof g_lerr, fmt, ap); va_end(ap); return g_lerr; }

static const char *local_check(const char *what, const char *tid, const char *to, bm_act_t *a) {
    if (!valid_tid(tid)) return "bad title id";
    conf_t *c = malloc(sizeof *c); if (!c) return "out of memory";
    read_conf(c);
    const char *err = NULL;
    pthread_mutex_lock(&g_lr_mu);
    if (!g_lr_loaded) local_load();
    lrec_t *mine = lr_get(tid);
    char busy[300] = "";
    for (int i = 0; i < g_nlr; i++) if (lr_job(&g_lr[i])) snprintf(busy, sizeof busy, "%s (%s)", g_lr[i].title[0] ? g_lr[i].title : g_lr[i].tid, g_lr[i].state);
    char mstate[12] = ""; if (mine) snprintf(mstate, sizeof mstate, "%s", mine->state);
    pthread_mutex_unlock(&g_lr_mu);

    if (!strcmp(what, "lcancel")) {
        if (!mine || (strcmp(mstate, "copying") && strcmp(mstate, "moving") && strcmp(mstate, "ready")))
            err = lfail("nothing is being copied for %s", tid);
        else {                                                    // stop the bytes now; the main loop deletes what arrived
            pthread_mutex_lock(&g_pull_mu); if (!strcmp(g_pull.state, "pulling") && !strcmp(g_pull.dest, mine->part)) g_pull.cancel = 1; pthread_mutex_unlock(&g_pull_mu);
            pthread_mutex_lock(&g_lc_mu); if (!strcmp(g_lc.state, "copying")) g_lc.cancel = 1; pthread_mutex_unlock(&g_lc_mu);
        }
        free(c); return err;
    }
    if (busy[0]) { free(c); return lfail("one copy at a time: %s is still going", busy); }
    if (host_games(c, 10) != 0 && !strcmp(what, "stream")) { free(c); return "the drives (mac1) do not answer: the local copy stays"; }
    hg_t *hg = NULL;
    for (int i = 0; i < g_nhg; i++) if (!strcmp(g_hg[i].tid, tid) && strchr(g_hg[i].name, '.')) { hg = &g_hg[i]; break; }
    g_nloc = scan_local(c, g_loc, MAX_LOCAL);
    local_t *L = active_copy(g_loc, g_nloc, tid), *img = NULL;
    for (int j = 0; j < g_nloc; j++) if (!strcmp(g_loc[j].tid, tid) && !strcmp(g_loc[j].kind, "image")) { img = &g_loc[j]; break; }
    memset(a, 0, sizeof *a);
    snprintf(a->what, sizeof a->what, "%s", what); snprintf(a->tid, sizeof a->tid, "%s", tid);
    const char *title = hg && hg->title[0] ? hg->title : tid;
    snprintf(a->title, sizeof a->title, "%s", title);

    if (!strcmp(what, "stream")) {
        if (!img) {
            if (L && !strcmp(L->kind, "pkg")) err = lfail("%s is an installed game: remove it in the PS5's Storage settings", title);
            else if (L && !strcmp(L->kind, "full")) err = lfail("%s is a full copy BlazingMount did not make: left alone", title);
            else err = lfail("%s has no local image on this console (it already streams)", title);
        } else if (game_running(tid)) err = lfail("%s is running: close it first, nothing changed", title);
        else {
            hg_t *same = NULL;
            for (int i = 0; i < g_nhg; i++) if (!strcmp(g_hg[i].tid, tid) && g_hg[i].size == img->bytes) { same = &g_hg[i]; break; }
            if (!same) err = lfail("the drives do not have the same image of %s any more (here %.2f GB%s): the local copy stays", title,
                                   img->bytes / 1e9, hg ? ", a different size on the drives" : ", none on the drives");
            else {
                snprintf(a->key, sizeof a->key, "%s", same->key); snprintf(a->name, sizeof a->name, "%s", base_of(img->path));
                snprintf(a->path, sizeof a->path, "%s", img->path); a->size = img->bytes;
            }
        }
        free(c); return err;
    }
    // local: copy from the drives, or move the local image to the other drive
    if (strcmp(to, "m2") && strcmp(to, "internal")) { free(c); return "to must be m2 or internal"; }
    const char *drive = drive_for(to);
    char dp[600];
    if (!strcmp(to, "m2") && (!exists(P(dp, sizeof dp, LOCAL_M2)) || free_mb(dp) <= 0)) { free(c); return "this console has no M.2 (/mnt/ext1)"; }
    long long size = 0; const char *name = NULL;
    if (img) {
        if (!strcmp(where_of(img->path), to)) { free(c); return lfail("%s is already on the %s", title, where_name(to)); }
        if (game_running(tid)) { free(c); return lfail("%s is running: close it first, nothing changed", title); }
        size = img->bytes; name = base_of(img->path);
        snprintf(a->path, sizeof a->path, "%s", img->path);
        if (hg) snprintf(a->key, sizeof a->key, "%s", hg->key);
    } else {
        if (L && !strcmp(L->kind, "pkg")) { free(c); return lfail("%s is already installed on this console", title); }
        if (L && !strcmp(L->kind, "full")) { free(c); return lfail("%s is already a full copy on this console", title); }
        if (!hg) {
            int folder = 0; for (int i = 0; i < g_nhg; i++) if (!strcmp(g_hg[i].tid, tid)) folder = 1;
            free(c);
            return folder ? lfail("%s is a folder game on the drives: only images can be copied for now", title)
                          : lfail("%s is not on the drives (or mac1 does not answer)", tid);
        }
        size = hg->size; name = hg->name;
        snprintf(a->key, sizeof a->key, "%s", hg->key);
    }
    if (size <= 0) { free(c); return "its size is not known"; }
    if (hg) {
        int cfw = console_fw(c), gfw = fw_hundredths(hg->fw);
        if (cfw && gfw > cfw && !has_backport_in(drive, tid)) {
            free(c);
            return lfail("%s needs FW %s (this console %d.%02d): its backport overlay must be in %s/homebrew/backports first", title, hg->fw, cfw / 100, cfw % 100, drive);
        }
    }
    char part[700]; snprintf(part, sizeof part, "%s/blz-stage/incoming/%s.part", drive, name);
    long long have = lsize(part); if (have < 0 || have > size) have = 0;
    long long fm = free_mb(P(dp, sizeof dp, drive)), need_mb = (size - have) / (1024 * 1024), keep = keep_mb(c, to);
    if (fm < 0 || fm - need_mb < keep) {
        free(c);
        return lfail("not enough space on the %s for %s: needs %.1f GB, %.1f GB free, and %.0f GB must stay free", where_name(to), title,
                     (size - have) / 1e9, fm / 1024.0 * 1.073741824, keep / 1024.0 * 1.073741824);
    }
    if (!img) {
        pthread_mutex_lock(&g_pull_mu); int other = !strcmp(g_pull.state, "pulling") && strcmp(g_pull.dest, part); pthread_mutex_unlock(&g_pull_mu);
        if (other) { free(c); return "another download is running on this console: try again when it is done"; }
    }
    snprintf(a->value, sizeof a->value, "%s", to); snprintf(a->name, sizeof a->name, "%s", name); a->size = size;
    free(c);
    return NULL;
}

static const char *local_enqueue(const char *what, const char *tid, const char *to) {
    static bm_act_t a;                                             // API thread only
    const char *err = local_check(what, tid, to, &a);
    if (err) { logf_("ui: %s %s refused: %s", what, tid, err); return err; }
    pthread_mutex_lock(&g_api_mu);
    if (g_nq >= API_MAXQ) { pthread_mutex_unlock(&g_api_mu); return "queue full"; }
    g_q[g_nq++] = a; g_gen++;
    pthread_cond_signal(&g_api_cv);
    pthread_mutex_unlock(&g_api_mu);
    return NULL;
}

// ---------------------------------------------------------------- main thread: start / switch / back
static void lr_msg(lrec_t *r, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(r->msg, sizeof r->msg, fmt, ap); va_end(ap);
}
static void lr_fail(lrec_t *r, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(r->msg, sizeof r->msg, fmt, ap); va_end(ap);
    snprintf(r->state, sizeof r->state, "failed"); r->at = time(NULL);
    api_recent("%s %s: %s", r->kind, r->tid, r->msg);
}

static void local_start_pull(const conf_t *c, lrec_t *r) {
    sb_t o = { 0 }; char src[700]; snprintf(src, sizeof src, "/raw/%s", r->name);
    int st = pull_begin(&o, src, c->port, r->part, r->size, c->local_conns);
    if (st == 200) lr_msg(r, "copying from the drives");
    else if (st == 409) lr_msg(r, "waits for another download on this console");
    else lr_fail(r, "the copy did not start: %s", o.b ? o.b : "?");
    free(o.b);
}

// applies one queued local / stream / lcancel (bm_api_pass_begin, holding nothing)
static void local_apply(const conf_t *c, const bm_act_t *a) {
    pthread_mutex_lock(&g_lr_mu);
    if (!g_lr_loaded) local_load();
    lrec_t *r = lr_get(a->tid);
    if (!strcmp(a->what, "lcancel")) {
        if (r && (!strcmp(r->state, "copying") || !strcmp(r->state, "moving") || !strcmp(r->state, "ready"))) {
            snprintf(r->state, sizeof r->state, "cancelling"); lr_msg(r, "stopping the copy");
            api_recent("stop copying %s: what arrived is deleted", r->title);
            local_save();
        }
        pthread_mutex_unlock(&g_lr_mu); return;
    }
    for (int i = 0; i < g_nlr; i++) if (lr_job(&g_lr[i])) {           // checked in the API thread too; a second press races it
        api_recent("%s %s: one copy at a time, %s is still going", a->what, a->tid, g_lr[i].tid);
        pthread_mutex_unlock(&g_lr_mu); return;
    }
    if (!r) {
        if (g_nlr >= LOCAL_MAX) { api_recent("%s %s: too many local copies recorded", a->what, a->tid); pthread_mutex_unlock(&g_lr_mu); return; }
        r = &g_lr[g_nlr++]; memset(r, 0, sizeof *r); snprintf(r->tid, sizeof r->tid, "%s", a->tid);
    } else if (!strcmp(r->state, "failed") && r->part[0] && strcmp(a->what, "stream")) {
        char np[700]; snprintf(np, sizeof np, "%s/blz-stage/incoming/%s.part", drive_for(a->value), a->name[0] ? a->name : r->name);
        if (strcmp(np, r->part)) { char rp[720]; lunlink_staged(r->part); snprintf(rp, sizeof rp, "%s.blzpull", r->part); lunlink_staged(rp); }
    }
    snprintf(r->title, sizeof r->title, "%s", a->title); if (a->key[0]) snprintf(r->key, sizeof r->key, "%s", a->key);
    r->at = time(NULL); r->idle = 0;
    if (!strcmp(a->what, "stream")) {
        if (game_running(a->tid)) { lr_fail(r, "%s is running: close it first, nothing changed", r->title); goto done; }
        if (lsize(a->path) != a->size) { lr_fail(r, "the local image changed: nothing done"); goto done; }
        snprintf(r->kind, sizeof r->kind, "stream"); snprintf(r->image, sizeof r->image, "%s", a->path);
        snprintf(r->name, sizeof r->name, "%s", a->name); r->size = a->size;
        char drive[64], st[32], lk[64]; drive_of(a->path, drive, sizeof drive); lstamp(st, sizeof st);
        snprintf(lk, sizeof lk, "/user/app/%s/mount.lnk", a->tid);
        r->had_link = lexists(lk);
        r->smp_off = smp_size();
        snprintf(r->old, sizeof r->old, "%s/blz-stage/incoming/%s.remove-%s", drive, r->name, st);
        if (lrename(a->path, r->old) != 0) { r->old[0] = 0; lr_fail(r, "the image could not be taken out (errno %d): nothing changed", errno); goto done; }
        snprintf(r->state, sizeof r->state, "releasing");
        lr_msg(r, "waiting for ShadowMount to let go of the copy (it waits while a game or app is open)");
        api_recent("back to streaming %s: copy taken out, waiting for ShadowMount", r->title);
        smp_scan();
        goto done;
    }
    // local
    snprintf(r->to, sizeof r->to, "%s", a->value); r->size = a->size;
    const char *drive = drive_for(a->value);
    if (a->path[0]) {                                                  // move the local image to the other drive, on the console
        snprintf(r->kind, sizeof r->kind, "move"); snprintf(r->image, sizeof r->image, "%s", a->path); snprintf(r->name, sizeof r->name, "%s", a->name);
        snprintf(r->part, sizeof r->part, "%s/blz-stage/incoming/%s.part", drive, a->name);
        if (lcopy_start(r->image, r->part, r->size) != 0) { lr_fail(r, "the copy on the console did not start"); goto done; }
        snprintf(r->state, sizeof r->state, "moving"); lr_msg(r, "copying to the %s on the console", where_name(r->to));
        api_recent("move %s to the %s: copying on the console", r->title, where_name(r->to));
    } else {
        snprintf(r->kind, sizeof r->kind, "local"); snprintf(r->name, sizeof r->name, "%s", a->name);
        snprintf(r->part, sizeof r->part, "%s/blz-stage/incoming/%s.part", drive, a->name);
        snprintf(r->state, sizeof r->state, "copying");
        local_start_pull(c, r);
        api_recent("copy %s to the %s: %.1f GB from the drives, it keeps streaming meanwhile", r->title, where_name(r->to), r->size / 1e9);
    }
done:
    local_save();
    pthread_mutex_unlock(&g_lr_mu);
}

// copied and checked, the game not running: the starter out, the image in (caller holds g_lr_mu)
static void local_switch(const conf_t *c, lrec_t *r) {
    char drive[64], st[32], final[800], rec[720];
    drive_of(r->part, drive, sizeof drive); lstamp(st, sizeof st);
    snprintf(final, sizeof final, "%s/homebrew/%s", drive, !strcmp(r->kind, "move") ? base_of(r->image) : r->name);
    snprintf(rec, sizeof rec, "%s.blzpull", r->part);
    if (lexists(final)) { lr_fail(r, "%s is already there: nothing switched (the copy stays in blz-stage/incoming)", final); return; }
    long long off = smp_size();
    char undo_a[4][800], undo_b[4][800]; int nu = 0;                   // what moved, to put back on a failure
    char old[700] = "", parked[700] = "", starter[700] = "", cfgoff[700] = "";
    if (!strcmp(r->kind, "move")) {
        char od[64]; drive_of(r->image, od, sizeof od);
        snprintf(old, sizeof old, "%s/blz-stage/incoming/%s.remove-%s", od, base_of(r->image), st);
        if (lrename(r->image, old) != 0) { lr_fail(r, "the old copy could not be taken out (errno %d): nothing switched", errno); return; }
        snprintf(undo_a[nu], 800, "%s", old); snprintf(undo_b[nu++], 800, "%s", r->image);
    } else {
        local_t *lo = malloc(sizeof *lo * MAX_LOCAL);
        int n = lo ? scan_local(c, lo, MAX_LOCAL) : 0;
        for (int j = 0; j < n && nu < 3; j++) {
            if (strcmp(lo[j].tid, r->tid) || strcmp(lo[j].kind, "starter")) continue;
            char sd[64], pk[700]; drive_of(lo[j].path, sd, sizeof sd);
            snprintf(pk, sizeof pk, "%s/blz-stage/stub/%s-off-%s", sd, base_of(lo[j].path), st);
            if (lrename(lo[j].path, pk) != 0) { free(lo); lo = NULL; goto undo; }
            snprintf(undo_a[nu], 800, "%s", pk); snprintf(undo_b[nu++], 800, "%s", lo[j].path);
            if (!parked[0]) { snprintf(parked, sizeof parked, "%s", pk); snprintf(starter, sizeof starter, "%s", lo[j].path); }
        }
        free(lo);
        char cfg[200]; snprintf(cfg, sizeof cfg, "%s/%s.cfg", LINK_DIR, r->tid);
        if (lexists(cfg)) {
            snprintf(cfgoff, sizeof cfgoff, "%s.off-%s", cfg, st);
            if (lrename(cfg, cfgoff) != 0) goto undo;
            snprintf(undo_a[nu], 800, "%s", cfgoff); snprintf(undo_b[nu++], 800, "%s", cfg);
        }
    }
    {
        char hp[900]; makedirs(P(hp, sizeof hp, drive)); snprintf(hp + strlen(hp), sizeof hp - strlen(hp), "/homebrew"); makedirs(hp);
    }
    if (lrename(r->part, final) != 0) goto undo;
    lunlink_staged(rec);
    if (!strcmp(r->kind, "move")) snprintf(r->old, sizeof r->old, "%s", old);
    else { snprintf(r->parked, sizeof r->parked, "%s", parked); snprintf(r->starter, sizeof r->starter, "%s", starter); snprintf(r->cfgoff, sizeof r->cfgoff, "%s", cfgoff); }
    snprintf(r->image, sizeof r->image, "%s", final); r->part[0] = 0;
    snprintf(r->to, sizeof r->to, "%s", where_of(final));
    snprintf(r->state, sizeof r->state, "local"); r->smp_off = off; r->idle = 0; r->at = time(NULL);
    lr_msg(r, "on the %s: ShadowMount registers it (when no game or app is open)", where_name(r->to));
    {
        char nt[400]; snprintf(nt, sizeof nt, "%s is on your %s now: it plays without the network.", r->title, where_name(r->to)); bm_notice(nt);
    }
    api_recent("%s is on the %s now%s", r->title, where_name(r->to), parked[0] ? " (its starter is parked: Back to streaming restores it)" : "");
    smp_scan();
    return;
undo:
    {
        int e = errno;
        for (int k = nu - 1; k >= 0; k--) lrename(undo_a[k], undo_b[k]);
        lr_fail(r, "the switch failed (errno %d): everything was put back, the copy stays in blz-stage/incoming", e);
    }
}

// ShadowMount let go of the image: the starter back (or built again next pass), the image deleted (caller holds g_lr_mu)
static int local_back(lrec_t *r) {                                    // 1 = the record is finished
    int restored = 0; char why[200] = "";
    if (r->parked[0] && lexists(r->parked) && r->starter[0] && !lexists(r->starter)) {
        char fk[900]; snprintf(fk, sizeof fk, "%s/fakelib/libSceAmpr.sprx", r->parked);
        char cp[900]; char *cfg = r->cfgoff[0] ? api_slurp(P(cp, sizeof cp, r->cfgoff), 1 << 16, NULL) : NULL;
        char want[80]; snprintf(want, sizeof want, "\nbase /t/%s\n", r->key);
        if (!lexists(fk)) snprintf(why, sizeof why, "the parked starter has no fakelib");
        else if (!cfg || !r->key[0] || !strstr(cfg, want)) snprintf(why, sizeof why, "the parked starter streams another image");
        else {
            char cfgp[200]; snprintf(cfgp, sizeof cfgp, "%s/%s.cfg", LINK_DIR, r->tid);
            if (lrename(r->parked, r->starter) == 0) {
                restored = 1;
                if (!lexists(cfgp)) lrename(r->cfgoff, cfgp);
            } else snprintf(why, sizeof why, "the parked starter could not be moved back");
        }
        free(cfg);
    }
    if (!restored) {                                                   // the next pass builds a new starter
        choices_t *ch = malloc(sizeof *ch);
        if (ch) { choices_load(ch); choice_set(ch, r->tid, ch->autob ? "" : "want"); choices_save(ch); free(ch); }
        state_forget_tid(r->tid);
    }
    lunlink_staged(r->old);
    char nt[400]; snprintf(nt, sizeof nt, "%s streams from your drives again (%s).", r->title, restored ? "its starter is back" : "a new starter is being built");
    bm_notice(nt);
    api_recent("back to streaming %s: local copy deleted, %s%s%s", r->title, restored ? "starter restored" : "a new starter is built now",
               why[0] ? " - " : "", why);
    smp_scan();
    api_wake();
    return 1;
}

// the main loop, every LOCAL_TICK_S while a job runs (bm_api_wait) and at the start of every pass
static time_t g_lt_last;
static void local_tick(void) {
    conf_t *c = malloc(sizeof *c); if (!c) return;
    read_conf(c);
    pthread_mutex_lock(&g_lr_mu);
    if (!g_lr_loaded) local_load();
    time_t now = time(NULL); long long dt = g_lt_last ? now - g_lt_last : 0; if (dt < 0 || dt > 60) dt = 0; g_lt_last = now;
    int changed = 0, any = any_game_running();
    for (int i = 0; i < g_nlr; i++) {
        lrec_t *r = &g_lr[i];
        char before[sizeof r->state + sizeof r->msg + sizeof r->old + 8]; snprintf(before, sizeof before, "%s|%s|%s", r->state, r->msg, r->old);
        if (!strcmp(r->state, "copying")) {
            char src[700]; snprintf(src, sizeof src, "/raw/%s", r->name);
            pthread_mutex_lock(&g_pull_mu);
            int ours = !strcmp(g_pull.dest, r->part) && !strcmp(g_pull.src, src);
            char ps[12], err[200]; snprintf(ps, sizeof ps, "%s", g_pull.state); snprintf(err, sizeof err, "%s", g_pull.err);
            pthread_mutex_unlock(&g_pull_mu);
            if (ours && !strcmp(ps, "pulling")) { if (strcmp(r->msg, "copying from the drives")) lr_msg(r, "copying from the drives"); }
            else if (ours && !strcmp(ps, "done")) {
                long long got = lsize(r->part);
                if (got == r->size) { snprintf(r->state, sizeof r->state, "ready"); lr_msg(r, "copied and checked (%.1f GB)", r->size / 1e9); }
                else lr_fail(r, "the copy is %lld B, the drives' image %lld B: not used", got, r->size);
            } else if (ours && !strcmp(ps, "failed")) lr_fail(r, "the copy stopped: %s (what arrived is kept: Copy again resumes, Cancel deletes it)", err);
            else if (ours && !strcmp(ps, "cancelled")) { snprintf(r->state, sizeof r->state, "cancelling"); }
            else local_start_pull(c, r);                                    // after a restart, or another download finished
        } else if (!strcmp(r->state, "moving")) {
            pthread_mutex_lock(&g_lc_mu);
            int ours = !strcmp(g_lc.dst, r->part); char ls[12], err[200]; snprintf(ls, sizeof ls, "%s", g_lc.state); snprintf(err, sizeof err, "%s", g_lc.err);
            pthread_mutex_unlock(&g_lc_mu);
            if (ours && !strcmp(ls, "done")) {
                if (lsize(r->part) == r->size && lsize(r->image) == r->size) { snprintf(r->state, sizeof r->state, "ready"); lr_msg(r, "copied and checked (%.1f GB)", r->size / 1e9); }
                else lr_fail(r, "the copy on the %s does not have the image's size: not used", where_name(r->to));
            } else if (ours && !strcmp(ls, "failed")) lr_fail(r, "the copy stopped: %s (the game still plays from where it was)", err);
            else if (ours && !strcmp(ls, "cancelled")) snprintf(r->state, sizeof r->state, "cancelling");
            else if (!ours || strcmp(ls, "copying")) {                    // after a restart: go on from what is there
                if (lcopy_start(r->image, r->part, r->size) != 0) lr_fail(r, "the copy on the console did not start");
            }
        } else if (!strcmp(r->state, "ready")) {
            if (game_running(r->tid)) lr_msg(r, "copied: it switches when %s is closed", r->title);
            else local_switch(c, r);
        } else if (!strcmp(r->state, "cancelling")) {
            pthread_mutex_lock(&g_pull_mu); int p = !strcmp(g_pull.state, "pulling") && !strcmp(g_pull.dest, r->part); if (p) g_pull.cancel = 1; pthread_mutex_unlock(&g_pull_mu);
            pthread_mutex_lock(&g_lc_mu); int m = !strcmp(g_lc.state, "copying") && !strcmp(g_lc.dst, r->part); if (m) g_lc.cancel = 1; pthread_mutex_unlock(&g_lc_mu);
            if (!p && !m) {
                char rp[720]; snprintf(rp, sizeof rp, "%s.blzpull", r->part);
                lunlink_staged(r->part); lunlink_staged(rp);
                api_recent("copy of %s stopped: its staged bytes are deleted, nothing else changed", r->title);
                if (!strcmp(r->kind, "move")) { snprintf(r->state, sizeof r->state, "local"); r->part[0] = 0; lr_msg(r, "move stopped: still on the %s", where_name(where_of(r->image))); snprintf(r->to, sizeof r->to, "%s", where_of(r->image)); }
                else snprintf(r->state, sizeof r->state, "gone");
            }
        } else if (!strcmp(r->state, "releasing")) {
            char lk[64]; snprintf(lk, sizeof lk, "/user/app/%s/mount.lnk", r->tid);
            if (smp_seen(r->smp_off, r->tid, "source removed", "removed stale mount link") || (r->had_link && !lexists(lk))) {
                if (local_back(r)) snprintf(r->state, sizeof r->state, "gone");
            } else if (!any) {
                r->idle += dt;
                if (r->idle >= LOCAL_SMP_WAIT_S) {
                    if (lrename(r->old, r->image) == 0) {
                        r->old[0] = 0; snprintf(r->state, sizeof r->state, "local"); r->smp_off = 0;
                        lr_msg(r, "back to streaming did not happen: ShadowMount kept the copy for %d %s with no game open; the copy is back and still plays",
                               LOCAL_SMP_WAIT_S >= 120 ? LOCAL_SMP_WAIT_S / 60 : LOCAL_SMP_WAIT_S, LOCAL_SMP_WAIT_S >= 120 ? "min" : "s");
                        api_recent("back to streaming %s: %s", r->title, r->msg);
                    } else lr_fail(r, "ShadowMount kept the copy and it could not be put back: it is at %s", r->old);
                }
            } else lr_msg(r, "waiting for ShadowMount: it does nothing while a game or app is open");
        } else if (!strcmp(r->state, "local") && r->old[0]) {           // a move: the old copy goes once ShadowMount let go of it
            if (smp_seen(r->smp_off, r->tid, "source removed", "mount.lnk created") || (!any && (r->idle += dt) >= LOCAL_SMP_WAIT_S)) {
                lunlink_staged(r->old); r->old[0] = 0;
                lr_msg(r, "on the %s (the old copy is deleted)", where_name(r->to));
            }
        } else if (!strcmp(r->state, "local") && r->smp_off > 0) {
            if (smp_seen(r->smp_off, r->tid, "mount.lnk created", NULL)) { r->smp_off = 0; lr_msg(r, "plays from the %s", where_name(r->to)); }
            else if (!any && (r->idle += dt) >= 3 * LOCAL_SMP_WAIT_S) { r->smp_off = 0; lr_msg(r, "on the %s; ShadowMount has not confirmed it yet", where_name(r->to)); }
        }
        char after[sizeof before]; snprintf(after, sizeof after, "%s|%s|%s", r->state, r->msg, r->old);
        if (strcmp(before, after)) changed = 1;
    }
    int o = 0; for (int i = 0; i < g_nlr; i++) if (strcmp(g_lr[i].state, "gone")) { if (o != i) g_lr[o] = g_lr[i]; o++; }
    if (o != g_nlr) { g_nlr = o; changed = 1; }
    if (changed) { local_save(); pthread_mutex_lock(&g_api_mu); g_gen++; pthread_mutex_unlock(&g_api_mu); }
    pthread_mutex_unlock(&g_lr_mu);
    free(c);
}

// ---------------------------------------------------------------- reads (API thread)
static void lr_json(sb_t *o, const lrec_t *r) {                       // caller holds g_lr_mu
    long long done = 0, rate = 0;
    if (!strcmp(r->state, "copying")) {
        pthread_mutex_lock(&g_pull_mu); if (!strcmp(g_pull.dest, r->part)) { done = g_pull.done; rate = g_pull.rate; } pthread_mutex_unlock(&g_pull_mu);
    } else if (!strcmp(r->state, "moving")) {
        pthread_mutex_lock(&g_lc_mu); if (!strcmp(g_lc.dst, r->part)) { done = g_lc.done; rate = g_lc.rate; } pthread_mutex_unlock(&g_lc_mu);
    } else if (!strcmp(r->state, "ready") || !strcmp(r->state, "local")) done = r->size;
    sb_puts(o, "{\"tid\":"); sb_js(o, r->tid); sb_puts(o, ",\"kind\":"); sb_js(o, r->kind); sb_puts(o, ",\"state\":"); sb_js(o, r->state);
    sb_puts(o, ",\"to\":"); sb_js(o, r->to); sb_puts(o, ",\"title\":"); sb_js(o, r->title); sb_puts(o, ",\"name\":"); sb_js(o, r->name);
    sb_f(o, ",\"size\":%lld,\"done\":%lld,\"rate\":%lld,\"eta\":%lld,\"at\":%lld,\"image\":", r->size, done, rate,
         rate > 0 ? (r->size - done) / rate : -1LL, r->at);
    sb_js(o, r->image); sb_puts(o, ",\"parked\":"); sb_js(o, r->parked); sb_puts(o, ",\"msg\":"); sb_js(o, r->msg);
    sb_f(o, ",\"active\":%s}", lr_job(r) ? "true" : "false");
}

static void api_local(sb_t *o) {
    pthread_mutex_lock(&g_lr_mu);
    if (!g_lr_loaded) local_load();
    sb_puts(o, "{\"jobs\":[");
    for (int i = 0; i < g_nlr; i++) { if (i) sb_puts(o, ","); lr_json(o, &g_lr[i]); }
    sb_puts(o, "]}");
    pthread_mutex_unlock(&g_lr_mu);
}

// one library row's v2.14 fields: where its local copy is, and its record (null when none)
static void local_row(sb_t *o, const char *tid, const local_t *L) {
    sb_puts(o, ",\"where\":"); sb_js(o, L && strcmp(L->kind, "starter") ? where_of(L->path) : "");
    pthread_mutex_lock(&g_lr_mu);
    if (!g_lr_loaded) local_load();
    lrec_t *r = lr_get(tid);
    sb_puts(o, ",\"lj\":");
    if (r) lr_json(o, r); else sb_puts(o, "null");
    pthread_mutex_unlock(&g_lr_mu);
}

static void local_status(sb_t *o, const conf_t *c) {
    char busy[16] = "";
    pthread_mutex_lock(&g_lr_mu);
    if (!g_lr_loaded) local_load();
    for (int i = 0; i < g_nlr; i++) if (lr_job(&g_lr[i])) snprintf(busy, sizeof busy, "%s", g_lr[i].tid);
    pthread_mutex_unlock(&g_lr_mu);
    char dp[600]; int m2 = exists(P(dp, sizeof dp, LOCAL_M2)) && free_mb(dp) > 0;
    sb_f(o, ",\"local\":{\"v\":1,\"m2\":%s,\"keep_internal_mb\":%lld,\"keep_m2_mb\":%lld,\"busy\":", m2 ? "true" : "false",
         c->local_keep_internal_mb, c->local_keep_m2_mb);
    sb_js(o, busy); sb_puts(o, "}");
}
