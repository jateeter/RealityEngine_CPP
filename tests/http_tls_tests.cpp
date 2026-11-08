// Lane TLS tests for reality::http (RealityEngine_CI#668, RealityEngine_CPP#193).
//
// The runtime contract (RealityEngine_CI docs/TRANSPORT_SECURITY.md): with
// RE_TLS_CERT/KEY/CA/CRL set the engine serves TLS 1.3 only on its own port,
// requires a client certificate from the lane CA (except on routes registered
// with allow_without_client_certificate), checks the CRL, and calls internal
// peers over https with the lane CA and its own certificate, on pooled
// connections.
//
// Everything is local: a throwaway CA, leaves and CRL made with the openssl CLI
// in a temporary directory, two servers on loopback ports, and s_client for the
// refusals. Nothing reads ~/.reality-engine.

#include "reality/http.hpp"

#include <arpa/inet.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
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
int skips = 0;

#define EXPECT(cond, label)                                              \
  do { if (!(cond)) {                                                    \
    std::cerr << "FAIL: " << label << " (" << __FILE__ << ":"           \
              << __LINE__ << ")\n";                                      \
    ++failures;                                                          \
  } else { std::cout << "  ok   " << label << "\n"; } } while (0)

std::string openssl_bin() {
  if (const char* explicitBin = std::getenv("OPENSSL_BIN")) return explicitBin;
  for (const char* candidate : {"/opt/homebrew/opt/openssl@3/bin/openssl", "/usr/local/opt/openssl@3/bin/openssl"})
    if (::access(candidate, X_OK) == 0) return candidate;
  return "openssl";
}

std::string run(const std::string& cmd) {
  std::string out;
  std::array<char, 4096> buf{};
  FILE* pipe = ::popen(("( " + cmd + " ) 2>&1").c_str(), "r");
  if (!pipe) return out;
  while (std::fgets(buf.data(), buf.size(), pipe)) out += buf.data();
  ::pclose(pipe);
  return out;
}

bool sh(const std::string& cmd) { return std::system((cmd + " >/dev/null 2>&1").c_str()) == 0; }

int free_port() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

void wait_for(int port) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    bool up = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    if (up) return;
    std::this_thread::sleep_for(20ms);
  }
}

// A plaintext request on the raw socket: the TLS port must not answer HTTP.
std::string plaintext_get(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  timeval tv{2, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  std::string got;
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
    const std::string req = "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ::send(fd, req.data(), req.size(), 0);
    char buf[512];
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n > 0) got.assign(buf, static_cast<size_t>(n));
  }
  ::close(fd);
  return got;
}

struct Pki {
  std::string dir, ca, crl, leafChain, leafKey, revokedChain, revokedKey, rogueChain, rogueKey;
};

void write_file(const std::string& path, const std::string& text) { std::ofstream(path) << text; }

bool concat(const std::string& a, const std::string& b, const std::string& out) {
  std::ifstream ia(a), ib(b);
  std::ofstream o(out);
  o << ia.rdbuf() << ib.rdbuf();
  return ia.good() && o.good();
}

// A CA in the profile's shape (ECDSA P-384), one leaf for this "instance"
// (serverAuth and clientAuth, localhost and 127.0.0.1), one revoked client, and
// a client from a different CA.
Pki make_pki() {
  Pki p;
  char tmpl[] = "/tmp/re-cpp-tls-XXXXXX";
  p.dir = ::mkdtemp(tmpl);
  const std::string o = openssl_bin(), d = p.dir;
  write_file(d + "/ca.cnf",
             "[ca]\ndefault_ca=ca_default\n[ca_default]\ndir=" + d + "\ndatabase=$dir/index.txt\n"
             "crlnumber=$dir/crlnumber\ndefault_md=sha384\ndefault_crl_days=2\n");
  write_file(d + "/index.txt", "");
  write_file(d + "/crlnumber", "01\n");
  write_file(d + "/leaf.ext", "subjectAltName=DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth,clientAuth\n"
                              "keyUsage=critical,digitalSignature\nbasicConstraints=critical,CA:FALSE\n");
  write_file(d + "/client.ext", "extendedKeyUsage=clientAuth\nkeyUsage=critical,digitalSignature\nbasicConstraints=critical,CA:FALSE\n");
  auto ca = [&](const std::string& name) {
    return sh(o + " req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-384 -nodes -sha384 -days 2 -subj /CN=" + name +
              " -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign,cRLSign" +
              " -keyout " + d + "/" + name + ".key -out " + d + "/" + name + ".crt");
  };
  auto leaf = [&](const std::string& name, const std::string& issuer, const std::string& ext) {
    return sh(o + " req -newkey ec -pkeyopt ec_paramgen_curve:P-384 -nodes -subj /CN=" + name + " -keyout " + d + "/" + name +
              ".key -out " + d + "/" + name + ".csr") &&
           sh(o + " x509 -req -sha384 -days 2 -in " + d + "/" + name + ".csr -CA " + d + "/" + issuer + ".crt -CAkey " + d + "/" +
              issuer + ".key -CAcreateserial -extfile " + d + "/" + ext + " -out " + d + "/" + name + ".crt") &&
           concat(d + "/" + name + ".crt", d + "/" + issuer + ".crt", d + "/" + name + ".chain");
  };
  bool ok = ca("ca") && ca("rogue-ca") && leaf("cpp-1", "ca", "leaf.ext") && leaf("revoked", "ca", "client.ext") &&
            leaf("rogue", "rogue-ca", "client.ext") &&
            sh(o + " ca -config " + d + "/ca.cnf -keyfile " + d + "/ca.key -cert " + d + "/ca.crt -revoke " + d + "/revoked.crt") &&
            sh(o + " ca -config " + d + "/ca.cnf -keyfile " + d + "/ca.key -cert " + d + "/ca.crt -gencrl -out " + d + "/ca.crl");
  if (!ok) {
    std::cerr << "could not build the test PKI with " << o << " in " << d << "\n";
    std::_Exit(2);
  }
  p.ca = d + "/ca.crt";
  p.crl = d + "/ca.crl";
  p.leafChain = d + "/cpp-1.chain";
  p.leafKey = d + "/cpp-1.key";
  p.revokedChain = d + "/revoked.chain";
  p.revokedKey = d + "/revoked.key";
  p.rogueChain = d + "/rogue.chain";
  p.rogueKey = d + "/rogue.key";
  return p;
}

