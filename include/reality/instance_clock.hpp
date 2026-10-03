#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace reality {

// The instance's Lamport clock (RealityEngine_CI#296): {instance, lamport, step}.
//
// A UUID belongs to an *instance* -- cpp-1, lsp-2 -- never to an engine type or
// an image, and no two instances of any engine type may share one. The instance
// registry allocates it and passes it as INSTANCE_UUID; an instance launched
// without one mints a version-7 UUID at boot, which is unique by construction.
//
// `lamport` ticks once per committed step and is never reset, so
// (instance, lamport) names one step uniquely. For an allocated instance that
// holds across restarts too: INSTANCE_CLOCK_DIR (default
// ~/.reality-engine/clock/) keeps <uuid>.lamport, a high-water mark reserved
// kLamportReservation ticks ahead. A boot resumes from the mark and reserves the
// next block before issuing any tick, and a tick past the persisted mark is
// always preceded by the write of the next block, never followed by it. The
// write is temp + rename, so a crash leaves the old mark or the new one.
//
// The instance also holds an exclusive lockf lock on <uuid>.lock for its whole
// life, so a second live process presenting the same UUID refuses to boot. The
// kernel releases it when the process exits; a crash leaves no stale lock. A
// separate file, because the clock file is replaced by rename and a lock on a
// replaced inode guards nothing.
//
// Not thread-safe on its own: the caller ticks under the step lock.
class InstanceClock {
public:
  static constexpr long long kLamportReservation = 1024;

  // The clock for this boot, from INSTANCE_UUID / INSTANCE_CLOCK_DIR. Throws
  // std::runtime_error when an allocated instance cannot keep its clock or
  // another live process holds its UUID: either way ticks could be issued
  // twice, which is the one thing the clock must not do.
  static InstanceClock boot();
  // A minted, unpersisted clock -- what an instance without an allocation runs.
  static InstanceClock minted();
  // An allocated clock kept in `file` (tests; boot() uses it).
  static InstanceClock persisted(const std::string& uuid, const std::filesystem::path& file);

  InstanceClock(InstanceClock&& other) noexcept;
  InstanceClock& operator=(InstanceClock&& other) noexcept;
  InstanceClock(const InstanceClock&) = delete;
  InstanceClock& operator=(const InstanceClock&) = delete;
  ~InstanceClock();

  // Advance for a committed step; returns the new value.
  long long tick();
  long long lamport() const { return lamport_; }
  const std::string& instance() const { return uuid_; }

  static bool canonical_uuid(const std::string& s);
  static long long read_reservation(const std::filesystem::path& file);

private:
  InstanceClock() = default;
  void reserve(long long through);

  std::string uuid_;
  long long lamport_ = 0;
  long long reserved_ = 0;
  std::optional<std::filesystem::path> file_;
  int lockFd_ = -1;
};

}  // namespace reality
