#include "reality/http.hpp"
#include "reality/json.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <chrono>
#include <utility>

namespace reality::http {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace beast_http = boost::beast::http;
namespace beast_websocket = boost::beast::websocket;
namespace ssl = boost::asio::ssl;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

static int env_int(const char* name, int fallback, int minimum = 1) {
  const char* value = std::getenv(name);
  if (!value) return fallback;
  return std::max(minimum, std::atoi(value));
}

static std::pair<std::regex, std::vector<std::string>> compile_pattern(const std::string& pattern) {
  std::vector<std::string> params;
  std::ostringstream re;
  re << "^";
  for (size_t i = 0; i < pattern.size();) {
    if (pattern[i] == ':') {
      size_t j = i + 1;
      while (j < pattern.size() && (std::isalnum(static_cast<unsigned char>(pattern[j])) || pattern[j] == '_')) ++j;
      params.push_back(pattern.substr(i + 1, j - i - 1));
      re << "([^/]+)";
      i = j;
    } else {
      char c = pattern[i++];
      if (std::string(".^$|()[]*+?{}\\").find(c) != std::string::npos) re << "\\";
      re << c;
    }
  }
  re << "$";
  return {std::regex(re.str()), params};
}

void Server::route(std::string method, std::string pattern, Handler handler) {
  auto [rx, params] = compile_pattern(pattern);
  routes.push_back({std::move(method), std::move(pattern), std::move(rx), std::move(params), std::move(handler)});
}

void Server::WebSocketHub::add(std::shared_ptr<WebSocketConnection> connection) {
  std::lock_guard<std::mutex> lock(mutex);
  connections.push_back(connection);
}

void Server::WebSocketHub::broadcast(const std::string& text) {
  std::lock_guard<std::mutex> lock(mutex);
  auto it = connections.begin();
  while (it != connections.end()) {
    if (auto connection = it->lock()) {
      connection->send(text);
      ++it;
    } else {
      it = connections.erase(it);
    }
  }
}

void Server::websocket(std::string pattern, std::shared_ptr<WebSocketHub> hub, WebSocketOpenHandler on_open) {
  auto [rx, params] = compile_pattern(pattern);
  websocketRoutes.push_back({std::move(pattern), std::move(rx), std::move(params), std::move(hub), std::move(on_open)});
}

void Server::sse(std::string pattern, std::shared_ptr<SseHub> hub) {
  auto [rx, params] = compile_pattern(pattern);
  sseRoutes.push_back({std::move(pattern), std::move(rx), std::move(params), std::move(hub)});
}

bool Server::match_sse(Request& req, std::shared_ptr<SseHub>& hub) const {
  for (const auto& r : sseRoutes) {
    std::smatch m;
    if (std::regex_match(req.path, m, r.regex)) {
      for (size_t i = 0; i < r.params.size(); ++i) req.pathParams[r.params[i]] = m[i + 1].str();
      hub = r.hub;
      return true;
    }
  }
  return false;
}

static Request to_request(const beast_http::request<beast_http::string_body>& in) {
  Request req;
  req.method = std::string(in.method_string());
  req.path = std::string(in.target());
  req.body = in.body();
  for (const auto& field : in) req.headers.emplace(std::string(field.name_string()), std::string(field.value()));
  auto q = req.path.find('?');
  if (q != std::string::npos) {
    std::string query = req.path.substr(q + 1);
    req.path = req.path.substr(0, q);
    size_t start = 0;
    while (start <= query.size()) {
      size_t end = query.find('&', start);
      std::string part = query.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (!part.empty()) {
        size_t eq = part.find('=');
        req.queryParams[part.substr(0, eq)] = eq == std::string::npos ? "" : part.substr(eq + 1);
      }
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }
  return req;
}

Response Server::handle(Request req) const {
  Response res = error_response("Not found", 404);
  for (const auto& r : routes) {
    if (r.method != req.method) continue;
    std::smatch m;
    if (std::regex_match(req.path, m, r.regex)) {
      for (size_t i = 0; i < r.params.size(); ++i) req.pathParams[r.params[i]] = m[i + 1].str();
      return r.handler(req);
    }
  }
  return res;
}

bool Server::match_websocket(Request& req, std::shared_ptr<WebSocketHub>& hub, WebSocketOpenHandler& on_open) const {
  for (const auto& r : websocketRoutes) {
    std::smatch m;
    if (std::regex_match(req.path, m, r.regex)) {
      for (size_t i = 0; i < r.params.size(); ++i) req.pathParams[r.params[i]] = m[i + 1].str();
      hub = r.hub;
      on_open = r.on_open;
      return true;
    }
  }
  return false;
}

static std::string header_value(const Request& req, const std::string& name) {
  auto it = req.headers.find(name);
  if (it != req.headers.end()) return it->second;
  std::string lowerName = name;
  std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const auto& [k, v] : req.headers) {
    std::string lowerKey = k;
    std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowerKey == lowerName) return v;
  }
  return "";
}

static std::string idempotency_cache_key(const Request& req) {
  std::string key = header_value(req, "Idempotency-Key");
  if (key.empty()) return "";
  return req.method + " " + req.path + " " + key;
}

