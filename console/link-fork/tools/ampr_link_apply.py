#!/usr/bin/env python3
"""ampr_link_apply.py - overlay the PS5 SMB link (tools/smblink/module) onto a clean checkout of drakmor/ampr_emu (GPL-3) at the pinned commit.

    python3 -I ampr_link_apply.py <ampr_emu checkout> <module dir (tools/smblink/module)> [--no-git-check]

Every change is an exact-string substitution that must match exactly once, so a changed upstream is noticed instead of silently half-patched:
  new files   include/{blz_http.h, blz_link.h, blz_link_hooks.h}   src/{blz_http.cpp, blz_link_core.cpp, blz_link_game.cpp}
  Makefile                 CPP_SOURCES += the three .cpp files
  include/ampr_emu_kernel_file.h   ampr_real_posix_open: tell the link about every successful real open (the funnel of the game's open/sceKernelOpen AND of the emulator's
                                   own APR opens); ampr_real_physical_close: forget the fd
  src/ampr_emu_pack.cpp    sceKernelClose / posix_close_emul: forget the fd; posix_fstat_emul: report the true size of a placeholder; posix_lseek_emul, posix_pread_emul and
                           posix_read_emul (the last two exist when built with AMPR_PROCESS_SYNC_READ_INTERCEPT=1): serve a remote file; sceKernelAioSubmitReadCommandsMultiple /
                           sceKernelAioPollRequests / sceKernelAioDeleteRequests: virtual AIO for requests on a remote fd (everything else falls through to the original body);
                           sceKernelAioSubmitReadCommands (one id per batch) + the single-id Poll/Wait/Cancel/Delete + WaitRequests/CancelRequests: the same for the
                           GAME's own AIO calls, which reach these bodies only in a build with AMPR_PROCESS_AIO_INTERCEPT=1 (build-fork.sh AMPR_PROCESS_AIO_INTERCEPT=1)
  src/ampr_emu_apr_services.cpp  resolve_index_path_to_sce: trace which paths the game's APR calls ask for (config trace_apr)
  src/sceampr_exports.cpp  module_start: one banner line after the hooks are installed + blz_link_early (temp-data shim, cfg appcontent_temp fail)
  src/ampr_libkernel_hook.cpp  blz_detour_loaded_symbol: the emulator's inline-detour engine on an export of any loaded module (the shim's hook)
Pinned upstream: cfa85df379f6eeeb165d7badf9b648e266fe77b7 (master, 2026-09-06, 4 commits after tag 0.4.2.1).
"""
import os
import shutil
import sys

PIN = "cfa85df379f6eeeb165d7badf9b648e266fe77b7"


