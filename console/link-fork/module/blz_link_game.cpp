// SPDX-License-Identifier: GPL-3.0-or-later (it is linked into drakmor's GPL-3.0 AMPR emulator PRX; personal use, never distributed)
//
// blz_link_game.cpp - PRX glue of the PS5 SMB link (tools/smblink). The engine (blz_link_core.cpp) is platform-free; this file wires it to the AMPR emulator's own helpers and
// to the libkernel hooks the emulator already installs (open, close, sceKernelClose, fstat, lseek, + pread/read when built with AMPR_PROCESS_SYNC_READ_INTERCEPT=1), and
// implements the virtual-AIO entry points the patched sceKernelAio* wrappers call.
//
// Lazy start: nothing happens until the first real open() the emulator or the game does. That call reads /app0/sce_sys/param.json (title id), then
// /data/blazing/link/<TID>.cfg (or /app0/blz_link.cfg). No config = the library behaves exactly like the stock emulator.
#include "blz_link.h"
#include "blz_link_hooks.h"

#include "ampr_emu_config.h"
#include "ampr_emu_kernel_file.h"
#include "ampr_emu_kernel_lookup.h"
#include "ampr_emu_log.h"

#include <_kernel.h>
#include <_pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <kernel.h>
#include <sys/stat.h>

// defined by the emulator (src/ampr_emu_pack.cpp) but not declared in the SDK headers
extern "C" int sceKernelAioWaitRequests(SceKernelAioSubmitId ids[], int count, int states[], uint32_t mode, SceKernelUseconds* usec);
extern "C" int sceKernelAioCancelRequests(SceKernelAioSubmitId ids[], int count, int states[]);

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char kParamJson[] = "/app0/sce_sys/param.json";
constexpr const char kCfgDir[] = "/data/blazing/link/";
constexpr const char kCfgInApp[] = "/app0/blz_link.cfg";
constexpr size_t kMaxCfg = 128 * 1024;             // a config with 1024 file lines is ~70 KB
constexpr size_t kLogCap = 1u << 20;
constexpr int kSoNosigpipe = 0x800;                      // FreeBSD / PS5 value

std::atomic<int> g_state{0};                             // 0 untouched, 1 initialising, 2 active, 3 off
std::atomic<bool> g_initThreadSet{false};
ScePthread g_initThread{};

char g_tid[16];
char g_logPath[96];
std::atomic<size_t> g_logBytes{0};
blzlink::Config g_cfgStore;                              // ~58 KB: static, never on a thread stack
char g_cfgText[kMaxCfg + 1];

using OpenFn = int (*)(const char*, int, ...);
using WriteFn = ssize_t (*)(int, const void*, size_t);
using ReadFn = ssize_t (*)(int, void*, size_t);
using LseekFn = off_t (*)(int, off_t, int);

struct NetSyms {
  int (*socket)(int, int, int);
  int (*connect)(int, const void*, unsigned);
  long (*send)(int, const void*, size_t, int);
  long (*recv)(int, void*, size_t, int);
  int (*setsockopt)(int, int, int, const void*, unsigned);
  int (*getsockopt)(int, int, int, void*, unsigned*);
  int (*fcntl)(int, int, ...);
  int (*poll)(void*, unsigned, int);
} g_net;

// ---- real (hook-bypassing) file operations -----------------------------------------------------------------------------------------------------

int real_open(const char* path, int flags, int mode = 0666) {
  OpenFn fn = ampr_fixed_kernel_slot<OpenFn>(kAmprLibkernelHook_open);       // the ORIGINAL open: does not call blz_link_note_open
  if (!fn) return -1;
  return fn(path, flags, mode);
}

ssize_t real_write(int fd, const void* b, size_t n) {
  static WriteFn const fn = ampr_dynamic_kernel_func_or_null<WriteFn>("write");
  return fn ? fn(fd, b, n) : -1;
}

ssize_t real_read(int fd, void* b, size_t n) {
#if AMPR_EMU_PACK_ENABLE && AMPR_EMU_PACK_INTERCEPT_PROCESS_SYNC_READS
  if (ReadFn fn = ampr_fixed_kernel_slot<ReadFn>(kAmprLibkernelHook_read)) return fn(fd, b, n);
#endif
  static ReadFn const dyn = ampr_dynamic_kernel_func_or_null<ReadFn>("read");
  return dyn ? dyn(fd, b, n) : -1;
}

off_t real_lseek(int fd, off_t off, int whence) {
#if AMPR_EMU_PACK_ENABLE && (AMPR_EMU_PACK_DIRECTORY_OVERLAY_ENABLE || AMPR_EMU_PACK_PROCESS_OPEN_ENABLE)
  if (LseekFn fn = ampr_fixed_kernel_slot<LseekFn>(kAmprLibkernelHook_lseek)) return fn(fd, off, whence);
#endif
  static LseekFn const dyn = ampr_dynamic_kernel_func_or_null<LseekFn>("lseek");
  return dyn ? dyn(fd, off, whence) : -1;
}

void file_log(const char* line) {
  if (!g_logPath[0] || g_logBytes.load(std::memory_order_relaxed) > kLogCap) return;
  const int fd = real_open(g_logPath, O_WRONLY | O_CREAT | O_APPEND);
  if (fd < 0) return;
  const size_t len = std::strlen(line);
  (void)real_write(fd, line, len);
  (void)real_write(fd, "\n", 1);
  g_logBytes.fetch_add(len + 1);
  (void)ampr_real_physical_close(fd);
}

// ---- ops for the engine ------------------------------------------------------------------------------------------------------------------------