static std::string make_idempotency_key() {
  static std::atomic<unsigned long long> counter{0};
  auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  return "post-" + std::to_string(now) + "-" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

static Response handle_with_idempotency(const Server& server, Request req) {
  struct Cache {
    std::mutex mutex;
    std::map<std::string, Response> responses;
    std::deque<std::string> order;
    size_t capacity = static_cast<size_t>(env_int("HTTP_IDEMPOTENCY_CACHE_SIZE", 2048));
  };
  static Cache cache;

  if (req.method != "POST") return server.handle(std::move(req));
  std::string key = idempotency_cache_key(req);
  if (key.empty()) return server.handle(std::move(req));
  {
    std::lock_guard<std::mutex> lock(cache.mutex);
    auto it = cache.responses.find(key);
    if (it != cache.responses.end()) return it->second;
  }
  Response response = server.handle(std::move(req));
  if (response.status >= 200 && response.status < 300) {
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (!cache.responses.count(key)) {
      cache.responses[key] = response;
      cache.order.push_back(key);
      while (cache.order.size() > cache.capacity) {
        cache.responses.erase(cache.order.front());
        cache.order.pop_front();
      }
    }
  }
  return response;
}

static void set_cors_headers(auto& out) {
  out.set("Access-Control-Allow-Origin", "*");
  out.set("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
  out.set("Access-Control-Allow-Headers", "Content-Type, Accept, Authorization");
}

static beast_http::response<beast_http::string_body> to_beast_response(const Response& res, bool keepAlive) {
  beast_http::response<beast_http::string_body> out;
  out.version(11);
  out.result(static_cast<unsigned>(res.status));
  out.set(beast_http::field::server, "RealityEngine_CPP");
  out.set(beast_http::field::content_type, res.contentType);
  set_cors_headers(out);
  out.body() = res.body;
  out.keep_alive(keepAlive);
  out.prepare_payload();
  return out;
}

// ---------------------------------------------------------------------------
// Lane TLS (RealityEngine_CI#668, RealityEngine_CPP#193)
//
// The runtime contract (RealityEngine_CI docs/TRANSPORT_SECURITY.md): RE_TLS_CERT,
// RE_TLS_KEY, RE_TLS_CA, RE_TLS_CRL. TLS 1.3 only, the profile's suites and
// groups (config/transport-profile.json), X25519MLKEM768 first where the
// linked OpenSSL has it. With RE_TLS_CERT unset nothing here runs and the
// engine serves and calls exactly as before.
// ---------------------------------------------------------------------------

static std::string env_string(const char* name) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

TlsSettings tls_settings_from_env() {
  return {env_string("RE_TLS_CERT"), env_string("RE_TLS_KEY"), env_string("RE_TLS_CA"), env_string("RE_TLS_CRL")};
}

namespace {

constexpr const char* kProfileSuites = "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256";
constexpr const char* kProfileGroupsPq = "X25519MLKEM768:X25519:secp384r1";
constexpr const char* kProfileGroupsFallback = "X25519:secp384r1";

std::string openssl_error(const std::string& what) {
  unsigned long code = ERR_get_error();
  char buf[256] = {0};
  if (code) ERR_error_string_n(code, buf, sizeof(buf));
  return what + (code ? std::string(": ") + buf : std::string());
}

// TLS 1.3 only, the profile's suites, and its groups. Returns the group list in
// force so the caller can report a fallback.
std::string apply_profile(SSL_CTX* ctx) {
  if (SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) != 1 || SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION) != 1)
    throw std::runtime_error(openssl_error("TLS: cannot pin TLS 1.3"));
  if (SSL_CTX_set_ciphersuites(ctx, kProfileSuites) != 1)
    throw std::runtime_error(openssl_error("TLS: cannot set the profile's cipher suites"));
  if (SSL_CTX_set1_groups_list(ctx, kProfileGroupsPq) == 1) return kProfileGroupsPq;
  ERR_clear_error();
  if (SSL_CTX_set1_groups_list(ctx, kProfileGroupsFallback) != 1)
    throw std::runtime_error(openssl_error("TLS: cannot set the profile's key-exchange groups"));
  return kProfileGroupsFallback;
}

void load_lane_trust(SSL_CTX* ctx, const TlsSettings& tls) {
  if (SSL_CTX_load_verify_locations(ctx, tls.ca.c_str(), nullptr) != 1)
    throw std::runtime_error(openssl_error("TLS: cannot load RE_TLS_CA " + tls.ca));
  if (!tls.crl.empty()) {
    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    X509_LOOKUP* lookup = X509_STORE_add_lookup(store, X509_LOOKUP_file());
    if (!lookup || X509_load_crl_file(lookup, tls.crl.c_str(), X509_FILETYPE_PEM) < 1)
      throw std::runtime_error(openssl_error("TLS: cannot load RE_TLS_CRL " + tls.crl));
    X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK);
  }
}

