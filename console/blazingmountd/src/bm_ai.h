// SPDX-License-Identifier: GPL-3.0-or-later
// bm_ai.h - Sony's package installer (libSceAppInstUtil) for BlazingMount: the structure layouts, how the symbols are found from a
// payload, the IPC between blazingmountd and its one-install helper process, and (host builds) a FAKE installer for the tests.
// Included by bm_inst_helper.c (the helper ELF) and bm_install.c (blazingmountd). Self-contained: libc + POSIX sockets only.
//
// Why a helper process: ONE AppInstUtil session installs ONE package on FW >= 9.60; the 2nd InstallByPackage in the same process
// answers 0x80B2116F (PLAYGO_ERROR_CORE_INVALID_SLOT) even after Terminate + Initialize (measured on DIGI5 13.60, 27 Oct 2026, and
// PKG Manager hit the same on 25 Sep: upstream itsPLK/ps5-pkg-manager#11). So every install runs in a fresh process, as PKG
// Manager's does: blazingmountd hands bm_inst_helper.elf to elfldr (127.0.0.1:9021); it connects back to 127.0.0.1:BM_INST_IPC,
// installs one URL, answers status polls, and exits when blazingmountd hangs up.
// The layouts are public homebrew-scene knowledge (PKG Manager's ARCHITECTURE.md documents the same ones).
#ifndef BM_AI_H
#define BM_AI_H
#include <stdint.h>

#ifndef BM_INST_PORT
#define BM_INST_PORT 8095                 // the proxy the installer reads (127.0.0.1)
#endif
#ifndef BM_INST_IPC
#define BM_INST_IPC 8096                  // the helper connects back here (127.0.0.1)
#endif

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
#define AI_INVALID_SLOT 0x80B2116Fu

// IPC: fixed-size, pointer-free messages on one loopback TCP connection
#define IH_MAGIC 0x484D4942u              // "BIMH"
enum { IH_INSTALL = 1, IH_STATUS = 2, IH_BYE = 3 };
typedef struct { uint32_t magic, op; char uri[256]; char name[128]; char cid[48]; } ih_req_t;
typedef struct { uint32_t magic; int32_t rc; char cid[48]; char status[16]; uint64_t downloaded, total; uint32_t remain; int32_t error; } ih_resp_t;

static int (*ai_init)(void), (*ai_term)(void);
static int (*ai_by_package)(ai_meta_t *, ai_pkg_info_t *, ai_playgo_t *);
static int (*ai_get_status)(const char *, ai_status_t *);
static char g_ai_why[160];
static int g_ai_pre[3];
#ifndef AI_TRACE
#define AI_TRACE(step) ((void)0)
#endif                    // NetCtl / UserService / Net init results (the helper logs them)

#if defined(__PROSPERO__) && defined(BM_AI_LINKED)
// the helper ELF: linked against Sony's libraries the way PKG Manager's helper is (-lSceNetCtl -lSceUserService -lSceSystemService
// -lSceAppInstUtil -lSceNet), so the modules come up at process start. DIGI5 27 Oct: the same helper resolving them at run time
// (sysmodule loader + dlsym) initialised fine but every InstallByPackage answered 0x80B22404.
int sceNetCtlInit(void); int sceUserServiceInitialize(int *); int sceNetInit(void);
int sceAppInstUtilInitialize(void); int sceAppInstUtilTerminate(void);
int sceAppInstUtilInstallByPackage(ai_meta_t *, ai_pkg_info_t *, ai_playgo_t *);
int sceAppInstUtilGetInstallStatus(const char *, ai_status_t *);
static int ai_bring_up(void) {
    int prio = 256;
    g_ai_pre[0] = sceNetCtlInit(); AI_TRACE("netctl");
    g_ai_pre[1] = sceUserServiceInitialize(&prio); AI_TRACE("userservice");
    g_ai_pre[2] = sceNetInit(); AI_TRACE("net");
    ai_init = sceAppInstUtilInitialize; ai_term = sceAppInstUtilTerminate;
    ai_by_package = sceAppInstUtilInstallByPackage; ai_get_status = sceAppInstUtilGetInstallStatus;
    int rc = ai_init();
    AI_TRACE("initialized");
    if (rc) { snprintf(g_ai_why, sizeof g_ai_why, "sceAppInstUtilInitialize 0x%08x", (unsigned)rc); return -1; }
    return 0;
}
#elif defined(__PROSPERO__)
#include <ps5/kernel.h>
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
    AI_TRACE("sysmodule loader");
    if (sysmod) { sysmod(0x8000001d); sysmod(0x80000011); sysmod(0x80000010); sysmod(0x80000014); }   // IPMI, UserService, SystemService, AppInstUtil
    AI_TRACE("sysmodules loaded");
    // then the session set-up PKG Manager's helper makes before AppInstUtil (NetCtl, UserService with priority 256, Net): a fresh
    // helper without it got 0x80B22404 for every URL on DIGI5 13.60 (27 Oct) while the long-running payload's own call worked.
    // Only in the helper (BM_AI_SESSION_SETUP): before the sysmodules were loaded these calls hung the helper.