long op_pread(int fd, void* b, size_t n, int64_t off) { return (long)ampr_real_physical_pread(fd, b, n, (off_t)off); }
// the disk netcache's own files (cfg disk_cache_mb): hook-bypassing open/pwrite/mkdir
int op_open(const char* path, int flags, int mode) { return real_open(path, flags, mode); }
long op_pwrite(int fd, const void* b, size_t n, int64_t off) {
  using PwriteFn = ssize_t (*)(int, const void*, size_t, off_t);
  static PwriteFn const fn = ampr_dynamic_kernel_func_or_null<PwriteFn>("pwrite");
  return fn ? (long)fn(fd, b, n, (off_t)off) : -1;
}
int op_mkdir(const char* path, int mode) {
  using MkdirFn = int (*)(const char*, mode_t);
  static MkdirFn const fn = ampr_dynamic_kernel_func_or_null<MkdirFn>("mkdir");
  return fn ? fn(path, (mode_t)mode) : -1;
}
long op_read(int fd, void* b, size_t n) { return (long)real_read(fd, b, n); }
int64_t op_lseek(int fd, int64_t off, int whence) { return (int64_t)real_lseek(fd, (off_t)off, whence); }
int op_errno() { return errno; }
uint64_t op_now_us() {
  timespec ts{};
  if (sceKernelClockGettime(SCE_KERNEL_CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
void op_sleep(unsigned us) { (void)sceKernelUsleep(us); }
bool op_start_thread(void* (*fn)(void*), void* arg) {    // the link's fetch workers (config "async 1"): plain game-process threads, like the emulator's own reactor
  ScePthread t{};
  return scePthreadCreate(&t, nullptr, fn, arg, "blz_link_fetch") == 0;
}
void op_log(const char* line) {
  AMPR_KLOGF("%s", line);
  file_log(line);
}

int n_socket(int d, int t, int p) { return g_net.socket(d, t, p); }
int n_connect(int fd, const void* a, unsigned l) { return g_net.connect(fd, a, l); }
long n_send(int fd, const void* b, size_t n, int f) { return g_net.send(fd, b, n, f); }
long n_recv(int fd, void* b, size_t n, int f) { return g_net.recv(fd, b, n, f); }
int n_close(int fd) { return ampr_real_physical_close(fd); }
int n_setsockopt(int fd, int lv, int op, const void* v, unsigned l) { return g_net.setsockopt(fd, lv, op, v, l); }
int n_getsockopt(int fd, int lv, int op, void* v, unsigned* l) { return g_net.getsockopt(fd, lv, op, v, l); }
int n_fcntl(int fd, int cmd, int arg) { return g_net.fcntl(fd, cmd, arg); }
int n_poll(void* fds, unsigned n, int to) { return g_net.poll(fds, n, to); }
int n_errno() { return errno; }

int slot_ok(AmprLibkernelHookId id) { return ampr_fixed_kernel_slot<void*>(id) != nullptr ? 1 : 0; }

bool resolve_net() {
  g_net.socket = ampr_dynamic_kernel_func_or_null<decltype(g_net.socket)>("socket");
  g_net.connect = ampr_dynamic_kernel_func_or_null<decltype(g_net.connect)>("connect");
  g_net.send = ampr_dynamic_kernel_func_or_null<decltype(g_net.send)>("send");
  g_net.recv = ampr_dynamic_kernel_func_or_null<decltype(g_net.recv)>("recv");
  g_net.setsockopt = ampr_dynamic_kernel_func_or_null<decltype(g_net.setsockopt)>("setsockopt");
  g_net.getsockopt = ampr_dynamic_kernel_func_or_null<decltype(g_net.getsockopt)>("getsockopt");
  g_net.fcntl = ampr_dynamic_kernel_func_or_null<decltype(g_net.fcntl)>("fcntl");
  g_net.poll = ampr_dynamic_kernel_func_or_null<decltype(g_net.poll)>("poll");
  return g_net.socket && g_net.connect && g_net.send && g_net.recv && g_net.setsockopt && g_net.getsockopt && g_net.fcntl && g_net.poll;
}

// ---- lazy start --------------------------------------------------------------------------------------------------------------------------------

// whole file (<= cap bytes) through the original open + the physical pread; -1 when it cannot be opened, else the byte count
long slurp(const char* path, char* out, size_t cap) {
  const int fd = real_open(path, O_RDONLY);
  if (fd < 0) return -1;
  size_t got = 0;
  while (got < cap) {
    const ssize_t n = ampr_real_physical_pread(fd, out + got, cap - got, (off_t)got);
    if (n <= 0) break;
    got += (size_t)n;
  }
  (void)ampr_real_physical_close(fd);
  return (long)got;
}

bool find_title_id(const char* text, long n, char out[16]) {          // first "PPSA" + 5 digits in param.json (titleId and contentId both carry it)
  for (long i = 0; i + 9 <= n; ++i) {
    if (text[i] == 'P' && text[i + 1] == 'P' && text[i + 2] == 'S' && text[i + 3] == 'A') {
      bool digits = true;
      for (int k = 4; k < 9; ++k) if (text[i + k] < '0' || text[i + k] > '9') digits = false;
      if (digits) { std::memcpy(out, text + i, 9); out[9] = 0; return true; }
    }
  }
  return false;
}

// ---- AppContent temp-data shim (cfg "appcontent_temp fail") ---------------------------------------------------------------------------------
// Onimusha (PPSA27836) on PHAT5 + SMP 1.7beta6-fix2, 21 Oct 2026: sceAppContentTemporaryDataMount2(FORMAT) goes over IPMI to ShellCore, whose
// SceAppContentServer then blocks for good in open("/dev/ssd0.app_temp0"); the game waits forever and its close leaves a 'Device busy' sandbox.
// The eboot checks the result (eboot+0x123ce8f call; +0x123ce94 test eax,eax; jne = skip the temp-data setup), so answering the call INSIDE
// the game with an error lets it go on without temp data and never touches the stuck service. An inline detour on the export of whichever
// loaded module provides it (the real libSceAppContent, or a dlc_emu fake in fakelib). Opt-in per title; works in every link mode.
constexpr int kTempDataNotAvailable = (int)0x80D9000A;  // an SCE_APP_CONTENT error (internal); the eboot only tests for non-zero
std::atomic<int> g_tdm2Shim{0};                          // 0 not installed, 1 installed, 2 install failed
std::atomic<int> g_tdm2Calls{0};

int tdm2_answer(uint32_t option, void* mountPoint) {
  const int n = g_tdm2Calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (mountPoint) std::memset(mountPoint, 0, 16);         // SceAppContentMountPoint is char data[16]
  if (n <= 8) AMPR_KLOGF("[blz_link] sceAppContentTemporaryDataMount2(option=%u) #%d answered in the game: 0x%08x (no temp data, ShellCore not asked)", option, n, (unsigned)kTempDataNotAvailable);
  return kTempDataNotAvailable;
}

// The eboot also calls sceAppContentTemporaryDataGetAvailableSpaceKb(&mountPoint, &kb) (eboot+0x123cf14, gated by its own flag, not by the
// mount result). With the shim the mount point is empty: answer that case in the game too, so no temp-data call at all reaches ShellCore.
using TempSpaceFn = int (*)(const void*, size_t*);
void* g_tempSpaceOrig = nullptr;

int temp_space_answer(const void* mountPoint, size_t* kb) {
  const char* mp = static_cast<const char*>(mountPoint);
  if (!mp || mp[0] == 0) {
    AMPR_KLOGF("[blz_link] sceAppContentTemporaryDataGetAvailableSpaceKb(empty mount point) answered in the game: 0x80d90002");
    if (kb) *kb = 0;
    return (int)0x80D90002;                              // SCE_APP_CONTENT_ERROR_PARAMETER
  }
  auto* const fn = reinterpret_cast<TempSpaceFn>(__atomic_load_n(&g_tempSpaceOrig, __ATOMIC_ACQUIRE));
  return fn ? fn(mountPoint, kb) : (int)0x80D90002;
}

void install_temp_shim(const char* when) {
  int expected = 0;
  if (!g_tdm2Shim.compare_exchange_strong(expected, 3)) return;     // installed, failed, or another thread is doing it
  void* original = nullptr;
  const int rc = blz_detour_loaded_symbol("sceAppContentTemporaryDataMount2", reinterpret_cast<void*>(&blz_link_tdm2_answer), &original);
  if (rc == 0) {
    g_tdm2Shim.store(1, std::memory_order_release);
    AMPR_KLOGF("[blz_link] temp-data shim INSTALLED (%s): sceAppContentTemporaryDataMount2 is answered in the game (original %p)", when, original);
    const int src = blz_detour_loaded_symbol("sceAppContentTemporaryDataGetAvailableSpaceKb", reinterpret_cast<void*>(&blz_link_temp_space_answer), &g_tempSpaceOrig);
    AMPR_KLOGF("[blz_link] temp-data space guard %s (rc=%d)", src == 0 ? "INSTALLED" : "not installed", src);
  } else {
    g_tdm2Shim.store(rc == -4 ? 0 : 2, std::memory_order_release);  // -4 = no loaded module exports it yet: try again later
    AMPR_KLOGF("[blz_link] temp-data shim NOT installed (%s): rc=%d%s", when, rc, rc == -4 ? " (symbol not loaded yet, will retry)" : "");
  }
}

// libSceAppContent is NOT loaded at module_start or at the first open (try #1, 21 Oct 04:59: rc=-4 both times): the game loads it itself with
// sceSysmoduleLoadModule(0xb4 = APP_CONTENT) right before sceAppContentInitialize + TemporaryDataMount2 (eboot+0x123ce4x). So we also detour
// sceSysmoduleLoadModule: after every successful load, if the shim is still pending, install it before the game gets control back.
using SysmoduleLoadFn = int (*)(uint16_t);
void* g_sysmoduleLoadOrig = nullptr;                    // trampoline to the original sceSysmoduleLoadModule
std::atomic<int> g_sysmodHook{0};                        // 0 not installed, 1 installed, 2 failed, 3 in progress

void install_sysmodule_hook(const char* when) {
  if (g_tdm2Shim.load(std::memory_order_acquire) == 1) return;      // the shim is already in place: nothing to watch for
  int expected = 0;
  if (!g_sysmodHook.compare_exchange_strong(expected, 3)) return;
  // g_sysmoduleLoadOrig itself is the slot: the trampoline is stored there BEFORE the jump goes live (no call can see it empty)
  const int rc = blz_detour_loaded_symbol("sceSysmoduleLoadModule", reinterpret_cast<void*>(&blz_link_sysmodule_load), &g_sysmoduleLoadOrig);
  if (rc == 0) {
    g_sysmodHook.store(1, std::memory_order_release);
    AMPR_KLOGF("[blz_link] sceSysmoduleLoadModule watched (%s): the temp-data shim goes in when libSceAppContent loads", when);
  } else {
    g_sysmodHook.store(rc == -4 ? 0 : 2, std::memory_order_release);
    AMPR_KLOGF("[blz_link] sceSysmoduleLoadModule NOT watched (%s): rc=%d", when, rc);
  }
}

blzlink::Config g_shimCfg;                               // the cfg the shim was set up from (module_start or first open); read by the sysmodule hook
std::atomic<bool> g_shimCfgSet{false};
int patch_got(const blzlink::Config& cfg, const char* when, bool appcontentOnly);

int sysmodule_load(uint16_t id) {
  auto* const fn = reinterpret_cast<SysmoduleLoadFn>(__atomic_load_n(&g_sysmoduleLoadOrig, __ATOMIC_ACQUIRE));
  const int rc = fn ? fn(id) : (int)0x80020004;          // (never without the original: it is stored before the slot/detour goes live)
  if (rc == 0) {
    char why[64];
    std::snprintf(why, sizeof(why), "after sceSysmoduleLoadModule(0x%x)", (unsigned)id);
    if (g_shimCfgSet.load(std::memory_order_acquire) && g_shimCfg.ngot > 0) {
      // try #3 (21 Oct 05:43): the loader RE-BINDS the game's AppContent slots when the game loads libSceAppContent, which undid the
      // module_start patch. Re-point them right here, after the load and before the game gets control back (Initialize + Mount2 follow).
      const int n = patch_got(g_shimCfg, why, false);   // all slots: also re-hooks sceSysmoduleLoadModule if its resolver just bound it
      if (n > 0 && g_tdm2Shim.exchange(1, std::memory_order_acq_rel) != 1) AMPR_KLOGF("[blz_link] temp-data shim LIVE via %d import slot(s) (%s)", n, why);
    } else if (g_tdm2Shim.load(std::memory_order_acquire) == 0) {
      install_temp_shim(why);
    }
  }
  return rc;
}

// The shim's main route is the game's OWN import slots: cfg "got <name> <GOT offset> <PLT offset>" (tools/eboot_imports.py finds them offline).
// try #2 (05:33): sceKernelDlsym found NEITHER export (rc=-4 even for sceSysmoduleLoadModule with libSceSysmodule loaded), so no inline detours.
// A slot is written only when it holds its PLT stub's push (stub + 6 = not bound yet) or an address inside a module (>= 0x8_0000_0000 = bound);
// a bound value is kept as the original. sceSysmoduleLoadModule must be bound (we call the original), and its hook re-points the AppContent
// slots after every library load (try #3: the loader re-binds them when the game loads libSceAppContent).
constexpr uint64_t kExeBase = 0x400000;                  // PS5 titles map the executable at 0x400000 (every Onimusha module map, 21 Oct)
constexpr uint64_t kModuleSpace = 0x800000000ull;        // modules (libkernel, libSceAmpr, libSceIpmi, ...) live above this
using MprotectFn = int (*)(const void*, size_t, int);
void* g_tdm2Orig = nullptr;                              // the bound sceAppContentTemporaryDataMount2 (kept for the record; the shim never calls it)

bool same_str(const char* a, const char* b) {             // no libc strcmp: the PRX may only import the emulator's own libc list
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}

// The cfg is VERIFIED for the running eboot when every AppContent entry's slot holds exactly its own lazy PLT stub (base + plt + 6) at the first
// pass (module_start: the game has not loaded libSceAppContent yet). A different eboot practically never matches all of them. Code bytes cannot
// be checked instead: PS5 game code pages are execute-only (reading eboot+0x8ec8150 = SYSTEM_XO_VIOLATION, crash, 21 Oct 06:14).
std::atomic<bool> g_gotVerified{false};

bool verify_got(const blzlink::Config& cfg) {
  int checked = 0;
  for (uint32_t i = 0; i < cfg.ngot; ++i) {
    const blzlink::GotPatch& g = cfg.got[i];
    if (same_str(g.name, "sceSysmoduleLoadModule")) continue;                     // bound at start: says nothing about the eboot
    if (g.got >= 0x40000000ull || g.plt >= 0x40000000ull) return false;
    const uint64_t now = __atomic_load_n(reinterpret_cast<uint64_t*>(kExeBase + g.got), __ATOMIC_ACQUIRE);
    if (now != kExeBase + g.plt + 6) {
      AMPR_KLOGF("[blz_link] got %s: slot +0x%llx holds 0x%llx, not its lazy stub 0x%llx - cfg does NOT match this eboot, shim off", g.name,
                 (unsigned long long)g.got, (unsigned long long)now, (unsigned long long)(kExeBase + g.plt + 6));
      return false;
    }
    ++checked;
  }
  return checked > 0;
}

int patch_got(const blzlink::Config& cfg, const char* when, bool appcontentOnly) {  // appcontentOnly: leave the sysmodule slot alone
  int patched = 0;
  for (uint32_t i = 0; i < cfg.ngot; ++i) {
    const blzlink::GotPatch& g = cfg.got[i];
    void* repl = nullptr;
    void** origSlot = nullptr;
    bool callableLazy = false;                           // sceSysmoduleLoadModule: we must be able to CALL the original
    if (same_str(g.name, "sceAppContentTemporaryDataMount2")) { repl = reinterpret_cast<void*>(&blz_link_tdm2_answer); origSlot = &g_tdm2Orig; }
    else if (same_str(g.name, "sceAppContentTemporaryDataGetAvailableSpaceKb")) { repl = reinterpret_cast<void*>(&blz_link_temp_space_answer); origSlot = &g_tempSpaceOrig; }
    else if (same_str(g.name, "sceSysmoduleLoadModule")) {
      if (appcontentOnly) continue;
      repl = reinterpret_cast<void*>(&blz_link_sysmodule_load); origSlot = &g_sysmoduleLoadOrig; callableLazy = true;
    }
    if (!repl) { AMPR_KLOGF("[blz_link] got %s: no shim for this import - skipped", g.name); continue; }
    if (g.got >= 0x40000000ull || g.plt >= 0x40000000ull) { AMPR_KLOGF("[blz_link] got %s: offsets out of range - skipped", g.name); continue; }
    uint64_t* slot = reinterpret_cast<uint64_t*>(kExeBase + g.got);
    const uint64_t lazy = kExeBase + g.plt + 6;
    const uint64_t now = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (now == reinterpret_cast<uint64_t>(repl)) { ++patched; continue; }
    const bool bound = now >= kModuleSpace;
    if (bound && !g_gotVerified.load(std::memory_order_acquire)) {   // a bound slot is only trusted once the cfg was proven to match this eboot
      AMPR_KLOGF("[blz_link] got %s: bound slot but the cfg is not verified for this eboot - NOT patched", g.name);
      continue;
    }
    if (now != lazy && !bound) {
      AMPR_KLOGF("[blz_link] got %s: slot +0x%llx holds 0x%llx (neither its lazy stub 0x%llx nor a module address) - NOT patched", g.name,
                 (unsigned long long)g.got, (unsigned long long)now, (unsigned long long)lazy);
      continue;
    }
    MprotectFn mp = ampr_fixed_kernel_slot<MprotectFn>(kAmprLibkernelHook_sceKernelMprotect);
    const uintptr_t page = reinterpret_cast<uintptr_t>(slot) & ~static_cast<uintptr_t>(0x3FFF);
    const int prc = mp ? mp(reinterpret_cast<const void*>(page), 0x4000, 0x3 /* READ|WRITE */) : -1;
    if (prc != 0) { AMPR_KLOGF("[blz_link] got %s: page not made writable (mprotect %s rc=%d) - NOT patched", g.name, mp ? "" : "missing", prc); continue; }
    // the original first, then the slot. A lazy sceSysmoduleLoadModule slot is callable too: calling stub+6 runs the loader's resolver, which
    // binds the slot (overwriting our hook) and calls the real function; sysmodule_load then hooks the slot again with the bound address.
    if (origSlot && (bound || callableLazy)) __atomic_store_n(origSlot, reinterpret_cast<void*>(now), __ATOMIC_RELEASE);
    __atomic_store_n(slot, reinterpret_cast<uint64_t>(repl), __ATOMIC_RELEASE);
    ++patched;
    AMPR_KLOGF("[blz_link] got %s: import slot +0x%llx (%s 0x%llx) now points at our code (%s)", g.name, (unsigned long long)g.got,
               bound ? "bound" : "lazy", (unsigned long long)now, when);
  }
  return patched;
}

void shim_by_cfg(const blzlink::Config& cfg, const char* when) {
  if (!cfg.appcontent_temp_fail || g_tdm2Shim.load(std::memory_order_acquire) == 1) return;
  if (cfg.ngot > 0) {
    if (!g_shimCfgSet.load(std::memory_order_acquire)) {
      if (!verify_got(cfg)) return;                     // first pass only: never touch a slot of an eboot the cfg was not made for
      g_shimCfg = cfg;
      g_gotVerified.store(true, std::memory_order_release);
      g_shimCfgSet.store(true, std::memory_order_release);
    }
    const int n = patch_got(g_shimCfg, when, false);
    AMPR_KLOGF("[blz_link] temp-data shim: %d import slot(s) set (%s); AppContent slots are set again after each sceSysmoduleLoadModule", n, when);
    return;
  }
  install_temp_shim(when);                               // fallback: inline detour on the export (needs dlsym to find it)
  install_sysmodule_hook(when);
}

// module_start: read param.json + the title's cfg once; only the shim is acted on here (the link itself still starts lazily at the first open)
void early_shim_check() {
  static char param[8192];
  static char text[kMaxCfg + 1];
  static blzlink::Config cfg;                            // ~58 KB: static, never on the loader's stack
  const long pn = slurp(kParamJson, param, sizeof(param));
  char tid[16];
  if (pn <= 0 || !find_title_id(param, pn, tid)) return;
  char path[96];
  std::snprintf(path, sizeof(path), "%s%s.cfg", kCfgDir, tid);
  long cn = slurp(path, text, kMaxCfg);
  if (cn < 0) cn = slurp(kCfgInApp, text, kMaxCfg);
  if (cn <= 0) return;
  char err[160] = "";
  if (!blzlink::parse_config(text, (size_t)cn, &cfg, err, sizeof(err)) || !cfg.appcontent_temp_fail) return;
  shim_by_cfg(cfg, "module_start");
}

void do_init() {
  static char param[8192];
  const long pn = slurp(kParamJson, param, sizeof(param));
  if (pn <= 0 || !find_title_id(param, pn, g_tid)) { g_state.store(3, std::memory_order_release); return; }     // not a title with a param.json: nothing to do
  char path[96];
  std::snprintf(path, sizeof(path), "%s%s.cfg", kCfgDir, g_tid);
  long cn = slurp(path, g_cfgText, kMaxCfg);
  const char* used = path;
  if (cn < 0) { cn = slurp(kCfgInApp, g_cfgText, kMaxCfg); used = kCfgInApp; }
  if (cn <= 0) { g_state.store(3, std::memory_order_release); return; }                                            // no config: stock behaviour, silently
  std::snprintf(g_logPath, sizeof(g_logPath), "%s%s.log", kCfgDir, g_tid);
  char err[160] = "";
  if (!blzlink::parse_config(g_cfgText, (size_t)cn, &g_cfgStore, err, sizeof(err))) {
    AMPR_KLOGF("[blz_link] %s: CONFIG REJECTED (%s) - link stays off", used, err);
    file_log("config rejected");
    g_state.store(3, std::memory_order_release);
    return;
  }
  shim_by_cfg(g_cfgStore, "first open");                                                                           // no-op when module_start did it
  if (g_cfgStore.mode == blzlink::kOff) {
    AMPR_KLOGF("[blz_link] %s: mode off", used);
    g_state.store(3, std::memory_order_release);
    return;
  }
  if (!resolve_net()) {
    AMPR_KLOGF("[blz_link] libkernel has no BSD socket API here - link stays off");
    g_state.store(3, std::memory_order_release);
    return;
  }
  blzlink::Ops ops{};
  ops.pread = op_pread; ops.read = op_read; ops.lseek = op_lseek; ops.get_errno = op_errno; ops.now_us = op_now_us; ops.sleep_us = op_sleep; ops.log = op_log;
  ops.start_thread = op_start_thread;
  ops.open = op_open; ops.pwrite = op_pwrite; ops.mkdir = op_mkdir;
  ops.net.socket_fn = n_socket; ops.net.connect_fn = n_connect; ops.net.send_fn = n_send; ops.net.recv_fn = n_recv; ops.net.close_fn = n_close;
  ops.net.setsockopt_fn = n_setsockopt; ops.net.getsockopt_fn = n_getsockopt; ops.net.fcntl_fn = n_fcntl; ops.net.poll_fn = n_poll; ops.net.errno_fn = n_errno;
  ops.net.so_nosigpipe = kSoNosigpipe;
  AMPR_KLOGF("[blz_link] title %s, config %s", g_tid, used);
  AMPR_KLOGF("[blz_link] libkernel hook slots: open=%d stat=%d close=%d sceKernelClose=%d fstat=%d lseek=%d pread=%d read=%d getdents=%d", slot_ok(kAmprLibkernelHook_open), slot_ok(kAmprLibkernelHook_stat),
             slot_ok(kAmprLibkernelHook_close), slot_ok(kAmprLibkernelHook_sceKernelClose), slot_ok(kAmprLibkernelHook_fstat), slot_ok(kAmprLibkernelHook_lseek), slot_ok(kAmprLibkernelHook_pread),
             slot_ok(kAmprLibkernelHook_read), slot_ok(kAmprLibkernelHook_getdents));
  if (!blzlink::configure(g_cfgStore, ops)) { g_state.store(3, std::memory_order_release); return; }
  g_state.store(2, std::memory_order_release);
}

bool ready() {
  const int s = g_state.load(std::memory_order_acquire);
  if (s == 2) return true;
  if (s == 3) return false;
  int expected = 0;
  if (g_state.compare_exchange_strong(expected, 1)) {
    g_initThread = scePthreadSelf();
    g_initThreadSet.store(true, std::memory_order_release);
    do_init();
    return g_state.load(std::memory_order_acquire) == 2;
  }
  if (g_initThreadSet.load(std::memory_order_acquire) && scePthreadSelf() == g_initThread) return false;        // our own init is opening files: not ready yet
  for (int i = 0; i < 3000 && g_state.load(std::memory_order_acquire) == 1; ++i) (void)sceKernelUsleep(1000);   // another thread is initialising: wait (at most 3 s)
  return g_state.load(std::memory_order_acquire) == 2;
}

inline bool on() { return g_state.load(std::memory_order_acquire) == 2; }

}  // namespace

// ---- entry points called from the patched emulator ---------------------------------------------------------------------------------------------

extern "C" {

void blz_link_banner(void) { AMPR_KLOGF("[blz_link] fork loaded: SMB link build %s %s (inert unless /data/blazing/link/<TID>.cfg exists)", __DATE__, __TIME__); }

void blz_link_early(void) { early_shim_check(); }

int blz_link_tdm2_answer(uint32_t option, void* mountPoint) { return tdm2_answer(option, mountPoint); }

int blz_link_sysmodule_load(uint16_t id) { return sysmodule_load(id); }

int blz_link_temp_space_answer(const void* mountPoint, size_t* kb) { return temp_space_answer(mountPoint, kb); }

void blz_link_note_open(const char* path, int flags, int fd) {
  if (!ready()) return;
  blzlink::trace_open(path, flags, fd);
  blzlink::note_open(path, flags, fd);
}

void blz_link_trace_apr(const char* path, int rc, uint64_t size) {
  if (!on()) return;
  blzlink::trace_apr("apr-resolve", path, rc, size);
}

void blz_link_note_close(int fd) {
  if (!on()) return;
  blzlink::note_close(fd);
}

int blz_link_try_pread(int fd, void* buf, size_t n, off_t off, ssize_t* res) {
  if (!on()) return 0;
  blzlink::trace_io("pread", fd, n, (int64_t)off);
  long r = 0;
  int e = 0;
  if (!blzlink::try_pread(fd, buf, n, (int64_t)off, &r, &e)) return 0;
  if (r < 0) errno = e;
  *res = (ssize_t)r;
  return 1;
}

int blz_link_try_read(int fd, void* buf, size_t n, ssize_t* res) {
  if (!on()) return 0;
  blzlink::trace_io("read", fd, n, -1);
  long r = 0;
  int e = 0;
  if (!blzlink::try_read(fd, buf, n, &r, &e)) return 0;
  if (r < 0) errno = e;
  *res = (ssize_t)r;
  return 1;
}

int blz_link_try_lseek(int fd, off_t off, int whence, off_t* res) {
  if (!on()) return 0;
  int64_t p = 0;
  int e = 0;
  if (!blzlink::try_lseek(fd, (int64_t)off, whence, &p, &e)) return 0;
  if (p < 0) errno = e;
  *res = (off_t)p;
  return 1;
}

void blz_link_fix_fstat(int fd, struct stat* st) {
  if (!on() || !st) return;
  int64_t size = (int64_t)st->st_size;
  blzlink::fix_fstat(fd, &size);
  st->st_size = (off_t)size;
}

// One AIO read request on a remote fd as the engine's request. Like the real AIO, its result struct reads "not done" until the read is finished.
static blzlink::AioReq blz_remote_req(SceKernelAioRWRequest* q) {
  blzlink::AioReq a{};
  a.offset = (int64_t)q->offset;
  a.nbyte = q->nbyte;
  a.buf = q->buf;
  a.res_return = q->result ? reinterpret_cast<int64_t*>(&q->result->returnValue) : nullptr;
  a.res_state = q->result ? reinterpret_cast<uint32_t*>(&q->result->state) : nullptr;
  a.fd = q->fd;
  if (q->result) {
    __atomic_store_n(&q->result->returnValue, (int64_t)0, __ATOMIC_RELAXED);
    __atomic_store_n(&q->result->state, (uint32_t)0, __ATOMIC_RELEASE);
  }
  return a;
}

// the emulator's own AIO (its APR reactor): with "async 1" the remote requests go to the fetch workers and the submit returns at once
// (the reactor polls them like real AIO); otherwise each runs here, one after the other
int blz_link_aio_submit(void* requests, int count, int priority, int* ids, int* rc) {
  if (!on() || !requests || !ids || count <= 0 || count > SCE_KERNEL_AIO_REQUEST_NUM_MAX) return 0;
  SceKernelAioRWRequest* reqs = static_cast<SceKernelAioRWRequest*>(requests);
  bool isRemote[SCE_KERNEL_AIO_REQUEST_NUM_MAX];
  int nr = 0;
  for (int i = 0; i < count; ++i) { isRemote[i] = blzlink::is_remote_fd(reqs[i].fd); if (isRemote[i]) ++nr; }
  if (nr == 0) return 0;
  int rids[SCE_KERNEL_AIO_REQUEST_NUM_MAX];
  if (!blzlink::aio_reserve(nr, rids)) { *rc = SCE_KERNEL_ERROR_EAGAIN; return 1; }
  const int nl = count - nr;
  if (nl > 0) {                                           // requests for ordinary files go the normal way first (the recursion finds no remote fd and falls through)
    SceKernelAioRWRequest lreq[SCE_KERNEL_AIO_REQUEST_NUM_MAX];
    SceKernelAioSubmitId lids[SCE_KERNEL_AIO_REQUEST_NUM_MAX];
    int lpos[SCE_KERNEL_AIO_REQUEST_NUM_MAX];
    int k = 0;
    for (int i = 0; i < count; ++i) if (!isRemote[i]) { lreq[k] = reqs[i]; lpos[k] = i; ++k; }
    const int r = sceKernelAioSubmitReadCommandsMultiple(lreq, nl, priority, lids);
    if (r != 0) { blzlink::aio_release(rids, nr); *rc = r; return 1; }
    for (int j = 0; j < nl; ++j) ids[lpos[j]] = lids[j];
  }
  int j = 0;
  for (int i = 0; i < count; ++i) {
    if (!isRemote[i]) continue;
    const int id = rids[j++];
    const blzlink::AioReq a = blz_remote_req(&reqs[i]);
    if (!blzlink::aio_queue(a, id)) blzlink::aio_execute(a);
    ids[i] = id;
  }
  *rc = 0;
  return 1;
}

int blz_link_aio_poll(int* ids, int count, int* states, int* rc) {
  if (!on() || !ids || !states || count <= 0 || count > SCE_KERNEL_AIO_ID_NUM_MAX) return 0;
  int mine = 0;
  for (int i = 0; i < count; ++i) if (blzlink::aio_is_mine(ids[i])) ++mine;
  if (mine == 0) return 0;
  int lids[SCE_KERNEL_AIO_ID_NUM_MAX], lstates[SCE_KERNEL_AIO_ID_NUM_MAX], lpos[SCE_KERNEL_AIO_ID_NUM_MAX];
  int nl = 0;
  for (int i = 0; i < count; ++i) {
    if (blzlink::aio_is_mine(ids[i])) states[i] = blzlink::aio_poll_one(ids[i]);
    else { lids[nl] = ids[i]; lpos[nl] = i; ++nl; }
  }
  if (nl > 0) {
    const int r = sceKernelAioPollRequests(lids, nl, lstates);
    if (r != 0) { *rc = r; return 1; }
    for (int k = 0; k < nl; ++k) states[lpos[k]] = lstates[k];
  }
  *rc = 0;
  return 1;
}

int blz_link_aio_delete(int* ids, int count, int* results, int* rc) {
  if (!on() || !ids || !results || count <= 0 || count > SCE_KERNEL_AIO_ID_NUM_MAX) return 0;
  int mine = 0;
  for (int i = 0; i < count; ++i) if (blzlink::aio_is_mine(ids[i])) ++mine;
  if (mine == 0) return 0;
  int lids[SCE_KERNEL_AIO_ID_NUM_MAX], lres[SCE_KERNEL_AIO_ID_NUM_MAX], lpos[SCE_KERNEL_AIO_ID_NUM_MAX];
  int nl = 0;
  for (int i = 0; i < count; ++i) {
    if (blzlink::aio_is_mine(ids[i])) results[i] = blzlink::aio_delete_one(ids[i]);
    else { lids[nl] = ids[i]; lpos[nl] = i; ++nl; }
  }
  if (nl > 0) {
    const int r = sceKernelAioDeleteRequests(lids, nl, lres);
    if (r != 0) { *rc = r; return 1; }
    for (int k = 0; k < nl; ++k) results[lpos[k]] = lres[k];
  }
  *rc = 0;
  return 1;
}

// ---- the game's own AIO (fork built with AMPR_PROCESS_AIO_INTERCEPT=1). Samson (PPSA38474) imports sceKernelAioSubmitReadCommands + sceKernelAioWaitRequest.
// sceKernelAioSubmitReadCommands: ONE id for the whole batch. Remote requests run now (their result structs are filled before we return); the
// local ones go to the normal single submit. All remote -> one virtual id (polls/waits as completed); mixed -> the real id of the local part,
// so the normal wait covers the batch (the remote part is already finished).
int blz_link_aio_submit_batch(void* requests, int count, int priority, int* id, int* rc) {
  if (!on() || !requests || !id || count <= 0 || count > SCE_KERNEL_AIO_REQUEST_NUM_MAX) return 0;
  SceKernelAioRWRequest* reqs = static_cast<SceKernelAioRWRequest*>(requests);
  bool isRemote[SCE_KERNEL_AIO_REQUEST_NUM_MAX];
  int nr = 0;
  for (int i = 0; i < count; ++i) { isRemote[i] = blzlink::is_remote_fd(reqs[i].fd); if (isRemote[i]) ++nr; }
  if (nr == 0) return 0;
  const int nl = count - nr;
  int bid = 0;
  if (nl == 0) {
    if (!blzlink::aio_reserve(1, &bid)) { *rc = SCE_KERNEL_ERROR_EAGAIN; return 1; }
  } else {
    SceKernelAioRWRequest lreq[SCE_KERNEL_AIO_REQUEST_NUM_MAX];
    int k = 0;
    for (int i = 0; i < count; ++i) if (!isRemote[i]) lreq[k++] = reqs[i];
    SceKernelAioSubmitId lid = 0;
    const int r = sceKernelAioSubmitReadCommands(lreq, nl, priority, &lid);   // the recursion finds no remote fd and falls through
    if (r != 0) { *rc = r; return 1; }
    bid = (int)lid;
  }
  blzlink::AioReq remote[SCE_KERNEL_AIO_REQUEST_NUM_MAX];   // done before we return (in parallel on the fetch workers when async is on)
  int n = 0;
  for (int i = 0; i < count; ++i) if (isRemote[i]) remote[n++] = blz_remote_req(&reqs[i]);
  blzlink::aio_run_all(remote, n);
  *id = bid;
  *rc = 0;
  return 1;
}

int blz_link_aio_poll_id(int id, int* state, int* rc) {                       // never blocks
  if (!on() || !state || !blzlink::aio_is_mine(id)) return 0;
  return blz_link_aio_poll(&id, 1, state, rc);
}

int blz_link_aio_wait_id(int id, int* state, void* usec, int* rc) {          // waits while a fetch worker still has it
  if (!on() || !state || !blzlink::aio_is_mine(id)) return 0;
  return blz_link_aio_wait_many(&id, 1, state, SCE_KERNEL_AIO_WAIT_AND, usec, rc);
}

int blz_link_aio_cancel_id(int id, int* state, int* rc) {                     // nothing to cancel: report the state (completed, or ESRCH)
  if (!on() || !state || !blzlink::aio_is_mine(id)) return 0;
  return blz_link_aio_poll(&id, 1, state, rc);
}

int blz_link_aio_delete_id(int id, int* result, int* rc) {                    // like the emulator's own virtual ids: the multi delete with one id
  if (!on() || !result || !blzlink::aio_is_mine(id)) return 0;
  return blz_link_aio_delete(&id, 1, result, rc);
}

// sceKernelAioWaitRequests with some of OUR ids (a fetch worker may still have them): AND = until all are terminal, OR = until one is.
// The ordinary ids are polled alongside. usec: in = the limit (null = none), out = what is left; ETIMEDOUT when it runs out (like the emulator's own wait).
int blz_link_aio_wait_many(int* ids, int count, int* states, uint32_t mode, void* usec, int* rc) {
  if (!on() || !ids || !states || count <= 0 || count > SCE_KERNEL_AIO_ID_NUM_MAX) return 0;
  int mine = 0;
  for (int i = 0; i < count; ++i) if (blzlink::aio_is_mine(ids[i])) ++mine;
  if (mine == 0) return 0;
  SceKernelUseconds* us = static_cast<SceKernelUseconds*>(usec);
  const bool waitAll = count == 1 || mode == SCE_KERNEL_AIO_WAIT_AND;
  const uint64_t limit = us ? (uint64_t)*us : 0;
  const uint64_t t0 = op_now_us();
  int lids[SCE_KERNEL_AIO_ID_NUM_MAX], lstates[SCE_KERNEL_AIO_ID_NUM_MAX], lpos[SCE_KERNEL_AIO_ID_NUM_MAX];
  int nl = 0;
  for (int i = 0; i < count; ++i) if (!blzlink::aio_is_mine(ids[i])) { lids[nl] = ids[i]; lpos[nl] = i; ++nl; }
  for (;;) {
    for (int i = 0; i < count; ++i) if (blzlink::aio_is_mine(ids[i])) states[i] = blzlink::aio_poll_one(ids[i]);
    if (nl > 0) {
      const int r = sceKernelAioPollRequests(lids, nl, lstates);
      if (r != 0) { *rc = r; return 1; }
      for (int k = 0; k < nl; ++k) states[lpos[k]] = lstates[k];
    }
    bool any = false, all = true;
    for (int i = 0; i < count; ++i) {
      const int st = states[i] < 0 ? -1 : (states[i] & ~(int)SCE_KERNEL_AIO_STATE_NOTIFIED);
      const bool terminal = st < 0 || st == SCE_KERNEL_AIO_STATE_COMPLETED || st == SCE_KERNEL_AIO_STATE_ABORTED;
      any = any || terminal;
      all = all && terminal;
    }
    const uint64_t el = op_now_us() - t0;
    if ((waitAll && all) || (!waitAll && any)) {
      if (us) *us = (SceKernelUseconds)(el < limit ? limit - el : 0);
      *rc = 0;
      return 1;
    }
    if (us && el >= limit) { *us = 0; *rc = SCE_KERNEL_ERROR_ETIMEDOUT; return 1; }
    (void)sceKernelUsleep(100);
  }
}

int blz_link_aio_cancel_many(int* ids, int count, int* states, int* rc) {
  if (!on() || !ids || !states || count <= 0 || count > SCE_KERNEL_AIO_ID_NUM_MAX) return 0;
  int mine = 0;
  for (int i = 0; i < count; ++i) if (blzlink::aio_is_mine(ids[i])) ++mine;
  if (mine == 0) return 0;
  int lids[SCE_KERNEL_AIO_ID_NUM_MAX], lstates[SCE_KERNEL_AIO_ID_NUM_MAX], lpos[SCE_KERNEL_AIO_ID_NUM_MAX];
  int nl = 0;
  for (int i = 0; i < count; ++i) {
    if (blzlink::aio_is_mine(ids[i])) states[i] = blzlink::aio_poll_one(ids[i]);
    else { lids[nl] = ids[i]; lpos[nl] = i; ++nl; }
  }
  if (nl > 0) {
    const int r = sceKernelAioCancelRequests(lids, nl, lstates);
    if (r != 0) { *rc = r; return 1; }
    for (int k = 0; k < nl; ++k) states[lpos[k]] = lstates[k];
  }
  *rc = 0;
  return 1;
}

}  // extern "C"