void load_own_certificate(SSL_CTX* ctx, const TlsSettings& tls) {
  if (SSL_CTX_use_certificate_chain_file(ctx, tls.cert.c_str()) != 1)
    throw std::runtime_error(openssl_error("TLS: cannot load RE_TLS_CERT " + tls.cert));
  if (SSL_CTX_use_PrivateKey_file(ctx, tls.key.c_str(), SSL_FILETYPE_PEM) != 1)
    throw std::runtime_error(openssl_error("TLS: cannot load RE_TLS_KEY " + tls.key));
  if (SSL_CTX_check_private_key(ctx) != 1)
    throw std::runtime_error(openssl_error("TLS: RE_TLS_KEY does not match RE_TLS_CERT"));
}

std::shared_ptr<ssl::context> make_server_context(const TlsSettings& tls, bool requireAtHandshake, std::string& groups) {
  if (tls.key.empty() || tls.ca.empty())
    throw std::runtime_error("TLS: RE_TLS_CERT is set, so RE_TLS_KEY and RE_TLS_CA must be too");
  auto ctx = std::make_shared<ssl::context>(ssl::context::tls_server);
  groups = apply_profile(ctx->native_handle());
  load_own_certificate(ctx->native_handle(), tls);
  load_lane_trust(ctx->native_handle(), tls);
  // A presented certificate is always verified. Whether one is required is
  // decided at the handshake, or per route when some routes are open to
  // devices (Server::allow_without_client_certificate).
  int mode = SSL_VERIFY_PEER | (requireAtHandshake ? SSL_VERIFY_FAIL_IF_NO_PEER_CERT : 0);
  SSL_CTX_set_verify(ctx->native_handle(), mode, nullptr);
  // A server that verifies client certificates must name its session context,
  // or OpenSSL refuses every resumed session with an internal_error alert. The
  // first live TLS universe hit exactly that: Manager's Node agent caches TLS
  // sessions, so its second connection to a C++ RE failed while curl's
  // one-shot connections succeeded.
  static const unsigned char kSessionContext[] = "realityengine";
  if (SSL_CTX_set_session_id_context(ctx->native_handle(), kSessionContext, sizeof(kSessionContext) - 1) != 1)
    throw std::runtime_error(openssl_error("TLS: cannot set the session id context"));
  return ctx;
}

using tls_stream = beast::ssl_stream<beast::tcp_stream>;

template <class Stream> constexpr bool is_tls_v = std::is_same_v<Stream, tls_stream>;

// The verified client certificate, if the peer presented one.
template <class Stream> bool has_verified_peer(Stream& stream) {
  if constexpr (is_tls_v<Stream>) {
    X509* peer = SSL_get_peer_certificate(stream.native_handle());
    bool ok = peer && SSL_get_verify_result(stream.native_handle()) == X509_V_OK;
    if (peer) X509_free(peer);
    return ok;
  } else {
    (void)stream;
    return true;
  }
}

} // namespace

void Server::allow_without_client_certificate(std::string pattern) {
  certificateOptional.push_back(compile_pattern(pattern).first);
}

bool Server::requires_client_certificate(const std::string& path) const {
  for (const auto& rx : certificateOptional)
    if (std::regex_match(path, rx)) return false;
  return true;
}

// The hub's sessions. One base for both transports, so SseHub can hold either.
class SseSession {
public:
  virtual ~SseSession() = default;
  virtual void send(std::string text) = 0;

protected:
  static void register_with(Server::SseHub& hub, std::weak_ptr<SseSession> session) { hub.add(std::move(session)); }
};

template <class Stream>
class WebSocketSession : public Server::WebSocketConnection, public std::enable_shared_from_this<WebSocketSession<Stream>> {
public:
  WebSocketSession(Stream stream, std::shared_ptr<Server::WebSocketHub> hub, Server::WebSocketOpenHandler on_open)
      : ws(std::move(stream)), hub(std::move(hub)), on_open(std::move(on_open)) {}

  void run(beast_http::request<beast_http::string_body> request) {
    // The tcp_stream arrives from HttpSession with that session's per-request
    // deadline (HTTP_SESSION_TIMEOUT_MS, 5 s) still armed. Left armed, it closed
    // every WebSocket 5 s after it opened, mid-read (close code 1006). The
    // websocket stream's own suggested timeouts take over from here, and Beast
    // requires the lower layer's expiry to be off when they do.
    beast::get_lowest_layer(ws).expires_never();
    ws.set_option(beast_websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws.set_option(beast_websocket::stream_base::decorator([](beast_websocket::response_type& res) {
      res.set(beast_http::field::server, "RealityEngine_CPP");
    }));
    ws.async_accept(request, [self = this->shared_from_this()](beast::error_code ec) {
      if (ec) return;
      if (self->hub) self->hub->add(self);
      if (self->on_open) self->on_open(self);
      self->read_next();
    });
  }

  void send(std::string text) override {
    asio::post(ws.get_executor(), [self = this->shared_from_this(), text = std::move(text)]() mutable {
      self->outbox.push_back(std::move(text));
      if (!self->writing) self->write_next();
    });
  }

private:
  void read_next() {
    ws.async_read(buffer, [self = this->shared_from_this()](beast::error_code ec, std::size_t) {
      self->buffer.consume(self->buffer.size());
      if (ec) return;
      self->read_next();
    });
  }

  void write_next() {
    if (outbox.empty()) {
      writing = false;
      return;
    }
    writing = true;
    ws.text(true);
    ws.async_write(asio::buffer(outbox.front()), [self = this->shared_from_this()](beast::error_code ec, std::size_t) {
      if (ec) {
        self->outbox.clear();
        self->writing = false;
        return;
      }
      self->outbox.pop_front();
      self->write_next();
    });
  }

  beast_websocket::stream<Stream> ws;
  beast::flat_buffer buffer;
  std::deque<std::string> outbox;
  std::shared_ptr<Server::WebSocketHub> hub;
  Server::WebSocketOpenHandler on_open;
  bool writing = false;
};

template <class Stream>
class SseSessionT : public SseSession, public std::enable_shared_from_this<SseSessionT<Stream>> {
public:
  SseSessionT(Stream stream, std::shared_ptr<Server::SseHub> hub)
      : stream(std::move(stream)), hub(std::move(hub)),
        heartbeatTimer(this->stream.get_executor()) {}

