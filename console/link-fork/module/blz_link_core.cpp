// SPDX-License-Identifier: GPL-3.0-or-later
// blz_link_core.cpp - platform-free engine of the PS5 SMB link. See blz_link.h. No allocation except one malloc each for the block cache and the verify buffer (both
// libc imports the PRX is allowed to have), no memcmp/strcmp/strstr (not on the PRX import list), no STL, no exceptions.
#include "blz_link.h"

#include <atomic>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace blzlink {

namespace {

constexpr int kFdTable = 4096;                          // fds above this are not tracked
constexpr int kEio = 5;
constexpr int32_t kSceEio = (int32_t)0x80020005u, kSceEsrch = (int32_t)0x80020003u, kSceEinval = (int32_t)0x80020016u;
constexpr uint32_t kAioCompleted = 3;

struct Stats {
  uint64_t reqs, bytes, http_us, http_max_us, errs;
  uint64_t cache_hit, cache_miss, cache_bypass, cache_wait;
  uint64_t n_pread, n_read, n_lseek, n_aio, n_open;
  uint64_t v_ok_bytes, v_ok_reqs, v_bad, v_err;
  uint64_t circuit_trips;
  uint64_t dc_hit, dc_miss, dc_bad, dc_full;
  uint64_t q_jobs, q_sync, q_max, pf_queued, pf_done, pf_dropped, seq_reqs, blk_streams;
};

Config g_cfg;
Ops g_ops;
std::atomic<int> g_active{0};
std::atomic<int> g_lock{0};
Stats g_stats;

int16_t g_fdFile[kFdTable];                             // -1 = not a remote fd (initialised in configure)
int64_t g_fdPos[kFdTable];                              // remote-mode virtual file position

blz::HttpConn g_conn[kMaxConns];
std::atomic<int> g_connBusy[kMaxConns];

// AIO slot state (read/written with __atomic builtins): 0 = done (or never queued: executed in the submit), 1 = queued for a worker, 2 = a worker is reading it
struct AioSlot { uint8_t used; uint8_t gen; uint8_t state; };
AioSlot g_aio[kMaxAio];
int g_aioHint;
constexpr int kStateSubmitted = 1, kStateProcessing = 2;

// cache slot states: empty, loading (a thread is fetching it: never evicted), valid, queued (a prefetch job will fetch it; any reader may take the fetch over)
enum : uint32_t { kSlotEmpty = 0, kSlotLoading = 1, kSlotValid = 2, kSlotQueued = 3 };
struct CacheSlot { uint32_t state; uint32_t file; uint64_t block; uint32_t len; uint64_t lru; };
constexpr size_t kCacheStaticBytes = 32u << 20;        // module .bss: the game's libc heap is tiny (malloc(32 MiB) failed in Onimusha), the emulator's own pool lives in .bss too
constexpr uint32_t kMaxSlots = 1024;
constexpr size_t kVerifyStaticBytes = 4u << 20;
alignas(4096) uint8_t g_cacheStatic[kCacheStaticBytes];
CacheSlot g_slotStatic[kMaxSlots];
alignas(64) uint8_t g_verifyStatic[kVerifyStaticBytes];
CacheSlot* g_slots;
uint8_t* g_cacheMem;
uint32_t g_nslots;
size_t g_blockBytes;
uint64_t g_tick;                                         // LRU clock (under g_lock)

// sequential streams, noted in SUBMIT order (the workers may finish requests out of order): a request that starts where an earlier one of the same file ended
struct Stream { uint32_t file; uint32_t used; uint64_t next; uint64_t lru; };
constexpr int kStreams = 16;
Stream g_streams[kStreams];
uint64_t g_lastBlk[kMaxFiles];                           // per remote file: the last cache block a reader touched, + 1 (0 = none yet)
constexpr uint64_t kBlockGap = 2;                        // a reader that moves forward by 1..kBlockGap blocks walks the file in block order

// work for the fetch workers: AIO requests first, prefetches only when no request waits
struct Job { AioReq req; int32_t slot; uint32_t file; uint64_t block; uint8_t gen; uint8_t seq; uint8_t kind; uint8_t pad; std::atomic<int>* left; };
enum : uint8_t { kJobAio = 0, kJobPrefetch = 1, kJobRun = 2 };
constexpr uint32_t kJobCap = 1024, kPrefetchCap = 64;     // powers of two
Job g_jobs[kJobCap];
Job g_pjobs[kPrefetchCap];
uint32_t g_jHead, g_jTail, g_pHead, g_pTail;              // under g_jlock
std::atomic<int> g_jlock{0};
std::atomic<int> g_workersLive{0};
std::atomic<int> g_workerStop{0};
std::atomic<int> g_async{0};
std::atomic<uint32_t> g_inflight{0};
uint64_t g_startUs;

uint8_t* g_verifyBuf;
size_t g_verifyCap;
std::atomic<int> g_verifyLock{0};
std::atomic<uint32_t> g_verifyTick{0};

std::atomic<uint32_t> g_failStreak{0};
std::atomic<uint64_t> g_deadUntilUs{0};
std::atomic<uint64_t> g_lastStatUs{0};
std::atomic<uint32_t> g_logCount[16];                    // detailed log budget per operation kind

enum LogKind { kLogOpen = 0, kLogPread, kLogRead, kLogLseek, kLogAio, kLogFetchErr, kLogVerify, kLogMisc, kLogFstat, kLogTraceOpen, kLogTraceIo, kLogTraceApr, kLogKinds };

const char* mode_name(int m) {
  switch (m) { case kOff: return "off"; case kEmul: return "emul"; case kVerify: return "verify"; case kRemote: return "remote"; default: return "?"; }
}

struct Guard {
  Guard() {
    unsigned spins = 0;
    while (g_lock.exchange(1, std::memory_order_acquire) != 0) {
      if (++spins > 32) { if (g_ops.sleep_us) g_ops.sleep_us(20); spins = 0; }
    }
  }
  ~Guard() { g_lock.store(0, std::memory_order_release); }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
};

struct JGuard {                                          // the job rings' lock; may be taken while g_lock is held, never the other way round
  JGuard() {
    unsigned spins = 0;
    while (g_jlock.exchange(1, std::memory_order_acquire) != 0) {
      if (++spins > 32) { if (g_ops.sleep_us) g_ops.sleep_us(20); spins = 0; }
    }
  }
  ~JGuard() { g_jlock.store(0, std::memory_order_release); }
  JGuard(const JGuard&) = delete;
  JGuard& operator=(const JGuard&) = delete;
};

void logf(const char* fmt, ...) {
  if (!g_ops.log) return;
  char line[400];
  int n = snprintf(line, sizeof(line), "[blz_link] ");
  if (n < 0) return;
  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
  va_end(ap);
  if (m < 0) return;
  g_ops.log(line);
}

// true while the budget for this kind of line is not used up
bool budget(LogKind k) {
  const uint32_t used = g_logCount[k].fetch_add(1, std::memory_order_relaxed);
  return used < g_cfg.log_first;
}

inline void add(uint64_t* p, uint64_t v) { __atomic_fetch_add(p, v, __ATOMIC_RELAXED); }

// ---------------------------------------------------------------------------------------------------------------------------------------------- config parsing

bool is_ws(char c) { return c == ' ' || c == '\t'; }

bool key_is(const char* s, size_t n, const char* key) {
  size_t k = strlen(key);
  if (k != n) return false;
  for (size_t i = 0; i < n; ++i) if (s[i] != key[i]) return false;
  return true;
}

bool parse_u64(const char* s, size_t n, uint64_t* out) {
  if (n == 0 || n > 20) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    const uint64_t d = (uint64_t)(s[i] - '0');
    if (v > (0xFFFFFFFFFFFFFFFFull - d) / 10) return false;
    v = v * 10 + d;
  }
  *out = v;
  return true;
}

void seterr(char* err, size_t cap, int line, const char* msg) {
  if (err && cap) snprintf(err, cap, "line %d: %s", line, msg);
}

bool parse_hex(const char* s, size_t n, uint64_t* out) {             // "0x1f" or "1f"
  if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; n -= 2; }
  if (n == 0 || n > 16) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    const int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
    if (d < 0) return false;
    v = (v << 4) | (uint64_t)d;
  }
  *out = v;
  return true;
}

// next whitespace-separated token of [*p, end): returns its start and length, advances *p
const char* next_token(const char** p, const char* end, size_t* len) {
  const char* s = *p;
  while (s < end && is_ws(*s)) ++s;
  const char* e = s;
  while (e < end && !is_ws(*e)) ++e;
  *len = (size_t)(e - s);
  *p = e;
  return s;
}

}  // namespace

