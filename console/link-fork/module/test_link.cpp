// test_link.cpp - host test of the SMB-link engine (blz_http + blz_link_core) against the mock Range server that test_link.py starts.
//   test_link <ip> <port> <datadir>      datadir holds data.bin (the file the server serves as /v/T.ffpfsc/data.bin) and the files for the fault routes
// Output: one line per check ("ok ..." / "FAIL ..."), exit code 0 only if every check passed.
#include "blz_http.h"
#include "blz_link.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static int g_fail = 0, g_ok = 0;
static void check(bool c, const char* fmt, ...) {
  char b[300];
  va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
  if (c) { ++g_ok; printf("ok   %s\n", b); } else { ++g_fail; printf("FAIL %s\n", b); }
  fflush(stdout);
}

// ---- host ops ----------------------------------------------------------------------------------------------------------------------------------
static int s_socket(int d, int t, int p) { return socket(d, t, p); }
static int s_connect(int fd, const void* a, unsigned l) { return connect(fd, (const sockaddr*)a, l); }
static long s_send(int fd, const void* b, size_t n, int f) { return (long)send(fd, b, n, f); }
static long s_recv(int fd, void* b, size_t n, int f) { return (long)recv(fd, b, n, f); }
static int s_close(int fd) { return close(fd); }
static int s_setsockopt(int fd, int lv, int op, const void* v, unsigned l) { return setsockopt(fd, lv, op, v, (socklen_t)l); }
static int s_getsockopt(int fd, int lv, int op, void* v, unsigned* l) { socklen_t sl = (socklen_t)*l; int r = getsockopt(fd, lv, op, v, &sl); *l = sl; return r; }
static int s_fcntl(int fd, int cmd, int arg) { return fcntl(fd, cmd, arg); }
static int s_poll(void* fds, unsigned n, int to) { return poll((pollfd*)fds, n, to); }
static int s_errno() { return errno; }

static blz::NetOps net_ops() {
  blz::NetOps o{};
  o.socket_fn = s_socket; o.connect_fn = s_connect; o.send_fn = s_send; o.recv_fn = s_recv; o.close_fn = s_close; o.setsockopt_fn = s_setsockopt; o.getsockopt_fn = s_getsockopt;
  o.fcntl_fn = s_fcntl; o.poll_fn = s_poll; o.errno_fn = s_errno; o.so_nosigpipe = SO_NOSIGPIPE;
  return o;
}