  void run(beast_http::request<beast_http::string_body> req) {
    // The tcp_stream arrives from HttpSession with that session's per-request
    // deadline (HTTP_SESSION_TIMEOUT_MS, 5 s) still armed. An event stream lives
    // as long as its client, so the deadline must go: left armed, the first
    // write after it — the 15 s keepalive — failed with a timeout and closed
    // the socket, so every /api/engine/stream and /api/events client was cut
    // off at 15 s and had to reconnect, losing any step sent in between. A dead
    // peer is detected by the heartbeat's failed write instead.
    beast::get_lowest_layer(stream).expires_never();
    auto res = std::make_shared<beast_http::response<beast_http::empty_body>>();
    res->version(req.version());
    res->result(beast_http::status::ok);
    res->set(beast_http::field::content_type, "text/event-stream");
    res->set(beast_http::field::cache_control, "no-cache");
    res->set("Access-Control-Allow-Origin", "*");
    res->set("X-Accel-Buffering", "no");
    res->chunked(true);
    auto sr = std::make_shared<beast_http::response_serializer<beast_http::empty_body>>(*res);
    beast_http::async_write_header(stream, *sr,
      [self = this->shared_from_this(), res, sr](beast::error_code ec, std::size_t) {
        if (ec) return;
        if (self->hub) register_with(*self->hub, self);
        self->send(": connected\r\n\r\n");
        self->schedule_heartbeat();
      });
  }

  void send(std::string text) override {
    asio::post(stream.get_executor(),
      [self = this->shared_from_this(), text = std::move(text)]() mutable {
        if (self->closed) return;
        self->outbox.push_back(std::move(text));
        if (!self->writing) self->write_next();
      });
  }

private:
  void write_next() {
    if (closed || outbox.empty()) { writing = false; return; }
    writing = true;
    const std::string& data = outbox.front();
    std::ostringstream oss;
    oss << std::hex << data.size() << "\r\n" << data << "\r\n";
    auto chunk = std::make_shared<std::string>(oss.str());
    asio::async_write(stream, asio::buffer(*chunk),
      [self = this->shared_from_this(), chunk](beast::error_code ec, std::size_t) {
        self->outbox.pop_front();
        if (ec) { self->close(); return; }
        self->write_next();
      });
  }

  // A failed write means the peer is gone. Close, and stop the heartbeat: it
  // holds the session alive, so without this a dead session kept waking every
  // interval to fail another write, indefinitely.
  void close() {
    if (closed) return;
    closed = true;
    writing = false;
    outbox.clear();
    heartbeatTimer.cancel();
    beast::error_code ignored;
    beast::get_lowest_layer(stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
    beast::get_lowest_layer(stream).close();
  }

  void schedule_heartbeat() {
    if (closed) return;
    heartbeatTimer.expires_after(std::chrono::milliseconds(heartbeat_ms()));
    heartbeatTimer.async_wait([self = this->shared_from_this()](beast::error_code ec) {
      if (ec || self->closed) return;
      self->send(": keepalive\r\n\r\n");
      self->schedule_heartbeat();
    });
  }

  // SURFACE_SPEC: a keepalive comment every 15 s. Overridable for tests.
  static int heartbeat_ms() {
    static const int ms = env_int("SSE_HEARTBEAT_MS", 15000, 10);
    return ms;
  }

  Stream stream;
  std::shared_ptr<Server::SseHub> hub;
  asio::steady_timer heartbeatTimer;
  std::deque<std::string> outbox;
  bool writing = false;
  bool closed = false;
};

void Server::SseHub::add(std::weak_ptr<SseSession> session) {
  std::lock_guard<std::mutex> lock(mutex);
  sessions.push_back(std::move(session));
}

void Server::SseHub::broadcast(const std::string& text) {
  const std::string event = "data: " + text + "\n\n";
  std::lock_guard<std::mutex> lock(mutex);
  auto it = sessions.begin();
  while (it != sessions.end()) {
    if (auto session = it->lock()) {
      session->send(event);
      ++it;
    } else {
      it = sessions.erase(it);
    }
  }
}

template <class Stream>
class HttpSession : public std::enable_shared_from_this<HttpSession<Stream>> {
public:
  // Plain TCP.
  HttpSession(tcp::socket socket, const Server& server, int timeoutMs, int maxRequests)
    requires(!is_tls_v<Stream>)
      : stream(std::move(socket)), server(server), timeoutMs(timeoutMs), maxRequests(maxRequests) {}
  // TLS over TCP.
  HttpSession(tcp::socket socket, ssl::context& ctx, const Server& server, int timeoutMs, int maxRequests)
    requires(is_tls_v<Stream>)
      : stream(std::move(socket), ctx), server(server), timeoutMs(timeoutMs), maxRequests(maxRequests) {}

