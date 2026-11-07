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

class SseSession;

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

  class SseHub {
  public:
    void broadcast(const std::string& text);
  private:
    friend class ::reality::http::SseSession;
    void add(std::weak_ptr<SseSession> session);
    std::vector<std::weak_ptr<SseSession>> sessions;
    std::mutex mutex;
  };

  void route(std::string method, std::string pattern, Handler handler);
  // A route a client may reach without a client certificate when the server
  // serves TLS (RealityEngine_CI#668, docs/TRANSPORT_SECURITY.md): the PE's
  // device ingest, which a phone reaches with server TLS and a token. With
  // none registered, the handshake itself requires a client certificate.
  void allow_without_client_certificate(std::string pattern);
  bool requires_client_certificate(const std::string& path) const;
  bool has_certificate_optional_routes() const { return !certificateOptional.empty(); }
  void websocket(std::string pattern, std::shared_ptr<WebSocketHub> hub, WebSocketOpenHandler on_open);
  void sse(std::string pattern, std::shared_ptr<SseHub> hub);
  void listen(int port);
  Response handle(Request req) const;
  bool match_websocket(Request& req, std::shared_ptr<WebSocketHub>& hub, WebSocketOpenHandler& on_open) const;
  bool match_sse(Request& req, std::shared_ptr<SseHub>& hub) const;

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
  struct SseRoute {
    std::string pattern;
    std::regex regex;
    std::vector<std::string> params;
    std::shared_ptr<SseHub> hub;
  };
  std::vector<Route> routes;
  std::vector<WebSocketRoute> websocketRoutes;
  std::vector<SseRoute> sseRoutes;
  std::vector<std::regex> certificateOptional;
};

// Lane TLS (RealityEngine_CI#668, the runtime contract in its
// docs/TRANSPORT_SECURITY.md). Read from RE_TLS_CERT, RE_TLS_KEY, RE_TLS_CA and
// RE_TLS_CRL. When RE_TLS_CERT is set, Server::listen serves TLS 1.3 only on the
// same port, with no plaintext listener, and outbound https to an internal peer
// verifies against RE_TLS_CA alone and presents this certificate.
struct TlsSettings {
  std::string cert, key, ca, crl;
  bool serving() const { return !cert.empty(); }
  bool lane() const { return !ca.empty(); }
};
TlsSettings tls_settings_from_env();
// Internal peers trust the lane CA only; anything else is an external party,
// verified against the system store for that link only.
bool is_internal_host(const std::string& host);

Response json_response(const std::string& body, int status = 200);
Response error_response(const std::string& message, int status = 400);
std::string request_json(const std::string& method, const std::string& url, const std::string& body = "");
std::string request_json(const std::string& method, const std::string& url, const std::string& body, const std::map<std::string, std::string>& headers);
std::string post_json(const std::string& url, const std::string& body);
std::string get(const std::string& url);
void close_persistent_connections();

} // namespace reality::http
