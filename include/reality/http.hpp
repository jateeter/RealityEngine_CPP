#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
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
  std::map<std::string, std::string> queryParams;
};

struct Response {
  int status = 200;
  std::string body = "{}";
  std::string contentType = "application/json";
};

class Server {
public:
  using Handler = std::function<Response(const Request&)>;
  class WebSocketConnection {
  public:
    virtual ~WebSocketConnection() = default;
    virtual void send(std::string text) = 0;
  };
  class WebSocketHub {
  public:
    void add(std::shared_ptr<WebSocketConnection> connection);
    void broadcast(const std::string& text);

  private:
    std::vector<std::weak_ptr<WebSocketConnection>> connections;
    std::mutex mutex;
  };
  using WebSocketOpenHandler = std::function<void(std::shared_ptr<WebSocketConnection>)>;

  void route(std::string method, std::string pattern, Handler handler);
  void websocket(std::string pattern, std::shared_ptr<WebSocketHub> hub, WebSocketOpenHandler on_open);
  void listen(int port);
  Response handle(Request req) const;
  bool match_websocket(Request& req, std::shared_ptr<WebSocketHub>& hub, WebSocketOpenHandler& on_open) const;

private:
  struct Route {
    std::string method;
    std::string pattern;
    std::regex regex;
    std::vector<std::string> params;
    Handler handler;
  };
  struct WebSocketRoute {
    std::string pattern;
    std::regex regex;
    std::vector<std::string> params;
    std::shared_ptr<WebSocketHub> hub;
    WebSocketOpenHandler on_open;
  };
  std::vector<Route> routes;
  std::vector<WebSocketRoute> websocketRoutes;
};

Response json_response(const std::string& body, int status = 200);
Response error_response(const std::string& message, int status = 400);
std::string post_json(const std::string& url, const std::string& body);
std::string get(const std::string& url);
void close_persistent_connections();

} // namespace reality::http