#ifdef BM_AI_SESSION_SETUP
    int (*netctl)(void) = (int (*)(void))ai_sym("libSceNetCtl.sprx", "sceNetCtlInit");
    g_ai_pre[0] = netctl ? netctl() : -1; AI_TRACE("netctl");
    int (*usersvc)(int *) = (int (*)(int *))ai_sym("libSceUserService.sprx", "sceUserServiceInitialize");
    int prio = 256; g_ai_pre[1] = usersvc ? usersvc(&prio) : -1; AI_TRACE("userservice");
    int (*netinit)(void) = (int (*)(void))ai_sym("libSceNet.sprx", "sceNetInit");
    g_ai_pre[2] = netinit ? netinit() : -1; AI_TRACE("net");
#endif
    ai_init = (int (*)(void))ai_sym("libSceAppInstUtil.sprx", "sceAppInstUtilInitialize");
    ai_term = (int (*)(void))ai_sym("libSceAppInstUtil.sprx", "sceAppInstUtilTerminate");
    ai_by_package = (int (*)(ai_meta_t *, ai_pkg_info_t *, ai_playgo_t *))ai_sym("libSceAppInstUtil.sprx", "sceAppInstUtilInstallByPackage");
    ai_get_status = (int (*)(const char *, ai_status_t *))ai_sym("libSceAppInstUtil.sprx", "sceAppInstUtilGetInstallStatus");
    if (!ai_init || !ai_by_package || !ai_get_status) { snprintf(g_ai_why, sizeof g_ai_why, "libSceAppInstUtil symbols not found"); return -1; }
    AI_TRACE("symbols");
    int rc = ai_init();
    AI_TRACE("initialized");
    if (rc) { snprintf(g_ai_why, sizeof g_ai_why, "sceAppInstUtilInitialize 0x%08x", (unsigned)rc); return -1; }
    return 0;
}
#else
// FAKE installer (host tests): reads the URL like Sony's (64 KiB header, then 1 MiB Range pieces on one keep-alive connection) and
// reports downloading -> playable. Like the real one it installs ONE package per process (the 2nd call = 0x80B2116F), so the tests
// prove the fresh-helper-per-install design. BM_FAKE_AI=off (unavailable) | refuse (0x80B21104) | stall (never finishes);
// BM_FAKE_AI_DELAY_MS between pieces (a cancel lands mid-way).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
static struct { char uri[256], status[16], cid[48]; long long got, total; int err, used; } g_fake;
static pthread_mutex_t g_fake_mu = PTHREAD_MUTEX_INITIALIZER;
static int fake_wr(int s, const char *b, size_t n) { while (n) { ssize_t w = write(s, b, n); if (w <= 0) return -1; b += w; n -= (size_t)w; } return 0; }
static long long fake_range(int s, int port, const char *path, long long a, long long b, long long *total) {
    char req[400]; int rl = snprintf(req, sizeof req, "GET %s?product=0284&serverIpAddr=127.0.0.1&r=00000000 HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nRange: bytes=%lld-%lld\r\n\r\n", path, port, a, b);   // the query Sony's installer adds
    if (fake_wr(s, req, (size_t)rl)) return -1;
    char h[2048]; size_t have = 0; char *e = NULL;
    while (!e) { if (have >= sizeof h - 1) return -1; ssize_t n = read(s, h + have, sizeof h - 1 - have); if (n <= 0) return -1; have += (size_t)n; h[have] = 0; e = strstr(h, "\r\n\r\n"); }
    if (atoi(h + 9) != 206) return -1;
    char *cl = strcasestr(h, "\nContent-Length: "), *cr = strcasestr(h, "\nContent-Range: bytes ");
    long long want = cl ? atoll(cl + 17) : -1, ra, rb; if (cr) sscanf(cr + 22, "%lld-%lld/%lld", &ra, &rb, total);
    long long got = (long long)(have - (size_t)(e + 4 - h)); char buf[65536];
    while (got < want) { ssize_t n = read(s, buf, sizeof buf); if (n <= 0) return -1; got += n; }
    return got;
}
static void *fake_sony(void *arg) {
    (void)arg;
    int port = 0; char path[200] = "";
    sscanf(g_fake.uri, "http://127.0.0.1:%d%199s", &port, path);
    int delay = getenv("BM_FAKE_AI_DELAY_MS") ? atoi(getenv("BM_FAKE_AI_DELAY_MS")) : 0;
    int s = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((unsigned short)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    long long total = -1, n = (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a)) ? -1 : fake_range(s, port, path, 0, 65535, &total);
    pthread_mutex_lock(&g_fake_mu); g_fake.total = total; g_fake.got = n > 0 ? n : 0; pthread_mutex_unlock(&g_fake_mu);
    for (long long p = 65536; n > 0 && p < total; p += 1 << 20) {
        if (delay) usleep((useconds_t)delay * 1000);
        long long b = p + (1 << 20) - 1; if (b >= total) b = total - 1;
        n = fake_range(s, port, path, p, b, &total);
        if (n > 0) { pthread_mutex_lock(&g_fake_mu); g_fake.got += n; pthread_mutex_unlock(&g_fake_mu); }
    }
    if (s >= 0) close(s);
    pthread_mutex_lock(&g_fake_mu);
    const char *mode = getenv("BM_FAKE_AI") ? getenv("BM_FAKE_AI") : "";
    if (n <= 0) { snprintf(g_fake.status, sizeof g_fake.status, "error"); g_fake.err = (int)0x80B2111Eu; }   // a read failed (a cancel)
    else if (strcmp(mode, "stall")) snprintf(g_fake.status, sizeof g_fake.status, "playable");
    pthread_mutex_unlock(&g_fake_mu);
    return NULL;
}
static int fake_init(void) { return 0; }
static int fake_term(void) { return 0; }
static int fake_by_package(ai_meta_t *m, ai_pkg_info_t *info, ai_playgo_t *pg) {
    (void)pg;
    const char *mode = getenv("BM_FAKE_AI") ? getenv("BM_FAKE_AI") : "";
    if (!strcmp(mode, "refuse")) return (int)0x80B21104u;
    if (!strcmp(mode, "hang")) { sleep(3600); return -1; }          // a Sony call that never returns
    pthread_mutex_lock(&g_fake_mu);
    if (g_fake.used) { pthread_mutex_unlock(&g_fake_mu); return (int)AI_INVALID_SLOT; }   // one install per process, like Sony's
    g_fake.used = 1;
    snprintf(g_fake.uri, sizeof g_fake.uri, "%s", m->uri); snprintf(g_fake.status, sizeof g_fake.status, "downloading");
    snprintf(g_fake.cid, sizeof g_fake.cid, "IV9999-FAKE00001_00-BLAZINGMOUNTTEST"); g_fake.got = 0; g_fake.total = 0; g_fake.err = 0;
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

// one install request -> rc + content id. Every argument stays alive and unchanged for the life of the process: the system may still
// refer to them after the call returns (PKG Manager's helper keeps them the same way). Static: one install per process anyway.
static char g_ai_uri[256], g_ai_name[128];
static ai_meta_t g_ai_meta;
static ai_pkg_info_t g_ai_info;
static ai_playgo_t g_ai_playgo;
static int ai_install_uri(const char *uri, const char *name, char *cid_out, size_t cn) {
    snprintf(g_ai_uri, sizeof g_ai_uri, "%s", uri); snprintf(g_ai_name, sizeof g_ai_name, "%s", name);
    g_ai_meta = (ai_meta_t){ g_ai_uri, "", "", "", g_ai_name, "" };
    memset(&g_ai_info, 0, sizeof g_ai_info); memset(&g_ai_playgo, 0, sizeof g_ai_playgo);
    int rc = ai_by_package(&g_ai_meta, &g_ai_info, &g_ai_playgo);
    snprintf(cid_out, cn, "%.47s", g_ai_info.content_id);
    return rc;
}
#endif
