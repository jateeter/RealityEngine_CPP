#include "reality/instance_clock.hpp"

#include "reality/reality.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>

#include <stdexcept>
#include <utility>

namespace reality {

namespace fs = std::filesystem;

bool InstanceClock::canonical_uuid(const std::string& s) {
  if (s.size() != 36) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (s[i] != '-') return false;
    } else if (!std::isxdigit(static_cast<unsigned char>(s[i]))) {
      return false;
    }
  }
  return true;
}

long long InstanceClock::read_reservation(const fs::path& file) {
  std::error_code ec;
  if (!fs::exists(file, ec)) return 0;  // the participant has never run
  std::ifstream in(file);
  if (!in) throw std::runtime_error("Lamport clock file " + file.string() + " cannot be read");
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); }),
             text.end());
  if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); }))
    throw std::runtime_error("Lamport clock file " + file.string() + " does not hold a whole number: \"" + text + "\"");
  return std::stoll(text);
}

namespace {

void write_reservation(const fs::path& file, long long value) {
  fs::create_directories(file.parent_path());
  fs::path tmp = file;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) throw std::runtime_error("Lamport clock file " + tmp.string() + " cannot be written");
    out << value << "\n";
    out.flush();
    if (!out) throw std::runtime_error("Lamport clock file " + tmp.string() + " cannot be written");
  }
  fs::rename(tmp, file);  // atomic: the old mark or the new one, never a torn one
}

int lock_instance(const fs::path& file, const std::string& uuid) {
  fs::path lock = file;
  lock.replace_extension(".lock");
  fs::create_directories(lock.parent_path());
  const int fd = ::open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) throw std::runtime_error("Instance lock " + lock.string() + " cannot be opened");
  if (::lockf(fd, F_TLOCK, 0) != 0) {
    ::close(fd);
    throw std::runtime_error("Instance " + uuid + " is already live: another process holds " +
                             lock.string() + ". Two participants may not share a UUID (RealityEngine_CI#296).");
  }
  return fd;
}

fs::path clock_dir() {
  if (const char* dir = std::getenv("INSTANCE_CLOCK_DIR"); dir && *dir) return dir;
  const char* home = std::getenv("HOME");
  return fs::path(home && *home ? home : ".") / ".reality-engine" / "clock";
}

bool lower_uuid(const std::string& s) {
  return InstanceClock::canonical_uuid(s) &&
         std::none_of(s.begin(), s.end(), [](unsigned char c) { return std::isupper(c); });
}

// A decimal integer with no sign and no leading zeros, in [0, kMaxLamport].
std::optional<long long> lamport_value(const std::string& s) {
  if (s.empty() || s.size() > 16) return std::nullopt;
  if (!std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); })) return std::nullopt;
  if (s.size() > 1 && s[0] == '0') return std::nullopt;
  const long long v = std::stoll(s);
  if (v > InstanceClock::kMaxLamport) return std::nullopt;
  return v;
}

std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(' ');
  if (b == std::string::npos) return "";
  return s.substr(b, s.find_last_not_of(' ') - b + 1);
}

}  // namespace

std::optional<long long> InstanceClock::parse_header(const std::string& value) {
  // "instance=<uuid>; lamport=<n>": exactly two parameters, in that order.
  const auto semi = value.find(';');
  if (semi == std::string::npos || value.find(';', semi + 1) != std::string::npos) return std::nullopt;
  const std::string first = trim(value.substr(0, semi));
  const std::string second = trim(value.substr(semi + 1));
  const std::string inst = "instance=", lam = "lamport=";
  if (first.rfind(inst, 0) != 0 || second.rfind(lam, 0) != 0) return std::nullopt;
  if (!lower_uuid(first.substr(inst.size()))) return std::nullopt;
  return lamport_value(second.substr(lam.size()));
}

std::string InstanceClock::header(long long lamport) const {
  return "instance=" + uuid_ + "; lamport=" + std::to_string(lamport);
}

InstanceClock InstanceClock::minted() {
  InstanceClock clock;
  clock.uuid_ = make_uuid();
  return clock;
}

