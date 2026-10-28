// SPDX-License-Identifier: GPL-3.0-or-later
// bm_prof.c - v2.12: this console's PROFILE (Markus 28 Oct 2026: "console profiles ... PHAT5 and DIGI5 each with default install target,
// sources and the user to install for. Stored on each console, editable in the app and from a host tool").
//
// /data/blazing/profile.conf, one "key value" per line (the same style as mount.conf):
//   name            what the console is called in the app and the tools (PHAT5, DIGI5, ...)
//   install_target  auto | internal | m2 : where the payload puts the starters/installs it makes. internal = /data/homebrew,
//                   m2 = /mnt/ext1/homebrew (both set mount.conf's homebrew_dir, so the main loop uses it from the next pass); auto = leave as is.
//                   (Sony's installer puts PKGs where the PS5's Settings > Storage > Install Location says; that is not ours to change.)
//   default_source  the network source (bm_src.c) the INSTALL tab opens first; must exist
//   user            the PS5 user the installs are for (a label the app and the host tools show; the PS5 installs for the console)
//
//   GET  /api/profile                    {name, install_target, default_source, user, homebrew_dir, applied, sources, version, power:"awake"}
//   POST /api/profile/set?name=&install_target=&default_source=&user=      any of them; "" clears   (header X-BM-Request: 1)
//
// #include'd ONCE into bm_api.c after bm_src.c (uses api_slurp, write_file, api_enqueue, src_find, sb_*, get_q, P(), read_conf).

static const char *api_enqueue(const char *what, const char *tid, const char *key, const char *value);   // below
#define PROF_PATH "/data/blazing/profile.conf"
typedef struct { char name[32], install_target[10], default_source[32], user[48]; } prof_t;
static pthread_mutex_t g_prof_mu = PTHREAD_MUTEX_INITIALIZER;

static void prof_load(prof_t *p) {
    memset(p, 0, sizeof *p); snprintf(p->install_target, sizeof p->install_target, "auto");
    char pp[600]; char *m = api_slurp(P(pp, sizeof pp, PROF_PATH), 1 << 14, NULL);
    if (!m) return;
    for (char *line = strtok(m, "\n"); line; line = strtok(NULL, "\n")) {
        char *sp = strchr(line, ' '); if (!sp) continue; *sp++ = 0;
        while (*sp == ' ') sp++;
        if (!strcmp(line, "name")) snprintf(p->name, sizeof p->name, "%s", sp);
        else if (!strcmp(line, "install_target")) snprintf(p->install_target, sizeof p->install_target, "%s", sp);
        else if (!strcmp(line, "default_source")) snprintf(p->default_source, sizeof p->default_source, "%s", sp);
        else if (!strcmp(line, "user")) snprintf(p->user, sizeof p->user, "%s", sp);
    }
    free(m);
}
static int prof_save(const prof_t *p) {
    char pp[600], t[800]; P(pp, sizeof pp, PROF_PATH);
    int n = snprintf(t, sizeof t, "name %s\ninstall_target %s\ndefault_source %s\nuser %s\n", p->name, p->install_target, p->default_source, p->user);
    return write_file(pp, t, (size_t)n);
}
static const char *prof_target_dir(const char *t) { return !strcmp(t, "internal") ? "/data/homebrew" : !strcmp(t, "m2") ? "/mnt/ext1/homebrew" : NULL; }