  void run() {
    if constexpr (is_tls_v<Stream>) {
      beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeoutMs));
      stream.async_handshake(ssl::stream_base::server, [self = this->shared_from_this()](beast::error_code ec) {
        // A refused handshake (no or untrusted client certificate, TLS 1.2,
        // plaintext on the TLS port) closes the connection; there is no
        // plaintext fallback to offer.
        if (ec) return self->drop();
        self->read_next();
      });
    } else {
      read_next();
    }
  }

private:
  void read_next() {
    if (handled >= maxRequests) return close();
    request = {};
    beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeoutMs));
    beast_http::async_read(stream, buffer, request, [self = this->shared_from_this()](beast::error_code ec, std::size_t) {
      if (ec) return self->close();
      self->write_current();
    });
  }

  void reply_and_close(const Response& response) {
    auto out = std::make_shared<beast_http::response<beast_http::string_body>>(to_beast_response(response, false));
    beast_http::async_write(stream, *out, [self = this->shared_from_this(), out](beast::error_code, std::size_t) { self->close(); });
  }

  void write_current() {
    ++handled;
    if (request.method() == beast_http::verb::options) {
      beast_http::response<beast_http::string_body> out;
      out.version(11);
      out.result(beast_http::status::no_content);
      out.set(beast_http::field::server, "RealityEngine_CPP");
      set_cors_headers(out);
      out.keep_alive(false);
      out.prepare_payload();
      auto outPtr = std::make_shared<beast_http::response<beast_http::string_body>>(std::move(out));
      beast_http::async_write(stream, *outPtr, [self = this->shared_from_this(), outPtr](beast::error_code, std::size_t) { self->close(); });
      return;
    }
    // Some routes are open to devices without a client certificate; every
    // other route on a TLS port requires one (the runtime contract's mTLS).
    if constexpr (is_tls_v<Stream>) {
      if (!has_verified_peer(stream) && server.requires_client_certificate(to_request(request).path))
        return reply_and_close(error_response("client certificate required", 403));
    }
    if (beast_websocket::is_upgrade(request)) {
      Request req = to_request(request);
      std::shared_ptr<Server::WebSocketHub> hub;
      Server::WebSocketOpenHandler on_open;
      if (server.match_websocket(req, hub, on_open)) {
        std::make_shared<WebSocketSession<Stream>>(std::move(stream), std::move(hub), std::move(on_open))->run(std::move(request));
        return;
      }
    }
    {
      Request req = to_request(request);
      std::shared_ptr<Server::SseHub> sseHub;
      if (server.match_sse(req, sseHub)) {
        if (request.method() != beast_http::verb::get) return reply_and_close(error_response("Method Not Allowed", 405));
        std::make_shared<SseSessionT<Stream>>(std::move(stream), std::move(sseHub))->run(std::move(request));
        return;
      }
    }
    bool keepAlive = request.keep_alive() && handled < maxRequests;
    Response response;
    try {
      response = handle_with_idempotency(server, to_request(request));
    } catch (const std::exception& e) {
      response = error_response(e.what(), 500);
      keepAlive = false;
    } catch (...) {
      response = error_response("unknown handler error", 500);
      keepAlive = false;
    }
    auto out = std::make_shared<beast_http::response<beast_http::string_body>>(to_beast_response(response, keepAlive));
    beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeoutMs));
    beast_http::async_write(stream, *out, [self = this->shared_from_this(), out, keepAlive](beast::error_code ec, std::size_t) {
      if (ec || !keepAlive) return self->close();
      self->read_next();
    });
  }

  void close() {
    if constexpr (is_tls_v<Stream>) {
      // close_notify, bounded by the session deadline, then the socket.
      beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeoutMs));
      stream.async_shutdown([self = this->shared_from_this()](beast::error_code) { self->drop(); });
    } else {
      beast::error_code ec;
      stream.socket().shutdown(tcp::socket::shutdown_send, ec);
    }
  }

  void drop() {
    beast::error_code ec;
    beast::get_lowest_layer(stream).socket().shutdown(tcp::socket::shutdown_both, ec);
    beast::get_lowest_layer(stream).close();
  }

  Stream stream;
  beast::flat_buffer buffer;
  beast_http::request<beast_http::string_body> request;
  const Server& server;
  int timeoutMs;
  int maxRequests;
  int handled = 0;
};

