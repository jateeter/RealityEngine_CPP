#include "reality/instance_clock.hpp"

#include "reality/reality.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
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
  if (!fs::exists(file, ec)) return 0;  // the instance has never run
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
  tmp.replace_extension(".lamport-tmp");
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
                             lock.string() + ". Two instances may not share a UUID (RealityEngine_CI#296).");
  }
  return fd;
}

fs::path clock_dir() {
  if (const char* dir = std::getenv("INSTANCE_CLOCK_DIR"); dir && *dir) return dir;
  const char* home = std::getenv("HOME");
  return fs::path(home && *home ? home : ".") / ".reality-engine" / "clock";
}

}  // namespace

InstanceClock InstanceClock::minted() {
  InstanceClock clock;
  clock.uuid_ = make_uuid();
  return clock;
}

InstanceClock InstanceClock::persisted(const std::string& uuid, const fs::path& file) {
  InstanceClock clock;
  clock.uuid_ = uuid;
  clock.lockFd_ = lock_instance(file, uuid);  // before anything is read or written
  clock.file_ = file;
  clock.lamport_ = read_reservation(file);
  clock.reserve(clock.lamport_);
  return clock;
}

InstanceClock InstanceClock::boot() {
  const char* raw = std::getenv("INSTANCE_UUID");
  std::string allocated = raw ? raw : "";
  if (!canonical_uuid(allocated)) {
    if (!allocated.empty())
      std::cerr << "INSTANCE_UUID \"" << allocated << "\" is not a canonical UUID; minting one\n";
    return minted();
  }
  std::transform(allocated.begin(), allocated.end(), allocated.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return persisted(allocated, clock_dir() / (allocated + ".lamport"));
}

void InstanceClock::reserve(long long through) {
  const long long next = through + kLamportReservation;
  write_reservation(*file_, next);
  reserved_ = next;
}

long long InstanceClock::tick() {
  const long long next = lamport_ + 1;
  if (file_ && next > reserved_) reserve(next);  // persist first, then issue
  lamport_ = next;
  return lamport_;
}

InstanceClock::InstanceClock(InstanceClock&& other) noexcept
    : uuid_(std::move(other.uuid_)),
      lamport_(other.lamport_),
      reserved_(other.reserved_),
      file_(std::move(other.file_)),
      lockFd_(std::exchange(other.lockFd_, -1)) {}

InstanceClock& InstanceClock::operator=(InstanceClock&& other) noexcept {
  if (this != &other) {
    if (lockFd_ >= 0) ::close(lockFd_);
    uuid_ = std::move(other.uuid_);
    lamport_ = other.lamport_;
    reserved_ = other.reserved_;
    file_ = std::move(other.file_);
    lockFd_ = std::exchange(other.lockFd_, -1);
  }
  return *this;
}

InstanceClock::~InstanceClock() {
  if (lockFd_ >= 0) ::close(lockFd_);
}

}  // namespace reality
