#include "reality/http.hpp"
#include "reality/json.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <cctype>
#include <iostream>
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

static void write_response(tcp::socket& socket, const Response& res) {
  beast_http::response<beast_http::string_body> out;
  out.version(11);
  out.result(static_cast<unsigned>(res.status));
  out.set(beast_http::field::server, "RealityEngine_CPP");
  out.set(beast_http::field::content_type, res.contentType);
  out.set(beast_http::field::connection, "close");
  out.body() = res.body;
  out.prepare_payload();
  beast_http::write(socket, out);
}

static void handle_session(tcp::socket socket, const Server& server) {
  beast::flat_buffer buffer;
  beast_http::request<beast_http::string_body> request;
  try {
    beast_http::read(socket, buffer, request);
    write_response(socket, server.handle(to_request(request)));
  } catch (const std::exception& e) {
    try {
      write_response(socket, error_response(e.what(), 500));
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

  std::cout << "listening on " << port << std::endl;
  while (true) {
    tcp::socket socket{ioc};
    acceptor.accept(socket, ec);
    if (ec) continue;
    std::thread(&handle_session, std::move(socket), std::cref(*this)).detach();
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

static std::string request_body(const std::string& method, const std::string& url, const std::string& body) {
  auto [host, port, target] = parse_url(url);
  asio::io_context ioc;
  tcp::resolver resolver{ioc};
  beast::tcp_stream stream{ioc};
  auto const results = resolver.resolve(host, port);
  stream.connect(results);

  beast_http::request<beast_http::string_body> req;
  req.version(11);
  req.method(method == "POST" ? beast_http::verb::post : beast_http::verb::get);
  req.target(target);
  req.set(beast_http::field::host, host);
  req.set(beast_http::field::user_agent, "RealityEngine_CPP");
  req.set(beast_http::field::connection, "close");
  if (method == "POST") {
    req.set(beast_http::field::content_type, "application/json");
    req.body() = body;
  }
  req.prepare_payload();
  beast_http::write(stream, req);

  beast::flat_buffer buffer;
  beast_http::response<beast_http::string_body> res;
  beast_http::read(stream, buffer, res);

  beast::error_code ec;
  stream.socket().shutdown(tcp::socket::shutdown_both, ec);
  return res.body();
}

std::string post_json(const std::string& url, const std::string& body) {
  return request_body("POST", url, body);
}

std::string get(const std::string& url) {
  return request_body("GET", url, "");
}

} // namespace reality::http