bool parse_config(const char* text, size_t len, Config* c, char* err, size_t errcap) {
  memset(c, 0, sizeof(*c));
  c->mode = kOff;
  memcpy(c->disk_cache_dir, "/mnt/ext1/blz-cache", sizeof("/mnt/ext1/blz-cache"));
  c->block_kb = 256; c->cache_mb = 32; c->timeout_ms = 8000; c->conns = 2; c->log_first = 24; c->verify_mb = 4; c->verify_every = 1; c->chunk_mb = 8; c->rcvbuf_kb = 1024;
  c->async = 0; c->readahead_kb = 0; c->stats_s = 10;
  bool have_server = false, have_base = false;
  int lineno = 0;
  size_t i = 0;
  while (i < len) {
    size_t j = i;
    while (j < len && text[j] != '\n') ++j;
    ++lineno;
    size_t a = i, b = j;
    i = j + 1;
    while (a < b && (is_ws(text[a]) || text[a] == '\r')) ++a;
    while (b > a && (is_ws(text[b - 1]) || text[b - 1] == '\r')) --b;
    if (a >= b || text[a] == '#') continue;
    size_t k = a;
    while (k < b && !is_ws(text[k])) ++k;
    const char* key = text + a;
    const size_t keylen = k - a;
    size_t vs = k;
    while (vs < b && is_ws(text[vs])) ++vs;
    const char* val = text + vs;
    const size_t vlen = b - vs;
    uint64_t num = 0;
    if (key_is(key, keylen, "mode")) {
      if (key_is(val, vlen, "off")) c->mode = kOff;
      else if (key_is(val, vlen, "emul")) c->mode = kEmul;
      else if (key_is(val, vlen, "verify")) c->mode = kVerify;
      else if (key_is(val, vlen, "remote")) c->mode = kRemote;
      else { seterr(err, errcap, lineno, "mode must be off, emul, verify or remote"); return false; }
    } else if (key_is(key, keylen, "appcontent_temp")) {
      if (key_is(val, vlen, "fail")) c->appcontent_temp_fail = 1;
      else if (key_is(val, vlen, "pass")) c->appcontent_temp_fail = 0;
      else { seterr(err, errcap, lineno, "appcontent_temp must be fail or pass"); return false; }
    } else if (key_is(key, keylen, "got")) {
      const char* p = val;
      const char* end = val + vlen;
      size_t nl = 0, gl = 0, pl = 0;
      const char* nm = next_token(&p, end, &nl);
      const char* gs = next_token(&p, end, &gl);
      const char* ps = next_token(&p, end, &pl);
      size_t rest = 0;
      (void)next_token(&p, end, &rest);
      uint64_t g = 0, pv = 0;
      if (nl == 0 || nl >= sizeof(c->got[0].name) || !parse_hex(gs, gl, &g) || !parse_hex(ps, pl, &pv) || rest != 0 || g == 0 || pv == 0) {
        seterr(err, errcap, lineno, "got needs: <import name> <GOT offset hex> <PLT offset hex>");
        return false;
      }
      if (c->ngot >= (uint32_t)kMaxGot) { seterr(err, errcap, lineno, "too many got lines"); return false; }
      GotPatch& gp = c->got[c->ngot++];
      memcpy(gp.name, nm, nl); gp.name[nl] = 0;
      gp.got = g; gp.plt = pv;
    } else if (key_is(key, keylen, "server")) {
      size_t p = 0;
      while (p < vlen && !is_ws(val[p])) ++p;
      char ipbuf[20];
      if (p == 0 || p >= sizeof(ipbuf)) { seterr(err, errcap, lineno, "server needs an IPv4 address and a port"); return false; }
      memcpy(ipbuf, val, p); ipbuf[p] = 0;
      size_t q = p; while (q < vlen && is_ws(val[q])) ++q;
      if (!blz::parse_ipv4(ipbuf, c->ip) || !parse_u64(val + q, vlen - q, &num) || num == 0 || num > 65535) { seterr(err, errcap, lineno, "server needs an IPv4 literal and a port 1-65535"); return false; }
      c->port = (uint16_t)num;
      have_server = true;
    } else if (key_is(key, keylen, "base")) {
      if (vlen < 1 || val[0] != '/' || vlen > sizeof(c->base) - 2) { seterr(err, errcap, lineno, "base must be a URL path starting with / (already percent-encoded)"); return false; }
      memcpy(c->base, val, vlen);
      size_t n = vlen;
      if (c->base[n - 1] != '/') c->base[n++] = '/';
      c->base[n] = 0;
      have_base = true;
    } else if (key_is(key, keylen, "file")) {
      size_t p = 0;
      while (p < vlen && val[p] >= '0' && val[p] <= '9') ++p;
      if (p == 0 || p >= vlen || !is_ws(val[p]) || !parse_u64(val, p, &num)) { seterr(err, errcap, lineno, "file needs: <size in bytes> <path relative to /app0>"); return false; }
      size_t q = p; while (q < vlen && is_ws(val[q])) ++q;
      const char* rel = val + q;
      size_t rlen = vlen - q;
      if (rlen > 6 && key_is(rel, 6, "/app0/")) { rel += 6; rlen -= 6; }
      if (rlen == 0 || rlen >= sizeof(c->files[0].rel) || rel[0] == '/') { seterr(err, errcap, lineno, "file path must be relative to /app0 and short"); return false; }
      for (size_t t = 0; t + 1 < rlen; ++t) if (rel[t] == '.' && rel[t + 1] == '.') { seterr(err, errcap, lineno, "file path must not contain .."); return false; }
      if (c->nfiles >= (uint32_t)kMaxFiles) { seterr(err, errcap, lineno, "too many file lines"); return false; }
      RemoteFile& f = c->files[c->nfiles++];
      memcpy(f.rel, rel, rlen); f.rel[rlen] = 0;
      f.size = num;
    } else if (key_is(key, keylen, "disk_cache_dir")) {
      if (vlen == 0 || vlen >= sizeof(c->disk_cache_dir) || val[0] != '/') { seterr(err, errcap, lineno, "disk_cache_dir must be an absolute path"); return false; }
      memcpy(c->disk_cache_dir, val, vlen); c->disk_cache_dir[vlen] = 0;
    } else if (key_is(key, keylen, "block_kb") || key_is(key, keylen, "cache_mb") || key_is(key, keylen, "timeout_ms") || key_is(key, keylen, "conns") || key_is(key, keylen, "log_first") ||
               key_is(key, keylen, "trace_opens") || key_is(key, keylen, "trace_io") || key_is(key, keylen, "trace_apr") || key_is(key, keylen, "verify_mb") || key_is(key, keylen, "verify_every") || key_is(key, keylen, "chunk_mb") || key_is(key, keylen, "rcvbuf_kb") ||
               key_is(key, keylen, "async") || key_is(key, keylen, "readahead_kb") || key_is(key, keylen, "stats_s") || key_is(key, keylen, "disk_cache_mb")) {
      if (!parse_u64(val, vlen, &num) || num > 0x7FFFFFFFull) { seterr(err, errcap, lineno, "number expected"); return false; }
      const uint32_t n32 = (uint32_t)num;
      if (key_is(key, keylen, "async")) { if (n32 > 1) { seterr(err, errcap, lineno, "async must be 0 or 1"); return false; } c->async = n32; }
      else if (key_is(key, keylen, "readahead_kb")) c->readahead_kb = n32;
      else if (key_is(key, keylen, "disk_cache_mb")) c->disk_cache_mb = n32;
      else if (key_is(key, keylen, "stats_s")) c->stats_s = n32 ? n32 : 1;
      else if (key_is(key, keylen, "block_kb")) c->block_kb = n32;
      else if (key_is(key, keylen, "cache_mb")) c->cache_mb = n32;
      else if (key_is(key, keylen, "timeout_ms")) c->timeout_ms = n32;
      else if (key_is(key, keylen, "conns")) c->conns = n32;
      else if (key_is(key, keylen, "log_first")) c->log_first = n32;
      else if (key_is(key, keylen, "verify_mb")) c->verify_mb = n32;
      else if (key_is(key, keylen, "verify_every")) c->verify_every = n32 ? n32 : 1;
      else if (key_is(key, keylen, "trace_opens")) c->trace_opens = n32;
      else if (key_is(key, keylen, "trace_io")) c->trace_io = n32;
      else if (key_is(key, keylen, "trace_apr")) c->trace_apr = n32;
      else if (key_is(key, keylen, "chunk_mb")) c->chunk_mb = n32;
      else c->rcvbuf_kb = n32;
    } else {
      seterr(err, errcap, lineno, "unknown key");
      return false;
    }
  }
  if (c->conns < 1) c->conns = 1;
  if (c->conns > (uint32_t)kMaxConns) c->conns = kMaxConns;
  if (c->chunk_mb < 1) c->chunk_mb = 1;
  if (c->chunk_mb > 64) c->chunk_mb = 64;
  if (c->timeout_ms < 500) c->timeout_ms = 500;
  if (c->block_kb != 0 && (c->block_kb < 16 || c->block_kb > 4096)) { seterr(err, errcap, 0, "block_kb must be 0 or 16..4096"); return false; }
  if (c->readahead_kb > (64u << 10)) { seterr(err, errcap, 0, "readahead_kb must be at most 65536"); return false; }
  if (c->mode == kOff) return true;
  if (!have_server || !have_base || c->nfiles == 0) { seterr(err, errcap, 0, "mode needs server, base and at least one file line"); return false; }
  for (uint32_t n = 0; n < c->nfiles; ++n) {
    char url[kMaxUrl];
    if (!file_url(c->base, c->files[n], url, sizeof(url))) { seterr(err, errcap, 0, "file url too long or not encodable"); return false; }
  }
  return true;
}