static long l_pread(int fd, void* b, size_t n, int64_t off) { return (long)pread(fd, b, n, (off_t)off); }
static long l_read(int fd, void* b, size_t n) { return (long)read(fd, b, n); }
static int64_t l_lseek(int fd, int64_t off, int wh) { return (int64_t)lseek(fd, (off_t)off, wh); }
static uint64_t l_now() { return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void l_sleep(unsigned us) { usleep(us); }
static std::mutex g_logm;
static std::vector<std::string> g_loglines;
static void l_log(const char* line) { std::lock_guard<std::mutex> g(g_logm); g_loglines.push_back(line); }
static size_t log_count(const char* needle) {
  std::lock_guard<std::mutex> g(g_logm);
  size_t n = 0;
  for (auto& s : g_loglines) if (s.find(needle) != std::string::npos) ++n;
  return n;
}

static bool l_start_thread(void* (*fn)(void*), void* arg) {
  pthread_t t;
  if (pthread_create(&t, nullptr, fn, arg) != 0) return false;
  pthread_detach(t);
  return true;
}

static int l_open(const char* p, int f, int m) { return ::open(p, f, m); }
static long l_pwrite(int fd, const void* b, size_t n, int64_t off) { return (long)::pwrite(fd, b, n, (off_t)off); }
static int l_mkdir(const char* p, int m) { return ::mkdir(p, (mode_t)m); }

static blzlink::Ops engine_ops() {
  blzlink::Ops o{};
  o.open = l_open; o.pwrite = l_pwrite; o.mkdir = l_mkdir;
  o.pread = l_pread; o.read = l_read; o.lseek = l_lseek; o.get_errno = s_errno; o.now_us = l_now; o.sleep_us = l_sleep; o.log = l_log; o.net = net_ops();
  o.start_thread = l_start_thread;
  return o;
}

static uint64_t stat_field(const std::string& line, const char* key) {      // the number right after `key` in a stats line
  const size_t p = line.find(key);
  return p == std::string::npos ? ~0ull : strtoull(line.c_str() + p + strlen(key), nullptr, 10);
}

static std::vector<uint8_t> load(const std::string& p) {
  std::vector<uint8_t> v;
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return v;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  v.resize((size_t)n);
  if (n && fread(v.data(), 1, (size_t)n, f) != (size_t)n) v.clear();
  fclose(f);
  return v;
}

static std::string cfg_text(const char* mode, const char* ip, int port, uint64_t size, const char* extra = "") {
  char b[1024];
  snprintf(b, sizeof(b), "# test\nmode %s\nserver %s %d\nbase /v/T.ffpfsc\nfile %llu data.bin\nfile 100 dir with space/other.bin\n%s", mode, ip, port, (unsigned long long)size, extra);
  return b;
}

static bool setup_engine(const std::string& text, blzlink::Config* cfg, char* err) {
  if (!blzlink::parse_config(text.c_str(), text.size(), cfg, err, 200)) return false;
  return blzlink::configure(*cfg, engine_ops());
}

int main(int argc, char** argv) {
  if (argc < 4) { fprintf(stderr, "usage: test_link ip port datadir\n"); return 2; }
  const char* ip = argv[1];
  const int port = atoi(argv[2]);
  const std::string dir = argv[3];
  const std::vector<uint8_t> data = load(dir + "/data.bin");
  if (data.empty()) { printf("FAIL no data.bin\n"); return 1; }
  const uint64_t size = data.size();
  char err[256] = "";

  // ---- T1 config parsing ----
  {
    blzlink::Config c;
    std::string good = cfg_text("remote", ip, port, size, "block_kb 64\ncache_mb 2\nconns 3\n");
    check(blzlink::parse_config(good.c_str(), good.size(), &c, err, sizeof(err)), "parse good config (%s)", err);
    check(c.mode == blzlink::kRemote && c.nfiles == 2 && c.block_kb == 64 && c.cache_mb == 2 && c.conns == 3 && c.port == (uint16_t)port, "parsed fields");
    char u0[blzlink::kMaxUrl], u1[blzlink::kMaxUrl];
    check(blzlink::file_url(c.base, c.files[0], u0, sizeof(u0)) && strcmp(c.files[0].rel, "data.bin") == 0 && strcmp(u0, "/v/T.ffpfsc/data.bin") == 0, "file 0 rel + url: %s %s", c.files[0].rel, u0);
    check(blzlink::file_url(c.base, c.files[1], u1, sizeof(u1)) && strcmp(u1, "/v/T.ffpfsc/dir%20with%20space/other.bin") == 0, "file 1 url is percent-encoded: %s", u1);
    const char* bad[] = {"mode weird\n", "mode remote\n", "mode remote\nserver 1.2.3.4 80\nbase /x\n", "bogus 1\n", "server 1.2.3 80\n", "mode remote\nserver 1.2.3.4 80\nbase x\nfile 1 a\n",
                         "mode remote\nserver 1.2.3.4 80\nbase /x\nfile 1 ../a\n", "mode remote\nserver 1.2.3.4 80\nbase /x\nfile abc a\n", "block_kb 7\nmode off\n", "mode remote\nserver 1.2.3.4 99999\nbase /x\nfile 1 a\n"};
    for (const char* b : bad) {
      blzlink::Config bc;
      check(!blzlink::parse_config(b, strlen(b), &bc, err, sizeof(err)), "rejects bad config [%.30s...] -> %s", b, err);
    }
    std::string offc = "mode off\n";
    check(blzlink::parse_config(offc.c_str(), offc.size(), &c, err, sizeof(err)) && c.mode == blzlink::kOff && c.appcontent_temp_fail == 0, "mode off parses (temp-data shim off by default)");
    std::string shim = "mode off\nappcontent_temp fail\n";
    check(blzlink::parse_config(shim.c_str(), shim.size(), &c, err, sizeof(err)) && c.mode == blzlink::kOff && c.appcontent_temp_fail == 1, "appcontent_temp fail parses in mode off (%s)", err);
    std::string shimv = cfg_text("verify", ip, port, size, "appcontent_temp fail\n");
    check(blzlink::parse_config(shimv.c_str(), shimv.size(), &c, err, sizeof(err)) && c.mode == blzlink::kVerify && c.appcontent_temp_fail == 1, "appcontent_temp fail parses with verify (%s)", err);
    std::string shimp = "mode off\nappcontent_temp fail\nappcontent_temp pass\n";
    check(blzlink::parse_config(shimp.c_str(), shimp.size(), &c, err, sizeof(err)) && c.appcontent_temp_fail == 0, "appcontent_temp pass switches it back off");
    std::string gotc = "mode off\nappcontent_temp fail\ngot sceAppContentTemporaryDataMount2 0xa7964f8 0x8ec8150\ngot sceAppContentTemporaryDataGetAvailableSpaceKb a796500 8EC8160\n";
    check(blzlink::parse_config(gotc.c_str(), gotc.size(), &c, err, sizeof(err)) && c.ngot == 2 && strcmp(c.got[0].name, "sceAppContentTemporaryDataMount2") == 0 &&
          c.got[0].got == 0xa7964f8ull && c.got[0].plt == 0x8ec8150ull && c.got[1].got == 0xa796500ull && c.got[1].plt == 0x8ec8160ull, "got lines parse (hex with/without 0x) (%s)", err);
    const char* badgot[] = {"got x 0x10\n", "got x 0x10 0x20 0x30\n", "got x zz 0x20\n", "got x 0 0x20\n", "got\n",
                            "got a 1 2\ngot b 1 2\ngot c 1 2\ngot d 1 2\ngot e 1 2\n"};
    for (const char* b : badgot) {
      blzlink::Config bc;
      check(!blzlink::parse_config(b, strlen(b), &bc, err, sizeof(err)), "rejects bad got line [%.24s...] -> %s", b, err);
    }
    std::string shimbad = "mode off\nappcontent_temp maybe\n";
    check(!blzlink::parse_config(shimbad.c_str(), shimbad.size(), &c, err, sizeof(err)), "appcontent_temp rejects other values -> %s", err);
    std::string crlf = "mode remote\r\nserver 1.2.3.4 80\r\nbase /x/\r\nfile 5 /app0/a b.bin\r\n";
    check(blzlink::parse_config(crlf.c_str(), crlf.size(), &c, err, sizeof(err)) && strcmp(c.files[0].rel, "a b.bin") == 0 && strcmp(c.base, "/x/") == 0, "CRLF + /app0/ prefix + spaces in the path");
  }

  // ---- T2 http client ----
  {
    blz::NetOps ops = net_ops();
    uint8_t a[4]; blz::parse_ipv4(ip, a);
    blz::HttpConn c;
    blz::http_init(&c, ops, a, (uint16_t)port, 3000);
    std::vector<uint8_t> buf(300000);
    long r = blz::http_get_range(&c, "/v/T.ffpfsc/data.bin", 1000, 200000, buf.data());
    check(r == 200000 && memcmp(buf.data(), data.data() + 1000, 200000) == 0, "range 200000 bytes at 1000 (rc=%ld)", r);
    r = blz::http_get_range(&c, "/v/T.ffpfsc/data.bin", 5, 7, buf.data());
    check(r == 7 && memcmp(buf.data(), data.data() + 5, 7) == 0 && c.requests == 2, "second request on the keep-alive connection (requests=%u)", c.requests);
    r = blz::http_get_range(&c, "/v/T.ffpfsc/slow.bin", 0, 4096, buf.data());
    check(r == 4096 && memcmp(buf.data(), data.data(), 4096) == 0, "headers that arrive one byte at a time (rc=%ld)", r);
    r = blz::http_get_range(&c, "/v/T.ffpfsc/close.bin", 100, 5000, buf.data());
    check(r == 5000 && memcmp(buf.data(), data.data() + 100, 5000) == 0, "Connection: close reply (rc=%ld)", r);
    r = blz::http_get_range(&c, "/v/T.ffpfsc/close.bin", 200, 5000, buf.data());
    check(r == 5000 && memcmp(buf.data(), data.data() + 200, 5000) == 0, "next request after Connection: close reconnects (rc=%ld)", r);
    r = blz::http_get_range(&c, "/v/T.ffpfsc/dropfirst.bin", 0, 1000, buf.data());
    check(r == 1000 && memcmp(buf.data(), data.data(), 1000) == 0, "server drops the first connection, the client reconnects once (rc=%ld)", r);
    r = blz::http_get_range(&c, "/v/T.ffpfsc/short.bin", 0, 10000, buf.data());
    check(r < 0, "truncated body is an error, not a short success (rc=%ld)", r);
    r = blz::http_get_range(&c, "/v/T.ffpfsc/missing.bin", 0, 100, buf.data());
    check(r == -4 && c.last_status == 404, "404 -> -4 (rc=%ld status=%d)", r, c.last_status);
    r = blz::http_get_range(&c, "/v/T.ffpfsc/data.bin", 0, 100, buf.data());
    check(r == 100, "connection usable again after errors (rc=%ld)", r);
    r = blz::http_get_range(&c, "/v/T.ffpfsc/wronglen.bin", 0, 100, buf.data());
    check(r == -6, "Content-Length different from what was asked -> -6 (rc=%ld)", r);
    // nothing listening: connect fails fast
    blz::HttpConn d;
    uint8_t lo[4] = {127, 0, 0, 1};
    blz::http_init(&d, ops, lo, 1, 1500);
    auto t0 = std::chrono::steady_clock::now();
    r = blz::http_get_range(&d, "/x", 0, 10, buf.data());
    double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(r < 0 && dt < 5.0, "connect to a closed port fails fast (rc=%ld in %.2f s)", r, dt);
    char enc[200];
    blz::url_encode_path("a b/(c)+d.pak", enc, sizeof(enc));
    check(strcmp(enc, "a%20b/%28c%29%2Bd.pak") == 0, "url_encode_path -> %s", enc);
  }

  // ---- T3 remote mode: a 0-byte placeholder fd, everything served from the server ----
  {
    blzlink::Config cfg;
    std::string t = cfg_text("remote", ip, port, size, "block_kb 64\ncache_mb 2\nconns 2\nlog_first 1000\n");
    check(setup_engine(t, &cfg, err), "configure remote (%s)", err);
    check(log_count("selftest OK") >= 1, "boot selftest logged OK against the mock server");
    const std::string ph = dir + "/placeholder.bin";
    { FILE* f = fopen(ph.c_str(), "wb"); fclose(f); }
    int fd = open(ph.c_str(), O_RDONLY);
    blzlink::note_open("/app0/data.bin", O_RDONLY, fd);
    check(blzlink::is_remote_fd(fd), "placeholder fd %d is registered", fd);
    int other = open("/dev/null", O_RDONLY);
    blzlink::note_open("/app0/not-configured.bin", O_RDONLY, other);
    check(!blzlink::is_remote_fd(other), "an unrelated path is not registered");
    int wfd = open((dir + "/w.bin").c_str(), O_WRONLY | O_CREAT, 0644);
    blzlink::note_open("/app0/data.bin", O_WRONLY, wfd);
    check(!blzlink::is_remote_fd(wfd), "a write open of a configured path is not registered");
    close(wfd);
    srand(7);
    int bad = 0;
    std::vector<uint8_t> got(1 << 20);
    for (int i = 0; i < 400; ++i) {
      uint64_t off = (uint64_t)rand() % (size + 10);
      size_t len = (size_t)(rand() % 3 == 0 ? rand() % (300 * 1024) : rand() % 5000);
      long res; int e;
      bool h = blzlink::try_pread(fd, got.data(), len, (int64_t)off, &res, &e);
      size_t want = off >= size ? 0 : (len > size - off ? size - off : len);
      if (!h || res != (long)want || (want && memcmp(got.data(), data.data() + off, want) != 0)) { ++bad; if (bad < 5) printf("  mismatch off=%llu len=%zu res=%ld want=%zu h=%d\n", (unsigned long long)off, len, res, want, (int)h); }
    }
    check(bad == 0, "400 random preads (cache path and direct path) equal the file, EOF clamped (%d bad)", bad);
    long res; int e;
    check(blzlink::try_pread(fd, got.data(), 0, 10, &res, &e) && res == 0, "zero-length pread -> 0");
    check(blzlink::try_pread(fd, got.data(), 100, (int64_t)size, &res, &e) && res == 0, "pread at EOF -> 0");
    check(blzlink::try_pread(fd, got.data(), 100, -5, &res, &e) && res == -1 && e == EINVAL, "negative offset -> EINVAL");
    check(blzlink::try_pread(fd, got.data(), 800000, 12345, &res, &e) && res == 800000 && memcmp(got.data(), data.data() + 12345, 800000) == 0, "big read (800000) straight into the destination");
    // sequential read + lseek on the virtual position
    int64_t pos;
    check(blzlink::try_lseek(fd, 0, 2, &pos, &e) && pos == (int64_t)size, "lseek SEEK_END = the configured size (%lld)", (long long)pos);
    check(blzlink::try_lseek(fd, 1000, 0, &pos, &e) && pos == 1000, "lseek SEEK_SET");
    check(blzlink::try_read(fd, got.data(), 500, &res, &e) && res == 500 && memcmp(got.data(), data.data() + 1000, 500) == 0, "read advances from the virtual position");
    check(blzlink::try_lseek(fd, 0, 1, &pos, &e) && pos == 1500, "lseek SEEK_CUR after the read (%lld)", (long long)pos);
    check(blzlink::try_read(fd, got.data(), 500, &res, &e) && res == 500 && memcmp(got.data(), data.data() + 1500, 500) == 0, "second sequential read continues");
    check(blzlink::try_lseek(fd, -10, 2, &pos, &e) && blzlink::try_read(fd, got.data(), 100, &res, &e) && res == 10 && memcmp(got.data(), data.data() + size - 10, 10) == 0, "read across EOF returns the 10 remaining bytes");
    check(blzlink::try_read(fd, got.data(), 100, &res, &e) && res == 0, "read at EOF -> 0");
    check(blzlink::try_lseek(fd, -1, 0, &pos, &e) && pos == -1 && e == EINVAL, "lseek to a negative position -> EINVAL");
    int64_t fsz = 0;
    blzlink::fix_fstat(fd, &fsz);
    check(fsz == (int64_t)size, "fstat size is the configured size (%lld)", (long long)fsz);
    int64_t osz = 7;
    blzlink::fix_fstat(other, &osz);
    check(osz == 7, "fstat of an unrelated fd untouched");
    char st[400]; blzlink::format_stats(st, sizeof(st));
    check(strstr(st, "cache hit") != nullptr && log_count("pread ") > 0, "stats line + detailed pread logging present");
    // close -> not remote; the fd number is reused by something else
    blzlink::note_close(fd);
    check(!blzlink::is_remote_fd(fd), "after close the fd is no longer remote");
    check(!blzlink::try_pread(fd, got.data(), 10, 0, &res, &e), "after close pread is not handled");
    close(fd); close(other);
  }

  // ---- T4 verify mode: the local file is the truth, the server is compared ----
  {
    blzlink::Config cfg;
    std::string t = cfg_text("verify", ip, port, size, "block_kb 64\ncache_mb 2\nlog_first 1000\n");
    check(setup_engine(t, &cfg, err), "configure verify (%s)", err);
    int fd = open((dir + "/data.bin").c_str(), O_RDONLY);
    blzlink::note_open("/app0/data.bin", O_RDONLY, fd);
    std::vector<uint8_t> got(1 << 20);
    long res; int e;
    check(blzlink::try_pread(fd, got.data(), 100000, 4096, &res, &e) && res == 100000 && memcmp(got.data(), data.data() + 4096, 100000) == 0, "verify pread returns the local bytes");
    check(log_count("MISMATCH") == 0, "no mismatch against an identical server");
    char st[600]; blzlink::format_stats(st, sizeof(st));
    check(strstr(st, "verify ok 1 req") != nullptr, "one verified request counted: %s", strstr(st, "verify ok") ? strstr(st, "verify ok") : "?");
    // a server that serves different bytes for the same file: the config points at the "mutated" copy
    {
      blzlink::Config c2;
      std::string t2 = "mode verify\nserver " + std::string(ip) + " " + std::to_string(port) + "\nbase /v/T.ffpfsc\nfile " + std::to_string(size) + " mutated.bin\nlog_first 1000\n";
      check(setup_engine(t2, &c2, err), "configure verify against the mutated file");
      int fd2 = open((dir + "/data.bin").c_str(), O_RDONLY);
      blzlink::note_open("/app0/mutated.bin", O_RDONLY, fd2);
      check(blzlink::try_pread(fd2, got.data(), 50000, 60000, &res, &e) && res == 50000 && memcmp(got.data(), data.data() + 60000, 50000) == 0, "mismatching server: the game still gets the LOCAL bytes");
      check(log_count("MISMATCH") == 1, "the mismatch is logged (%zu)", log_count("MISMATCH"));
      close(fd2);
    }
    close(fd);
  }

  // ---- T5 AIO protocol (remote, emul, verify) ----
  for (int variant = 0; variant < 3; ++variant) {
    const char* mode = variant == 0 ? "remote" : variant == 1 ? "emul" : "verify";
    blzlink::Config cfg;
    std::string t = cfg_text(mode, ip, port, size, "block_kb 64\ncache_mb 2\nlog_first 1000\n");
    check(setup_engine(t, &cfg, err), "configure %s for AIO", mode);
    std::string pf = variant == 0 ? dir + "/placeholder.bin" : dir + "/data.bin";
    int fd = open(pf.c_str(), O_RDONLY);
    blzlink::note_open("/app0/data.bin", O_RDONLY, fd);
    const int N = 40;
    std::vector<std::vector<uint8_t>> bufs(N, std::vector<uint8_t>(70000));
    std::vector<int64_t> rv(N, 12345);
    std::vector<uint32_t> rs(N, 0);
    int ids[N];
    check(blzlink::aio_reserve(N, ids), "[%s] reserve %d ids", mode, N);
    bool uniq = true, pos = true, mine = true;
    for (int i = 0; i < N; ++i) { for (int j = 0; j < i; ++j) if (ids[i] == ids[j]) uniq = false; if (ids[i] <= 0) pos = false; if (!blzlink::aio_is_mine(ids[i])) mine = false; }
    check(uniq && pos && mine, "[%s] ids unique, positive and tagged", mode);
    int bad = 0;
    for (int i = 0; i < N; ++i) {
      blzlink::AioReq r{};
      r.offset = (int64_t)(i * 3001); r.nbyte = (size_t)(1000 + i * 1500); r.buf = bufs[i].data(); r.res_return = &rv[i]; r.res_state = &rs[i]; r.fd = fd;
      blzlink::aio_execute(r);
      size_t want = r.offset >= (int64_t)size ? 0 : std::min<size_t>(r.nbyte, size - (size_t)r.offset);
      if (rs[i] != 3 || rv[i] != (int64_t)want || memcmp(bufs[i].data(), data.data() + r.offset, want) != 0) ++bad;
    }
    check(bad == 0, "[%s] %d AIO requests completed with the right bytes and result structs (%d bad)", mode, N, bad);
    int states = 0;
    for (int i = 0; i < N; ++i) if (blzlink::aio_poll_one(ids[i]) == 3) ++states;
    check(states == N, "[%s] poll reports COMPLETED for every live id", mode);
    int dels = 0;
    for (int i = 0; i < N; ++i) if (blzlink::aio_delete_one(ids[i]) == 0) ++dels;
    check(dels == N, "[%s] delete frees every id", mode);
    check(blzlink::aio_delete_one(ids[0]) < 0 && blzlink::aio_poll_one(ids[0]) < 0, "[%s] a deleted id is ESRCH for poll and delete", mode);
    int again[N];
    check(blzlink::aio_reserve(N, again), "[%s] reserve again", mode);
    bool reused = false;
    for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) if (again[i] == ids[j]) reused = true;
    check(!reused, "[%s] no id is handed out twice while an old handle might still be around (generation)", mode);
    check(blzlink::aio_poll_one(ids[1]) < 0, "[%s] an old id does not alias a new slot", mode);
    blzlink::aio_release(again, N);
    int huge[blzlink::kMaxAio + 1];
    check(!blzlink::aio_reserve(blzlink::kMaxAio + 1, huge), "[%s] reserving more than the table holds fails cleanly", mode);
    int one[1];
    check(blzlink::aio_reserve(1, one), "[%s] table is intact after the failed reserve", mode);
    blzlink::aio_release(one, 1);
    {
      static int all[blzlink::kMaxAio];
      check(blzlink::aio_reserve(blzlink::kMaxAio, all), "[%s] the whole table can be reserved once everything was deleted/released (no slot leaked)", mode);
      check(blzlink::aio_poll_one(ids[1]) < 0 && blzlink::aio_delete_one(ids[2]) < 0, "[%s] while every slot is in use again, the OLD ids of those slots are still ESRCH (generation)", mode);
      blzlink::aio_release(all, blzlink::kMaxAio);
    }
    close(fd);
  }

  // ---- T6 concurrency: 4 threads, remote mode, shared cache ----
  {
    blzlink::Config cfg;
    std::string t = cfg_text("remote", ip, port, size, "block_kb 32\ncache_mb 1\nconns 3\nlog_first 0\n");
    check(setup_engine(t, &cfg, err), "configure remote for threads");
    int fd = open((dir + "/placeholder.bin").c_str(), O_RDONLY);
    blzlink::note_open("/app0/data.bin", O_RDONLY, fd);
    std::atomic<int> bad{0}, total{0};
    std::vector<std::thread> th;
    for (int k = 0; k < 4; ++k) th.emplace_back([&, k]() {
      unsigned seed = 100 + k;
      std::vector<uint8_t> g(200000);
      for (int i = 0; i < 150; ++i) {
        uint64_t off = (uint64_t)(rand_r(&seed) % (unsigned)size);
        size_t len = (size_t)(rand_r(&seed) % 6 == 0 ? rand_r(&seed) % 150000 : rand_r(&seed) % 9000);
        long res; int e;
        bool h = blzlink::try_pread(fd, g.data(), len, (int64_t)off, &res, &e);
        size_t want = off >= size ? 0 : std::min<size_t>(len, size - off);
        if (!h || res != (long)want || (want && memcmp(g.data(), data.data() + off, want) != 0)) ++bad;
        ++total;
      }
    });
    for (auto& x : th) x.join();
    check(bad == 0, "4 threads x 150 random preads through a 1 MiB shared cache and 3 connections (%d bad of %d)", bad.load(), total.load());
    close(fd);
  }

  // ---- T7 circuit breaker: nothing listens on the configured port ----
  {
    blzlink::Config cfg;
    std::string t = cfg_text("remote", "127.0.0.1", 1, size, "block_kb 0\ntimeout_ms 600\nconns 1\nlog_first 100\n");
    check(setup_engine(t, &cfg, err), "configure remote against a dead server");
    check(log_count("selftest FAILED") == 1, "boot selftest reports the dead server instead of hanging");
    int fd = open((dir + "/placeholder.bin").c_str(), O_RDONLY);
    blzlink::note_open("/app0/data.bin", O_RDONLY, fd);
    std::vector<uint8_t> g(4096);
    long res; int e;
    auto t0 = std::chrono::steady_clock::now();
    int failed = 0;
    for (int i = 0; i < 12; ++i) if (blzlink::try_pread(fd, g.data(), 100, i * 100, &res, &e) && res == -1 && e == EIO) ++failed;
    double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(failed == 12, "every read against a dead server is a clean EIO, never a hang (%d of 12)", failed);
    check(log_count("circuit OPEN") >= 1, "the circuit opens after repeated failures");
    check(dt < 20.0, "12 failing reads took %.2f s (fail fast once the circuit is open)", dt);
    close(fd);
  }

  // ---- T8 two files share a one-slot cache: a hit must never hand out the other file's block ----
  {
    blzlink::Config cfg;
    std::string t = "mode remote\nserver " + std::string(ip) + " " + std::to_string(port) + "\nbase /v/T.ffpfsc\nfile " + std::to_string(size) + " data.bin\nfile " + std::to_string(size) +
                    " mutated.bin\nblock_kb 1024\ncache_mb 1\nlog_first 0\n";
    check(setup_engine(t, &cfg, err), "configure two files over a single cache slot");
    const std::vector<uint8_t> mut = load(dir + "/mutated.bin");
    int fa = open((dir + "/placeholder.bin").c_str(), O_RDONLY), fb = open((dir + "/placeholder.bin").c_str(), O_RDONLY);
    blzlink::note_open("/app0/data.bin", O_RDONLY, fa);
    blzlink::note_open("/app0/mutated.bin", O_RDONLY, fb);
    std::vector<uint8_t> ga(4096), gb(4096);
    long res; int e;
    int bad = 0;
    for (int i = 0; i < 6; ++i) {
      const int64_t off = 69000 + i * 100;                    // mutated.bin differs from data.bin at 70000
      bool h1 = blzlink::try_pread(fa, ga.data(), 3000, off, &res, &e) && res == 3000;
      bool h2 = blzlink::try_pread(fb, gb.data(), 3000, off, &res, &e) && res == 3000;
      if (!h1 || !h2 || memcmp(ga.data(), data.data() + off, 3000) != 0 || memcmp(gb.data(), mut.data() + off, 3000) != 0) ++bad;
    }
    check(bad == 0, "alternating reads of two files through one cache slot each get their own bytes (%d bad)", bad);
    close(fa); close(fb);
  }

  // ---- T9 config keys of the async/read-ahead path ----
  {
    blzlink::Config c;
    std::string t = cfg_text("remote", ip, port, size, "async 1\nreadahead_kb 2048\nstats_s 0\nconns 8\n");
    check(blzlink::parse_config(t.c_str(), t.size(), &c, err, sizeof(err)) && c.async == 1 && c.readahead_kb == 2048 && c.stats_s == 1 && c.conns == 8, "async / readahead_kb / stats_s / conns 8 parse (%s)", err);
    std::string b1 = cfg_text("remote", ip, port, size, "async 2\n"), b2 = cfg_text("remote", ip, port, size, "readahead_kb 70000\n");
    check(!blzlink::parse_config(b1.c_str(), b1.size(), &c, err, sizeof(err)), "async 2 is rejected (%s)", err);
    check(!blzlink::parse_config(b2.c_str(), b2.size(), &c, err, sizeof(err)), "readahead_kb above 64 MiB is rejected (%s)", err);
    std::string d = cfg_text("remote", ip, port, size, "");
    check(blzlink::parse_config(d.c_str(), d.size(), &c, err, sizeof(err)) && c.async == 0 && c.readahead_kb == 0 && c.stats_s == 10, "defaults: async off, no read-ahead, stats every 10 s");
    // GreedFall streams 39 pack files, COD hundreds: up to kMaxFiles (1024) file lines parse, one more is refused
    std::string m1 = std::string("mode remote\nserver ") + ip + " " + std::to_string(port) + "\nbase /d/GF\n";   // cfg_text's buffer is 1 KB: build it here
    for (int i = 0; i < blzlink::kMaxFiles; ++i) m1 += "file 1000 packs/p" + std::to_string(i) + ".spk\n";
    std::string m2 = m1 + "file 1000 packs/one_too_many.spk\n";
    check(blzlink::parse_config(m1.c_str(), m1.size(), &c, err, sizeof(err)) && c.nfiles == (uint32_t)blzlink::kMaxFiles, "%d file lines parse (kMaxFiles) (%s)", blzlink::kMaxFiles, err);
    check(!blzlink::parse_config(m2.c_str(), m2.size(), &c, err, sizeof(err)), "file line %d is refused (%s)", blzlink::kMaxFiles + 1, err);
  }

  // ---- T10 async AIO: 4 workers, 4 interleaved sequential streams + random reads, read-ahead through a small cache ----
  for (int variant = 0; variant < 2; ++variant) {
    const bool ra = variant == 1;
    blzlink::Config cfg;
    std::string t = cfg_text("remote", ip, port, size, ra ? "block_kb 64\ncache_mb 1\nconns 4\nasync 1\nreadahead_kb 256\nlog_first 0\n" : "block_kb 64\ncache_mb 1\nconns 4\nasync 1\nlog_first 0\n");
    check(setup_engine(t, &cfg, err), "[async%s] configure (%s)", ra ? "+ra" : "", err);
    check(blzlink::async_on() && log_count("async=on (4 workers)") >= 1, "[async%s] 4 workers started", ra ? "+ra" : "");
    int fd = open((dir + "/placeholder.bin").c_str(), O_RDONLY);
    blzlink::note_open("/app0/data.bin", O_RDONLY, fd);
    const int N = 240;
    std::vector<std::vector<uint8_t>> bufs(N, std::vector<uint8_t>(60000));
    std::vector<int64_t> rv(N, 12345);
    std::vector<uint32_t> rs(N, 7);
    std::vector<int> ids(N);
    std::vector<int64_t> offs(N);
    std::vector<size_t> lens(N);
    unsigned seed = 77;
    uint64_t next[4] = {0, 700001, 1400003, 2100005};
    for (int i = 0; i < N; ++i) {
      if (i % 6 == 5) { offs[i] = (int64_t)(rand_r(&seed) % (unsigned)size); lens[i] = 1 + rand_r(&seed) % 60000; }     // random
      else { const int k = i % 4; offs[i] = (int64_t)next[k]; lens[i] = 37000; next[k] += 37000; }               // 4 streams, unaligned pieces
    }
    check(blzlink::aio_reserve(N, ids.data()), "[async%s] reserve %d ids", ra ? "+ra" : "", N);
    int queued = 0, notDone = 0;
    for (int i = 0; i < N; ++i) {
      blzlink::AioReq r{};
      r.offset = offs[i]; r.nbyte = lens[i]; r.buf = bufs[i].data(); r.res_return = &rv[i]; r.res_state = &rs[i]; r.fd = fd;
      rs[i] = 0;
      if (blzlink::aio_queue(r, ids[i])) ++queued; else blzlink::aio_execute(r);
      if (blzlink::aio_poll_one(ids[i]) != 3) ++notDone;
    }
    check(queued == N, "[async%s] every request went to the workers (%d of %d)", ra ? "+ra" : "", queued, N);
    check(notDone > 0, "[async%s] the submit returns before the read is done (%d were still pending right after their submit)", ra ? "+ra" : "", notDone);
    auto t0 = std::chrono::steady_clock::now();
    int done = 0;
    while (done < N && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 60) {
      done = 0;
      for (int i = 0; i < N; ++i) if (blzlink::aio_poll_one(ids[i]) == 3) ++done;
      if (done < N) usleep(1000);
    }
    check(done == N, "[async%s] all %d requests complete (polled)", ra ? "+ra" : "", N);
    int bad = 0;
    for (int i = 0; i < N; ++i) {
      const size_t want = offs[i] >= (int64_t)size ? 0 : std::min<size_t>(lens[i], size - (size_t)offs[i]);
      if (rs[i] != 3 || rv[i] != (int64_t)want || memcmp(bufs[i].data(), data.data() + offs[i], want) != 0) ++bad;
    }
    check(bad == 0, "[async%s] bytes + result structs right for all %d (%d bad)", ra ? "+ra" : "", N, bad);
    int dels = 0;
    for (int i = 0; i < N; ++i) if (blzlink::aio_delete_one(ids[i]) == 0) ++dels;
    check(dels == N && blzlink::aio_poll_one(ids[0]) < 0, "[async%s] delete frees every id; a deleted id is ESRCH", ra ? "+ra" : "");
    char st[700];
    blzlink::format_stats(st, sizeof(st));
    const std::string line = st;
    check(stat_field(line, "| q ") == (uint64_t)N && stat_field(line, " seq ") >= 150, "[async%s] stats count the jobs and the sequential requests: %s", ra ? "+ra" : "", strstr(st, "| q ") ? strstr(st, "| q ") : st);
    if (ra) check(stat_field(line, "cache hit ") > 0, "[async+ra] sequential pieces share whole cache blocks (deep queue: the requests themselves fetch ahead): %s", strstr(st, "cache") ? strstr(st, "cache") : st);
    if (ra) {                                                  // shallow: one request at a time (the next is submitted when the last is done) - only the prefetch can run ahead
      std::vector<uint8_t> g(37000);
      int64_t v = 0; uint32_t sst = 0;
      int bad2 = 0;
      for (int i = 0; i < 60; ++i) {
        int id1;
        if (!blzlink::aio_reserve(1, &id1)) { ++bad2; break; }
        blzlink::AioReq r{}; r.offset = 1500000 + (int64_t)i * 37000; r.nbyte = 37000; r.buf = g.data(); r.res_return = &v; r.res_state = &sst; r.fd = fd;
        if (!blzlink::aio_queue(r, id1)) blzlink::aio_execute(r);
        const size_t want = r.offset >= (int64_t)size ? 0 : std::min<size_t>(37000, size - (size_t)r.offset);      // the last pieces run past EOF
        if (blzlink::aio_wait_one(id1, 0) != 3 || blzlink::aio_delete_one(id1) != 0 || v != (int64_t)want || memcmp(g.data(), data.data() + r.offset, want) != 0) ++bad2;
        usleep(2000);                                          // the "game" works on the data a little
      }
      blzlink::format_stats(st, sizeof(st));
      const std::string l2 = st;
      check(bad2 == 0 && stat_field(l2, "dropped") < ~0ull && stat_field(l2, " done ") > 0 && stat_field(l2, "wait ") + stat_field(l2, "cache hit ") > 213, "[async+ra] shallow stream: prefetches ran ahead and later pieces hit them (%d bad): %s", bad2, strstr(st, "cache") ? strstr(st, "cache") : st);
    }
    close(fd);
  }

  // ---- T11 delete / wait on a request a worker still has (the server answers after 150 ms) ----
  {
    blzlink::Config cfg;
    std::string t = "mode remote\nserver " + std::string(ip) + " " + std::to_string(port) + "\nbase /v/T.ffpfsc\nfile " + std::to_string(size) + " delay.bin\nblock_kb 0\nconns 2\nasync 1\nlog_first 0\n";
    check(setup_engine(t, &cfg, err), "configure the delayed file with async (%s)", err);
    int fd = open((dir + "/placeholder.bin").c_str(), O_RDONLY);
    blzlink::note_open("/app0/delay.bin", O_RDONLY, fd);
    std::vector<uint8_t> b1(50000), b2(50000);
    int64_t v1 = 0, v2 = 0; uint32_t s1 = 0, s2 = 0;
    int id[2];
    check(blzlink::aio_reserve(2, id), "reserve 2 ids");
    blzlink::AioReq r1{}; r1.offset = 1000; r1.nbyte = 50000; r1.buf = b1.data(); r1.res_return = &v1; r1.res_state = &s1; r1.fd = fd;
    blzlink::AioReq r2 = r1; r2.offset = 900000; r2.buf = b2.data(); r2.res_return = &v2; r2.res_state = &s2;
    check(blzlink::aio_queue(r1, id[0]) && blzlink::aio_queue(r2, id[1]), "two requests queued");
    const int p0 = blzlink::aio_poll_one(id[0]);
    check(p0 == 1 || p0 == 2, "poll right after the submit says SUBMITTED/PROCESSING (%d)", p0);
    check(blzlink::aio_delete_one(id[0]) == 0 && s1 == 3 && v1 == 50000 && memcmp(b1.data(), data.data() + 1000, 50000) == 0, "delete of a pending request waits until the worker is done with the buffer");
    check(blzlink::aio_wait_one(id[1], 0) == 3 && s2 == 3 && memcmp(b2.data(), data.data() + 900000, 50000) == 0, "wait (no limit) returns COMPLETED with the bytes in place");
    check(blzlink::aio_delete_one(id[1]) == 0, "then delete");
    // aio_run_all: 12 requests on the workers, all done when it returns
    std::vector<std::vector<uint8_t>> bb(12, std::vector<uint8_t>(30000));
    std::vector<int64_t> vv(12, 0); std::vector<uint32_t> ss(12, 0);
    std::vector<blzlink::AioReq> rq(12);
    for (int i = 0; i < 12; ++i) { rq[i] = r1; rq[i].offset = 5000 + i * 200000; rq[i].nbyte = 30000; rq[i].buf = bb[i].data(); rq[i].res_return = &vv[i]; rq[i].res_state = &ss[i]; }
    auto t0 = std::chrono::steady_clock::now();
    blzlink::aio_run_all(rq.data(), 12);
    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    int bad = 0;
    for (int i = 0; i < 12; ++i) if (ss[i] != 3 || vv[i] != 30000 || memcmp(bb[i].data(), data.data() + rq[i].offset, 30000) != 0) ++bad;
    check(bad == 0, "aio_run_all: 12 reads done with the right bytes when it returns (%d bad)", bad);
    check(dt < 12 * 0.15 * 0.75, "aio_run_all ran them in parallel on 2 workers (%.2f s for 12 x 150 ms)", dt);
    close(fd);
  }

  // ---- T12 stress: 4 threads submit + poll + delete random and sequential reads through a 4-slot cache with read-ahead ----
  {
    blzlink::Config cfg;
    std::string t = cfg_text("remote", ip, port, size, "block_kb 64\ncache_mb 1\nconns 3\nasync 1\nreadahead_kb 192\nlog_first 0\n");
    check(setup_engine(t, &cfg, err), "configure the stress run (%s)", err);
    int fd = open((dir + "/placeholder.bin").c_str(), O_RDONLY);
    blzlink::note_open("/app0/data.bin", O_RDONLY, fd);
    std::atomic<int> bad{0}, total{0};
    std::vector<std::thread> th;
    for (int k = 0; k < 4; ++k) th.emplace_back([&, k]() {
      unsigned seed2 = 500 + k;
      uint64_t pos = (uint64_t)k * 600000;
      for (int round = 0; round < 30; ++round) {
        const int n = 1 + (int)(rand_r(&seed2) % 6);
        int id[6]; std::vector<std::vector<uint8_t>> b(n, std::vector<uint8_t>(90000)); int64_t v[6]; uint32_t s[6]; int64_t o[6]; size_t l[6];
        if (!blzlink::aio_reserve(n, id)) { ++bad; continue; }
        for (int i = 0; i < n; ++i) {
          if (rand_r(&seed2) % 3) { o[i] = (int64_t)pos; l[i] = 20000 + rand_r(&seed2) % 70000; pos = (pos + l[i]) % size; }
          else { o[i] = (int64_t)(rand_r(&seed2) % (unsigned)size); l[i] = 1 + rand_r(&seed2) % 90000; }
          blzlink::AioReq r{}; r.offset = o[i]; r.nbyte = l[i]; r.buf = b[i].data(); r.res_return = &v[i]; r.res_state = &s[i]; r.fd = fd; s[i] = 0;
          if (!blzlink::aio_queue(r, id[i])) blzlink::aio_execute(r);
        }
        for (int i = 0; i < n; ++i) {
          if (rand_r(&seed2) % 2) (void)blzlink::aio_wait_one(id[i], 0);
          if (blzlink::aio_delete_one(id[i]) != 0) ++bad;          // delete waits for a pending one
          const size_t want = o[i] >= (int64_t)size ? 0 : std::min<size_t>(l[i], size - (size_t)o[i]);
          if (s[i] != 3 || v[i] != (int64_t)want || memcmp(b[i].data(), data.data() + o[i], want) != 0) ++bad;
          ++total;
        }
      }
    });
    for (auto& x : th) x.join();
    check(bad == 0, "4 threads x 30 rounds of submit/wait/delete with read-ahead through 16 cache slots: every read right (%d bad of %d)", bad.load(), total.load());
    close(fd);
  }

  // ---- T13 block-order walk with gaps (GreedFall, 21 Oct): small reads that skip most of each block never continue the previous read
  //      byte-exactly, but they enter the blocks in order; the block stream check queues read-ahead, so the later blocks are cache hits ----
  for (int variant = 0; variant < 2; ++variant) {
    const bool ra = variant == 1;
    blzlink::Config cfg;
    std::string t = cfg_text("remote", ip, port, size, ra ? "block_kb 64\ncache_mb 1\nconns 4\nasync 1\nreadahead_kb 256\nlog_first 0\n" : "block_kb 64\ncache_mb 1\nconns 4\nasync 1\nlog_first 0\n");
    check(setup_engine(t, &cfg, err), "[gaps%s] configure (%s)", ra ? "+ra" : "", err);
    int fd = open((dir + "/placeholder.bin").c_str(), O_RDONLY);
    blzlink::note_open("/app0/data.bin", O_RDONLY, fd);
    std::vector<uint8_t> g(4096);
    char st[1024];
    int bad3 = 0;
    const uint64_t bs = 64 * 1024;
    const int nb = (int)std::min<uint64_t>(60, size / bs - 1);
    for (int b = 0; b < nb; ++b) {
      const uint64_t off = (uint64_t)b * bs + 1000 + (uint64_t)(b % 3) * 7000;     // a few KiB somewhere in each block, never right after the last read
      long res = 0; int e = 0;
      if (!blzlink::try_pread(fd, g.data(), g.size(), (int64_t)off, &res, &e) || res != (long)g.size() || memcmp(g.data(), data.data() + off, g.size()) != 0) ++bad3;
      usleep(1500);                                            // the "game" works on the data a little
    }
    blzlink::format_stats(st, sizeof(st));
    const std::string l3 = st;
    if (!ra) check(bad3 == 0 && stat_field(l3, " bseq ") == 0 && stat_field(l3, "| pf ") == 0 && stat_field(l3, "miss ") >= (uint64_t)nb,
                   "[gaps] without read-ahead every block is a miss (%d bad): %s", bad3, strstr(st, "cache") ? strstr(st, "cache") : st);
    else check(bad3 == 0 && stat_field(l3, " seq ") == 0 && stat_field(l3, " bseq ") >= (uint64_t)nb - 2 && stat_field(l3, "miss ") < (uint64_t)nb / 2,
               "[gaps+ra] block-order walk: no byte-exact stream, yet the block stream queued read-ahead and most blocks were hits (%d bad): %s", bad3, strstr(st, "cache") ? strstr(st, "cache") : st);
    close(fd);
  }


  // ---- T14 disk netcache (smb-link-6, 21 Oct): blocks fetched once are read back from the console's disk (hash checked), a damaged block
  //      is fetched again, and the cap stops new blocks ----
  {
    auto disk = [](const std::string& l, int k) -> uint64_t {      // k: 0 hit, 1 miss, 2 bad, 3 full, 4 MiB
      size_t p = l.find("disk hit ");
      if (p == std::string::npos) return ~0ull;
      p += 9;
      uint64_t v[5] = {0, 0, 0, 0, 0};
      for (int i = 0; i < 5; ++i) {
        v[i] = strtoull(l.c_str() + p, nullptr, 10);
        if (i < 4) { const char* names[] = {" miss ", " bad ", " full ", " MiB "}; size_t q = l.find(names[i], p); if (q == std::string::npos) return ~0ull; p = q + strlen(names[i]); }
      }
      return v[k];
    };
    const std::string dcdir = dir + "/dc";
    char extra[400];
    snprintf(extra, sizeof(extra), "block_kb 64\ncache_mb 1\nconns 2\nlog_first 0\ndisk_cache_mb 64\ndisk_cache_dir %s\n", dcdir.c_str());
    const uint64_t offs[] = {0, 300000, 700001, 1048576, 2000000, 2900000};
    char st[1024];
    for (int round = 0; round < 3; ++round) {
      blzlink::test_reset();
      blzlink::Config cfg;
      check(setup_engine(cfg_text("remote", ip, port, size, extra), &cfg, err), "[netcache %d] configure (%s)", round, err);
      if (round == 2) {                                           // damage block 0 on disk: it must fail its hash and come from the server again
        int dfd = ::open((dcdir + "/v_T.ffpfsc/f0.dat").c_str(), O_RDWR);
        check(dfd >= 0, "[netcache] the data file exists after round 1 (%s/v_T.ffpfsc/f0.dat)", dcdir.c_str());
        if (dfd >= 0) { const char junk[16] = "garbage-garbage"; ::pwrite(dfd, junk, sizeof(junk), 10); close(dfd); }
        blzlink::test_reset();
        check(setup_engine(cfg_text("remote", ip, port, size, extra), &cfg, err), "[netcache 2] configure again (%s)", err);
      }
      int fd = open((dir + "/placeholder.bin").c_str(), O_RDONLY);
      blzlink::note_open("/app0/data.bin", O_RDONLY, fd);
      int bad = 0;
      std::vector<uint8_t> g(70000);
      for (uint64_t o : offs) {
        if (o >= size) continue;
        const size_t n = (size_t)std::min<uint64_t>(g.size(), size - o);
        long res = 0; int e = 0;
        if (!blzlink::try_pread(fd, g.data(), n, (int64_t)o, &res, &e) || res != (long)n || memcmp(g.data(), data.data() + o, n) != 0) ++bad;
      }
      close(fd);
      blzlink::format_stats(st, sizeof(st));
      const std::string l = st;
      const uint64_t reqs = stat_field(l, " http ");
      if (round == 0) check(bad == 0 && disk(l, 0) == 0 && disk(l, 1) > 0 && reqs > 0 && disk(l, 4) > 0,
                            "[netcache 0] first reads come from the server and land on disk (%d bad): %s", bad, strstr(st, "disk") ? strstr(st, "disk") : st);
      if (round == 1) check(bad == 0 && reqs == 0 && disk(l, 0) > 0 && disk(l, 1) == 0,
                            "[netcache 1] the same reads after a restart: all from disk, 0 HTTP requests (%d bad): %s", bad, strstr(st, "disk") ? strstr(st, "disk") : st);
      if (round == 2) check(bad == 0 && disk(l, 2) == 1 && reqs == 1,
                            "[netcache 2] a damaged block fails its hash and is fetched again (1 request), data still right (%d bad): %s", bad, strstr(st, "disk") ? strstr(st, "disk") : st);
    }
    // cap: 1 MiB = 4 blocks of 256 KiB, then nothing more is added
    blzlink::test_reset();
    snprintf(extra, sizeof(extra), "block_kb 64\ncache_mb 1\nconns 2\nlog_first 0\ndisk_cache_mb 1\ndisk_cache_dir %s\n", (dcdir + "-cap").c_str());
    blzlink::Config cfg;
    check(setup_engine(cfg_text("remote", ip, port, size, extra), &cfg, err), "[netcache cap] configure (%s)", err);
    int fd = open((dir + "/placeholder.bin").c_str(), O_RDONLY);
    blzlink::note_open("/app0/data.bin", O_RDONLY, fd);
    int bad = 0;
    std::vector<uint8_t> g(4096);
    for (uint64_t o = 0; o + 4096 <= size && o < 10ull * 262144; o += 262144) {
      long res = 0; int e = 0;
      if (!blzlink::try_pread(fd, g.data(), g.size(), (int64_t)o, &res, &e) || res != (long)g.size() || memcmp(g.data(), data.data() + o, g.size()) != 0) ++bad;
    }
    close(fd);
    blzlink::format_stats(st, sizeof(st));
    const std::string l = st;
    check(bad == 0 && disk(l, 3) > 0 && disk(l, 4) <= 1, "[netcache cap] 1 MiB cap: later blocks are served but not stored (%d bad): %s", bad, strstr(st, "disk") ? strstr(st, "disk") : st);
  }
  blzlink::test_reset();                                     // stop the workers before exit
  printf("%d ok, %d FAIL\n", g_ok, g_fail);
  return g_fail ? 1 : 0;
}
