// mmap_probe.cpp - the FEASIBILITY HINGE for demand-paging the .rpkg (see DEMAND-PAGING.md).
//
// Question it answers: inside a PS5 title (007's process, with kstuff loaded), can WE install an
// in-process SIGSEGV handler that catches a fault on a PROT_NONE anonymous page, make the page
// readable, and let the faulting instruction retry? If yes -> "Try A" (a module-only demand pager
// is enough). If the handler never fires (kstuff / the kernel owns the fault path) -> "Try B"
// (kernel-side fault handling via our payload).
//
// This is intentionally tiny: one page, no network, no 86 GB. Run it in the SAME process state the
// real game runs in (shipped via pswait into a running title, or as a standalone payload after a
// game launch), and read the klog. A pass here is necessary before building blz_link_mmap.cpp.
//
// Build: with the ps5-payload-sdk (same chain as the AMPR fork / mods/tokon/tools/ampr-fork-build.sh).
// On PS5 the POSIX names below resolve through libkernel. `log()` is klog; swap in AMPR_KLOGF or
// sceKernelDebugOutText when built inside the module. A host build (macOS/Linux) also runs and must
// print PASS - that proves the LOGIC before the on-device run rules on the PLATFORM.

#include <signal.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef PROBE_LOG
#define PROBE_LOG(...) do { printf("[mmap_probe] " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#endif

namespace {
volatile sig_atomic_t g_handler_fired = 0;
volatile sig_atomic_t g_mprotect_rc = -999;
void*  g_page = nullptr;
size_t g_pagelen = 0;

// On fault: if it is OUR page, make it RW, drop a sentinel, and return so the load retries.
void on_segv(int sig, siginfo_t* info, void* /*uctx*/) {
  g_handler_fired = 1;
  void* at = info ? info->si_addr : nullptr;
  if (g_page && at >= g_page && at < (void*)((uint8_t*)g_page + g_pagelen)) {
    g_mprotect_rc = mprotect(g_page, g_pagelen, PROT_READ | PROT_WRITE);
    if (g_mprotect_rc == 0)
      ((volatile uint32_t*)g_page)[0] = 0xC0FFEEu;   // the byte the demand pager would have fetched
    return;                                           // retry the faulting instruction
  }
  // Not ours: re-raise the default so we never swallow a real crash.
  signal(sig, SIG_DFL);
  raise(sig);
}
}  // namespace

extern "C" int mmap_probe_run() {
  const size_t len = (size_t)sysconf(_SC_PAGESIZE);
  g_pagelen = len ? len : 0x4000;   // PS5 page is 16 KiB

  void* p = mmap(nullptr, g_pagelen, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) { PROBE_LOG("mmap(PROT_NONE) FAILED - cannot even reserve; approach needs rework"); return 2; }
  g_page = p;

  struct sigaction sa, old;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = on_segv;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGSEGV, &sa, &old) != 0) PROBE_LOG("sigaction(SIGSEGV) failed - note SIGBUS too on PS5");
  // PS5 file-backed faults can arrive as SIGBUS; install the same handler there as well.
  sigaction(SIGBUS, &sa, nullptr);

  PROBE_LOG("touching PROT_NONE page %p (len %zu) ...", g_page, g_pagelen);
  volatile uint32_t v = ((volatile uint32_t*)g_page)[0];   // <-- the fault

  sigaction(SIGSEGV, &old, nullptr);
  if (g_handler_fired && g_mprotect_rc == 0 && v == 0xC0FFEEu) {
    PROBE_LOG("PASS: in-process SIGSEGV caught the fault, mprotect+fill+retry worked (read 0x%X). Try A is viable.", v);
    return 0;
  }
  PROBE_LOG("FAIL: handler_fired=%d mprotect_rc=%d read=0x%X. If handler never fired on PS5 -> kstuff owns the fault; use Try B (kernel-side).",
            (int)g_handler_fired, (int)g_mprotect_rc, v);
  return 1;
}

#ifdef MMAP_PROBE_MAIN
int main() { return mmap_probe_run(); }
#endif