bool file_url(const char* base, const RemoteFile& f, char* out, size_t cap) {
  char enc[sizeof(f.rel) * 3 + 4];
  if (blz::url_encode_path(f.rel, enc, sizeof(enc)) == 0) return false;
  if (strlen(base) + strlen(enc) + 1 > cap) return false;
  snprintf(out, cap, "%s%s", base, enc);
  return true;
}

namespace {

// ---------------------------------------------------------------------------------------------------------------------------------------------- network

int acquire_conn() {
  const uint64_t t0 = g_ops.now_us();
  for (;;) {
    for (uint32_t i = 0; i < g_cfg.conns; ++i) {
      int expected = 0;
      if (g_connBusy[i].compare_exchange_strong(expected, 1, std::memory_order_acquire)) return (int)i;
    }
    g_ops.sleep_us(100);
    if (g_ops.now_us() - t0 > 60ull * 1000000ull) return -1;
  }
}

void release_conn(int i) { g_connBusy[i].store(0, std::memory_order_release); }

void maybe_emit_stats(uint64_t now) {
  uint64_t last = g_lastStatUs.load(std::memory_order_relaxed);
  if (now - last < (uint64_t)g_cfg.stats_s * 1000000ull) return;
  if (!g_lastStatUs.compare_exchange_strong(last, now)) return;
  char line[700];
  format_stats(line, sizeof(line));
  if (g_ops.log) g_ops.log(line);
}

// Range-fetch [off, off+len) of one remote file into dst, in chunks of at most chunk_mb. Returns len or a negative error (-7 no connection slot, -8 circuit open).
long net_fetch(const RemoteFile& f, uint64_t off, size_t len, void* dst) {
  size_t done = 0;
  const size_t maxc = (size_t)g_cfg.chunk_mb << 20;
  while (done < len) {
    const uint64_t t0 = g_ops.now_us();
    if (g_deadUntilUs.load(std::memory_order_relaxed) > t0) return -8;
    size_t n = len - done;
    if (n > maxc) n = maxc;
    const int ci = acquire_conn();
    if (ci < 0) return -7;
    char url[kMaxUrl];
    if (!file_url(g_cfg.base, f, url, sizeof(url))) { release_conn(ci); return -3; }   // (checked at parse time)
    const long r = blz::http_get_range(&g_conn[ci], url, off + done, n, (uint8_t*)dst + done);
    const int lerr = g_conn[ci].last_error, lstatus = g_conn[ci].last_status;
    release_conn(ci);
    const uint64_t t1 = g_ops.now_us();
    if (r != (long)n) {
      add(&g_stats.errs, 1);
      const uint32_t streak = g_failStreak.fetch_add(1) + 1;
      if (budget(kLogFetchErr)) logf("fetch FAILED %s off=%llu len=%zu rc=%ld last_error=%d http=%d streak=%u", f.rel, (unsigned long long)(off + done), n, r, lerr, lstatus, streak);
      if (streak >= 6) {
        g_deadUntilUs.store(t1 + 4ull * 1000000ull);
        g_failStreak.store(0);
        add(&g_stats.circuit_trips, 1);
        logf("circuit OPEN: 6 failed requests in a row, failing fast for 4 s (is the server on mac1 up?)");
      }
      return r < 0 ? r : -3;
    }
    g_failStreak.store(0, std::memory_order_relaxed);
    add(&g_stats.reqs, 1);
    add(&g_stats.bytes, n);
    const uint64_t dt = t1 - t0;
    add(&g_stats.http_us, dt);
    uint64_t mx = __atomic_load_n(&g_stats.http_max_us, __ATOMIC_RELAXED);
    while (dt > mx && !__atomic_compare_exchange_n(&g_stats.http_max_us, &mx, dt, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    maybe_emit_stats(t1);
    done += n;
  }
  return (long)done;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------- disk netcache
// smb-link-6, 21 Oct 2026: Unreal games re-read the same data over and over (The Mound re-read ~400 MB of pakchunk0 26x per 96 s at its
// menu = 110 MB/s from mac1 forever; the RAM cache is 32 MiB of module .bss). Every 256 KiB block fetched from mac1 is also written to
// <dir>/<game>/f<N>.dat (sparse, same offsets as the remote file) and its hash to cache.map; a later read of the block comes from the
// console's own disk and is checked against the hash (a mismatch = fetch again). A block is marked only after its data write succeeded,
// so a crash can only forget blocks. Adding stops at disk_cache_mb (no eviction).
constexpr size_t kDcBlk = 256u << 10;
constexpr uint32_t kDcMaxBlocks = 1u << 20;              // 4 MiB of hashes in .bss = 256 GiB of remote data per game; files past it are not cached
constexpr uint64_t kDcNoBase = ~0ull;
constexpr int kDcBufs = 8;                               // blocks for partial reads (2 MiB of .bss); none free = that read skips the netcache
constexpr int64_t kDcHdr = 64;
uint32_t g_dcHash[kDcMaxBlocks];                         // 0 = not cached, else blkhash() (top bit always set)
uint64_t g_dcBase[kMaxFiles];
int g_dcFd[kMaxFiles];
int g_dcMap = -1;
bool g_dcOn;
std::atomic<uint64_t> g_dcBytes;
std::atomic<int> g_dcOpenLock;
char g_dcDir[200];
alignas(4096) uint8_t g_dcBuf[kDcBufs][kDcBlk];
std::atomic<uint8_t> g_dcBufBusy[kDcBufs];

uint32_t blkhash(const uint8_t* p, size_t n) {
  uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t)n;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t v;
    memcpy(&v, p + i, 8);
    h = (h ^ (v * 0xff51afd7ed558ccdull)) * 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 29;
  }
  for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  return (uint32_t)(h ^ (h >> 32)) | 0x80000000u;
}

int dc_buf_get() {
  for (int i = 0; i < kDcBufs; ++i) {
    uint8_t z = 0;
    if (g_dcBufBusy[i].compare_exchange_strong(z, 1, std::memory_order_acquire)) return i;
  }
  return -1;
}
void dc_buf_put(int i) { g_dcBufBusy[i].store(0, std::memory_order_release); }

int dc_fd(uint32_t fi) {                                  // the data file of remote file fi, opened on first use
  int fd = __atomic_load_n(&g_dcFd[fi], __ATOMIC_ACQUIRE);
  if (fd >= 0 || fd == -2) return fd;
  int z = 0;
  while (!g_dcOpenLock.compare_exchange_weak(z, 1, std::memory_order_acquire)) { z = 0; }
  fd = g_dcFd[fi];
  if (fd == -1) {
    char path[260];
    snprintf(path, sizeof(path), "%s/f%u.dat", g_dcDir, fi);
    fd = g_ops.open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { fd = -2; logf("netcache: cannot open %s (errno %d): this file is not cached", path, g_ops.get_errno ? g_ops.get_errno() : 0); }
    __atomic_store_n(&g_dcFd[fi], fd, __ATOMIC_RELEASE);
  }
  g_dcOpenLock.store(0, std::memory_order_release);
  return fd;
}

struct DcHeader { char magic[8]; uint32_t blk; uint32_t nfiles; uint64_t sig; uint64_t blocks; uint8_t pad[32]; };

void dc_init() {
  g_dcOn = false;
  g_dcMap = -1;
  g_dcBytes.store(0);
  for (int i = 0; i < kMaxFiles; ++i) { g_dcFd[i] = -1; g_dcBase[i] = kDcNoBase; }
  if (!g_cfg.disk_cache_mb || !g_ops.open || !g_ops.pwrite || !g_ops.mkdir || g_cfg.mode != kRemote) return;
  uint64_t total = 0, sig = 0xcbf29ce484222325ull;
  for (uint32_t fi = 0; fi < g_cfg.nfiles; ++fi) {
    const uint64_t nb = (g_cfg.files[fi].size + kDcBlk - 1) / kDcBlk;
    if (total + nb <= kDcMaxBlocks) { g_dcBase[fi] = total; total += nb; }
    for (const char* q = g_cfg.files[fi].rel; *q; ++q) sig = (sig ^ (uint8_t)*q) * 0x100000001b3ull;
    sig = (sig ^ g_cfg.files[fi].size) * 0x100000001b3ull;
  }
  char game[96];
  size_t k = 0;
  for (const char* q = g_cfg.base; *q && k + 1 < sizeof(game); ++q) {
    const char ch = *q;
    const bool keep = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '@' || ch == '.' || ch == '-';
    if (keep) game[k++] = ch; else if (k && game[k - 1] != '_') game[k++] = '_';
  }
  while (k && game[k - 1] == '_') --k;
  game[k] = 0;
  (void)g_ops.mkdir(g_cfg.disk_cache_dir, 0777);
  snprintf(g_dcDir, sizeof(g_dcDir), "%s/%s", g_cfg.disk_cache_dir, game[0] ? game : "game");
  (void)g_ops.mkdir(g_dcDir, 0777);
  char path[260];
  snprintf(path, sizeof(path), "%s/cache.map", g_dcDir);
  int fd = g_ops.open(path, O_RDWR | O_CREAT, 0644);
  if (fd < 0) { logf("netcache OFF: cannot open %s (errno %d)", path, g_ops.get_errno ? g_ops.get_errno() : 0); return; }
  DcHeader h{}, want{};
  memcpy(want.magic, "BLZDC1", 6);
  want.blk = (uint32_t)kDcBlk; want.nfiles = g_cfg.nfiles; want.sig = sig; want.blocks = total;
  uint64_t have = 0;
  if (g_ops.pread(fd, &h, sizeof(h), 0) == (long)sizeof(h) && memcmp(&h, &want, sizeof(h)) == 0) {
    for (uint64_t at = 0; at < total;) {                 // the hashes of the blocks already on disk
      const uint64_t n = total - at < 65536 ? total - at : 65536;
      const long r = g_ops.pread(fd, &g_dcHash[at], (size_t)n * 4, kDcHdr + (int64_t)at * 4);
      if (r != (long)(n * 4)) {                          // the map ends early (blocks never written past here): the rest is not cached
        const uint64_t got = r > 0 ? (uint64_t)r / 4 : 0;
        memset(&g_dcHash[at + got], 0, (size_t)(total - at - got) * 4);
        break;
      }
      at += n;
    }
    for (uint64_t i = 0; i < total; ++i) if (g_dcHash[i]) ++have;
  } else {
    memset(g_dcHash, 0, sizeof(g_dcHash));
    const int fd2 = g_ops.open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);   // new game layout: start the map over (old data files are overwritten as blocks come in)
    if (fd2 >= 0) fd = fd2;
    if (g_ops.pwrite(fd, &want, sizeof(want), 0) != (long)sizeof(want)) { logf("netcache OFF: cannot write %s", path); return; }
  }
  g_dcMap = fd;
  g_dcBytes.store(have * kDcBlk);
  g_dcOn = true;
  logf("netcache ON %s: %llu of %llu blocks already on disk (%llu MiB), cap %u MiB", g_dcDir, (unsigned long long)have, (unsigned long long)total,
       (unsigned long long)((have * kDcBlk) >> 20), g_cfg.disk_cache_mb);
}

// Every network read goes through here: blocks already on the console's disk are read from there (hash checked), the others are fetched
// whole from mac1, written to disk (until the cap) and copied out. Same contract as net_fetch.
long fetch(const RemoteFile& f, uint64_t off, size_t len, void* dst) {
  const uint32_t fi = (uint32_t)(&f - g_cfg.files);
  if (!g_dcOn || fi >= g_cfg.nfiles || g_dcBase[fi] == kDcNoBase) return net_fetch(f, off, len, dst);
  const uint64_t cap = (uint64_t)g_cfg.disk_cache_mb << 20;
  size_t done = 0;
  while (done < len) {
    const uint64_t pos = off + done, b = pos / kDcBlk, bstart = b * kDcBlk;
    if (bstart >= f.size) break;
    const size_t blen = (size_t)(f.size - bstart < kDcBlk ? f.size - bstart : kDcBlk);
    const size_t inb = (size_t)(pos - bstart);
    size_t take = blen - inb;
    if (take > len - done) take = len - done;
    uint8_t* out = (uint8_t*)dst + done;
    const uint64_t slot = g_dcBase[fi] + b;
    const bool whole = inb == 0 && take == blen;
    int bi = -1;
    uint8_t* buf = out;
    if (!whole) {
      bi = dc_buf_get();
      if (bi < 0) {                                       // all blocks busy: this piece skips the netcache
        const long r = net_fetch(f, pos, take, out);
        if (r != (long)take) return r < 0 ? r : -3;
        done += take;
        continue;
      }
      buf = g_dcBuf[bi];
    }
    const int fd = dc_fd(fi);
    const uint32_t h = __atomic_load_n(&g_dcHash[slot], __ATOMIC_ACQUIRE);
    bool ok = false;
    if (h && fd >= 0 && g_ops.pread(fd, buf, blen, (int64_t)bstart) == (long)blen) {
      if (blkhash(buf, blen) == h) { ok = true; add(&g_stats.dc_hit, 1); }
      else {
        uint32_t e = h;
        if (__atomic_compare_exchange_n(&g_dcHash[slot], &e, 0u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) g_dcBytes.fetch_sub(blen);
        add(&g_stats.dc_bad, 1);
        if (budget(kLogFetchErr)) logf("netcache: block %llu of %s failed its hash check: fetched again", (unsigned long long)b, f.rel);
      }
    }
    if (!ok) {
      const long r = net_fetch(f, bstart, blen, buf);
      if (r != (long)blen) { if (bi >= 0) dc_buf_put(bi); return r < 0 ? r : -3; }
      add(&g_stats.dc_miss, 1);
      if (fd >= 0 && __atomic_load_n(&g_dcHash[slot], __ATOMIC_ACQUIRE) == 0) {
        if (g_dcBytes.load(std::memory_order_relaxed) + blen > cap) add(&g_stats.dc_full, 1);
        else if (g_ops.pwrite(fd, buf, blen, (int64_t)bstart) == (long)blen) {
          uint32_t nh = blkhash(buf, blen), e = 0;
          if (__atomic_compare_exchange_n(&g_dcHash[slot], &e, nh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            g_dcBytes.fetch_add(blen);
            (void)g_ops.pwrite(g_dcMap, &nh, 4, kDcHdr + (int64_t)slot * 4);
          }
        }
      }
    }
    if (!whole) memcpy(out, buf + inb, take);
    if (bi >= 0) dc_buf_put(bi);
    done += take;
  }
  return (long)done;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------- block cache

// Fully associative block cache (slots are few: 32 MiB / block size), LRU, under g_lock. A slot that is LOADING is never evicted or reused.
int cache_find(uint32_t file, uint64_t block) {
  for (uint32_t i = 0; i < g_nslots; ++i) {
    const CacheSlot& s = g_slots[i];
    if (s.state != kSlotEmpty && s.file == file && s.block == block) return (int)i;
  }
  return -1;
}

int cache_victim() {                                     // an empty slot, else the least recently used one that nobody is filling
  int best = -1;
  uint64_t bestLru = ~0ull;
  for (uint32_t i = 0; i < g_nslots; ++i) {
    const CacheSlot& s = g_slots[i];
    if (s.state == kSlotEmpty) return (int)i;
    if (s.state != kSlotLoading && s.lru < bestLru) { bestLru = s.lru; best = (int)i; }
  }
  return best;
}

size_t block_len(uint32_t fi, uint64_t blk) {            // bytes of block blk that exist (the last block of a file is short)
  const uint64_t size = g_cfg.files[fi].size, start = blk * g_blockBytes;
  return start >= size ? 0 : (size_t)(size - start < g_blockBytes ? size - start : g_blockBytes);
}

void schedule_prefetch(uint32_t fi, uint64_t from, uint64_t bytes);

// The reader entered a block 1..kBlockGap blocks past the one it touched last in this file: it walks the file in block order even when its
// reads leave gaps (GreedFall, 21 Oct: 63% of its 256 KiB misses on data.spk were the block right after the previous one, while only 3.4k of
// its 43k reads continued the previous read byte-exactly, so the byte-exact stream check never asked for read-ahead). Queue the next
// readahead_kb for the fetch workers, as for a byte-exact stream. Same block, backwards or a jump: only remember the block.
void note_block(uint32_t fi, uint64_t blk) {
  if (g_cfg.readahead_kb == 0 || fi >= (uint32_t)kMaxFiles) return;
  const uint64_t prev1 = __atomic_exchange_n(&g_lastBlk[fi], blk + 1, __ATOMIC_RELAXED);
  if (prev1 == 0 || prev1 - 1 >= blk || blk - (prev1 - 1) > kBlockGap) return;
  add(&g_stats.blk_streams, 1);
  schedule_prefetch(fi, (blk + 1) * g_blockBytes, (uint64_t)g_cfg.readahead_kb << 10);
}

// copy [inblk, inblk+take) of one cache block to dst, filling the block first when it is not there. A block another thread is
// loading is WAITED for (it is on its way: fetching it again would double the traffic); a block queued for a prefetch is taken over.
long block_copy(uint32_t fi, uint64_t blk, size_t blen, size_t inblk, size_t take, uint8_t* dst) {
  const RemoteFile& f = g_cfg.files[fi];
  const uint64_t t0 = g_ops.now_us();
  const uint64_t waitLimit = (uint64_t)g_cfg.timeout_ms * 2000ull + 1000000ull;
  bool waited = false;
  int idx = -1;
  note_block(fi, blk);
  for (;;) {
    bool loadingElsewhere = false;
    {
      Guard g;
      const int i = cache_find(fi, blk);
      if (i >= 0) {
        CacheSlot& s = g_slots[i];
        if (s.state == kSlotValid && s.len >= inblk + take) {
          memcpy(dst, g_cacheMem + (size_t)i * g_blockBytes + inblk, take);
          s.lru = ++g_tick;
          add(&g_stats.cache_hit, 1);
          if (waited) add(&g_stats.cache_wait, 1);
          return (long)take;
        }
        if (s.state == kSlotLoading) loadingElsewhere = true;
        else { s.state = kSlotLoading; s.len = 0; s.lru = ++g_tick; idx = i; }   // queued for a prefetch (or a short valid one): fetch it here
      } else {
        const int v = cache_victim();
        if (v >= 0) {
          CacheSlot& s = g_slots[v];
          s.state = kSlotLoading; s.file = fi; s.block = blk; s.len = 0; s.lru = ++g_tick;
          idx = v;
        }
      }
    }
    if (idx >= 0) break;
    if (!loadingElsewhere || g_ops.now_us() - t0 > waitLimit) {                    // every slot is being filled, or the loader takes too long: read the exact range
      add(&g_stats.cache_bypass, 1);
      return fetch(f, blk * g_blockBytes + inblk, take, dst);
    }
    waited = true;                                                                 // another thread is bringing this block: wait for it (a failed load frees the slot)
    g_ops.sleep_us(100);
  }
  add(&g_stats.cache_miss, 1);
  uint8_t* mem = g_cacheMem + (size_t)idx * g_blockBytes;
  const long r = fetch(f, blk * g_blockBytes, blen, mem);
  Guard g;
  CacheSlot& s = g_slots[idx];
  if (r == (long)blen) {
    memcpy(dst, mem + inblk, take);                        // copy under the lock: the slot may be claimed again right after it is published
    s.state = kSlotValid; s.len = (uint32_t)blen; s.lru = ++g_tick;
    return (long)take;
  }
  s.state = kSlotEmpty;
  return r < 0 ? r : -3;
}

// Was this request the continuation of an earlier one of the same file? Called in SUBMIT order. Under g_lock.
bool note_stream_locked(uint32_t fi, uint64_t off, uint64_t len) {
  int victim = 0;
  for (int i = 0; i < kStreams; ++i) {
    Stream& s = g_streams[i];
    if (s.used && s.file == fi && s.next == off) { s.next = off + len; s.lru = ++g_tick; return true; }
    if (!s.used) victim = i;
    else if (g_streams[victim].used && s.lru < g_streams[victim].lru) victim = i;
  }
  Stream& s = g_streams[victim];
  s.used = 1; s.file = fi; s.next = off + len; s.lru = ++g_tick;
  return false;
}

bool note_stream(uint32_t fi, uint64_t off, uint64_t len) {
  Guard g;
  const bool seq = note_stream_locked(fi, off, len);
  if (seq) add(&g_stats.seq_reqs, 1);
  return seq;
}

bool push_job(const Job& j, bool prefetch) {             // under JGuard
  if (prefetch) {
    if (g_pTail - g_pHead >= kPrefetchCap) return false;
    g_pjobs[g_pTail++ & (kPrefetchCap - 1)] = j;
    return true;
  }
  if (g_jTail - g_jHead >= kJobCap) return false;
  g_jobs[g_jTail++ & (kJobCap - 1)] = j;
  const uint64_t depth = g_jTail - g_jHead;
  if (depth > g_stats.q_max) g_stats.q_max = depth;
  return true;
}

// Queue prefetches for the blocks of [from, from + bytes) that are not cached yet (a sequential stream's next blocks).
void schedule_prefetch(uint32_t fi, uint64_t from, uint64_t bytes) {
  if (!g_async.load(std::memory_order_acquire) || g_blockBytes == 0 || g_nslots < 4 || bytes == 0) return;
  const uint64_t size = g_cfg.files[fi].size;
  if (from >= size) return;
  const uint64_t end = from + bytes < size ? from + bytes : size;
  const uint64_t b0 = from / g_blockBytes, b1 = (end - 1) / g_blockBytes;
  for (uint64_t b = b0; b <= b1; ++b) {
    Guard g;
    if (cache_find(fi, b) >= 0) continue;
    const int v = cache_victim();
    if (v < 0) { add(&g_stats.pf_dropped, 1); return; }
    CacheSlot& s = g_slots[v];
    if (s.state == kSlotQueued) { add(&g_stats.pf_dropped, 1); return; }    // the cache is full of prefetches already: the reader is behind, stop here
    s.state = kSlotQueued; s.file = fi; s.block = b; s.len = 0; s.lru = ++g_tick;
    Job j{};
    j.kind = kJobPrefetch; j.file = fi; j.block = b;
    bool ok;
    { JGuard jg; ok = push_job(j, true); }
    if (!ok) { s.state = kSlotEmpty; add(&g_stats.pf_dropped, 1); return; }
    add(&g_stats.pf_queued, 1);
  }
}

void run_prefetch(const Job& j) {
  int idx;
  {
    Guard g;
    idx = cache_find(j.file, j.block);
    if (idx < 0 || g_slots[idx].state != kSlotQueued) return;          // evicted, or a reader took the fetch over
    g_slots[idx].state = kSlotLoading;
  }
  const size_t blen = block_len(j.file, j.block);
  const long r = blen ? fetch(g_cfg.files[j.file], j.block * g_blockBytes, blen, g_cacheMem + (size_t)idx * g_blockBytes) : -3;
  Guard g;
  CacheSlot& s = g_slots[idx];
  if (r == (long)blen) { s.state = kSlotValid; s.len = (uint32_t)blen; add(&g_stats.pf_done, 1); }
  else s.state = kSlotEmpty;
}

// Read [off, off+len) of remote file fi into dst (clamped to the file size). Returns the byte count (0 at/after EOF) or a negative error.
// seq = this read continues a stream: with read-ahead on it goes through the cache in whole blocks (the next requests find them there).
long read_remote(uint32_t fi, uint64_t off, size_t len, void* dst, bool seq = false) {
  const RemoteFile& f = g_cfg.files[fi];
  if (len == 0 || off >= f.size) return 0;
  if ((uint64_t)len > f.size - off) len = (size_t)(f.size - off);
  const size_t bs = g_blockBytes;
  if (bs == 0 || g_nslots == 0) return fetch(f, off, len, dst);                    // no cache: straight into the destination
  if (len >= bs && !(seq && g_cfg.readahead_kb)) {                                // a big read that is not part of a stream: straight into the destination
    bool cached = false;                                                           // ...unless a prefetch already brought (or is bringing) its first block
    if (g_cfg.readahead_kb) { Guard g; cached = cache_find(fi, off / bs) >= 0; }
    if (!cached) return fetch(f, off, len, dst);
  }
  uint8_t* out = (uint8_t*)dst;
  size_t done = 0;
  while (done < len) {
    const uint64_t pos = off + done;
    const uint64_t blk = pos / bs;
    const size_t inblk = (size_t)(pos - blk * bs);
    const uint64_t bstart = blk * bs;
    const size_t blen = (size_t)(f.size - bstart < bs ? f.size - bstart : bs);
    size_t take = bs - inblk;
    if (take > len - done) take = len - done;
    if (inblk + take > blen) take = blen - inblk;
    if (take == 0) break;
    const long r = block_copy(fi, blk, blen, inblk, take, out + done);
    if (r < 0) return r;
    done += take;
  }
  return (long)done;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------- verify

// compare local bytes with the same range fetched from the server; the local bytes stay what the game sees
void verify_range(uint32_t fi, uint64_t off, const void* local, size_t n) {
  size_t cap = (size_t)g_cfg.verify_mb << 20;
  if (cap > kVerifyStaticBytes) cap = kVerifyStaticBytes;
  size_t vn = n < cap ? n : cap;
  if (vn == 0 || cap == 0) return;
  if (g_cfg.verify_every > 1 && (g_verifyTick.fetch_add(1, std::memory_order_relaxed) % g_cfg.verify_every) != 0) return;     // sampling: every Nth request only
  unsigned spins = 0;
  while (g_verifyLock.exchange(1, std::memory_order_acquire) != 0) { if (++spins > 16) { g_ops.sleep_us(200); spins = 0; } }
  if (!g_verifyBuf) { g_verifyBuf = g_verifyStatic; g_verifyCap = kVerifyStaticBytes; }
  if (vn > g_verifyCap) vn = g_verifyCap;
  const long r = read_remote(fi, off, vn, g_verifyBuf);
  if (r != (long)vn) {
    add(&g_stats.v_err, 1);
    if (budget(kLogVerify)) logf("verify: remote read failed %s off=%llu len=%zu rc=%ld", g_cfg.files[fi].rel, (unsigned long long)off, vn, r);
  } else {
    const uint8_t* a = (const uint8_t*)local;
    const uint8_t* b = g_verifyBuf;
    size_t bad = vn;
    for (size_t i = 0; i < vn; ++i) { if (a[i] != b[i]) { bad = i; break; } }
    if (bad == vn) { add(&g_stats.v_ok_bytes, vn); add(&g_stats.v_ok_reqs, 1); }
    else {
      add(&g_stats.v_bad, 1);
      if (budget(kLogVerify)) logf("verify MISMATCH %s off=%llu len=%zu first difference at +%zu: local=0x%02x remote=0x%02x", g_cfg.files[fi].rel, (unsigned long long)off, vn, bad, a[bad], b[bad]);
    }
  }
  g_verifyLock.store(0, std::memory_order_release);
}

int errno_now() { return g_ops.get_errno ? g_ops.get_errno() : kEio; }

int32_t sce_from_errno(int e) { return (int32_t)(0x80020000u | (uint32_t)(e & 0xffff)); }

inline int fd_file(int fd) { return (fd >= 0 && fd < kFdTable) ? (int)__atomic_load_n(&g_fdFile[fd], __ATOMIC_ACQUIRE) : -1; }

}  // namespace

namespace { void* worker_main(void*); }                  // the fetch workers (below, with the virtual AIO)

// One small ranged read of the first configured file, right at start: proves (in the klog, before the game touches anything) that sockets work inside this title's sandbox
// and that the server answers. Never fatal: a failure is logged and the engine stays up (the circuit breaker handles the rest).
static void selftest() {
  const RemoteFile& f = g_cfg.files[0];
  uint8_t head[4096];
  size_t n = f.size < sizeof(head) ? (size_t)f.size : sizeof(head);
  if (n == 0) return;
  const int saved = g_conn[0].timeout_ms;
  g_conn[0].timeout_ms = 3000;
  const uint64_t t0 = g_ops.now_us();
  char url[kMaxUrl];
  if (!file_url(g_cfg.base, f, url, sizeof(url))) return;
  const long r = blz::http_get_range(&g_conn[0], url, 0, n, head);
  const uint64_t dt = g_ops.now_us() - t0;
  g_conn[0].timeout_ms = saved;
  if (r == (long)n) logf("selftest OK: %zu bytes of %s in %llu us (first bytes %02x %02x %02x %02x)", n, f.rel, (unsigned long long)dt, head[0], head[1], head[2], head[3]);
  else logf("selftest FAILED: rc=%ld last_error=%d http=%d after %llu us (server %u.%u.%u.%u:%u)", r, g_conn[0].last_error, g_conn[0].last_status, (unsigned long long)dt, g_cfg.ip[0], g_cfg.ip[1], g_cfg.ip[2], g_cfg.ip[3], (unsigned)g_cfg.port);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------- public API

bool configure(const Config& cfg, const Ops& ops) {
  if (cfg.mode == kOff || cfg.nfiles == 0) return false;
  test_reset();
  g_cfg = cfg;
  g_ops = ops;
  for (int i = 0; i < kFdTable; ++i) { g_fdFile[i] = -1; g_fdPos[i] = 0; }
  for (uint32_t i = 0; i < g_cfg.conns; ++i) {
    blz::http_init(&g_conn[i], ops.net, g_cfg.ip, g_cfg.port, (int)g_cfg.timeout_ms);
    g_conn[i].rcvbuf = (int)(g_cfg.rcvbuf_kb << 10);
    g_connBusy[i].store(0);
  }
  g_blockBytes = (size_t)g_cfg.block_kb << 10;
  if (g_blockBytes && g_cfg.cache_mb && g_blockBytes <= kCacheStaticBytes) {
    size_t total = (size_t)g_cfg.cache_mb << 20;
    if (total > kCacheStaticBytes) total = kCacheStaticBytes;
    g_nslots = (uint32_t)(total / g_blockBytes);
    if (g_nslots > kMaxSlots) g_nslots = kMaxSlots;
    if (g_nslots) {
      g_cacheMem = g_cacheStatic;
      g_slots = g_slotStatic;
      memset(g_slotStatic, 0, sizeof(g_slotStatic));
    } else {
      g_blockBytes = 0;
    }
  } else {
    g_blockBytes = 0;
  }
  dc_init();
  g_startUs = g_ops.now_us();
  g_lastStatUs.store(g_startUs);
  g_active.store(1, std::memory_order_release);
  uint32_t workers = 0;
  if (g_cfg.async && g_cfg.mode == kRemote && g_ops.start_thread) {          // one fetch worker per connection; a failed start leaves fewer (0 = async off)
    for (uint32_t i = 0; i < g_cfg.conns; ++i) {
      if (!g_ops.start_thread(worker_main, nullptr)) { logf("worker %u did not start", i); break; }
      ++workers;
    }
    if (workers) g_async.store(1, std::memory_order_release);
  }
  logf("ACTIVE mode=%s server=%u.%u.%u.%u:%u files=%u conns=%u cache=%u KiB x %u slots chunk=%u MiB timeout=%u ms async=%s (%u workers) readahead=%u KiB", mode_name(g_cfg.mode), g_cfg.ip[0], g_cfg.ip[1], g_cfg.ip[2], g_cfg.ip[3],
       (unsigned)g_cfg.port, g_cfg.nfiles, g_cfg.conns, (unsigned)(g_blockBytes >> 10), g_nslots, g_cfg.chunk_mb, g_cfg.timeout_ms, async_on() ? "on" : "off", workers,
       (g_cfg.readahead_kb && g_blockBytes && async_on()) ? g_cfg.readahead_kb : 0u);
  for (uint32_t i = 0; i < g_cfg.nfiles && i < 12; ++i) {          // the first 12 (a big game lists hundreds)
    char url[kMaxUrl];
    if (file_url(g_cfg.base, g_cfg.files[i], url, sizeof(url))) logf("  file /app0/%s size=%llu url=%s", g_cfg.files[i].rel, (unsigned long long)g_cfg.files[i].size, url);
  }
  if (g_cfg.nfiles > 12) logf("  ... and %u more files", g_cfg.nfiles - 12);
  selftest();
  return true;
}

bool active() { return g_active.load(std::memory_order_acquire) != 0; }
int mode() { return g_active.load(std::memory_order_acquire) ? g_cfg.mode : (int)kOff; }

int match_path(const char* path) {                     // "/app0/<rel>" of a configured file -> its index
  if (!active() || !path) return -1;
  static const char kApp0[] = "/app0/";                  // (no strncmp: the PRX may only import the libc calls its build allows)
  for (int k = 0; k < 6; ++k) if (path[k] != kApp0[k]) return -1;
  const char* p = path + 6;
  for (uint32_t i = 0; i < g_cfg.nfiles; ++i) {
    const char* a = p;
    const char* b = g_cfg.files[i].rel;
    while (*a && *a == *b) { ++a; ++b; }
    if (*a == 0 && *b == 0) return (int)i;
  }
  return -1;
}

void note_open(const char* path, int flags, int fd) {
  if (!active() || fd < 0 || fd >= kFdTable) return;
  if ((flags & 3) != 0) return;                           // read-only opens only (O_ACCMODE)
  const int fi = match_path(path);
  if (fi < 0) return;
  {
    Guard g;
    g_fdPos[fd] = 0;
    __atomic_store_n(&g_fdFile[fd], (int16_t)fi, __ATOMIC_RELEASE);
  }
  add(&g_stats.n_open, 1);
  if (budget(kLogOpen)) logf("open %s fd=%d -> file %d (size %llu) mode=%s", path, fd, fi, (unsigned long long)g_cfg.files[fi].size, mode_name(g_cfg.mode));
}

void trace_open(const char* path, int flags, int fd) {
  if (!active() || g_cfg.trace_opens == 0) return;
  if (g_logCount[kLogTraceOpen].fetch_add(1, std::memory_order_relaxed) < g_cfg.trace_opens) logf("trace open %s flags=0x%x -> fd %d", path ? path : "(null)", flags, fd);
}

void trace_io(const char* what, int fd, size_t n, int64_t off) {
  if (!active() || g_cfg.trace_io == 0) return;
  if (g_logCount[kLogTraceIo].fetch_add(1, std::memory_order_relaxed) < g_cfg.trace_io) logf("trace %s fd=%d n=%zu off=%lld%s", what, fd, n, (long long)off, fd_file(fd) >= 0 ? " [REMOTE FD]" : "");
}

void trace_apr(const char* what, const char* path, int rc, uint64_t size) {
  if (!active() || g_cfg.trace_apr == 0) return;
  if (g_logCount[kLogTraceApr].fetch_add(1, std::memory_order_relaxed) < g_cfg.trace_apr) logf("trace %s %s rc=0x%x size=%llu%s", what, path ? path : "(null)", (unsigned)rc, (unsigned long long)size, match_path(path) >= 0 ? " [CONFIGURED FILE]" : "");
}

void note_close(int fd) {
  if (fd < 0 || fd >= kFdTable || !active()) return;
  if (__atomic_load_n(&g_fdFile[fd], __ATOMIC_ACQUIRE) < 0) return;
  Guard g;
  __atomic_store_n(&g_fdFile[fd], (int16_t)-1, __ATOMIC_RELEASE);
}

bool is_remote_fd(int fd) { return active() && fd_file(fd) >= 0; }

bool try_pread(int fd, void* buf, size_t n, int64_t off, long* res, int* err) {
  if (!active()) return false;
  const int fi = fd_file(fd);
  if (fi < 0) return false;
  add(&g_stats.n_pread, 1);
  if (g_cfg.mode == kRemote) {
    if (off < 0) { *res = -1; *err = 22; return true; }
    const uint64_t t0 = g_ops.now_us();
    const bool seq = note_stream((uint32_t)fi, (uint64_t)off, n);
    if (seq && g_cfg.readahead_kb) schedule_prefetch((uint32_t)fi, (uint64_t)off + n, (uint64_t)g_cfg.readahead_kb << 10);
    const long r = read_remote((uint32_t)fi, (uint64_t)off, n, buf, seq);
    if (budget(kLogPread)) logf("pread %s off=%lld n=%zu -> %ld (%llu us)", g_cfg.files[fi].rel, (long long)off, n, r, (unsigned long long)(g_ops.now_us() - t0));
    if (r < 0) { *res = -1; *err = kEio; } else { *res = r; *err = 0; }
    return true;
  }
  if (g_cfg.mode == kVerify) {
    const long r = g_ops.pread(fd, buf, n, off);
    *res = r; *err = r < 0 ? errno_now() : 0;
    if (r > 0) verify_range((uint32_t)fi, (uint64_t)off, buf, (size_t)r);
    if (budget(kLogPread)) logf("pread (verify) %s off=%lld n=%zu -> %ld", g_cfg.files[fi].rel, (long long)off, n, r);
    return true;
  }
  return false;
}

bool try_read(int fd, void* buf, size_t n, long* res, int* err) {
  if (!active()) return false;
  const int fi = fd_file(fd);
  if (fi < 0) return false;
  add(&g_stats.n_read, 1);
  if (g_cfg.mode == kRemote) {
    const int64_t pos = __atomic_load_n(&g_fdPos[fd], __ATOMIC_RELAXED);
    const bool seq = note_stream((uint32_t)fi, (uint64_t)pos, n);
    if (seq && g_cfg.readahead_kb) schedule_prefetch((uint32_t)fi, (uint64_t)pos + n, (uint64_t)g_cfg.readahead_kb << 10);
    const long r = read_remote((uint32_t)fi, (uint64_t)pos, n, buf, seq);
    if (budget(kLogRead)) logf("read %s pos=%lld n=%zu -> %ld", g_cfg.files[fi].rel, (long long)pos, n, r);
    if (r < 0) { *res = -1; *err = kEio; } else { __atomic_store_n(&g_fdPos[fd], pos + r, __ATOMIC_RELAXED); *res = r; *err = 0; }
    return true;
  }
  if (g_cfg.mode == kVerify) {
    const int64_t pos = g_ops.lseek(fd, 0, 1 /*SEEK_CUR*/);
    const long r = g_ops.read(fd, buf, n);
    *res = r; *err = r < 0 ? errno_now() : 0;
    if (r > 0 && pos >= 0) verify_range((uint32_t)fi, (uint64_t)pos, buf, (size_t)r);
    if (budget(kLogRead)) logf("read (verify) %s pos=%lld n=%zu -> %ld", g_cfg.files[fi].rel, (long long)pos, n, r);
    return true;
  }
  return false;
}

bool try_lseek(int fd, int64_t off, int whence, int64_t* res, int* err) {
  if (!active() || g_cfg.mode != kRemote) return false;
  const int fi = fd_file(fd);
  if (fi < 0) return false;
  add(&g_stats.n_lseek, 1);
  int64_t cur = __atomic_load_n(&g_fdPos[fd], __ATOMIC_RELAXED);
  int64_t np;
  switch (whence) {
    case 0: np = off; break;                                               // SEEK_SET
    case 1: np = cur + off; break;                                         // SEEK_CUR
    case 2: np = (int64_t)g_cfg.files[fi].size + off; break;               // SEEK_END
    default: *res = -1; *err = 22; return true;
  }
  if (np < 0) { *res = -1; *err = 22; return true; }
  __atomic_store_n(&g_fdPos[fd], np, __ATOMIC_RELAXED);
  if (budget(kLogLseek)) logf("lseek %s off=%lld whence=%d -> %lld", g_cfg.files[fi].rel, (long long)off, whence, (long long)np);
  *res = np; *err = 0;
  return true;
}

void fix_fstat(int fd, int64_t* st_size) {
  if (!active() || !st_size) return;
  const int fi = fd_file(fd);
  if (fi < 0) return;
  const int64_t want = (int64_t)g_cfg.files[fi].size;
  if (budget(kLogFstat)) logf("fstat %s (%s): size seen %lld -> %lld", g_cfg.files[fi].rel, mode_name(g_cfg.mode), (long long)*st_size, g_cfg.mode == kRemote ? (long long)want : (long long)*st_size);
  if (g_cfg.mode == kRemote) { *st_size = want; return; }
  if (*st_size != want && budget(kLogMisc)) logf("fstat %s: local size %lld differs from the config size %lld", g_cfg.files[fi].rel, (long long)*st_size, (long long)want);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------- virtual AIO

bool aio_reserve(int count, int* ids) {
  if (count <= 0) return true;
  Guard g;
  int found = 0;
  for (int k = 0; k < kMaxAio && found < count; ++k) {
    const int s = (g_aioHint + k) % kMaxAio;
    if (g_aio[s].used) continue;
    g_aio[s].used = 1;
    g_aio[s].gen = (uint8_t)(g_aio[s].gen + 1);
    __atomic_store_n(&g_aio[s].state, (uint8_t)0, __ATOMIC_RELEASE);
    ids[found++] = kAioTag | ((int)g_aio[s].gen << 16) | s;
  }
  if (found < count) {
    for (int k = 0; k < found; ++k) g_aio[ids[k] & 0xFFFF].used = 0;
    return false;
  }
  g_aioHint = (ids[count - 1] & 0xFFFF) + 1;
  return true;
}

namespace {
bool aio_decode(int id, int* slot) {
  if ((id & (int)0xFF000000) != kAioTag) return false;
  const int s = id & 0xFFFF;
  const uint8_t gen = (uint8_t)((id >> 16) & 0xFF);
  if (s >= kMaxAio || !g_aio[s].used || g_aio[s].gen != gen) return false;
  *slot = s;
  return true;
}

int state_to_sce(uint8_t st) { return st == 0 ? (int)kAioCompleted : st == 1 ? kStateSubmitted : kStateProcessing; }

// one remote read, reported through the request's result struct (seq = it continues a stream, noted at submit time)
void exec_remote(const AioReq& r, int fi, bool seq) {
  add(&g_stats.n_aio, 1);
  int64_t ret = 0;
  if (fi < 0 || !active()) {
    ret = (int64_t)kSceEinval;
  } else if (r.nbyte == 0) {
    ret = 0;
  } else {
    const uint64_t t0 = g_ops.now_us();
    const long n = r.offset < 0 ? -1 : read_remote((uint32_t)fi, (uint64_t)r.offset, r.nbyte, r.buf, seq);
    ret = n >= 0 ? (int64_t)n : (int64_t)kSceEio;
    if (budget(kLogAio)) logf("aio %s off=%lld n=%zu -> %lld (%llu us)%s", g_cfg.files[fi].rel, (long long)r.offset, r.nbyte, (long long)ret, (unsigned long long)(g_ops.now_us() - t0), seq ? " seq" : "");
  }
  if (r.res_return) __atomic_store_n(r.res_return, ret, __ATOMIC_RELAXED);
  if (r.res_state) __atomic_store_n(r.res_state, kAioCompleted, __ATOMIC_RELEASE);
}

bool pop_job(Job* j) {
  JGuard jg;
  if (g_jHead != g_jTail) { *j = g_jobs[g_jHead++ & (kJobCap - 1)]; return true; }
  if (g_pHead != g_pTail) { *j = g_pjobs[g_pHead++ & (kPrefetchCap - 1)]; return true; }
  return false;
}

void run_job(const Job& j) {
  if (j.kind == kJobPrefetch) { run_prefetch(j); return; }
  if (j.kind == kJobAio) __atomic_store_n(&g_aio[j.slot].state, (uint8_t)2, __ATOMIC_RELEASE);   // the slot cannot be freed meanwhile: delete waits for state 0
  exec_remote(j.req, (int)j.file, j.seq != 0);
  if (j.kind == kJobAio) __atomic_store_n(&g_aio[j.slot].state, (uint8_t)0, __ATOMIC_RELEASE);
  else if (j.left) j.left->fetch_sub(1, std::memory_order_acq_rel);
  g_inflight.fetch_sub(1, std::memory_order_relaxed);
}

void* worker_main(void*) {
  g_workersLive.fetch_add(1);
  unsigned idle = 0;
  while (!g_workerStop.load(std::memory_order_acquire)) {
    Job j;
    if (pop_job(&j)) { idle = 0; run_job(j); continue; }
    ++idle;                                                    // nothing to do: short naps while busy, longer ones after a quiet second
    g_ops.sleep_us(idle < 200 ? 50 : idle < 2000 ? 250 : 1000);
  }
  g_workersLive.fetch_sub(1);
  return nullptr;
}

// queue one read for the workers (kind aio: id slot s, or kind run: counted down in *left). false = the ring is full.
bool queue_read(const AioReq& r, int fi, int slot, uint8_t gen, uint8_t kind, std::atomic<int>* left) {
  bool seq;
  {
    Guard g;
    seq = note_stream_locked((uint32_t)fi, (uint64_t)(r.offset < 0 ? 0 : r.offset), r.nbyte);
    if (seq) add(&g_stats.seq_reqs, 1);
    Job j{};
    j.req = r; j.slot = slot; j.gen = gen; j.kind = kind; j.file = (uint32_t)fi; j.seq = seq ? 1 : 0; j.left = left;
    if (kind == kJobAio) __atomic_store_n(&g_aio[slot].state, (uint8_t)1, __ATOMIC_RELEASE);
    g_inflight.fetch_add(1, std::memory_order_relaxed);
    bool ok;
    { JGuard jg; ok = push_job(j, false); }
    if (!ok) {
      if (kind == kJobAio) __atomic_store_n(&g_aio[slot].state, (uint8_t)0, __ATOMIC_RELEASE);
      g_inflight.fetch_sub(1, std::memory_order_relaxed);
      add(&g_stats.q_sync, 1);
      return false;
    }
  }
  add(&g_stats.q_jobs, 1);
  if (seq && g_cfg.readahead_kb && r.offset >= 0) schedule_prefetch((uint32_t)fi, (uint64_t)r.offset + r.nbyte, (uint64_t)g_cfg.readahead_kb << 10);
  return true;
}
}  // namespace

bool async_on() { return g_async.load(std::memory_order_acquire) != 0; }

void aio_release(const int* ids, int count) {
  Guard g;
  for (int i = 0; i < count; ++i) { int s; if (aio_decode(ids[i], &s)) g_aio[s].used = 0; }
}

bool aio_is_mine(int id) { return (id & (int)0xFF000000) == kAioTag; }

int aio_poll_one(int id) {
  Guard g;
  int s;
  if (!aio_decode(id, &s)) return (int)kSceEsrch;
  return state_to_sce(__atomic_load_n(&g_aio[s].state, __ATOMIC_ACQUIRE));
}

int aio_wait_one(int id, uint64_t timeout_us) {
  const uint64_t t0 = g_ops.now_us();
  for (;;) {
    const int st = aio_poll_one(id);
    if (st < 0 || st == (int)kAioCompleted) return st;
    if (timeout_us && g_ops.now_us() - t0 >= timeout_us) return st;
    g_ops.sleep_us(100);
  }
}

int aio_delete_one(int id) {
  for (;;) {
    {
      Guard g;
      int s;
      if (!aio_decode(id, &s)) return (int)kSceEsrch;
      if (__atomic_load_n(&g_aio[s].state, __ATOMIC_ACQUIRE) == 0) { g_aio[s].used = 0; return 0; }
    }
    g_ops.sleep_us(100);                                       // a worker still writes into this request's buffer: the caller may free it once we return
  }
}

void aio_execute(const AioReq& r) {
  const int fi = fd_file(r.fd);
  if (g_cfg.mode == kRemote || fi < 0 || !active()) {
    const bool seq = fi >= 0 && r.offset >= 0 && note_stream((uint32_t)fi, (uint64_t)r.offset, r.nbyte);
    if (seq && g_cfg.readahead_kb) schedule_prefetch((uint32_t)fi, (uint64_t)r.offset + r.nbyte, (uint64_t)g_cfg.readahead_kb << 10);
    exec_remote(r, fi, seq);
    return;
  }
  add(&g_stats.n_aio, 1);                                                    // emul / verify: the local file is the truth
  const long n = g_ops.pread(r.fd, r.buf, r.nbyte, r.offset);
  const int64_t ret = n >= 0 ? (int64_t)n : (int64_t)sce_from_errno(errno_now());
  if (g_cfg.mode == kVerify && n > 0) verify_range((uint32_t)fi, (uint64_t)r.offset, r.buf, (size_t)n);
  if (budget(kLogAio)) logf("aio (%s) %s off=%lld n=%zu -> %lld", mode_name(g_cfg.mode), g_cfg.files[fi].rel, (long long)r.offset, r.nbyte, (long long)ret);
  if (r.res_return) __atomic_store_n(r.res_return, ret, __ATOMIC_RELAXED);
  if (r.res_state) __atomic_store_n(r.res_state, kAioCompleted, __ATOMIC_RELEASE);
}

bool aio_queue(const AioReq& r, int id) {
  if (!async_on() || g_cfg.mode != kRemote) return false;
  const int fi = fd_file(r.fd);
  if (fi < 0) return false;
  int s;
  uint8_t gen;
  { Guard g; if (!aio_decode(id, &s)) return false; gen = g_aio[s].gen; }
  return queue_read(r, fi, s, gen, kJobAio, nullptr);
}

void aio_run_all(const AioReq* reqs, int count) {
  if (count <= 0) return;
  std::atomic<int> left{0};
  int queued = 0;
  for (int i = 0; i < count; ++i) {
    const int fi = fd_file(reqs[i].fd);
    if (count > 1 && async_on() && g_cfg.mode == kRemote && fi >= 0) {
      left.fetch_add(1, std::memory_order_acq_rel);
      if (queue_read(reqs[i], fi, -1, 0, kJobRun, &left)) { ++queued; continue; }
      left.fetch_sub(1, std::memory_order_acq_rel);
    }
    aio_execute(reqs[i]);
  }
  while (queued && left.load(std::memory_order_acquire) > 0) g_ops.sleep_us(100);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------- stats / test hooks

void format_stats(char* out, size_t cap) {            // one klog line (the emulator's klog buffer holds ~500 characters)
  const Stats& s = g_stats;
  const uint64_t avg = s.reqs ? s.http_us / s.reqs : 0;
  const uint64_t t = g_ops.now_us ? (g_ops.now_us() - g_startUs) / 1000ull : 0;
  snprintf(out, cap,
           "[blz_link] stats t=%llu.%01llu s %s http %llu req %llu MiB avg %llu us max %llu us errs %llu trips %llu | cache hit %llu miss %llu bypass %llu wait %llu | aio %llu pread %llu read %llu lseek %llu open %llu"
           " | q %llu sync %llu qmax %llu inflight %u seq %llu bseq %llu | pf %llu done %llu dropped %llu | disk hit %llu miss %llu bad %llu full %llu MiB %llu"
           " | verify ok %llu req %llu MiB, mismatch %llu, err %llu",
           (unsigned long long)(t / 1000), (unsigned long long)((t % 1000) / 100), mode_name(g_cfg.mode), (unsigned long long)s.reqs, (unsigned long long)(s.bytes >> 20), (unsigned long long)avg,
           (unsigned long long)s.http_max_us, (unsigned long long)s.errs, (unsigned long long)s.circuit_trips, (unsigned long long)s.cache_hit, (unsigned long long)s.cache_miss,
           (unsigned long long)s.cache_bypass, (unsigned long long)s.cache_wait, (unsigned long long)s.n_aio, (unsigned long long)s.n_pread, (unsigned long long)s.n_read,
           (unsigned long long)s.n_lseek, (unsigned long long)s.n_open, (unsigned long long)s.q_jobs, (unsigned long long)s.q_sync, (unsigned long long)s.q_max,
           g_inflight.load(std::memory_order_relaxed), (unsigned long long)s.seq_reqs, (unsigned long long)s.blk_streams, (unsigned long long)s.pf_queued, (unsigned long long)s.pf_done, (unsigned long long)s.pf_dropped,
           (unsigned long long)s.dc_hit, (unsigned long long)s.dc_miss, (unsigned long long)s.dc_bad, (unsigned long long)s.dc_full,
           (unsigned long long)(g_dcBytes.load(std::memory_order_relaxed) >> 20),
           (unsigned long long)s.v_ok_reqs, (unsigned long long)(s.v_ok_bytes >> 20), (unsigned long long)s.v_bad, (unsigned long long)s.v_err);
}

void test_reset() {
  g_async.store(0);
  if (g_workersLive.load() > 0) {                                  // host tests reconfigure: stop the old workers first (the game configures once)
    g_workerStop.store(1, std::memory_order_release);
    for (int i = 0; i < 50000 && g_workersLive.load() > 0; ++i) { if (g_ops.sleep_us) g_ops.sleep_us(100); }
  }
  g_workerStop.store(0);
  g_jHead = g_jTail = g_pHead = g_pTail = 0;
  g_inflight.store(0);
  g_active.store(0);
  for (uint32_t i = 0; i < (uint32_t)kMaxConns; ++i) {
    if (g_conn[i].ops.close_fn) blz::http_close(&g_conn[i]);        // never initialised before the first configure(): nothing to close
    memset(&g_conn[i], 0, sizeof(g_conn[i]));
    g_connBusy[i].store(0);
  }
  g_cacheMem = nullptr; g_slots = nullptr; g_verifyBuf = nullptr; g_verifyCap = 0; g_nslots = 0; g_blockBytes = 0; g_tick = 0;
  memset(g_streams, 0, sizeof(g_streams));
  memset(g_lastBlk, 0, sizeof(g_lastBlk));
  g_dcOn = false; g_dcMap = -1; g_dcBytes.store(0);
  for (int i = 0; i < kMaxFiles; ++i) g_dcFd[i] = -1;
  memset(&g_stats, 0, sizeof(g_stats));
  memset(g_aio, 0, sizeof(g_aio));
  g_aioHint = 0;
  g_failStreak.store(0); g_deadUntilUs.store(0); g_lastStatUs.store(0); g_verifyTick.store(0);
  for (auto& c : g_logCount) c.store(0);
}

}  // namespace blzlink
