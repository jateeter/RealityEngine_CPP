#include "reality/http.hpp"
#include "reality/json.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <openssl/err.h>

#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
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

static beast_http::response<beast_http::string_body> to_beast_response(const Response& res, bool keepAlive) {
  beast_http::response<beast_http::string_body> out;
  out.version(11);
  out.result(static_cast<unsigned>(res.status));
  out.set(beast_http::field::server, "RealityEngine_CPP");
  out.set(beast_http::field::content_type, res.contentType);
  out.body() = res.body;
  out.keep_alive(keepAlive);
  out.prepare_payload();
  return out;
}

class WebSocketSession : public Server::WebSocketConnection, public std::enable_shared_from_this<WebSocketSession> {
public:
  WebSocketSession(beast::tcp_stream stream, std::shared_ptr<Server::WebSocketHub> hub, Server::WebSocketOpenHandler on_open)
      : ws(std::move(stream)), hub(std::move(hub)), on_open(std::move(on_open)) {}

  void run(beast_http::request<beast_http::string_body> request) {
    ws.set_option(beast_websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws.set_option(beast_websocket::stream_base::decorator([](beast_websocket::response_type& res) {
      res.set(beast_http::field::server, "RealityEngine_CPP");
    }));
    ws.async_accept(request, [self = shared_from_this()](beast::error_code ec) {
      if (ec) return;
      if (self->hub) self->hub->add(self);
      if (self->on_open) self->on_open(self);
      self->read_next();
    });
  }

  void send(std::string text) override {
    asio::post(ws.get_executor(), [self = shared_from_this(), text = std::move(text)]() mutable {
      self->outbox.push_back(std::move(text));
      if (!self->writing) self->write_next();
    });
  }

private:
  void read_next() {
    ws.async_read(buffer, [self = shared_from_this()](beast::error_code ec, std::size_t) {
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
    ws.async_write(asio::buffer(outbox.front()), [self = shared_from_this()](beast::error_code ec, std::size_t) {
      if (ec) {
        self->outbox.clear();
        self->writing = false;
        return;
      }
      self->outbox.pop_front();
      self->write_next();
    });
  }

  beast_websocket::stream<beast::tcp_stream> ws;
  beast::flat_buffer buffer;
  std::deque<std::string> outbox;
  std::shared_ptr<Server::WebSocketHub> hub;
  Server::WebSocketOpenHandler on_open;
  bool writing = false;
};

class SseSession : public std::enable_shared_from_this<SseSession> {
public:
  SseSession(beast::tcp_stream stream, std::shared_ptr<Server::SseHub> hub)
      : stream(std::move(stream)), hub(std::move(hub)),
        heartbeatTimer(this->stream.get_executor()) {}

  void run(beast_http::request<beast_http::string_body> req) {
    beast_http::response<beast_http::empty_body> res;
    res.version(req.version());
    res.result(beast_http::status::ok);
    res.set(beast_http::field::content_type, "text/event-stream");
    res.set(beast_http::field::cache_control, "no-cache");
    res.set("Access-Control-Allow-Origin", "*");
    res.set("X-Accel-Buffering", "no");
    res.chunked(true);
    auto sr = std::make_shared<beast_http::response_serializer<beast_http::empty_body>>(res);
    beast_http::async_write_header(stream, *sr,
      [self = shared_from_this(), sr](beast::error_code ec, std::size_t) {
        if (ec) return;
        if (self->hub) self->hub->add(self);
        self->send(": connected\r\n\r\n");
        self->schedule_heartbeat();
      });
  }

  void send(std::string text) {
    asio::post(stream.get_executor(),
      [self = shared_from_this(), text = std::move(text)]() mutable {
        self->outbox.push_back(std::move(text));
        if (!self->writing) self->write_next();
      });
  }

private:
  void write_next() {
    if (outbox.empty()) { writing = false; return; }
    writing = true;
    const std::string& data = outbox.front();
    std::ostringstream oss;
    oss << std::hex << data.size() << "\r\n" << data << "\r\n";
    auto chunk = std::make_shared<std::string>(oss.str());
    asio::async_write(stream, asio::buffer(*chunk),
      [self = shared_from_this(), chunk](beast::error_code ec, std::size_t) {
        self->outbox.pop_front();
        if (ec) { self->writing = false; return; }
        self->write_next();
      });
  }

  void schedule_heartbeat() {
    heartbeatTimer.expires_after(std::chrono::seconds(15));
    heartbeatTimer.async_wait([self = shared_from_this()](beast::error_code ec) {
      if (ec) return;
      self->send(": keepalive\r\n\r\n");
      self->schedule_heartbeat();
    });
  }

  beast::tcp_stream stream;
  std::shared_ptr<Server::SseHub> hub;
  asio::steady_timer heartbeatTimer;
  std::deque<std::string> outbox;
  bool writing = false;
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

class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
  HttpSession(tcp::socket socket, const Server& server, int timeoutMs, int maxRequests)
      : stream(std::move(socket)), server(server), timeoutMs(timeoutMs), maxRequests(maxRequests) {}

  void run() { read_next(); }

private:
  void read_next() {
    if (handled >= maxRequests) return close();
    request = {};
    stream.expires_after(std::chrono::milliseconds(timeoutMs));
    beast_http::async_read(stream, buffer, request, [self = shared_from_this()](beast::error_code ec, std::size_t) {
      if (ec) return self->close();
      self->write_current();
    });
  }

  void write_current() {
    ++handled;
    if (beast_websocket::is_upgrade(request)) {
      Request req = to_request(request);
      std::shared_ptr<Server::WebSocketHub> hub;
      Server::WebSocketOpenHandler on_open;
      if (server.match_websocket(req, hub, on_open)) {
        std::make_shared<WebSocketSession>(std::move(stream), std::move(hub), std::move(on_open))->run(std::move(request));
        return;
      }
    }
    {
      Request req = to_request(request);
      std::shared_ptr<Server::SseHub> sseHub;
      if (server.match_sse(req, sseHub)) {
        if (request.method() != beast_http::verb::get) {
          auto out = std::make_shared<beast_http::response<beast_http::string_body>>(
            to_beast_response(error_response("Method Not Allowed", 405), false));
          beast_http::async_write(stream, *out,
            [self = shared_from_this(), out](beast::error_code, std::size_t) { self->close(); });
          return;
        }
        std::make_shared<SseSession>(std::move(stream), std::move(sseHub))->run(std::move(request));
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
    stream.expires_after(std::chrono::milliseconds(timeoutMs));
    beast_http::async_write(stream, *out, [self = shared_from_this(), out, keepAlive](beast::error_code ec, std::size_t) {
      if (ec || !keepAlive) return self->close();
      self->read_next();
    });
  }

  void close() {
    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_send, ec);
  }

  beast::tcp_stream stream;
  beast::flat_buffer buffer;
  beast_http::request<beast_http::string_body> request;
  const Server& server;
  int timeoutMs;
  int maxRequests;
  int handled = 0;
};

class Listener : public std::enable_shared_from_this<Listener> {
public:
  Listener(asio::io_context& ioc, tcp::endpoint endpoint, const Server& server, int timeoutMs, int maxRequests)
      : ioc(ioc), acceptor(ioc), server(server), timeoutMs(timeoutMs), maxRequests(maxRequests) {
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
      if (!ec) std::make_shared<HttpSession>(std::move(socket), self->server, self->timeoutMs, self->maxRequests)->run();
      self->accept_next();
    });
  }

  asio::io_context& ioc;
  tcp::acceptor acceptor;
  const Server& server;
  int timeoutMs;
  int maxRequests;
};

void Server::listen(int port) {
  size_t defaultWorkers = static_cast<size_t>(std::max(2u, std::thread::hardware_concurrency()));
  size_t workerCount = static_cast<size_t>(env_int("HTTP_WORKERS", static_cast<int>(defaultWorkers)));
  asio::io_context ioc{static_cast<int>(workerCount)};
  tcp::endpoint endpoint{tcp::v4(), static_cast<unsigned short>(port)};
  int timeoutMs = env_int("HTTP_SESSION_TIMEOUT_MS", 5000, 100);
  int maxRequests = env_int("HTTP_MAX_KEEPALIVE_REQUESTS", 32, 1);

  std::make_shared<Listener>(ioc, endpoint, *this, timeoutMs, maxRequests)->run();
  std::vector<std::thread> workers;
  workers.reserve(workerCount > 0 ? workerCount - 1 : 0);
  for (size_t i = 1; i < workerCount; ++i) workers.emplace_back([&]() { ioc.run(); });
  std::cout << "listening on " << port << std::endl;
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

class PersistentConnection {
public:
  PersistentConnection(std::string host, std::string port)
      : host(std::move(host)), port(std::move(port)), stream(std::make_unique<beast::tcp_stream>(ioc)) {}

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
  std::string request_once(const std::string& method,
                           const std::string& target,
                           const std::string& body,
                           int timeoutMs,
                           const std::string& idempotencyKey,
                           const std::map<std::string, std::string>& headers) {
    connect_if_needed(timeoutMs);

    beast_http::request<beast_http::string_body> req;
    req.version(11);
    if (method == "POST") req.method(beast_http::verb::post);
    else if (method == "PATCH") req.method(beast_http::verb::patch);
    else if (method == "PUT") req.method(beast_http::verb::put);
    else if (method == "DELETE") req.method(beast_http::verb::delete_);
    else req.method(beast_http::verb::get);
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
    stream->expires_after(std::chrono::milliseconds(timeoutMs));
    beast_http::write(*stream, req);

    beast::flat_buffer buffer;
    beast_http::response<beast_http::string_body> res;
    stream->expires_after(std::chrono::milliseconds(timeoutMs));
    beast_http::read(*stream, buffer, res);
    if (!res.keep_alive()) connected = false;
    return res.body();
  }

  void connect_if_needed(int timeoutMs) {
    if (connected && stream->socket().is_open()) return;
    tcp::resolver resolver{ioc};
    auto const results = resolver.resolve(host, port);
    stream->expires_after(std::chrono::milliseconds(timeoutMs));
    stream->connect(results);
    connected = true;
  }

  void reconnect() {
    close_unlocked();
    stream = std::make_unique<beast::tcp_stream>(ioc);
    connected = false;
  }

  void close_unlocked() {
    beast::error_code ec;
    if (stream && stream->socket().is_open()) stream->socket().shutdown(tcp::socket::shutdown_both, ec);
    if (stream) stream->close();
    connected = false;
  }

  std::string host;
  std::string port;
  asio::io_context ioc;
  std::unique_ptr<beast::tcp_stream> stream;
  std::mutex mutex;
  bool connected = false;
};

static std::mutex clientMutex;
static std::map<std::string, std::vector<std::unique_ptr<PersistentConnection>>> clients;
static std::map<std::string, size_t> clientCursor;

static void set_method(beast_http::request<beast_http::string_body>& req, const std::string& method) {
  if (method == "POST") req.method(beast_http::verb::post);
  else if (method == "PATCH") req.method(beast_http::verb::patch);
  else if (method == "PUT") req.method(beast_http::verb::put);
  else if (method == "DELETE") req.method(beast_http::verb::delete_);
  else req.method(beast_http::verb::get);
}

static std::string request_body_https(const std::string& method,
                                      const ParsedUrl& url,
                                      const std::string& body,
                                      int timeoutMs,
                                      const std::string& idempotencyKey,
                                      const std::map<std::string, std::string>& headers) {
  asio::io_context ioc;
  ssl::context ctx{ssl::context::tls_client};
  ctx.set_default_verify_paths();
  beast::ssl_stream<beast::tcp_stream> stream{ioc, ctx};
  if (!SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str())) {
    throw beast::system_error(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category());
  }
  stream.set_verify_mode(ssl::verify_peer);
  stream.set_verify_callback(ssl::host_name_verification(url.host));

  tcp::resolver resolver{ioc};
  auto const results = resolver.resolve(url.host, url.port);
  beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeoutMs));
  beast::get_lowest_layer(stream).connect(results);
  beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeoutMs));
  stream.handshake(ssl::stream_base::client);

  beast_http::request<beast_http::string_body> req;
  req.version(11);
  set_method(req, method);
  req.target(url.path);
  req.set(beast_http::field::host, url.host);
  req.set(beast_http::field::user_agent, "RealityEngine_CPP");
  for (const auto& [key, value] : headers) req.set(key, value);
  req.keep_alive(false);
  if (method == "POST" || method == "PATCH" || method == "PUT") {
    req.set(beast_http::field::content_type, "application/json");
    if (!idempotencyKey.empty()) req.set("Idempotency-Key", idempotencyKey);
    req.body() = body;
  }
  req.prepare_payload();
  beast_http::write(stream, req);

  beast::flat_buffer buffer;
  beast_http::response<beast_http::string_body> res;
  beast_http::read(stream, buffer, res);
  beast::error_code ec;
  stream.shutdown(ec);
  if (ec == asio::error::eof) ec = {};
  if (ec) throw beast::system_error{ec};
  return res.body();
}

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
  if (parsed.scheme == "https") return request_body_https(method, parsed, body, timeoutMs, idempotencyKey, headers);

  std::string key = parsed.host + ":" + parsed.port;
  PersistentConnection* client = nullptr;
  {
    std::lock_guard<std::mutex> lock(clientMutex);
    auto& pool = clients[key];
    if (pool.empty()) {
      pool.reserve(poolSize);
      for (size_t i = 0; i < poolSize; ++i) pool.push_back(std::make_unique<PersistentConnection>(parsed.host, parsed.port));
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
