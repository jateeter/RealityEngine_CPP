#pragma once

#include "reality/http.hpp"
#include "reality/instance_clock.hpp"
#include "reality/json.hpp"

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
// `headers` on a clock route: the inbound RE-Clock headers since boot.
inline json::Value clock_headers_json(const InstanceClock& clock) {
  const auto counts = clock.header_counts();
  return json::Value::Object{
    {"received", static_cast<double>(counts.received)},
    {"missing", static_cast<double>(counts.missing)},
    {"rejected", static_cast<double>(counts.rejected)}
  };
}

// `clock` / `causedBy` on a stored or forwarded message: {instance, lamport}.
inline json::Value event_clock_json(const InstanceClock& clock, long long lamport) {
  return json::Value::Object{{"instance", clock.instance()}, {"lamport", static_cast<double>(lamport)}};
}

// The client half alone: every outbound call to an internal peer a send, its
// response a receive. install_participant_clock installs it too; a participant
// that calls its peers before it serves (the PE's boot reads of its RE)
// installs it first, so no boot call goes out unclocked.
inline void install_client_clock(InstanceClock& clock) {
  http::set_client_clock(
      [&clock]() { return clock.header(clock.send()); },
      [&clock](const std::optional<std::string>& header) {
        // A response's header is the responder's clock. One that is absent or
        // unreadable still marks a receive, as a local event.
        clock.receive(header ? InstanceClock::parse_header(*header) : std::nullopt);
      });
}

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
        clock.count_header(InstanceClock::HeaderKind::Rejected);
        clock.receive(std::nullopt);
        return http::error_response("malformed RE-Clock header", 400);
      }
    }
    clock.count_header(header ? InstanceClock::HeaderKind::Received : InstanceClock::HeaderKind::Missing);
    clock.receive(received);
    return std::nullopt;
  };
  server.afterResponse = [&clock](http::Response& res) {
    res.headers.emplace_back("RE-Clock", clock.header(clock.send()));
  };
  install_client_clock(clock);
}

}  // namespace reality
