// probe.cpp - host utility: fetch ranges from tools/imgserve (or any Range server) with the SAME client code the PS5 module uses, and report throughput.
//   probe <ip> <port> <url path> <offset> <len> [repeat] [outfile]
#include "blz_http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <vector>

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

int main(int argc, char** argv) {
  if (argc < 6) { fprintf(stderr, "usage: probe ip port urlpath offset len [repeat] [outfile]\n"); return 2; }
  blz::NetOps o{};
  o.socket_fn = s_socket; o.connect_fn = s_connect; o.send_fn = s_send; o.recv_fn = s_recv; o.close_fn = s_close; o.setsockopt_fn = s_setsockopt; o.getsockopt_fn = s_getsockopt;
  o.fcntl_fn = s_fcntl; o.poll_fn = s_poll; o.errno_fn = s_errno; o.so_nosigpipe = SO_NOSIGPIPE;
  uint8_t ip[4];
  if (!blz::parse_ipv4(argv[1], ip)) { fprintf(stderr, "bad ip\n"); return 2; }
  blz::HttpConn c;
  blz::http_init(&c, o, ip, (uint16_t)atoi(argv[2]), 20000);
  c.rcvbuf = 1 << 20;
  const uint64_t off = strtoull(argv[4], nullptr, 10);
  const size_t len = (size_t)strtoull(argv[5], nullptr, 10);
  const int rep = argc > 6 ? atoi(argv[6]) : 1;
  std::vector<uint8_t> buf(len);
  double total = 0;
  for (int i = 0; i < rep; ++i) {
    auto t0 = std::chrono::steady_clock::now();
    long r = blz::http_get_range(&c, argv[3], off + (uint64_t)i * len, len, buf.data());
    double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (r != (long)len) { printf("FAIL rc=%ld last_error=%d http=%d\n", r, c.last_error, c.last_status); return 1; }
    total += dt;
    printf("request %d: %zu bytes in %.3f s = %.1f MB/s\n", i, len, dt, len / dt / 1e6);
  }
  if (argc > 7) { FILE* f = fopen(argv[7], "wb"); fwrite(buf.data(), 1, len, f); fclose(f); }
  printf("OK %d requests, %.1f MB/s average, keep-alive requests=%u\n", rep, (double)len * rep / total / 1e6, c.requests);
  return 0;
}
