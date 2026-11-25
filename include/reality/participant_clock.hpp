#pragma once

#include "reality/http.hpp"
#include "reality/instance_clock.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace reality {

// The participant clock on a process's HTTP boundaries (RealityEngine_CI#600;
// SURFACE_SPEC.md "The instance clock"), shared by the RE and the PE so the two
// cannot drift:
//
// - Server: every request is a receive, applied before it is handled --
//   max(local, received) + 1 with an RE-Clock header, + 1 without -- and a
//   malformed or repeated header is refused with 400 (still a receive). Every
//   response, a 404 or failure included, is a send: the clock ticks, then
//   stamps RE-Clock.
// - Client: every outbound call to an internal peer is a send (tick, stamp
//   RE-Clock) and its response a receive. An external party gets no header:
//   the clock is carried across the local integration boundaries only, and an
//   instance UUID never leaves the install.
inline void install_participant_clock(http::Server& server, InstanceClock& clock) {
  server.beforeRequest = [&clock](const http::Request& req) -> std::optional<http::Response> {
    std::optional<std::string> header;
    for (const auto& [name, value] : req.headers) {
      std::string lower = name;
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (lower == "re-clock") header = value;
    }
    std::optional<long long> received;
    if (header) {
      received = InstanceClock::parse_header(*header);
      if (!received) {
        clock.receive(std::nullopt);
        return http::error_response("malformed RE-Clock header", 400);
      }
    }
    clock.receive(received);
    return std::nullopt;
  };
  server.afterResponse = [&clock](http::Response& res) {
    res.headers.emplace_back("RE-Clock", clock.header(clock.send()));
  };
  http::set_client_clock(
      [&clock]() { return clock.header(clock.send()); },
      [&clock](const std::optional<std::string>& header) {
        // A response's header is the responder's clock. One that is absent or
        // unreadable still marks a receive, as a local event.
        clock.receive(header ? InstanceClock::parse_header(*header) : std::nullopt);
      });
}

}  // namespace reality
