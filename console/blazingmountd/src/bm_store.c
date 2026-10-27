// SPDX-License-Identifier: GPL-3.0-or-later
// bm_store.c - v2.9: the homebrew STORE (Markus 26 Oct 2026: "the store will list all three, show which are installed, install each
// with one press"). The host's imgserve lists "Apps and Emulators/PS5" (/homebrew.json) and serves its files (/hb/<path>, ?list).
// Three kinds, three installs, one at a time:
//   app      a homebrew folder (sce_sys/param.json): every file pulled by bm_pull.c into <drive>/blz-stage/incoming/store-<n>/,
//            run bits set like a starter (file_mode), then the folder renamed into the homebrew dir (ShadowMount registers the tile)
//   payload  an ELF: pulled the same way, then put in its own payload-manager slot /data/pldmgr/payloads/local/<slot>/ with a
//            meta.json; NOT loaded and NOT added to the boot list (that stays Markus's choice in the payload manager)
//   pkg      v2.10: BlazingMount's own installer (bm_install.c, progress in /api/install; PKG Manager is its fallback)
// #include'd ONCE into bm_api.c after api_pm (it uses pull_begin, g_pull, pm_http, api_get, js_str, read_conf, file_mode).
//
//   GET  /api/store                         {"feed": <host /homebrew.json or null>, "have": {"folders": [...], "slots": [...]}, "job": {...}}
//   POST /api/store/install?kind=&path=&name=   (header X-BM-Request: 1) -> 409 while a store install or a pull runs

static void reply(int s, int code, const char *ctype, const char *body, size_t n);   // below in bm_api.c

#define STORE_SLOTS "/data/pldmgr/payloads/local"
#define STORE_PKG_SHARE "mac1-8TB"          // the 8TB's SMB share on the host (PKG Manager's mac1 source)

typedef struct {
    char state[12];                         // idle | running | done | failed
    char kind[10], path[512], name[200], step[200], err[200];
    int files, files_done;
    long long bytes, bytes_done;
    time_t started, finished;
} store_job_t;
static store_job_t g_store = { .state = "idle" };
static pthread_mutex_t g_store_mu = PTHREAD_MUTEX_INITIALIZER;

static void store_set(const char *state, const char *fmt, ...) {
    pthread_mutex_lock(&g_store_mu);
    if (state) snprintf(g_store.state, sizeof g_store.state, "%s", state);
    va_list ap; va_start(ap, fmt);
    vsnprintf(!strcmp(g_store.state, "failed") ? g_store.err : g_store.step, sizeof g_store.step, fmt, ap);
    va_end(ap);
    if (state && strcmp(state, "running")) g_store.finished = time(NULL);
    pthread_mutex_unlock(&g_store_mu);
}

static int store_name_ok(const char *s) {                // one path below the shelf: no "..", no hidden parts, no control bytes
    if (!*s || strstr(s, "..") || strstr(s, "//") || s[0] == '/' || s[0] == '.' || strstr(s, "/.")) return 0;
    for (const unsigned char *q = (const unsigned char *)s; *q; q++) if (*q < 0x20 || *q == 0x7f) return 0;
    return 1;
}

static void store_slug(const char *in, char *out, size_t on) {   // a payload slot name: lowercase a-z 0-9 - _
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < on; p++) {
        char ch = (char)tolower((unsigned char)*p);
        out[o++] = (isalnum((unsigned char)ch) || ch == '-' || ch == '_') ? ch : '-';
    }
    out[o] = 0;
}

// Pull one host file to dest (a console path under incoming/) and wait for it. 0 = whole, else -1 with the reason in g_store.err.
static int store_pull_one(const char *src, const char *dest, long long size) {
    sb_t o = { 0 };
    int st = pull_begin(&o, src, 0, dest, size, size > (64LL << 20) ? 4 : 1);
    if (st != 200) { store_set("failed", "%s: %s", src, o.b ? o.b : "pull refused"); free(o.b); return -1; }
    free(o.b);
    for (;;) {
        usleep(200 * 1000);
        pthread_mutex_lock(&g_pull_mu);
        int running = !strcmp(g_pull.state, "pulling");
        int ok = !strcmp(g_pull.state, "done");
        char err[200]; snprintf(err, sizeof err, "%s", g_pull.err);
        pthread_mutex_unlock(&g_pull_mu);
        if (running) continue;
        if (!ok) { store_set("failed", "%s: %s", src, err[0] ? err : "the pull stopped"); return -1; }
        char rec[800], fp[800]; snprintf(rec, sizeof rec, "%s.blzpull", P(fp, sizeof fp, dest)); unlink(rec);
        chmod(fp, file_mode(fp));                               // eboot.bin / .sprx / .prx / .elf 0755, the rest 0644
        return 0;
    }
}

