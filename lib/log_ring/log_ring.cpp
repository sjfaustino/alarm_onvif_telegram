#include "log_ring.h"
#include <cstring>

void LogRing::write(const char* data, size_t len) {
  if (cap_ == 0) return;
  // Only the last cap_ bytes can survive; positions still advance by the whole len.
  if (len > cap_) {
    end_ += (uint32_t)(len - cap_);
    data += len - cap_;
    len = cap_;
  }
  size_t at = end_ % cap_;
  size_t first = len < cap_ - at ? len : cap_ - at;
  memcpy(buf_ + at, data, first);
  memcpy(buf_, data + first, len - first);
  end_ += (uint32_t)len;
  stored_ = stored_ + len > cap_ ? cap_ : stored_ + len;
}

size_t LogRing::read(uint32_t from, char* out, size_t maxLen, uint32_t* next) const {
  if ((uint32_t)(end_ - from) > stored_) from = oldest();
  size_t avail = (size_t)(uint32_t)(end_ - from);
  size_t n = avail < maxLen ? avail : maxLen;
  for (size_t i = 0; i < n; i++) out[i] = buf_[(uint32_t)(from + i) % cap_];
  if (next) *next = from + (uint32_t)n;
  return n;
}