InstanceClock InstanceClock::persisted(const std::string& uuid, const fs::path& file) {
  InstanceClock clock;
  clock.uuid_ = uuid;
  clock.lockFd_ = lock_instance(file, uuid);  // before anything is read or written
  fs::path commitFile = file;
  commitFile.replace_extension(".commit");
  clock.lamportFile_ = file;
  clock.commitFile_ = commitFile;
  std::error_code ec;
  const bool migrating = !fs::exists(commitFile, ec);
  clock.lamport_ = read_reservation(file);
  // A directory from before #600 holds only <uuid>.lamport, which counted
  // committed steps: it seeds both marks, which keeps lamport >= commit.
  clock.commit_ = migrating ? clock.lamport_ : read_reservation(commitFile);
  if (clock.commit_ > clock.lamport_) clock.lamport_ = clock.commit_;
  clock.reserve_commit(clock.commit_);
  clock.reserve_lamport(clock.lamport_);
  return clock;
}

InstanceClock InstanceClock::boot(const char* uuidVariable) {
  const char* raw = std::getenv(uuidVariable);
  std::string allocated = raw ? raw : "";
  if (!canonical_uuid(allocated)) {
    if (!allocated.empty())
      std::cerr << uuidVariable << " \"" << allocated << "\" is not a canonical UUID; minting one\n";
    return minted();
  }
  std::transform(allocated.begin(), allocated.end(), allocated.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return persisted(allocated, clock_dir() / (allocated + ".lamport"));
}

void InstanceClock::reserve_lamport(long long through) {
  const long long next = through + kReservation;
  write_reservation(*lamportFile_, next);
  lamportReserved_ = next;
}

void InstanceClock::reserve_commit(long long through) {
  const long long next = through + kReservation;
  write_reservation(*commitFile_, next);
  commitReserved_ = next;
}

InstanceClock::Stamp InstanceClock::commit_step() {
  std::lock_guard<std::mutex> lock(mutex_);
  const long long commit = commit_ + 1;
  const long long lamport = lamport_ + 1;
  // Persist first, then issue: a mark is never behind what was handed out.
  if (commitFile_ && commit > commitReserved_) reserve_commit(commit);
  if (lamportFile_ && lamport > lamportReserved_) reserve_lamport(lamport);
  commit_ = commit;
  lamport_ = lamport;
  return Stamp{lamport_, commit_};
}

long long InstanceClock::send() {
  std::lock_guard<std::mutex> lock(mutex_);
  const long long next = lamport_ + 1;
  if (lamportFile_ && next > lamportReserved_) reserve_lamport(next);
  lamport_ = next;
  return lamport_;
}

long long InstanceClock::receive(std::optional<long long> received) {
  std::lock_guard<std::mutex> lock(mutex_);
  const long long next = std::max(lamport_, received.value_or(0)) + 1;
  // A jump past the reserved mark is made durable before it is used: one jump
  // can exceed the reservation, and adopting it first could reissue a tick
  // after a crash.
  if (lamportFile_ && next > lamportReserved_) reserve_lamport(next);
  lamport_ = next;
  return lamport_;
}

void InstanceClock::count_header(HeaderKind kind) {
  std::lock_guard<std::mutex> lock(mutex_);
  switch (kind) {
    case HeaderKind::Received: ++headerCounts_.received; break;
    case HeaderKind::Missing: ++headerCounts_.missing; break;
    case HeaderKind::Rejected: ++headerCounts_.rejected; break;
  }
}

InstanceClock::HeaderCounts InstanceClock::header_counts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return headerCounts_;
}

long long InstanceClock::lamport() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return lamport_;
}

long long InstanceClock::commit() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return commit_;
}

InstanceClock::InstanceClock(InstanceClock&& other) noexcept
    : uuid_(std::move(other.uuid_)),
      lamport_(other.lamport_),
      commit_(other.commit_),
      lamportReserved_(other.lamportReserved_),
      commitReserved_(other.commitReserved_),
      lamportFile_(std::move(other.lamportFile_)),
      commitFile_(std::move(other.commitFile_)),
      headerCounts_(other.headerCounts_),
      lockFd_(std::exchange(other.lockFd_, -1)) {}

InstanceClock& InstanceClock::operator=(InstanceClock&& other) noexcept {
  if (this != &other) {
    if (lockFd_ >= 0) ::close(lockFd_);
    uuid_ = std::move(other.uuid_);
    lamport_ = other.lamport_;
    commit_ = other.commit_;
    lamportReserved_ = other.lamportReserved_;
    commitReserved_ = other.commitReserved_;
    lamportFile_ = std::move(other.lamportFile_);
    commitFile_ = std::move(other.commitFile_);
    headerCounts_ = other.headerCounts_;
    lockFd_ = std::exchange(other.lockFd_, -1);
  }
  return *this;
}

InstanceClock::~InstanceClock() {
  if (lockFd_ >= 0) ::close(lockFd_);
}

}  // namespace reality
