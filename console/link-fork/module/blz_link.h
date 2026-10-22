// SPDX-License-Identifier: GPL-3.0-or-later
// blz_link.h - the engine of the PS5 "SMB link" (tools/smblink): a game file that is NOT on the console (a 0-byte placeholder in the stub folder) is read from a server on
// mac1 (tools/imgserve, HTTP Range) instead. Platform-free core: every platform call comes in through Ops, so the host test can drive it against a mock server and a
// fake "real file". The PRX glue (blz_link_game.cpp) wires the ops to the AMPR emulator's own helpers and hooks.
//
// Modes (config line "mode ..."):
//   off     the module does nothing (this is also what a missing config means: stock behaviour)
//   emul    local file is the truth; APR/AIO reads of the file go through OUR virtual-AIO layer (proves the layer with local bytes, no network)
//   verify  local file is the truth; every pread/read/AIO read is ALSO fetched from the server and compared; mismatches are logged, the game always gets the local bytes
//   remote  the server is the truth; the local file is an empty placeholder (open + pread/read/lseek/fstat + AIO are all served from the server)
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "blz_http.h"

namespace blzlink {

enum Mode : int { kOff = 0, kEmul = 1, kVerify = 2, kRemote = 3 };

constexpr int kMaxFiles = 1024;        // remote files per game (16 -> 64 -> 1024 on 21 Oct: COD / FC 27 / Outlaws have hundreds of big files;
                                       // a RemoteFile is only rel + size since then = 184 B, ~188 KB per Config; URL and /app0 path are built on use)
constexpr size_t kMaxUrl = 1024;       // base + url-encoded rel
constexpr int kMaxFds = 64;
constexpr int kMaxAio = 1024;
constexpr int kMaxConns = 8;                 // = the most fetch workers (one per connection)
constexpr int kAioTag = 0x6A000000;          // virtual AIO ids: tag | gen << 16 | slot (the pack layer uses 0x65-0x68)

struct RemoteFile {
  char rel[176];        // "re_chunk_000.pak" (relative to /app0)
  uint64_t size;
};

// "<base><url-encoded rel>" of file f into out (cap >= kMaxUrl). false = it does not fit.
bool file_url(const char* base, const RemoteFile& f, char* out, size_t cap);

constexpr int kMaxGot = 4;
struct GotPatch {       // cfg "got <import name> <GOT slot offset> <PLT stub offset>" (hex, relative to the executable base; tools/eboot_imports.py prints them)
  char name[64];
  uint64_t got;
  uint64_t plt;
};

struct Config {
  int mode;
  uint8_t ip[4];
  uint16_t port;
  char base[240];
  uint32_t nfiles;
  RemoteFile files[kMaxFiles];
  uint32_t block_kb;        // cache block size in KiB for small reads (0 = no cache)        default 256
  uint32_t cache_mb;        // cache size in MiB                                              default 32
  uint32_t timeout_ms;      // network silence limit                                          default 8000
  uint32_t conns;           // parallel keep-alive connections (1..8) = fetch workers         default 2
  uint32_t async;           // "async 1": remote AIO requests go to `conns` worker threads and the submit returns at once (the
                            // emulator polls them like real AIO); 0 = every request runs inside the submit, one at a time   default 0
  uint32_t readahead_kb;    // sequential streams: whole cache blocks are fetched for them, and the blocks up to this far past the
                            // request are prefetched by the workers (needs async + a cache; 0 = off)                       default 0
  uint32_t stats_s;         // seconds between stats lines                                                                  default 10
  uint32_t log_first;       // log the first N operations of each kind in detail              default 24
  uint32_t verify_mb;       // verify mode: compare at most this many MiB per request         default 4
  uint32_t verify_every;    // verify mode: compare every Nth request only (1 = all)           default 1
  uint32_t chunk_mb;        // largest single HTTP Range request                               default 8
  uint32_t rcvbuf_kb;       // SO_RCVBUF to ask for (0 = default)                              default 1024
  uint32_t trace_opens;     // log the first N opens of ANY path (path, flags, fd): finds out what the game really opens   default 0
  uint32_t trace_io;        // log the first N pread/read calls on ANY fd                                                  default 0
  uint32_t trace_apr;       // log the first N APR path resolutions (what the game asks the emulator for)                  default 0
  uint32_t appcontent_temp_fail;  // "appcontent_temp fail": answer sceAppContentTemporaryDataMount2 INSIDE the game with an error, so the call never
                                  // reaches ShellCore (works in every mode, also "off"; Onimusha PPSA27836, 21 Oct 2026)                default 0 = pass
  uint32_t ngot;                  // the game's own import slots to point at the shim (needed when the game loads libSceAppContent itself)
  GotPatch got[kMaxGot];
  uint32_t disk_cache_mb;   // NETCACHE on the console's own storage (smb-link-6, 21 Oct 2026): every block fetched from mac1 is also written to
                            // <disk_cache_dir>/<game>/f<N>.dat (sparse) + a hash in cache.map, and later reads of it come from there
                            // (Unreal games re-read the same data: The Mound re-read ~400 MB 26x per 96 s at the menu). Stops adding
                            // at this many MiB (no eviction). 0 = off                                                            default 0
  char disk_cache_dir[128]; // default /mnt/ext1/blz-cache (the M.2; SMP mounts it read-write into fakelib games' sandboxes)
};

// Parse the text config (see tools/smblink/README.md). false + a message in err on a bad line or an unusable result.
bool parse_config(const char* text, size_t len, Config* out, char* err, size_t errcap);

struct Ops {
  long (*pread)(int fd, void* buf, size_t n, int64_t off);          // REAL file ops (bypass every hook)
  long (*read)(int fd, void* buf, size_t n);
  int64_t (*lseek)(int fd, int64_t off, int whence);
  int (*get_errno)();
  uint64_t (*now_us)();
  void (*sleep_us)(unsigned us);
  void (*log)(const char* line);                                     // one line, no newline
  bool (*start_thread)(void* (*fn)(void*), void* arg);               // start a detached thread (null = no workers: async stays off)
  int (*open)(const char* path, int flags, int mode);                // REAL open/pwrite/mkdir for the disk netcache (null = no netcache)
  long (*pwrite)(int fd, const void* buf, size_t n, int64_t off);
  int (*mkdir)(const char* path, int mode);
  blz::NetOps net;
};

// Install the config and ops (once). Returns false if the config is unusable; the engine then stays inactive.
bool configure(const Config& cfg, const Ops& ops);
bool active();
int mode();

// path (as the game or the emulator opens it, e.g. "/app0/re_chunk_000.pak") -> index into Config::files, or -1
int match_path(const char* path);
// after a successful open: remember fd -> file (only for read-only opens of a configured path)
void note_open(const char* path, int flags, int fd);
void note_close(int fd);
void trace_open(const char* path, int flags, int fd);                        // budgeted by trace_opens
void trace_io(const char* what, int fd, size_t n, int64_t off);              // budgeted by trace_io (logged BEFORE the call: the result is not known here)
void trace_apr(const char* what, const char* path, int rc, uint64_t size);  // budgeted by trace_apr
bool is_remote_fd(int fd);

// pread/read/lseek/fstat interception. Each returns true when it handled the call (result in *res, errno in *err).
bool try_pread(int fd, void* buf, size_t n, int64_t off, long* res, int* err);
bool try_read(int fd, void* buf, size_t n, long* res, int* err);
bool try_lseek(int fd, int64_t off, int whence, int64_t* res, int* err);
void fix_fstat(int fd, int64_t* st_size);

// virtual AIO (APR reads). One request: where to read, where to put it, where to report.
struct AioReq {
  int64_t offset;
  size_t nbyte;
  void* buf;
  int64_t* res_return;      // SceKernelAioResult::returnValue (may be null)
  uint32_t* res_state;      // SceKernelAioResult::state (may be null)
  int fd;
};
bool aio_reserve(int count, int* ids);                 // allocate `count` ids (all or nothing)
void aio_release(const int* ids, int count);           // give ids back without having run them (rollback)
void aio_execute(const AioReq& r);                     // do the read now and report through the request's result struct
bool aio_queue(const AioReq& r, int id);               // async: a worker does the read (id = this request's reserved id); false = not queued, call aio_execute
void aio_run_all(const AioReq* reqs, int count);       // do `count` reads (in parallel on the workers when async is on) and return when all are done
bool aio_is_mine(int id);
int aio_poll_one(int id);                              // COMPLETED (3), SUBMITTED (1) / PROCESSING (2) while a worker has it, or a negative SCE error
int aio_wait_one(int id, uint64_t timeout_us);         // like poll, but waits (timeout 0 = no limit) while a worker has it
int aio_delete_one(int id);                            // 0 or a negative SCE error; waits while a worker still has the request (its buffer may be freed after)
bool async_on();                                       // workers are running

// engine statistics as one log line (also emitted periodically by the engine itself)
void format_stats(char* out, size_t cap);

// test hooks
void test_reset();

}  // namespace blzlink
