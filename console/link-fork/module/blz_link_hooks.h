// blz_link_hooks.h - the C entry points the patched AMPR emulator sources call (tools/smblink/tools/ampr_link_apply.py inserts the calls). Implemented in blz_link_game.cpp.
// Every function is a cheap no-op until the link is configured for the running title (no /data/blazing/link/<TID>.cfg = stock behaviour).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct stat;

void blz_link_banner(void);                                                   // one klog line from module_start: proves OUR library is the one that loaded
void blz_link_early(void);                                                    // module_start, after the banner: installs the temp-data shim when the title's cfg asks for it
int blz_link_tdm2_answer(uint32_t option, void* mountPoint);                  // the shim: replaces sceAppContentTemporaryDataMount2 (cfg "appcontent_temp fail")
int blz_link_sysmodule_load(uint16_t id);                                     // wraps sceSysmoduleLoadModule: installs the shim once libSceAppContent is loaded
int blz_link_temp_space_answer(const void* mountPoint, size_t* kb);           // guards sceAppContentTemporaryDataGetAvailableSpaceKb for the shim's empty mount point
// implemented in the emulator's src/ampr_libkernel_hook.cpp (added by ampr_link_apply.py): inline detour on the export `symbol` of the first loaded module
// that has it. 0 = installed (*original = trampoline to the old code), -4 = no loaded module exports it (yet), other negatives = failure
int blz_detour_loaded_symbol(const char* symbol, void* replacement, void** original);
void blz_link_note_open(const char* path, int flags, int fd);                // after every successful real open (first call initialises the link)
void blz_link_note_close(int fd);                                             // before every close
int blz_link_try_pread(int fd, void* buf, size_t n, off_t off, ssize_t* res); // 1 = handled (errno is set when *res == -1)
int blz_link_try_read(int fd, void* buf, size_t n, ssize_t* res);
int blz_link_try_lseek(int fd, off_t off, int whence, off_t* res);
void blz_link_trace_apr(const char* path, int rc, uint64_t size);              // the emulator resolved a path for the game's APR calls
void blz_link_fix_fstat(int fd, struct stat* st);                             // after a successful real fstat
// virtual AIO for APR reads: 1 = the call was handled (result in *rc), 0 = not ours, run the normal path
int blz_link_aio_submit(void* requests, int count, int priority, int* ids, int* rc);
int blz_link_aio_poll(int* ids, int count, int* states, int* rc);
int blz_link_aio_delete(int* ids, int count, int* results, int* rc);
// the GAME's own AIO calls (only reached when the fork is built with AMPR_PROCESS_AIO_INTERCEPT=1, e.g. Samson PPSA38474):
// sceKernelAioSubmitReadCommands gives ONE id for a whole batch; the single-id poll/wait/cancel/delete and the multi wait/cancel must know our ids
int blz_link_aio_submit_batch(void* requests, int count, int priority, int* id, int* rc);
int blz_link_aio_poll_id(int id, int* state, int* rc);                        // sceKernelAioPollRequest: never blocks
int blz_link_aio_wait_id(int id, int* state, void* usec, int* rc);            // sceKernelAioWaitRequest: waits while a fetch worker has the request
int blz_link_aio_cancel_id(int id, int* state, int* rc);
int blz_link_aio_delete_id(int id, int* result, int* rc);
int blz_link_aio_wait_many(int* ids, int count, int* states, uint32_t mode, void* usec, int* rc);
int blz_link_aio_cancel_many(int* ids, int count, int* states, int* rc);

#ifdef __cplusplus
}
#endif
