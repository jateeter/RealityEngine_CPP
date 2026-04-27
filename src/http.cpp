#include "reality/http.hpp"
#include "reality/json.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace reality::http {

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

static std::string reason(int status) {
  if (status == 200) return "OK";
  if (status == 201) return "Created";
  if (status == 400) return "Bad Request";
  if (status == 404) return "Not Found";
  if (status == 500) return "Internal Server Error";
  if (status == 502) return "Bad Gateway";
  return "OK";
}

static void send_response(int fd, const Response& res) {
  std::ostringstream out;
  out << "HTTP/1.1 " << res.status << " " << reason(res.status) << "\r\n";
  out << "Content-Type: " << res.contentType << "\r\n";
  out << "Content-Length: " << res.body.size() << "\r\n";
  out << "Connection: close\r\n\r\n";
  out << res.body;
  std::string raw = out.str();
  ::send(fd, raw.data(), raw.size(), 0);
}

static Request read_request(int fd) {
  std::string raw;
  char buf[4096];
  while (raw.find("\r\n\r\n") == std::string::npos) {
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    raw.append(buf, static_cast<size_t>(n));
    if (raw.size() > 1024 * 1024) break;
  }
  auto headerEnd = raw.find("\r\n\r\n");
  std::string head = headerEnd == std::string::npos ? raw : raw.substr(0, headerEnd);
  std::istringstream hs(head);
  Request req;
  hs >> req.method >> req.path;
  std::string line;
  std::getline(hs, line);
  int contentLength = 0;
  while (std::getline(hs, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    req.headers[key] = value;
    if (key == "Content-Length" || key == "content-length") contentLength = std::stoi(value);
  }
  size_t bodyStart = headerEnd == std::string::npos ? raw.size() : headerEnd + 4;
  req.body = raw.substr(bodyStart);
  while (static_cast<int>(req.body.size()) < contentLength) {
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    req.body.append(buf, static_cast<size_t>(n));
  }
  auto q = req.path.find('?');
  if (q != std::string::npos) req.path = req.path.substr(0, q);
  return req;
}

void Server::listen(int port) {
  int serverFd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (serverFd < 0) throw std::runtime_error("socket failed");
  int yes = 1;
  setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::bind(serverFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) throw std::runtime_error("bind failed");
  if (::listen(serverFd, 64) < 0) throw std::runtime_error("listen failed");
  std::cout << "listening on " << port << std::endl;
  while (true) {
    int fd = ::accept(serverFd, nullptr, nullptr);
    if (fd < 0) continue;
    try {
      Request req = read_request(fd);
      Response res = error_response("Not found", 404);
      for (const auto& r : routes) {
        if (r.method != req.method) continue;
        std::smatch m;
        if (std::regex_match(req.path, m, r.regex)) {
          for (size_t i = 0; i < r.params.size(); ++i) req.pathParams[r.params[i]] = m[i + 1].str();
          res = r.handler(req);
          break;
        }
      }
      send_response(fd, res);
    } catch (const std::exception& e) {
      send_response(fd, error_response(e.what(), 500));
    }
    ::close(fd);
  }
}

Response json_response(const std::string& body, int status) { return {status, body, "application/json"}; }

Response error_response(const std::string& message, int status) {
  return json_response(json::stringify(json::Value::Object{{"error", message}}), status);
}

static std::tuple<std::string, int, std::string> parse_url(const std::string& url) {
  std::string u = url;
  const std::string prefix = "http://";
  if (u.rfind(prefix, 0) == 0) u = u.substr(prefix.size());
  auto slash = u.find('/');
  std::string hostPort = slash == std::string::npos ? u : u.substr(0, slash);
  std::string path = slash == std::string::npos ? "/" : u.substr(slash);
  int port = 80;
  auto colon = hostPort.find(':');
  std::string host = hostPort;
  if (colon != std::string::npos) {
    host = hostPort.substr(0, colon);
    port = std::stoi(hostPort.substr(colon + 1));
  }
  return {host, port, path};
}

std::string post_json(const std::string& url, const std::string& body) {
  auto [host, port, path] = parse_url(url);
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) throw std::runtime_error("getaddrinfo failed");
  int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) throw std::runtime_error("socket failed");
  if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
    freeaddrinfo(res);
    ::close(fd);
    throw std::runtime_error("connect failed");
  }
  freeaddrinfo(res);
  std::ostringstream req;
  req << "POST " << path << " HTTP/1.1\r\nHost: " << host << "\r\nContent-Type: application/json\r\nContent-Length: " << body.size() << "\r\nConnection: close\r\n\r\n" << body;
  std::string rawReq = req.str();
  ::send(fd, rawReq.data(), rawReq.size(), 0);
  std::string raw;
  char buf[4096];
  ssize_t n;
  while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) raw.append(buf, static_cast<size_t>(n));
  ::close(fd);
  auto split = raw.find("\r\n\r\n");
  return split == std::string::npos ? raw : raw.substr(split + 4);
}

std::string get(const std::string& url) {
  auto [host, port, path] = parse_url(url);
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) throw std::runtime_error("getaddrinfo failed");
  int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) throw std::runtime_error("socket failed");
  if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
    freeaddrinfo(res);
    ::close(fd);
    throw std::runtime_error("connect failed");
  }
  freeaddrinfo(res);
  std::ostringstream req;
  req << "GET " << path << " HTTP/1.1\r\nHost: " << host << "\r\nConnection: close\r\n\r\n";
  std::string rawReq = req.str();
  ::send(fd, rawReq.data(), rawReq.size(), 0);
  std::string raw;
  char buf[4096];
  ssize_t n;
  while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) raw.append(buf, static_cast<size_t>(n));
  ::close(fd);
  auto split = raw.find("\r\n\r\n");
  return split == std::string::npos ? raw : raw.substr(split + 4);
}

} // namespace reality::http