// One request through s_client. `flags` adds or removes TLS options; the
// request is held open for `holdSeconds` so a streamed answer can arrive.
std::string s_client(const Pki& p, int port, const std::string& path, const std::string& flags, int holdSeconds = 1) {
  std::string req = "GET " + path + " HTTP/1.1\\r\\nHost: localhost\\r\\nConnection: close\\r\\n\\r\\n";
  // Bounded: s_client waits for the peer's close, which an event stream never
  // sends. perl's alarm is the portable timeout (macOS has no timeout(1)).
  return run("( printf '" + req + "'; sleep " + std::to_string(holdSeconds) + " ) | perl -e 'alarm shift; exec @ARGV' " +
             std::to_string(holdSeconds + 4) + " " + openssl_bin() +
             " s_client -quiet -no_ign_eof -connect 127.0.0.1:" + std::to_string(port) + " -servername localhost -CAfile " + p.ca + " " + flags);
}

bool answered(const std::string& out, const std::string& status) { return out.find("HTTP/1.1 " + status) != std::string::npos; }

} // namespace

int main() {
  ::setenv("HTTP_WORKERS", "2", 1);
  ::setenv("SSE_HEARTBEAT_MS", "100", 1);
  Pki p = make_pki();
  ::setenv("RE_TLS_CERT", p.leafChain.c_str(), 1);
  ::setenv("RE_TLS_KEY", p.leafKey.c_str(), 1);
  ::setenv("RE_TLS_CA", p.ca.c_str(), 1);
  ::setenv("RE_TLS_CRL", p.crl.c_str(), 1);
  const std::string own = "-cert " + p.leafChain + " -key " + p.leafKey;
  std::cout << "http tls (openssl " << openssl_bin() << ")\n";

  // An RE-like server: every route requires a client certificate.
  auto re = std::make_shared<http::Server>();
  re->route("GET", "/health", [](const http::Request&) { return http::json_response("{\"status\":\"ok\"}"); });
  auto sseHub = std::make_shared<http::Server::SseHub>();
  re->sse("/stream", sseHub);
  const int rePort = free_port();
  std::thread([re, rePort]() { re->listen(rePort); }).detach();

  // A PE-like server: one device route open without a client certificate.
  auto pe = std::make_shared<http::Server>();
  pe->allow_without_client_certificate("/device");
  pe->route("GET", "/device", [](const http::Request&) { return http::json_response("{\"device\":true}"); });
  pe->route("GET", "/service", [](const http::Request&) { return http::json_response("{\"service\":true}"); });
  const int pePort = free_port();
  std::thread([pe, pePort]() { pe->listen(pePort); }).detach();
  wait_for(rePort);
  wait_for(pePort);

  // --- calling: the engine's own client --------------------------------------
  const std::string base = "https://127.0.0.1:" + std::to_string(rePort);
  std::string body;
  try { body = http::get(base + "/health"); } catch (const std::exception& e) { body = std::string("threw: ") + e.what(); }
  EXPECT(body == "{\"status\":\"ok\"}", "client: https to an IP SAN with the lane CA and its own certificate (" + body + ")");
  try { body = http::get("https://localhost:" + std::to_string(rePort) + "/health"); } catch (const std::exception& e) { body = e.what(); }
  EXPECT(body == "{\"status\":\"ok\"}", "client: https to a DNS SAN");
  int good = 0;
  for (int i = 0; i < 25; ++i) {
    try { if (http::get(base + "/health") == "{\"status\":\"ok\"}") ++good; } catch (...) {}
  }
  EXPECT(good == 25, "client: 25 calls on pooled TLS connections (" + std::to_string(good) + "/25)");
  EXPECT(http::is_internal_host("127.0.0.1") && http::is_internal_host("engine-cpp-re") && http::is_internal_host("cpp-1.localhost") &&
             http::is_internal_host("192.168.1.20") && http::is_internal_host("host.docker.internal") &&
             !http::is_internal_host("api.openai.com") && !http::is_internal_host("8.8.8.8"),
         "client: internal peers are loopback, private addresses and local names; others are external");

  // --- serving: what the port accepts -----------------------------------------
  EXPECT(answered(s_client(p, rePort, "/health", own), "200"), "serve: a lane client certificate over TLS 1.3 is served");
  EXPECT(!answered(s_client(p, rePort, "/health", ""), "200"), "serve: no client certificate is refused at the handshake");
  EXPECT(!answered(s_client(p, rePort, "/health", own + " -tls1_2"), "200"), "serve: TLS 1.2 is refused");
  EXPECT(!answered(s_client(p, rePort, "/health", own + " -tls1_3 -ciphersuites TLS_AES_128_GCM_SHA256"), "200"),
         "serve: a suite outside the profile (AES-128-GCM) is refused");
  EXPECT(!answered(s_client(p, rePort, "/health", "-cert " + p.rogueChain + " -key " + p.rogueKey), "200"),
         "serve: a certificate from another CA is refused");
  EXPECT(!answered(s_client(p, rePort, "/health", "-cert " + p.revokedChain + " -key " + p.revokedKey), "200"),
         "serve: a revoked certificate is refused (RE_TLS_CRL)");
  EXPECT(plaintext_get(rePort).rfind("HTTP/", 0) != 0, "serve: plaintext on the TLS port gets no HTTP answer");
  std::string suite = s_client(p, rePort, "/health", own + " -brief");
  EXPECT(suite.find("TLS_AES_256_GCM_SHA384") != std::string::npos || suite.find("TLS_CHACHA20_POLY1305_SHA256") != std::string::npos,
         "serve: the negotiated suite is in the profile");
  if (run(openssl_bin() + " list -tls-groups").find("X25519MLKEM768") != std::string::npos) {
    std::string grp = s_client(p, rePort, "/health", own + " -brief -groups X25519MLKEM768");
    EXPECT(grp.find("X25519MLKEM768") != std::string::npos && answered(grp, "200"), "serve: X25519MLKEM768 is negotiated");
  } else {
    ++skips;
    std::cout << "  skip X25519MLKEM768: this OpenSSL has no post-quantum hybrid group\n";
  }
  // Session resumption (TLS 1.3 tickets): a client that caches its session,
  // as Node's https agent does, must be served on its next connection too.
  const std::string sess = p.dir + "/session.pem";
  s_client(p, rePort, "/health", own + " -sess_out " + sess);
  std::string resumed = s_client(p, rePort, "/health", own + " -sess_in " + sess);
  std::string reuse = run("echo | perl -e 'alarm shift; exec @ARGV' 5 " + openssl_bin() + " s_client -connect 127.0.0.1:" +
                          std::to_string(rePort) + " -servername localhost -CAfile " + p.ca + " " + own + " -sess_in " + sess);
  EXPECT(answered(resumed, "200") && reuse.find("Reused") != std::string::npos,
         "serve: a resumed TLS 1.3 session with a client certificate is served");
  std::string sse = s_client(p, rePort, "/stream", own, 1);
  EXPECT(sse.find("text/event-stream") != std::string::npos && sse.find(": connected") != std::string::npos,
         "serve: an event stream over TLS");

  // --- serving: a device route ------------------------------------------------
  EXPECT(answered(s_client(p, pePort, "/device", ""), "200"), "device route: served with server TLS and no client certificate");
  EXPECT(answered(s_client(p, pePort, "/service", ""), "403"), "device route: any other route without a certificate gets 403");
  EXPECT(answered(s_client(p, pePort, "/service", own), "200"), "device route: the other routes still serve a lane client");
  EXPECT(!answered(s_client(p, pePort, "/device", "-cert " + p.rogueChain + " -key " + p.rogueKey), "200"),
         "device route: a presented untrusted certificate is still refused");

  if (failures) std::cerr << failures << " http tls test(s) failed\n";
  else std::cout << "http tls tests passed" << (skips ? " (" + std::to_string(skips) + " skipped)" : std::string()) << "\n";
  sh("rm -rf " + p.dir);
  std::cout.flush();
  std::_Exit(failures ? 1 : 0);
}
