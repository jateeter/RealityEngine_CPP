#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace reality {

// A participant's clock (RealityEngine_CI#296, #600; SURFACE_SPEC.md "The
// instance clock"): {instance, lamport, commit, step}.
//
// A UUID belongs to a *participant* -- an instance's RE (cpp-1) or its PE --
// never to an engine type or an image, and no two participants may share one.
// CI allocates it (INSTANCE_UUID for an RE, PE_INSTANCE_UUID for a PE); a
// participant launched without one mints a version-7 UUID at boot.
//
// - `lamport` is the full Lamport clock: it ticks on every committed step,
//   every send and every receive, and a receive is max(local, received) + 1.
// - `commit` counts committed steps: what `lamport` counted before #600.
// Neither ever resets, and lamport >= commit always holds. (`step` is the
// caller's: it restarts on reset.)
//
// For an allocated participant both hold across restarts. INSTANCE_CLOCK_DIR
// (default ~/.reality-engine/clock/) keeps two high-water marks, <uuid>.lamport
// and <uuid>.commit, each reserved kReservation ticks ahead. A boot resumes
// from the marks and reserves the next blocks before issuing any tick; a tick
// past a persisted mark -- including a receive that jumps past it -- is always
// preceded by the write of the next mark, never followed by it. Writes are
// temp + rename, so a crash leaves the old mark or the new one. A directory
// written before #600 holds only <uuid>.lamport, which counted committed
// steps; the first boot under this rule seeds both marks from it.
//
// The participant holds an exclusive lockf lock on <uuid>.lock for its whole
// life, so a second live process presenting the same UUID refuses to boot. A
// separate file, because the marks are replaced by rename and a lock on a
// replaced inode guards nothing.
//
// Thread-safe: receives and sends arrive from every HTTP thread.
class InstanceClock {
public:
  static constexpr long long kReservation = 1024;
  // The largest lamport an RE-Clock header may carry: 2^53 - 1, the largest
  // integer every runtime's JSON number represents exactly.
  static constexpr long long kMaxLamport = 9007199254740991LL;

  // The participant's clock for this boot, from `uuidVariable` (INSTANCE_UUID
  // for an RE, PE_INSTANCE_UUID for a PE) and INSTANCE_CLOCK_DIR. Throws
  // std::runtime_error when an allocated participant cannot keep its clock or
  // another live process holds its UUID: either way ticks could be issued
  // twice, which is the one thing the clock must not do.
  static InstanceClock boot(const char* uuidVariable = "INSTANCE_UUID");
  // A minted, unpersisted clock -- what a participant without an allocation runs.
  static InstanceClock minted();
  // An allocated clock whose lamport mark is `file` (<dir>/<uuid>.lamport) and
  // commit mark the same path with extension .commit (tests; boot() uses it).
  static InstanceClock persisted(const std::string& uuid, const std::filesystem::path& file);

  InstanceClock(InstanceClock&& other) noexcept;
  InstanceClock& operator=(InstanceClock&& other) noexcept;
  InstanceClock(const InstanceClock&) = delete;
  InstanceClock& operator=(const InstanceClock&) = delete;
  ~InstanceClock();

  struct Stamp {
    long long lamport = 0;
    long long commit = 0;
  };

  // A committed step: ticks commit and lamport together; returns both.
  Stamp commit_step();
  // A send (a response, an outbound call): ticks lamport; returns it.
  long long send();
  // A receive, applied before the request is handled: max(local, received) + 1,
  // or local + 1 when no header was presented; returns the new lamport.
  long long receive(std::optional<long long> received);

  long long lamport() const;
  long long commit() const;
  const std::string& instance() const { return uuid_; }

  // The RE-Clock header for `lamport`: "instance=<uuid>; lamport=<n>".
  std::string header(long long lamport) const;

  // An RE-Clock header value, parsed: the sender's lamport, or nullopt when the
  // value is malformed (SURFACE_SPEC "The header": a canonical lower-case UUID
  // and an integer in [0, 2^53 - 1] with no sign or leading zeros, each once,
  // in that order, separated by `;` with optional spaces).
  static std::optional<long long> parse_header(const std::string& value);

  static bool canonical_uuid(const std::string& s);
  static long long read_reservation(const std::filesystem::path& file);

private:
  InstanceClock() = default;
  void reserve_lamport(long long through);
  void reserve_commit(long long through);

  std::string uuid_;
  long long lamport_ = 0;
  long long commit_ = 0;
  long long lamportReserved_ = 0;
  long long commitReserved_ = 0;
  std::optional<std::filesystem::path> lamportFile_;
  std::optional<std::filesystem::path> commitFile_;
  int lockFd_ = -1;
  mutable std::mutex mutex_;
};

}  // namespace reality
