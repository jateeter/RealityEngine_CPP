#include "reality/http.hpp"
#include "reality/json.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace reality::http {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace beast_http = boost::beast::http;
using tcp = asio::ip::tcp;

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

static Request to_request(const beast_http::request<beast_http::string_body>& in) {
  Request req;
  req.method = std::string(in.method_string());
  req.path = std::string(in.target());
  req.body = in.body();
  for (const auto& field : in) req.headers.emplace(std::string(field.name_string()), std::string(field.value()));
  auto q = req.path.find('?');
  if (q != std::string::npos) req.path = req.path.substr(0, q);
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

static void write_response(tcp::socket& socket, const Response& res, bool keepAlive) {
  beast_http::response<beast_http::string_body> out;
  out.version(11);
  out.result(static_cast<unsigned>(res.status));
  out.set(beast_http::field::server, "RealityEngine_CPP");
  out.set(beast_http::field::content_type, res.contentType);
  out.body() = res.body;
  out.keep_alive(keepAlive);
  out.prepare_payload();
  beast_http::write(socket, out);
}

static void handle_session(tcp::socket socket, const Server& server) {
  beast::flat_buffer buffer;
  try {
    while (true) {
      beast_http::request<beast_http::string_body> request;
      beast_http::read(socket, buffer, request);
      bool keepAlive = request.keep_alive();
      write_response(socket, server.handle(to_request(request)), keepAlive);
      if (!keepAlive) break;
    }
  } catch (const std::exception& e) {
    try {
      write_response(socket, error_response(e.what(), 500), false);
    } catch (...) {
    }
  }

  beast::error_code ec;
  socket.shutdown(tcp::socket::shutdown_send, ec);
}

void Server::listen(int port) {
  asio::io_context ioc{1};
  tcp::acceptor acceptor{ioc};
  beast::error_code ec;
  tcp::endpoint endpoint{tcp::v4(), static_cast<unsigned short>(port)};

  acceptor.open(endpoint.protocol(), ec);
  if (ec) throw std::runtime_error("acceptor open failed: " + ec.message());
  acceptor.set_option(asio::socket_base::reuse_address(true), ec);
  if (ec) throw std::runtime_error("acceptor option failed: " + ec.message());
  acceptor.bind(endpoint, ec);
  if (ec) throw std::runtime_error("bind failed: " + ec.message());
  acceptor.listen(asio::socket_base::max_listen_connections, ec);
  if (ec) throw std::runtime_error("listen failed: " + ec.message());

  const char* workersEnv = std::getenv("HTTP_WORKERS");
  const char* backlogEnv = std::getenv("HTTP_QUEUE_CAPACITY");
  size_t workerCount = workersEnv ? static_cast<size_t>(std::max(1, std::atoi(workersEnv))) : static_cast<size_t>(std::max(2u, std::thread::hardware_concurrency()));
  size_t queueCapacity = backlogEnv ? static_cast<size_t>(std::max(1, std::atoi(backlogEnv))) : workerCount * 64;
  std::mutex queueMutex;
  std::condition_variable queueCondition;
  std::deque<tcp::socket> queue;

  std::vector<std::thread> workers;
  workers.reserve(workerCount);
  for (size_t i = 0; i < workerCount; ++i) {
    workers.emplace_back([&]() {
      while (true) {
        tcp::socket socket{ioc};
        {
          std::unique_lock<std::mutex> lock(queueMutex);
          queueCondition.wait(lock, [&]() { return !queue.empty(); });
          socket = std::move(queue.front());
          queue.pop_front();
        }
        handle_session(std::move(socket), *this);
      }
    });
  }

  std::cout << "listening on " << port << std::endl;
  while (true) {
    tcp::socket socket{ioc};
    acceptor.accept(socket, ec);
    if (ec) continue;
    {
      std::lock_guard<std::mutex> lock(queueMutex);
      if (queue.size() >= queueCapacity) {
        socket.close(ec);
        continue;
      }
      queue.push_back(std::move(socket));
    }
    queueCondition.notify_one();
  }
}

Response json_response(const std::string& body, int status) { return {status, body, "application/json"}; }

Response error_response(const std::string& message, int status) {
  return json_response(json::stringify(json::Value::Object{{"error", message}}), status);
}

static std::tuple<std::string, std::string, std::string> parse_url(const std::string& url) {
  std::string u = url;
  const std::string prefix = "http://";
  if (u.rfind(prefix, 0) == 0) u = u.substr(prefix.size());
  auto slash = u.find('/');
  std::string hostPort = slash == std::string::npos ? u : u.substr(0, slash);
  std::string path = slash == std::string::npos ? "/" : u.substr(slash);
  std::string port = "80";
  auto colon = hostPort.find(':');
  std::string host = hostPort;
  if (colon != std::string::npos) {
    host = hostPort.substr(0, colon);
    port = hostPort.substr(colon + 1);
  }
  return {host, port, path};
}

class PersistentConnection {
public:
  PersistentConnection(std::string host, std::string port)
      : host(std::move(host)), port(std::move(port)), stream(std::make_unique<beast::tcp_stream>(ioc)) {}

  std::string request(const std::string& method, const std::string& target, const std::string& body) {
    std::lock_guard<std::mutex> lock(mutex);
    try {
      return request_once(method, target, body);
    } catch (...) {
      reconnect();
      return request_once(method, target, body);
    }
  }

  void close() {
    std::lock_guard<std::mutex> lock(mutex);
    close_unlocked();
  }

private:
  std::string request_once(const std::string& method, const std::string& target, const std::string& body) {
    connect_if_needed();

    beast_http::request<beast_http::string_body> req;
    req.version(11);
    req.method(method == "POST" ? beast_http::verb::post : beast_http::verb::get);
    req.target(target);
    req.set(beast_http::field::host, host);
    req.set(beast_http::field::user_agent, "RealityEngine_CPP");
    req.keep_alive(true);
    if (method == "POST") {
      req.set(beast_http::field::content_type, "application/json");
      req.body() = body;
    }
    req.prepare_payload();
    beast_http::write(*stream, req);

    beast::flat_buffer buffer;
    beast_http::response<beast_http::string_body> res;
    beast_http::read(*stream, buffer, res);
    if (!res.keep_alive()) connected = false;
    return res.body();
  }

  void connect_if_needed() {
    if (connected && stream->socket().is_open()) return;
    tcp::resolver resolver{ioc};
    auto const results = resolver.resolve(host, port);
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
static std::map<std::string, std::unique_ptr<PersistentConnection>> clients;

static std::string request_body(const std::string& method, const std::string& url, const std::string& body) {
  auto [host, port, target] = parse_url(url);
  std::string key = host + ":" + port;
  PersistentConnection* client = nullptr;
  {
    std::lock_guard<std::mutex> lock(clientMutex);
    auto& ptr = clients[key];
    if (!ptr) ptr = std::make_unique<PersistentConnection>(host, port);
    client = ptr.get();
  }
  return client->request(method, target, body);
}

std::string post_json(const std::string& url, const std::string& body) {
  return request_body("POST", url, body);
}

std::string get(const std::string& url) {
  return request_body("GET", url, "");
}

void close_persistent_connections() {
  std::lock_guard<std::mutex> lock(clientMutex);
  for (auto& [_, client] : clients) client->close();
  clients.clear();
}

} // namespace reality::http
