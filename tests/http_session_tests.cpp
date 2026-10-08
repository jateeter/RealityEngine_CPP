// Long-lived connection tests for reality::http::Server.
//
// An event stream (SSE) or WebSocket outlives the request that opened it. The
// tcp_stream those sessions are built on comes from HttpSession, which arms a
// per-request deadline (HTTP_SESSION_TIMEOUT_MS) before every read; left armed,
// it closed every /api/engine/stream and /api/events client at the first
// keepalive (15 s in production) and every /ws client at the deadline itself
// (5 s, close code 1006). These run the real server with a 200 ms session
// timeout and a 100 ms heartbeat, and hold each connection open for many times
// the deadline.

#include "reality/http.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

namespace http = reality::http;
using namespace std::chrono_literals;

namespace {

int failures = 0;

#define EXPECT(cond, label)                                              \
  do { if (!(cond)) {                                                    \
    std::cerr << "FAIL: " << label << " (" << __FILE__ << ":"           \
              << __LINE__ << ")\n";                                      \
    ++failures;                                                          \
  } } while (0)

constexpr int kSessionTimeoutMs = 200;
constexpr int kHeartbeatMs = 100;

int free_port() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

int connect_to(int port) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      timeval tv{0, 50 * 1000};  // 50 ms reads, so the loops below can poll
      ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      return fd;
    }
    ::close(fd);
    std::this_thread::sleep_for(20ms);
  }
  return -1;
}

void send_all(int fd, const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
    if (n <= 0) return;
    off += static_cast<size_t>(n);
  }
}

// Read for `duration`, appending to `out`. Returns false if the peer closed.
bool read_for(int fd, std::chrono::milliseconds duration, std::string& out) {
  auto until = std::chrono::steady_clock::now() + duration;
  char buf[4096];
  while (std::chrono::steady_clock::now() < until) {
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n > 0) { out.append(buf, static_cast<size_t>(n)); continue; }
    if (n == 0) return false;                       // orderly close
    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
    return false;                                   // reset
  }
  return true;
}

size_t count(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t pos = hay.find(needle); pos != std::string::npos; pos = hay.find(needle, pos + needle.size())) ++n;
  return n;
}

void test_sse_outlives_the_session_deadline(int port, const std::shared_ptr<http::Server::SseHub>& hub) {
  int fd = connect_to(port);
  EXPECT(fd >= 0, "sse: connect");
  if (fd < 0) return;
  send_all(fd, "GET /stream HTTP/1.1\r\nHost: localhost\r\nAccept: text/event-stream\r\n\r\n");

  // Ten session deadlines. The bug closed the stream at the first keepalive
  // written after the first deadline.
  std::string got;
  bool open = read_for(fd, std::chrono::milliseconds(kSessionTimeoutMs * 10), got);
  EXPECT(open, "sse: the stream stays open past the per-request session deadline");
  EXPECT(got.find("200 OK") != std::string::npos, "sse: 200 OK");
  EXPECT(got.find(": connected") != std::string::npos, "sse: connected comment");
  EXPECT(count(got, ": keepalive") >= 5, "sse: keepalives keep arriving (got " + std::to_string(count(got, ": keepalive")) + ")");

  // And it still carries events.
  hub->broadcast("{\"stepNumber\":42}");
  std::string after;
  open = read_for(fd, 300ms, after);
  EXPECT(open, "sse: still open after a broadcast");
  EXPECT(after.find("data: {\"stepNumber\":42}") != std::string::npos, "sse: broadcast delivered after the deadline");
  ::close(fd);
}

void test_sse_closed_peer_is_released(int port, const std::shared_ptr<http::Server::SseHub>& hub) {
  // A client that goes away: the heartbeat's failed write must close the
  // session rather than keep it waking forever. Observable here only as "the
  // server keeps serving": broadcasts to a dead peer must not wedge the hub.
  int fd = connect_to(port);
  EXPECT(fd >= 0, "sse-dead: connect");
  if (fd < 0) return;
  send_all(fd, "GET /stream HTTP/1.1\r\nHost: localhost\r\n\r\n");
  std::string got;
  read_for(fd, 100ms, got);
  ::close(fd);
  std::this_thread::sleep_for(std::chrono::milliseconds(kHeartbeatMs * 4));
  for (int i = 0; i < 5; ++i) hub->broadcast("{\"stepNumber\":" + std::to_string(i) + "}");

  int fd2 = connect_to(port);
  EXPECT(fd2 >= 0, "sse-dead: a new client still connects");
  if (fd2 < 0) return;
  send_all(fd2, "GET /stream HTTP/1.1\r\nHost: localhost\r\n\r\n");
  std::string got2;
  read_for(fd2, 300ms, got2);
  EXPECT(got2.find(": connected") != std::string::npos, "sse-dead: new client served");
  ::close(fd2);
}

void test_websocket_outlives_the_session_deadline(int port, const std::shared_ptr<http::Server::WebSocketHub>& hub) {
  int fd = connect_to(port);
  EXPECT(fd >= 0, "ws: connect");
  if (fd < 0) return;
  send_all(fd,
    "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
  std::string got;
  bool open = read_for(fd, 200ms, got);
  EXPECT(got.find("101") != std::string::npos, "ws: 101 Switching Protocols");

  // Ten session deadlines with nothing sent either way. The bug closed the
  // socket at the first one, mid-read (1006).
  std::string idle;
  open = read_for(fd, std::chrono::milliseconds(kSessionTimeoutMs * 10), idle);
  EXPECT(open, "ws: the socket stays open past the per-request session deadline");

  hub->broadcast("step-42");
  std::string frame;
  open = read_for(fd, 300ms, frame);
  EXPECT(open, "ws: still open after a broadcast");
  // Unmasked server text frame: 0x81, length, payload.
  EXPECT(frame.size() >= 2 + 7 && static_cast<unsigned char>(frame[0]) == 0x81 &&
         frame.find("step-42") != std::string::npos,
         "ws: broadcast delivered after the deadline");
  ::close(fd);
}

}  // namespace

int main() {
  ::setenv("HTTP_SESSION_TIMEOUT_MS", std::to_string(kSessionTimeoutMs).c_str(), 1);
  ::setenv("SSE_HEARTBEAT_MS", std::to_string(kHeartbeatMs).c_str(), 1);
  ::setenv("HTTP_WORKERS", "2", 1);

  auto server = std::make_shared<http::Server>();
  auto sseHub = std::make_shared<http::Server::SseHub>();
  auto wsHub = std::make_shared<http::Server::WebSocketHub>();
  server->sse("/stream", sseHub);
  server->websocket("/ws", wsHub, nullptr);

  const int port = free_port();
  std::thread([server, port]() { server->listen(port); }).detach();

  test_sse_outlives_the_session_deadline(port, sseHub);
  test_sse_closed_peer_is_released(port, sseHub);
  test_websocket_outlives_the_session_deadline(port, wsHub);

  if (failures) {
    std::cerr << failures << " http session test(s) failed\n";
  } else {
    std::cout << "http session tests passed\n";
  }
  // listen() never returns; leave without joining its threads.
  std::cout.flush();
  std::_Exit(failures ? 1 : 0);
}