static void *store_thread(void *arg) {
    (void)arg;
    conf_t *c = malloc(sizeof *c);
    if (!c) { store_set("failed", "out of memory"); return NULL; }
    read_conf(c);
    char kind[10], path[512], name[200];
    pthread_mutex_lock(&g_store_mu);
    snprintf(kind, sizeof kind, "%s", g_store.kind); snprintf(path, sizeof path, "%s", g_store.path); snprintf(name, sizeof name, "%s", g_store.name);
    pthread_mutex_unlock(&g_store_mu);
    char drive[300]; snprintf(drive, sizeof drive, "%s", c->homebrew_dir);   // incoming/ sits on the homebrew dir's drive
    char *hb = strstr(drive, "/homebrew"); if (hb) *hb = 0; if (!drive[0]) snprintf(drive, sizeof drive, "/data");
    char slug[120]; store_slug(name[0] ? name : path, slug, sizeof slug);
    char inc[700], src[1100], dest[1400], fp[1400], fp2[1400];
    snprintf(inc, sizeof inc, "%s/blz-stage/incoming/store-%s", drive, slug);

    if (!strcmp(kind, "pkg")) {                                   // v2.10: BlazingMount's own installer (bm_install.c; PKG Manager is its fallback)
        sb_t b = { 0 }; char rel[700]; snprintf(rel, sizeof rel, "Apps and Emulators/PS5/%s", path);
        int st = inst_begin_shelf(rel, name[0] ? name : NULL, &b);
        logf_("store: pkg %s -> install job HTTP %d", rel, st);
        if (st == 200) store_set("done", "installing it (Install tab shows the progress)");
        else store_set("failed", "%s", b.b ? b.b : "refused");
        free(b.b); free(c); return NULL;
    }
    if (!strcmp(kind, "payload")) {
        const char *file = strrchr(path, '/'); file = file ? file + 1 : path;
        char slotdir[600]; snprintf(slotdir, sizeof slotdir, "%s/%s", STORE_SLOTS, slug);
        if (exists(P(fp, sizeof fp, slotdir))) { store_set("failed", "a payload slot %s is already there", slotdir); free(c); return NULL; }
        long long sz;                                             // the feed's size (the pull checks it against the host's Content-Range)
        snprintf(src, sizeof src, "/hb/%s", path);
        pthread_mutex_lock(&g_store_mu); sz = g_store.bytes; g_store.files = 1; pthread_mutex_unlock(&g_store_mu);
        snprintf(dest, sizeof dest, "%s/%s", inc, file);
        store_set("running", "pulling %s", file);
        if (store_pull_one(src, dest, sz) != 0) { free(c); return NULL; }
        makedirs(P(fp, sizeof fp, slotdir));
        snprintf(fp2, sizeof fp2, "%s/%s", fp, file);
        if (rename(P(fp, sizeof fp, dest), fp2) != 0) { store_set("failed", "could not move it into %s (errno %d)", slotdir, errno); free(c); return NULL; }
        chmod(fp2, 0755);
        char meta[900], mp[700]; time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
        int ml = snprintf(meta, sizeof meta, "{\n  \"schema\": 2,\n  \"kind\": \"local\",\n  \"filename\": \"%s\",\n  \"installed_at\": \"%04d-%02d-%02dT%02d:%02d:%02d\",\n"
                          "  \"origin\": \"lan\",\n  \"origin_detail\": \"BlazingMount store: Apps and Emulators/PS5/%s\"\n}\n",
                          file, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, path);
        snprintf(mp, sizeof mp, "%s/meta.json", P(fp, sizeof fp, slotdir));
        write_file(mp, meta, (size_t)ml);
        rmdir(P(fp, sizeof fp, inc));
        pthread_mutex_lock(&g_store_mu); g_store.files_done = 1; g_store.bytes_done = sz; pthread_mutex_unlock(&g_store_mu);
        logf_("store: payload %s -> %s (not loaded, not in the boot list)", path, slotdir);
        store_set("done", "in the payload manager as local/%s (load it there)", slug);
        free(c); return NULL;
    }
    // app folder: the host's file list, every file pulled, then one rename into the homebrew dir
    char target[700]; snprintf(target, sizeof target, "%s/%s", c->homebrew_dir, path);
    if (exists(P(fp, sizeof fp, target))) { store_set("failed", "%s is already on this console", target); free(c); return NULL; }
    char lp[700], *list = NULL; snprintf(lp, sizeof lp, "/hb/%s/?list", path);
    if (api_get(c, lp, &list, 30) < 0 || !list) { store_set("failed", "the host has no file list for %s (imgserve /hb)", path); free(c); return NULL; }
    int nfiles = 0; long long total = 0;
    for (const char *p = list; (p = strstr(p, "\"size\":")); p += 7) { total += atoll(p + 7); nfiles++; }
    pthread_mutex_lock(&g_store_mu); g_store.files = nfiles; g_store.bytes = total; pthread_mutex_unlock(&g_store_mu);
    int done = 0; long long bytes = 0;
    for (const char *p = list; (p = strstr(p, "\"path\":")); ) {
        p += 7; while (*p == ' ') p++;                            // "path": "x" (with or without the space)
        if (*p != '"') continue;
        char rel[600]; p = js_str(p, rel, sizeof rel);
        const char *sp = strstr(p, "\"size\":"); long long sz = sp ? atoll(sp + 7) : -1;
        if (!store_name_ok(rel) || sz < 0) { store_set("failed", "a bad file name in the host's list: %s", rel); free(list); free(c); return NULL; }
        snprintf(src, sizeof src, "/hb/%s/%s", path, rel);
        snprintf(dest, sizeof dest, "%s/%s", inc, rel);
        store_set("running", "%d of %d files: %s", done + 1, nfiles, rel);
        if (sz == 0) { parent_dirs(P(fp, sizeof fp, dest)); write_file(fp, "", 0); chmod(fp, file_mode(fp)); }
        else if (store_pull_one(src, dest, sz) != 0) { free(list); free(c); return NULL; }
        done++; bytes += sz;
        pthread_mutex_lock(&g_store_mu); g_store.files_done = done; g_store.bytes_done = bytes; pthread_mutex_unlock(&g_store_mu);
    }
    free(list);
    parent_dirs(P(fp, sizeof fp, target));
    if (rename(P(fp, sizeof fp, inc), P(fp2, sizeof fp2, target)) != 0) { store_set("failed", "could not move it into %s (errno %d)", target, errno); free(c); return NULL; }
    logf_("store: app %s -> %s (%d files, %lld B)", path, target, done, bytes);
    store_set("done", "on the console in %s", target);
    free(c);
    return NULL;
}

