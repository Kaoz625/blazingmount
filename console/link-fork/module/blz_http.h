// blz_http.h - a tiny HTTP/1.1 Range client over plain TCP for the PS5 SMB link (tools/imgserve on mac1 answers it). Platform-free: every socket call comes in as a
// function pointer (PRX: libkernel exports resolved by name at run time, so no new import is needed; host test: libc). Plain C-style C++, no allocation, no STL.
//
// Sockets stay NON-BLOCKING and every wait goes through poll() with a timeout, the way MF's net/pack_fetch.h does it inside a PS5 title (a blocking connect() has no
// timeout on FreeBSD: a dead server would hang the game for ~75 s).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace blz {

struct NetOps {
  int (*socket_fn)(int domain, int type, int protocol);
  int (*connect_fn)(int fd, const void* addr, unsigned len);
  long (*send_fn)(int fd, const void* buf, size_t n, int flags);
  long (*recv_fn)(int fd, void* buf, size_t n, int flags);
  int (*close_fn)(int fd);
  int (*setsockopt_fn)(int fd, int level, int opt, const void* val, unsigned len);
  int (*getsockopt_fn)(int fd, int level, int opt, void* val, unsigned* len);
  int (*fcntl_fn)(int fd, int cmd, int arg);             // plain 3-argument form (the variadic libc function is wrapped by the platform)
  int (*poll_fn)(void* fds, unsigned nfds, int timeout_ms);
  int (*errno_fn)();
  int so_nosigpipe;                                      // platform value of SO_NOSIGPIPE (FreeBSD/PS5 0x800, macOS 0x1022); 0 = do not set
};

struct HttpConn {
  NetOps ops;
  uint8_t ip[4];
  uint16_t port;
  char host[40];                // "a.b.c.d:port" for the Host header
  int fd;                       // -1 = closed
  int timeout_ms;               // longest silence allowed in connect / send / recv
  int rcvbuf;                   // SO_RCVBUF to request (0 = leave the default)
  uint32_t requests;
  uint64_t bytes;
  int last_error;               // negative = our code (see http_get_range), positive = errno-like from the platform
  int last_status;              // HTTP status of the last reply (0 = none)
  uint8_t rbuf[2048];           // header read-ahead
};

// "192.168.1.130" -> ip[4]; false on anything else (IPv4 literals only: no resolver in the game process)
bool parse_ipv4(const char* s, uint8_t out[4]);
void http_init(HttpConn* c, const NetOps& ops, const uint8_t ip[4], uint16_t port, int timeout_ms);
bool http_connect(HttpConn* c);
void http_close(HttpConn* c);
// GET <path> with Range bytes=off..off+len-1 into dst. path must already be URL-encoded. Returns the byte count (== len on success) or a negative error:
// -1 connect, -2 send, -3 recv/short/timeout, -4 bad status, -5 bad header, -6 length mismatch. One transparent reconnect + retry on a dead keep-alive connection.
long http_get_range(HttpConn* c, const char* path, uint64_t off, size_t len, void* dst);
// UTF-8 path -> "%XX" form (keeps / and [A-Za-z0-9-._~]); returns the length written (NUL-terminated), 0 if cap too small
size_t url_encode_path(const char* utf8, char* out, size_t cap);

}  // namespace blz
