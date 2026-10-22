#include "blz_http.h"

#include <string.h>

namespace blz {

namespace {
// FreeBSD socket constants (PS5). macOS (the host test) agrees on every one used here; SO_NOSIGPIPE differs and comes in through NetOps.
constexpr int kAfInet = 2, kSockStream = 1, kSolSocket = 0xffff, kSoRcvBuf = 0x1002, kSoError = 0x1007, kIpprotoTcp = 6, kTcpNodelay = 1;
constexpr int kFGetFl = 3, kFSetFl = 4, kONonBlock = 4;
constexpr short kPollIn = 0x1, kPollOut = 0x4, kPollHup = 0x10;
constexpr int kEintr = 4, kEagain = 35, kEinprogress = 36;

struct PollFd { int fd; short events; short revents; };

int u64_to_dec(char* out, uint64_t v) {
  char tmp[24]; int n = 0;
  if (v == 0) tmp[n++] = '0';
  while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
  for (int i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
  return n;
}

// 1 = ready, 0 = timeout, -1 = error
int wait_fd(HttpConn* c, short events) {
  for (int tries = 0; tries < 16; ++tries) {
    PollFd p{c->fd, events, 0};
    const int r = c->ops.poll_fn(&p, 1, c->timeout_ms);
    if (r < 0) { if (c->ops.errno_fn() == kEintr) continue; return -1; }
    if (r == 0) return 0;
    if (p.revents & (events | kPollHup)) return 1;     // HUP: the following recv/send reports it
    return -1;                                          // POLLERR / POLLNVAL
  }
  return -1;
}

bool send_all(HttpConn* c, const char* p, size_t n) {
  while (n) {
    const long w = c->ops.send_fn(c->fd, p, n, 0);
    if (w > 0) { p += w; n -= (size_t)w; continue; }
    if (w == 0) return false;
    const int e = c->ops.errno_fn();
    if (e == kEintr) continue;
    if (e != kEagain) return false;
    if (wait_fd(c, kPollOut) != 1) return false;
  }
  return true;
}

// > 0 bytes, 0 = the peer closed, -1 = error or timeout
long recv_some(HttpConn* c, void* p, size_t n) {
  for (;;) {
    const long r = c->ops.recv_fn(c->fd, p, n, 0);
    if (r >= 0) return r;
    const int e = c->ops.errno_fn();
    if (e == kEintr) continue;
    if (e != kEagain) return -1;
    if (wait_fd(c, kPollIn) != 1) return -1;
  }
}

bool recv_exact(HttpConn* c, uint8_t* p, size_t n) {
  while (n) {
    const long r = recv_some(c, p, n);
    if (r <= 0) return false;
    p += r; n -= (size_t)r;
  }
  return true;
}

// Reads until the blank line. Returns the header length (through "\r\n\r\n") and sets *have to the number of bytes in rbuf; negative on error.
int read_headers(HttpConn* c, int* have) {
  int n = 0;
  while (n < (int)sizeof(c->rbuf)) {
    const int from = n > 3 ? n - 3 : 0;
    const long r = recv_some(c, c->rbuf + n, sizeof(c->rbuf) - (size_t)n);
    if (r <= 0) return -3;
    n += (int)r;
    for (int i = from; i + 3 < n; ++i) {
      if (c->rbuf[i] == '\r' && c->rbuf[i + 1] == '\n' && c->rbuf[i + 2] == '\r' && c->rbuf[i + 3] == '\n') { *have = n; return i + 4; }
    }
  }
  return -5;
}

bool ieq_prefix(const uint8_t* s, const uint8_t* end, const char* prefix) {   // case-insensitive "starts with", bounded by end
  for (; *prefix; ++s, ++prefix) {
    if (s >= end) return false;
    char a = (char)*s, b = *prefix;
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
    if (a != b) return false;
  }
  return true;
}
}  // namespace

bool parse_ipv4(const char* s, uint8_t out[4]) {
  for (int part = 0; part < 4; ++part) {
    if (*s < '0' || *s > '9') return false;
    unsigned v = 0; int digits = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (unsigned)(*s - '0'); ++s; if (++digits > 3) return false; }
    if (v > 255) return false;
    out[part] = (uint8_t)v;
    if (part < 3) { if (*s != '.') return false; ++s; }
  }
  return *s == 0;
}

void http_init(HttpConn* c, const NetOps& ops, const uint8_t ip[4], uint16_t port, int timeout_ms) {
  memset(c, 0, sizeof(*c));
  c->ops = ops;
  memcpy(c->ip, ip, 4);
  c->port = port;
  c->fd = -1;
  c->timeout_ms = timeout_ms;
  c->rcvbuf = 0;
  int n = 0;
  for (int i = 0; i < 4; ++i) { n += u64_to_dec(c->host + n, ip[i]); if (i < 3) c->host[n++] = '.'; }
  c->host[n++] = ':';
  n += u64_to_dec(c->host + n, port);
  c->host[n] = 0;
}

void http_close(HttpConn* c) {
  if (c->fd >= 0) { c->ops.close_fn(c->fd); c->fd = -1; }
}

bool http_connect(HttpConn* c) {
  http_close(c);
  const int fd = c->ops.socket_fn(kAfInet, kSockStream, 0);
  if (fd < 0) { c->last_error = -1; return false; }
  const int one = 1;
  if (c->ops.so_nosigpipe) c->ops.setsockopt_fn(fd, kSolSocket, c->ops.so_nosigpipe, &one, (unsigned)sizeof(one));   // a write to a dead peer must not raise SIGPIPE in the game
  c->ops.setsockopt_fn(fd, kIpprotoTcp, kTcpNodelay, &one, (unsigned)sizeof(one));
  if (c->rcvbuf > 0) c->ops.setsockopt_fn(fd, kSolSocket, kSoRcvBuf, &c->rcvbuf, (unsigned)sizeof(c->rcvbuf));
  const int flags = c->ops.fcntl_fn(fd, kFGetFl, 0);
  c->ops.fcntl_fn(fd, kFSetFl, (flags < 0 ? 0 : flags) | kONonBlock);
  uint8_t sa[16];
  memset(sa, 0, sizeof(sa));
  sa[0] = 16; sa[1] = (uint8_t)kAfInet;
  sa[2] = (uint8_t)(c->port >> 8); sa[3] = (uint8_t)(c->port & 0xff);
  memcpy(sa + 4, c->ip, 4);
  c->fd = fd;
  if (c->ops.connect_fn(fd, sa, 16) != 0) {
    const int e = c->ops.errno_fn();
    if (e != kEinprogress || wait_fd(c, kPollOut) != 1) { c->last_error = -1; http_close(c); return false; }
    int soerr = 0; unsigned sl = (unsigned)sizeof(soerr);
    if (c->ops.getsockopt_fn(fd, kSolSocket, kSoError, &soerr, &sl) != 0 || soerr != 0) { c->last_error = -1; http_close(c); return false; }
  }
  return true;
}

static long get_once(HttpConn* c, const char* path, uint64_t off, size_t len, void* dst) {
  char req[1024];
  int n = 0;
  auto put = [&](const char* s) { size_t l = strlen(s); if ((size_t)n + l < sizeof(req)) { memcpy(req + n, s, l); n += (int)l; } };
  put("GET "); put(path); put(" HTTP/1.1\r\nHost: "); put(c->host); put("\r\nRange: bytes=");
  { char t[24]; int m = u64_to_dec(t, off); t[m] = 0; put(t); }
  put("-");
  { char t[24]; int m = u64_to_dec(t, off + len - 1); t[m] = 0; put(t); }
  put("\r\nConnection: keep-alive\r\n\r\n");
  if (!send_all(c, req, (size_t)n)) return -2;
  int have = 0;
  const int hl = read_headers(c, &have);
  if (hl < 0) return hl;
  const uint8_t* hdr = c->rbuf;
  const uint8_t* hend = c->rbuf + hl;
  // status line: "HTTP/1.x SSS ..."
  int status = 0;
  if (hl >= 12 && hdr[0] == 'H' && hdr[1] == 'T' && hdr[2] == 'T' && hdr[3] == 'P' && hdr[4] == '/' && hdr[8] == ' ') {
    for (int i = 9; i < 12; ++i) { if (hdr[i] < '0' || hdr[i] > '9') { status = 0; break; } status = status * 10 + (hdr[i] - '0'); }
  }
  c->last_status = status;
  if (!(status == 206 || (status == 200 && off == 0))) return -4;
  long long clen = -1;
  bool close_after = false;
  for (const uint8_t* p = hdr; p < hend; ) {
    const uint8_t* e = p; while (e < hend && *e != '\n') ++e;
    if (ieq_prefix(p, e, "content-length:")) {
      const uint8_t* q = p + 15; while (q < e && *q == ' ') ++q;
      long long v = 0; bool any = false;
      while (q < e && *q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); ++q; any = true; }
      if (any) clen = v;
    } else if (ieq_prefix(p, e, "connection:")) {
      const uint8_t* q = p + 11; while (q < e && *q == ' ') ++q;
      if (ieq_prefix(q, e, "close")) close_after = true;
    }
    p = e < hend ? e + 1 : e;
  }
  if (clen < 0) return -5;
  if ((unsigned long long)clen != (unsigned long long)len) return -6;
  const size_t leftover = (size_t)(have - hl);
  if (leftover > len) return -6;                         // the server sent more than we asked for: the stream is out of step
  if (leftover) memcpy(dst, c->rbuf + hl, leftover);
  if (!recv_exact(c, (uint8_t*)dst + leftover, len - leftover)) return -3;
  if (close_after) http_close(c);
  return (long)len;
}

long http_get_range(HttpConn* c, const char* path, uint64_t off, size_t len, void* dst) {
  if (len == 0) return 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (c->fd < 0 && !http_connect(c)) continue;
    const long r = get_once(c, path, off, len, dst);
    if (r == (long)len) { c->requests++; c->bytes += len; return r; }
    c->last_error = (int)r;
    http_close(c);                                  // a keep-alive connection may have died (or the reply was bad): reconnect once
  }
  return c->last_error < 0 ? c->last_error : -3;
}

size_t url_encode_path(const char* utf8, char* out, size_t cap) {
  static const char hex[] = "0123456789ABCDEF";
  size_t n = 0;
  for (const unsigned char* p = (const unsigned char*)utf8; *p; ++p) {
    const unsigned char ch = *p;
    const bool keep = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' || ch == '~' || ch == '/';
    if (keep) { if (n + 2 > cap) return 0; out[n++] = (char)ch; }
    else { if (n + 4 > cap) return 0; out[n++] = '%'; out[n++] = hex[ch >> 4]; out[n++] = hex[ch & 15]; }
  }
  if (n + 1 > cap) return 0;
  out[n] = 0;
  return n;
}

}  // namespace blz