class Listener : public std::enable_shared_from_this<Listener> {
public:
  Listener(asio::io_context& ioc, tcp::endpoint endpoint, const Server& server, int timeoutMs, int maxRequests,
           std::shared_ptr<ssl::context> tls)
      : ioc(ioc), acceptor(ioc), server(server), timeoutMs(timeoutMs), maxRequests(maxRequests), tls(std::move(tls)) {
    beast::error_code ec;
    acceptor.open(endpoint.protocol(), ec);
    if (ec) throw std::runtime_error("acceptor open failed: " + ec.message());
    acceptor.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) throw std::runtime_error("acceptor option failed: " + ec.message());
    acceptor.bind(endpoint, ec);
    if (ec) throw std::runtime_error("bind failed: " + ec.message());
    acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) throw std::runtime_error("listen failed: " + ec.message());
  }

  void run() { accept_next(); }

private:
  void accept_next() {
    acceptor.async_accept(asio::make_strand(ioc), [self = shared_from_this()](beast::error_code ec, tcp::socket socket) {
      if (!ec) {
        if (self->tls)
          std::make_shared<HttpSession<tls_stream>>(std::move(socket), *self->tls, self->server, self->timeoutMs, self->maxRequests)->run();
        else
          std::make_shared<HttpSession<beast::tcp_stream>>(std::move(socket), self->server, self->timeoutMs, self->maxRequests)->run();
      }
      self->accept_next();
    });
  }

  asio::io_context& ioc;
  tcp::acceptor acceptor;
  const Server& server;
  int timeoutMs;
  int maxRequests;
  std::shared_ptr<ssl::context> tls;
};

void Server::listen(int port) {
  size_t defaultWorkers = static_cast<size_t>(std::max(2u, std::thread::hardware_concurrency()));
  size_t workerCount = static_cast<size_t>(env_int("HTTP_WORKERS", static_cast<int>(defaultWorkers)));
  asio::io_context ioc{static_cast<int>(workerCount)};
  tcp::endpoint endpoint{tcp::v4(), static_cast<unsigned short>(port)};
  int timeoutMs = env_int("HTTP_SESSION_TIMEOUT_MS", 5000, 100);
  int maxRequests = env_int("HTTP_MAX_KEEPALIVE_REQUESTS", 32, 1);

  // RE_TLS_CERT set: TLS 1.3 on this port, never a plaintext listener beside
  // it. A context that cannot be built stops the server rather than starting it
  // plaintext.
  std::shared_ptr<ssl::context> tls;
  std::string groups;
  TlsSettings settings = tls_settings_from_env();
  if (settings.serving()) tls = make_server_context(settings, !has_certificate_optional_routes(), groups);

  auto work = asio::make_work_guard(ioc);
  auto listener = std::make_shared<Listener>(ioc, endpoint, *this, timeoutMs, maxRequests, tls);
  listener->run();
  std::vector<std::thread> workers;
  workers.reserve(workerCount > 0 ? workerCount - 1 : 0);
  for (size_t i = 1; i < workerCount; ++i) workers.emplace_back([&]() { ioc.run(); });
  if (tls) {
    std::cout << "listening on " << port << " (TLS 1.3, client certificate "
              << (has_certificate_optional_routes() ? "required except device routes" : "required")
              << ", groups " << groups << (groups == kProfileGroupsFallback ? " — this OpenSSL has no X25519MLKEM768" : "")
              << ")" << std::endl;
  } else {
    std::cout << "listening on " << port << std::endl;
  }
  ioc.run();
  for (auto& worker : workers) if (worker.joinable()) worker.join();
}

Response json_response(const std::string& body, int status) { return {status, body, "application/json"}; }

Response error_response(const std::string& message, int status) {
  return json_response(json::stringify(json::Value::Object{{"error", message}}), status);
}

struct ParsedUrl {
  std::string scheme = "http";
  std::string host;
  std::string port = "80";
  std::string path = "/";
};

static ParsedUrl parse_url(const std::string& url) {
  std::string u = url;
  ParsedUrl parsed;
  const std::string httpPrefix = "http://";
  const std::string httpsPrefix = "https://";
  if (u.rfind(httpPrefix, 0) == 0) {
    parsed.scheme = "http";
    parsed.port = "80";
    u = u.substr(httpPrefix.size());
  } else if (u.rfind(httpsPrefix, 0) == 0) {
    parsed.scheme = "https";
    parsed.port = "443";
    u = u.substr(httpsPrefix.size());
  }
  auto slash = u.find('/');
  std::string hostPort = slash == std::string::npos ? u : u.substr(0, slash);
  parsed.path = slash == std::string::npos ? "/" : u.substr(slash);
  auto colon = hostPort.find(':');
  parsed.host = hostPort;
  if (colon != std::string::npos) {
    parsed.host = hostPort.substr(0, colon);
    parsed.port = hostPort.substr(colon + 1);
  }
  return parsed;
}

