#pragma once

#include <functional>
#include <map>
#include <regex>
#include <string>
#include <vector>

namespace reality::http {

struct Request {
  std::string method;
  std::string path;
  std::string body;
  std::map<std::string, std::string> headers;
  std::map<std::string, std::string> pathParams;
};

struct Response {
  int status = 200;
  std::string body = "{}";
  std::string contentType = "application/json";
};

class Server {
public:
  using Handler = std::function<Response(const Request&)>;

  void route(std::string method, std::string pattern, Handler handler);
  void listen(int port);
  Response handle(Request req) const;

private:
  struct Route {
    std::string method;
    std::string pattern;
    std::regex regex;
    std::vector<std::string> params;
    Handler handler;
  };
  std::vector<Route> routes;
};

Response json_response(const std::string& body, int status = 200);
Response error_response(const std::string& message, int status = 400);
std::string post_json(const std::string& url, const std::string& body);
std::string get(const std::string& url);
void close_persistent_connections();

} // namespace reality::http
