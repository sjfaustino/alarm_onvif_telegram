#pragma once
#include <cstddef>
#include <cstdint>

// Byte ring over a caller-owned buffer, read by position so a poller can ask for
// "everything since X". Positions keep counting past the capacity (and wrap
// safely at 4GB). Not thread-safe; the caller locks.
class LogRing {
 public:
  LogRing(char* buffer, size_t capacity) : buf_(buffer), cap_(capacity) {}

  void write(const char* data, size_t len);
  uint32_t end() const { return end_; }         // position after the newest byte
  uint32_t oldest() const { return end_ - stored_; }

  // Copies up to maxLen bytes from position `from` and returns the count. A
  // position no longer held (evicted, or in the future) reads from the oldest.
  // *next is the position to pass next time.
  size_t read(uint32_t from, char* out, size_t maxLen, uint32_t* next) const;

 private:
  char* buf_;
  size_t cap_;
  uint32_t end_ = 0;
  size_t stored_ = 0;
};