// An internal peer: loopback, a private or link-local address, a single-label
// name (a compose service), or a name under .localhost, .internal or .local.
// Internal links trust the lane CA alone and present this instance's
// certificate; every other host is an external party, verified against the
// system store for that link only (docs/TRANSPORT_SECURITY.md, "Calling").
bool is_internal_host(const std::string& host) {
  if (host.empty()) return false;
  auto ends_with = [&](const std::string& suffix) {
    return host.size() >= suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
  };
  if (host == "localhost" || ends_with(".localhost") || ends_with(".internal") || ends_with(".local")) return true;
  beast::error_code ec;
  auto address = asio::ip::make_address(host, ec);
  if (!ec) {
    if (address.is_loopback()) return true;
    if (address.is_v4()) {
      auto b = address.to_v4().to_bytes();
      return b[0] == 10 || (b[0] == 172 && (b[1] & 0xF0) == 16) || (b[0] == 192 && b[1] == 168) || (b[0] == 169 && b[1] == 254);
    }
    auto v6 = address.to_v6();
    auto b = v6.to_bytes();
    return v6.is_link_local() || (b[0] & 0xFE) == 0xFC;
  }
  return host.find('.') == std::string::npos;
}

namespace {

// One client context per kind of peer, built once. With the lane off (no
// RE_TLS_CA) an https call keeps its earlier behaviour: the system store, the
// library's default versions.
std::shared_ptr<ssl::context> client_context(bool internal) {
  static std::mutex mutex;
  static std::shared_ptr<ssl::context> internalCtx, externalCtx;
  std::lock_guard<std::mutex> lock(mutex);
  auto& slot = internal ? internalCtx : externalCtx;
  if (slot) return slot;
  TlsSettings tls = tls_settings_from_env();
  auto ctx = std::make_shared<ssl::context>(ssl::context::tls_client);
  if (tls.lane()) {
    // The profile holds on every link once the lane is on, external included:
    // an outside party that cannot meet it is refused, not excepted.
    apply_profile(ctx->native_handle());
    if (internal) {
      load_lane_trust(ctx->native_handle(), tls);
      if (tls.serving()) load_own_certificate(ctx->native_handle(), tls);
    } else {
      ctx->set_default_verify_paths();
    }
  } else {
    ctx->set_default_verify_paths();
  }
  ctx->set_verify_mode(ssl::verify_peer);
  slot = ctx;
  return slot;
}

// Hostname or IP verification, by OpenSSL, plus SNI for a DNS name.
void bind_peer_identity(SSL* ssl_handle, const std::string& host) {
  beast::error_code ec;
  asio::ip::make_address(host, ec);
  X509_VERIFY_PARAM* param = SSL_get0_param(ssl_handle);
  if (!ec) {
    if (X509_VERIFY_PARAM_set1_ip_asc(param, host.c_str()) != 1)
      throw std::runtime_error(openssl_error("TLS: cannot verify the peer address " + host));
  } else {
    X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (X509_VERIFY_PARAM_set1_host(param, host.c_str(), 0) != 1)
      throw std::runtime_error(openssl_error("TLS: cannot verify the peer name " + host));
    if (!SSL_set_tlsext_host_name(ssl_handle, host.c_str()))
      throw beast::system_error(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category());
  }
}

} // namespace

static void set_method(beast_http::request<beast_http::string_body>& req, const std::string& method) {
  if (method == "POST") req.method(beast_http::verb::post);
  else if (method == "PATCH") req.method(beast_http::verb::patch);
  else if (method == "PUT") req.method(beast_http::verb::put);
  else if (method == "DELETE") req.method(beast_http::verb::delete_);
  else req.method(beast_http::verb::get);
}

// A kept-alive connection to one host:port, plaintext or TLS. TLS connections
// are pooled like plaintext ones: an engine instance's PE calls its RE many
// times per step, and a handshake per call would put one on every step.
class PersistentConnection {
public:
  PersistentConnection(std::string scheme, std::string host, std::string port)
      : tls(scheme == "https"), host(std::move(host)), port(std::move(port)) {
    reset_stream();
  }

  std::string request(const std::string& method,
                      const std::string& target,
                      const std::string& body,
                      int timeoutMs,
                      const std::string& idempotencyKey,
                      const std::map<std::string, std::string>& headers) {
    std::lock_guard<std::mutex> lock(mutex);
    try {
      return request_once(method, target, body, timeoutMs, idempotencyKey, headers);
    } catch (...) {
      if ((method == "POST" || method == "PATCH") && idempotencyKey.empty()) throw;
      reconnect();
      return request_once(method, target, body, timeoutMs, idempotencyKey, headers);
    }
  }

  void close() {
    std::lock_guard<std::mutex> lock(mutex);
    close_unlocked();
  }

private:
  beast::tcp_stream& lowest() { return tls ? beast::get_lowest_layer(*secure) : *plain; }

  void reset_stream() {
    if (tls) {
      plain.reset();
      ctx = client_context(is_internal_host(host));
      secure = std::make_unique<tls_stream>(ioc, *ctx);
    } else {
      secure.reset();
      plain = std::make_unique<beast::tcp_stream>(ioc);
    }
  }