static void prof_json(sb_t *b, const char *applied) {
    prof_t p; pthread_mutex_lock(&g_prof_mu); prof_load(&p); pthread_mutex_unlock(&g_prof_mu);
    conf_t *c = malloc(sizeof *c); char hd[64] = "";
    if (c) { read_conf(c); snprintf(hd, sizeof hd, "%s", c->homebrew_dir); free(c); }
    const char *want = prof_target_dir(p.install_target);
    sb_puts(b, "{\"name\":"); sb_js(b, p.name); sb_puts(b, ",\"install_target\":"); sb_js(b, p.install_target);
    sb_puts(b, ",\"default_source\":"); sb_js(b, p.default_source); sb_puts(b, ",\"user\":"); sb_js(b, p.user);
    sb_puts(b, ",\"homebrew_dir\":"); sb_js(b, hd);
    sb_f(b, ",\"target_in_effect\":%s,\"applied\":", !want || !strcmp(want, hd) ? "true" : "false"); sb_js(b, applied ? applied : "");
    sb_puts(b, ",\"version\":"); sb_js(b, BM_VERSION); sb_puts(b, ",\"power\":\"awake\"}");   // a console that answers is awake (rest mode = no answer)
}
static void api_prof(int s, const char *method, const char *target, const char *query, const char *req) {
    sb_t b = { 0 };
    if (!strcmp(method, "GET") && !strcmp(target, "/api/profile")) { prof_json(&b, NULL); reply_json(s, 200, &b); free(b.b); return; }
    if (strcmp(method, "POST") || strcmp(target, "/api/profile/set")) { sb_puts(&b, "{\"error\":\"not found\"}"); reply_json(s, 404, &b); free(b.b); return; }
    if (!strcasestr(req, "\nX-BM-Request: 1")) { sb_puts(&b, "{\"ok\":false,\"error\":\"missing X-BM-Request header\"}"); reply_json(s, 403, &b); free(b.b); return; }
    char v[200]; const char *err = NULL; char applied[160] = "";
    pthread_mutex_lock(&g_prof_mu);
    prof_t p; prof_load(&p);
    if (get_q(query, "name", v, sizeof v)) { if (strlen(v) > 31 || !src_text_ok(v)) err = "name: up to 31 characters, no control characters"; else snprintf(p.name, sizeof p.name, "%s", v); }
    if (!err && get_q(query, "user", v, sizeof v)) { if (strlen(v) > 47 || !src_text_ok(v)) err = "user: up to 47 characters, no control characters"; else snprintf(p.user, sizeof p.user, "%s", v); }
    if (!err && get_q(query, "default_source", v, sizeof v)) {
        src_t sc;
        if (v[0] && !src_find(v, &sc)) err = "default_source: no such source (add it first)"; else snprintf(p.default_source, sizeof p.default_source, "%s", v);
    }
    int target_changed = 0; char tgt[10] = "";
    if (!err && get_q(query, "install_target", v, sizeof v)) {
        if (strcmp(v, "auto") && strcmp(v, "internal") && strcmp(v, "m2")) err = "install_target: auto, internal or m2";
        else { target_changed = strcmp(p.install_target, v) != 0; snprintf(p.install_target, sizeof p.install_target, "%s", v); snprintf(tgt, sizeof tgt, "%s", v); }
    }
    if (!err && prof_save(&p)) err = "could not write /data/blazing/profile.conf";
    pthread_mutex_unlock(&g_prof_mu);
    if (!err && tgt[0] && prof_target_dir(tgt)) {                    // the main loop applies it (conf_set_key) at the start of its next pass
        const char *e2 = api_enqueue("set", "", "homebrew_dir", prof_target_dir(tgt));
        snprintf(applied, sizeof applied, "%s", e2 ? e2 : "homebrew_dir queued");
        if (target_changed || !e2) logf_("ui: profile: install_target %s -> homebrew_dir %s (%s)", tgt, prof_target_dir(tgt), applied);
    }
    if (!err) logf_("ui: profile saved (name %s, target %s, default source %s, user %s)", p.name, p.install_target, p.default_source[0] ? p.default_source : "-", p.user[0] ? p.user : "-");
    if (err) { sb_puts(&b, "{\"ok\":false,\"error\":"); sb_js(&b, err); sb_puts(&b, "}"); reply_json(s, 400, &b); }
    else { prof_json(&b, applied); reply_json(s, 200, &b); }
    free(b.b);
}
