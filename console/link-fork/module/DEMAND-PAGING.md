# Demand-paging the .rpkg (007 First Light / HITMAN 3, Glacier engine)

Goal (Markus, 22 Oct 2026): 007 (PPSA11386) and HITMAN 3 (PPSA16387) must STREAM
from mac1 like every other game - not stay full copies. Their whole payload is a
couple of memory-mapped .rpkg (007: `Runtime/chunk0.rpkg` 29 GB + `chunk1.rpkg`
57 GB = all 86.7 GB). The Glacier engine `mmap`s them and reads resource bytes
straight through the mapping.

## Why the normal starter crashes
The link hooks the emulator's libkernel slots `open/close/fstat/lseek/read/pread/
stat/sceKernelMprotect` (see `blz_link_game.cpp`). **There is no `mmap` slot.** So
the game's `mmap` of the .rpkg reaches the kernel unhooked and maps the on-disk
placeholder: 0 bytes -> SIGBUS on first touch (smb-link-6's 007 crash); a real-size
sparse file -> zeros -> garbage -> crash. `pread`-served streaming never applies
because the engine never calls `pread` for pak bytes - it faults them in through the
mapping. **Confirmed surface: `grep kAmprLibkernelHook_` shows no mmap/munmap slot.**

## Design: intercept mmap, serve pages on fault from imgserve
1. **mmap hook.** Add a `kAmprLibkernelHook_mmap` (and `munmap`) slot to OUR AMPR
   emulator fork (`mods/tokon/tools/ampr-fork-build.sh` builds it), OR patch the
   game's `sceKernelMmap` import in-process from the link (we already resolve the
   libkernel module to install the got patches). When the fd is a link-streamed
   file (we know its size from the cfg), DON'T map the file: `mmap` an **anonymous**
   region of the file's real size, `PROT_NONE`, and record `{base,len,fd,fileoff}`.
2. **Page-fault handler.** On access the PROT_NONE region faults. Catch it, map in
   the page(s):
   - **Try A - in-process SIGSEGV.** `sigaction(SIGSEGV, SA_SIGINFO)`; `si_addr`
     is the faulting address. Find its region, fetch the aligned range from imgserve
     (`blz_http.h` `http_get_range`, the same client the link already uses), write it
     into the pages, `sceKernelMprotect(..., READ)` (we hold that slot), return so the
     instruction retries. **FEASIBILITY HINGE (test first, see probe):** kstuff /
     the kernel may own the fault path on PS5 and never deliver SIGSEGV to us.
   - **Try B - kernel-side.** If SIGSEGV never fires, handle the fault in a payload
     via kstuff (we have kernel access): a vm_fault hook for our regions that pulls
     the range (kernel socket to mac1 or a shared ring to a userland fetcher) and
     inserts the page. Harder; only if A fails.
3. **Working set, not the whole file (57 GB won't fit under the 4096 MB payload cap).**
   - Fetch 1-4 MB around each fault (one Range GET), not 16 KB - network RTT dominates.
   - Read-ahead on sequential faults (bump the window; Glacier reads resources in runs).
   - LRU-evict mapped pages back to PROT_NONE to stay under `disk_cache_mb`/the RAM cap.
   - **Reuse the disk cache:** a faulted range also lands in `/mnt/ext1/blz-cache`
     (the netcache the starter already keeps), so a replay/re-fault is local, not network.

## Build + test (needs mac2 for the fork build, then a PHAT5 window)
0. **Probe the hinge first** (`mmap_probe.cpp`, small, no 86 GB): anonymous PROT_NONE
   page + SIGSEGV handler, touch it, log whether the handler fired and `mprotect`+retry
   worked. Decides Try A vs Try B before building the full path. Ship via pswait, read klog.
1. If A works: add the mmap/munmap slots to the fork, implement the handler + fetch +
   prefetch/LRU in a new `blz_link_mmap.cpp`, rebuild the fork, build the 007 starter
   (`chunk0/1.rpkg` become 0-byte streamed entries like any pak), push, launch.
2. **Verify:** 007 to title + in-game from the ~0.2 GB starter; every pak byte from
   mac1 (imgserve PPSA11386 Range hits, 0 local reads of chunk0/1); 0 crashes; GB on
   the M.2 stays near the cap, not 86 GB. Then repeat for HITMAN 3 (same engine).
3. Study-library the result (team archive) when it works.

## Status (smb-link-7, 22 Oct 2026)
Surface mapped and design fixed (above). NOT yet built: this needs the mac2 fork
toolchain + iterative on-device page-fault debugging (a multi-session job), and the
Try-A/Try-B hinge must be probed on PHAT5 first. `mmap_probe.cpp` is the next concrete
step. Refs: userfaultfd(2), CRIU --lazy-pages, PandaStack microVM RAM streaming.