  template <class S>
  std::string exchange(S& s, beast_http::request<beast_http::string_body>& req, int timeoutMs) {
    lowest().expires_after(std::chrono::milliseconds(timeoutMs));
    beast_http::write(s, req);
    beast::flat_buffer buffer;
    beast_http::response_parser<beast_http::string_body> parser;
    parser.body_limit((std::numeric_limits<std::uint64_t>::max)());
    lowest().expires_after(std::chrono::milliseconds(timeoutMs));
    beast_http::read(s, buffer, parser);
    auto& res = parser.get();
    if (!res.keep_alive()) connected = false;
    return res.body();
  }

  std::string request_once(const std::string& method,
                           const std::string& target,
                           const std::string& body,
                           int timeoutMs,
                           const std::string& idempotencyKey,
                           const std::map<std::string, std::string>& headers) {
    connect_if_needed(timeoutMs);

    beast_http::request<beast_http::string_body> req;
    req.version(11);
    set_method(req, method);
    req.target(target);
    req.set(beast_http::field::host, host);
    req.set(beast_http::field::user_agent, "RealityEngine_CPP");
    for (const auto& [key, value] : headers) req.set(key, value);
    req.keep_alive(true);
    if (method == "POST" || method == "PATCH" || method == "PUT") {
      req.set(beast_http::field::content_type, "application/json");
      if (!idempotencyKey.empty()) req.set("Idempotency-Key", idempotencyKey);
      req.body() = body;
    }
    req.prepare_payload();
    return tls ? exchange(*secure, req, timeoutMs) : exchange(*plain, req, timeoutMs);
  }

  void connect_if_needed(int timeoutMs) {
    if (connected && lowest().socket().is_open()) return;
    if (tls) reset_stream();  // a TLS stream cannot be re-handshaken after use
    tcp::resolver resolver{ioc};
    auto const results = resolver.resolve(host, port);
    lowest().expires_after(std::chrono::milliseconds(timeoutMs));
    lowest().connect(results);
    if (tls) {
      bind_peer_identity(secure->native_handle(), host);
      lowest().expires_after(std::chrono::milliseconds(timeoutMs));
      secure->handshake(ssl::stream_base::client);
    }
    connected = true;
  }

  void reconnect() {
    close_unlocked();
    reset_stream();
    connected = false;
  }

  void close_unlocked() {
    beast::error_code ec;
    if (tls && secure && lowest().socket().is_open() && connected) {
      lowest().expires_after(std::chrono::milliseconds(500));
      secure->shutdown(ec);
    }
    if ((tls && secure) || (!tls && plain)) {
      if (lowest().socket().is_open()) lowest().socket().shutdown(tcp::socket::shutdown_both, ec);
      lowest().close();
    }
    connected = false;
  }

  bool tls;
  std::string host;
  std::string port;
  asio::io_context ioc;
  std::shared_ptr<ssl::context> ctx;
  std::unique_ptr<beast::tcp_stream> plain;
  std::unique_ptr<tls_stream> secure;
  std::mutex mutex;
  bool connected = false;
};

static std::mutex clientMutex;
static std::map<std::string, std::vector<std::unique_ptr<PersistentConnection>>> clients;
static std::map<std::string, size_t> clientCursor;

static std::string request_body(const std::string& method,
                                const std::string& url,
                                const std::string& body,
                                const std::map<std::string, std::string>& headers = {}) {
  ParsedUrl parsed = parse_url(url);
  size_t poolSize = static_cast<size_t>(env_int("HTTP_CLIENT_POOL_SIZE", 4));
  int timeoutMs = env_int("HTTP_CLIENT_TIMEOUT_MS", 5000, 100);
  std::string idempotencyKey;
  if (method == "POST" || method == "PATCH") {
    idempotencyKey = make_idempotency_key();
  }

  std::string key = parsed.scheme + "://" + parsed.host + ":" + parsed.port;
  PersistentConnection* client = nullptr;
  {
    std::lock_guard<std::mutex> lock(clientMutex);
    auto& pool = clients[key];
    if (pool.empty()) {
      pool.reserve(poolSize);
      for (size_t i = 0; i < poolSize; ++i) pool.push_back(std::make_unique<PersistentConnection>(parsed.scheme, parsed.host, parsed.port));
      clientCursor[key] = 0;
    }
    size_t index = clientCursor[key]++ % pool.size();
    client = pool[index].get();
  }
  return client->request(method, parsed.path, body, timeoutMs, idempotencyKey, headers);
}

std::string request_json(const std::string& method, const std::string& url, const std::string& body) {
  return request_body(method, url, body);
}

std::string request_json(const std::string& method, const std::string& url, const std::string& body, const std::map<std::string, std::string>& headers) {
  return request_body(method, url, body, headers);
}

std::string post_json(const std::string& url, const std::string& body) {
  return request_body("POST", url, body);
}

std::string get(const std::string& url) {
  return request_body("GET", url, "");
}

void close_persistent_connections() {
  std::lock_guard<std::mutex> lock(clientMutex);
  for (auto& [_, pool] : clients) for (auto& client : pool) client->close();
  clients.clear();
  clientCursor.clear();
}

} // namespace reality::http