static void api_store(int s, const char *method, const char *target, const char *query, const char *req) {
    sb_t b = { 0 };
    if (!strcmp(method, "GET") && !strcmp(target, "/api/store")) {
        conf_t *c = malloc(sizeof *c); if (!c) { reply(s, 500, "text/plain", "oom", 3); return; }
        read_conf(c);
        char *feed = NULL;
        sb_puts(&b, "{\"feed\":");
        if (api_get(c, "/homebrew.json", &feed, 15) >= 0 && feed && feed[0] == '[') sb_puts(&b, feed); else sb_puts(&b, "null");
        free(feed);
        sb_puts(&b, ",\"have\":{\"folders\":[");
        static const char *dirs[] = { "/data/homebrew", "/mnt/ext0/homebrew", "/mnt/ext1/homebrew", "/mnt/usb0/homebrew", "/mnt/usb1/homebrew", 0 };
        int n = 0; char dp[700];
        for (int i = 0; dirs[i]; i++) {
            DIR *d = opendir(P(dp, sizeof dp, dirs[i])); if (!d) continue;
            struct dirent *e;
            while ((e = readdir(d))) if (e->d_name[0] != '.') { if (n++) sb_puts(&b, ","); sb_js(&b, e->d_name); }
            closedir(d);
        }
        sb_puts(&b, "],\"slots\":["); n = 0;
        DIR *d = opendir(P(dp, sizeof dp, STORE_SLOTS));
        if (d) { struct dirent *e; while ((e = readdir(d))) if (e->d_name[0] != '.') { if (n++) sb_puts(&b, ","); sb_js(&b, e->d_name); } closedir(d); }
        pthread_mutex_lock(&g_store_mu);
        sb_puts(&b, "]},\"job\":{\"state\":"); sb_js(&b, g_store.state);
        sb_puts(&b, ",\"kind\":"); sb_js(&b, g_store.kind); sb_puts(&b, ",\"path\":"); sb_js(&b, g_store.path);
        sb_puts(&b, ",\"name\":"); sb_js(&b, g_store.name); sb_puts(&b, ",\"step\":"); sb_js(&b, g_store.step);
        sb_puts(&b, ",\"err\":"); sb_js(&b, g_store.err);
        sb_f(&b, ",\"files\":%d,\"files_done\":%d,\"bytes\":%lld,\"bytes_done\":%lld,\"started\":%lld,\"finished\":%lld}}",
             g_store.files, g_store.files_done, g_store.bytes, g_store.bytes_done, (long long)g_store.started, (long long)g_store.finished);
        pthread_mutex_unlock(&g_store_mu);
        free(c);
        reply_json(s, 200, &b); free(b.b); return;
    }
    if (!strcmp(method, "POST") && !strcmp(target, "/api/store/install")) {
        if (!strcasestr(req, "\nX-BM-Request: 1")) { sb_puts(&b, "{\"ok\":false,\"error\":\"missing X-BM-Request header\"}"); reply_json(s, 403, &b); free(b.b); return; }
        char kind[16] = "", path[512] = "", name[200] = "", v[32] = "";
        get_q(query, "kind", kind, sizeof kind); get_q(query, "path", path, sizeof path); get_q(query, "name", name, sizeof name);
        long long size = get_q(query, "size", v, sizeof v) ? atoll(v) : -1;
        if ((strcmp(kind, "app") && strcmp(kind, "payload") && strcmp(kind, "pkg")) || !store_name_ok(path) || (!strcmp(kind, "payload") && size <= 0)) {
            sb_puts(&b, "{\"ok\":false,\"error\":\"kind app|payload|pkg, a path from the store feed (and size for a payload)\"}"); reply_json(s, 400, &b); free(b.b); return;
        }
        pthread_mutex_lock(&g_store_mu);
        pthread_mutex_lock(&g_pull_mu); int pulling = !strcmp(g_pull.state, "pulling"); pthread_mutex_unlock(&g_pull_mu);
        if (!strcmp(g_store.state, "running") || pulling) {
            pthread_mutex_unlock(&g_store_mu);
            sb_puts(&b, "{\"ok\":false,\"error\":\"one install at a time: another store install or a download is running\"}"); reply_json(s, 409, &b); free(b.b); return;
        }
        memset(&g_store, 0, sizeof g_store);
        snprintf(g_store.state, sizeof g_store.state, "running"); snprintf(g_store.kind, sizeof g_store.kind, "%s", kind);
        snprintf(g_store.path, sizeof g_store.path, "%s", path); snprintf(g_store.name, sizeof g_store.name, "%s", name);
        g_store.bytes = size; g_store.started = time(NULL);
        pthread_mutex_unlock(&g_store_mu);
        pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 512u << 10);
        int r = pthread_create(&t, &at, store_thread, NULL);
        pthread_attr_destroy(&at);
        if (r != 0) { store_set("failed", "install thread did not start (%d)", r); sb_puts(&b, "{\"ok\":false,\"error\":\"install thread did not start\"}"); reply_json(s, 500, &b); free(b.b); return; }
        pthread_detach(t);
        logf_("store: install %s %s (%s)", kind, path, name);
        sb_puts(&b, "{\"ok\":true}"); reply_json(s, 200, &b); free(b.b); return;
    }
    sb_puts(&b, "{\"error\":\"not found\"}"); reply_json(s, 404, &b); free(b.b);
}