def sub(path, old, new, count=1):
    # the upstream sources use CRLF line endings: keep them (anchors and replacements are written with \n and converted to the file's style)
    with open(path, encoding="utf-8", newline="") as fh:
        s = fh.read()
    if "\r\n" in s:
        old, new = old.replace("\n", "\r\n"), new.replace("\n", "\r\n")
    if s.count(old) != count:
        raise SystemExit(f"anchor not found exactly {count}x in {path}: {old[:90]!r} (found {s.count(old)})")
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(s.replace(old, new))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    src, mod = args[0], args[1]
    head = "(unchecked)"
    if "--no-git-check" not in sys.argv:
        head = os.popen(f"git -C '{src}' rev-parse HEAD").read().strip()
        if head != PIN:
            raise SystemExit(f"checkout is at {head}, pinned commit is {PIN}")
        if os.popen(f"git -C '{src}' status --porcelain").read().strip():
            raise SystemExit("the checkout is not clean: use a fresh clone")
    for name, dst in (("blz_http.h", "include"), ("blz_link.h", "include"), ("blz_link_hooks.h", "include"),
                      ("blz_http.cpp", "src"), ("blz_link_core.cpp", "src"), ("blz_link_game.cpp", "src")):
        shutil.copy(os.path.join(mod, name), os.path.join(src, dst, name))

    sub(os.path.join(src, "Makefile"), "  src/ampr_libkernel_hook.cpp \\\n  src/hde64.cpp\n",
        "  src/ampr_libkernel_hook.cpp \\\n  src/blz_http.cpp \\\n  src/blz_link_core.cpp \\\n  src/blz_link_game.cpp \\\n  src/hde64.cpp\n")

    kf = os.path.join(src, "include", "ampr_emu_kernel_file.h")
    sub(kf, '#include "ampr_emu_pack.h"\n\n#include <_kernel.h>', '#include "ampr_emu_pack.h"\n#include "blz_link_hooks.h"\n\n#include <_kernel.h>')
    sub(kf, "    if (fn) return fn(path, flags, mode);\n",
        "    if (fn) {\n        const int blzFd = fn(path, flags, mode);\n        if (blzFd >= 0) blz_link_note_open(path, flags, blzFd);   // SMB link: remember placeholder fds\n        return blzFd;\n    }\n")
    sub(kf, "static inline int ampr_real_physical_close(int fd) {\n    using Fn = int (*)(int);\n",
        "static inline int ampr_real_physical_close(int fd) {\n    using Fn = int (*)(int);\n    blz_link_note_close(fd);\n")

    pk = os.path.join(src, "src", "ampr_emu_pack.cpp")
    sub(pk, '#include "ampr_emu_pack.h"\n\n#include "ampr_emu_aio_broker.h"', '#include "ampr_emu_pack.h"\n#include "blz_link_hooks.h"\n\n#include "ampr_emu_aio_broker.h"')
    sub(pk, 'extern "C" int sceKernelClose(int fd) {\n    bool handled = false;\n',
        'extern "C" int sceKernelClose(int fd) {\n    blz_link_note_close(fd);\n    bool handled = false;\n')
    sub(pk, 'extern "C" int posix_close_emul(int fd) {\n    bool handled = false;\n',
        'extern "C" int posix_close_emul(int fd) {\n    blz_link_note_close(fd);\n    bool handled = false;\n')
    sub(pk, '    KernelFstatFn fn = real_fstat();\n    return ampr_klog_io_hook_result(\n        "fstat", fn ? fn(fd, stat) : posix_missing_original<int>());\n',
        '    KernelFstatFn fn = real_fstat();\n    const int blzRc = fn ? fn(fd, stat) : posix_missing_original<int>();\n    if (blzRc == 0) blz_link_fix_fstat(fd, stat);\n    return ampr_klog_io_hook_result("fstat", blzRc);\n')
    sub(pk, 'extern "C" off_t posix_lseek_emul(int fd, off_t offset, int whence) {\n    bool handled = false;\n',
        'extern "C" off_t posix_lseek_emul(int fd, off_t offset, int whence) {\n    {\n        off_t blzPos = 0;\n        if (blz_link_try_lseek(fd, offset, whence, &blzPos)) return blzPos;\n    }\n    bool handled = false;\n')
    sub(pk, 'extern "C" ssize_t posix_pread_emul(int fd, void* buffer, size_t size,\n                                      off_t offset) {\n    bool handled = false;\n',
        'extern "C" ssize_t posix_pread_emul(int fd, void* buffer, size_t size,\n                                      off_t offset) {\n    {\n        ssize_t blzRes = 0;\n        if (blz_link_try_pread(fd, buffer, size, offset, &blzRes)) return blzRes;\n    }\n    bool handled = false;\n')
    sub(pk, 'extern "C" ssize_t posix_read_emul(int fd, void* buffer, size_t size) {\n    bool handled = false;\n',
        'extern "C" ssize_t posix_read_emul(int fd, void* buffer, size_t size) {\n    {\n        ssize_t blzRes = 0;\n        if (blz_link_try_read(fd, buffer, size, &blzRes)) return blzRes;\n    }\n    bool handled = false;\n')
    sub(pk, 'extern "C" int sceKernelAioSubmitReadCommandsMultiple(\n    SceKernelAioRWRequest requests[], int count, int priority,\n    SceKernelAioSubmitId ids[]) {\n#if !AMPR_EMU_PACK_ENABLE\n',
        'extern "C" int sceKernelAioSubmitReadCommandsMultiple(\n    SceKernelAioRWRequest requests[], int count, int priority,\n    SceKernelAioSubmitId ids[]) {\n    {\n        int blzRc = 0;\n        if (blz_link_aio_submit(requests, count, priority, ids, &blzRc)) return blzRc;\n    }\n#if !AMPR_EMU_PACK_ENABLE\n')
    sub(pk, 'extern "C" int sceKernelAioPollRequests(SceKernelAioSubmitId ids[],\n                                         int count,\n                                         int states[]) {\n#if !AMPR_EMU_PACK_ENABLE\n',
        'extern "C" int sceKernelAioPollRequests(SceKernelAioSubmitId ids[],\n                                         int count,\n                                         int states[]) {\n    {\n        int blzRc = 0;\n        if (blz_link_aio_poll(ids, count, states, &blzRc)) return blzRc;\n    }\n#if !AMPR_EMU_PACK_ENABLE\n')
    sub(pk, 'extern "C" int sceKernelAioDeleteRequests(SceKernelAioSubmitId ids[],\n                                           int count,\n                                           int results[]) {\n#if !AMPR_EMU_PACK_ENABLE\n',
        'extern "C" int sceKernelAioDeleteRequests(SceKernelAioSubmitId ids[],\n                                           int count,\n                                           int results[]) {\n    {\n        int blzRc = 0;\n        if (blz_link_aio_delete(ids, count, results, &blzRc)) return blzRc;\n    }\n#if !AMPR_EMU_PACK_ENABLE\n')
    # the GAME's own AIO calls reach these bodies only in a build with AMPR_PROCESS_AIO_INTERCEPT=1 (Samson PPSA38474: sceKernelAioSubmitReadCommands +
    # sceKernelAioWaitRequest). One id per batch (single submit) and the single-id poll/wait/cancel/delete + multi wait/cancel must know the link's ids.
    def first(fn_head, call):
        sub(pk, fn_head + "#if !AMPR_EMU_PACK_ENABLE\n", fn_head + "    {\n        int blzRc = 0;\n        if (" + call + ") return blzRc;\n    }\n#if !AMPR_EMU_PACK_ENABLE\n")
    first('extern "C" int sceKernelAioSubmitReadCommands(\n    SceKernelAioRWRequest requests[], int count, int priority,\n    SceKernelAioSubmitId* id) {\n',
          "blz_link_aio_submit_batch(requests, count, priority, id, &blzRc)")
    first('extern "C" int sceKernelAioPollRequest(SceKernelAioSubmitId id,\n                                         int* stateOut) {\n    if (!stateOut) return SCE_KERNEL_ERROR_EINVAL;\n',
          "blz_link_aio_poll_id(id, stateOut, &blzRc)")
    first('extern "C" int sceKernelAioWaitRequest(SceKernelAioSubmitId id,\n                                         int* stateOut,\n                                         SceKernelUseconds* usec) {\n    if (!stateOut) return SCE_KERNEL_ERROR_EINVAL;\n',
          "blz_link_aio_wait_id(id, stateOut, usec, &blzRc)")
    first('extern "C" int sceKernelAioWaitRequests(SceKernelAioSubmitId ids[],\n                                          int count,\n                                          int states[],\n                                          uint32_t mode,\n                                          SceKernelUseconds* usec) {\n',
          "blz_link_aio_wait_many(ids, count, states, mode, usec, &blzRc)")
    first('extern "C" int sceKernelAioCancelRequest(SceKernelAioSubmitId id,\n                                           int* stateOut) {\n    if (!stateOut) return SCE_KERNEL_ERROR_EINVAL;\n',
          "blz_link_aio_cancel_id(id, stateOut, &blzRc)")
    first('extern "C" int sceKernelAioCancelRequests(SceKernelAioSubmitId ids[],\n                                            int count,\n                                            int states[]) {\n',
          "blz_link_aio_cancel_many(ids, count, states, &blzRc)")
    first('extern "C" int sceKernelAioDeleteRequest(SceKernelAioSubmitId id,\n                                           int* resultOut) {\n    if (!resultOut) return SCE_KERNEL_ERROR_EINVAL;\n',
          "blz_link_aio_delete_id(id, resultOut, &blzRc)")

    sv = os.path.join(src, "src", "ampr_emu_apr_services.cpp")
    sub(sv, '#include "ampr_emu_sync.h"\n\n#include <atomic>', '#include "ampr_emu_sync.h"\n#include "blz_link_hooks.h"\n\n#include <atomic>')
    sub(sv, "    const int rc = ampr_index_resolve_path_to_id(path, &id, outSize ? &size : nullptr);\n    if (rc != 0) {\n        return ampr_sce_errno_from_posix(rc < 0 ? -rc : rc);\n    }\n    *outId = id;",
        "    const int rc = ampr_index_resolve_path_to_id(path, &id, outSize ? &size : nullptr);\n    blz_link_trace_apr(path, rc, size);\n    if (rc != 0) {\n        return ampr_sce_errno_from_posix(rc < 0 ? -rc : rc);\n    }\n    *outId = id;")

    ex = os.path.join(src, "src", "sceampr_exports.cpp")
    sub(ex, '#include "ampr_libkernel_hook.h"\n\nusing u64 = uint64_t;', '#include "ampr_libkernel_hook.h"\n#include "blz_link_hooks.h"\n\nusing u64 = uint64_t;')
    sub(ex, "    (void)args;\n    (void)argp;\n    return amprInstallLibkernelHooks();\n}",
        "    (void)args;\n    (void)argp;\n    const int blzHookRc = amprInstallLibkernelHooks();\n    blz_link_banner();   // SMB link: one klog line proving this library loaded\n    blz_link_early();    // SMB link: temp-data shim when the title cfg says appcontent_temp fail\n    return blzHookRc;\n}")

    hk = os.path.join(src, "src", "ampr_libkernel_hook.cpp")
    # the temp-data shim needs an inline detour on an export of ANOTHER module (libSceAppContent): reuse the emulator's own detour engine
    # (install_inline_detour + the loaded-module scan the external equeue hooks use), appended after its last function
    sub(hk, 'extern "C" AMPR_LIBKERNEL_HOOK_EXPORT void* amprResolveLibkernelFunction(const char* symbol) {\n    return resolve_libkernel_symbol(kKnownLibkernelHandle, symbol);\n}',
        'extern "C" AMPR_LIBKERNEL_HOOK_EXPORT void* amprResolveLibkernelFunction(const char* symbol) {\n    return resolve_libkernel_symbol(kKnownLibkernelHandle, symbol);\n}\n'
        '\n'
        '// SMB link (blazing): inline detour on export `symbol` of the first loaded module that has it (at most 4). 0 = installed, -4 = not loaded yet.\n'
        'extern "C" int blz_detour_loaded_symbol(const char* symbol, void* replacement, void** original) {\n'
        '    static InlineDetour s_detours[4];\n'
        '    static void* s_originals[4];\n'
        '    static int s_used = 0;\n'
        '    if (!symbol || !replacement) return -1;\n'
        '    if (s_used >= 4) return -5;\n'
        '    auto* const getModuleList = reinterpret_cast<KernelGetModuleListFn>(resolve_libkernel_symbol(kKnownLibkernelHandle, "sceKernelGetModuleList"));\n'
        '    if (!getModuleList) return -2;\n'
        '    static SceKernelModule modules[kLoadedModuleScanCapacity];\n'
        '    size_t count = 0;\n'
        '    if (getModuleList(modules, kLoadedModuleScanCapacity, &count) != 0) return -3;\n'
        '    if (count > kLoadedModuleScanCapacity) count = kLoadedModuleScanCapacity;\n'
        '    void* target = nullptr;\n'
        '    for (size_t i = 0; i < count; ++i) {\n'
        '        if (sceKernelDlsym(modules[i], symbol, &target) == 0 && target) break;\n'
        '        target = nullptr;\n'
        '    }\n'
        '    if (!target) return -4;\n'
        '    HookFailReason reason = kHookFailNone;\n'
        '    // the caller\'s slot gets the trampoline BEFORE the jump is written (install_inline_detour stores it first): no window without it\n'
        '    void** slot = original ? original : &s_originals[s_used];\n'
        '    if (!install_inline_detour(slot, s_detours[s_used], target, replacement, reason)) {\n'
        '        return -(100 + static_cast<int>(reason));\n'
        '    }\n'
        '    ++s_used;\n'
        '    return 0;\n'
        '}')
    print("SMB link overlay applied on", head)


if __name__ == "__main__":
    main()
