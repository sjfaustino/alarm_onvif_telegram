#include "snapshot_storage.h"
#include <cctype>
#include <cstdio>

static String sanitizeToAlnumHyphen(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-') out += c;
  }
  return out;
}

// FNV-1a, 32-bit folded to 16 - not a security hash, just cheap and good
// enough to make two different camera names very unlikely to collide once
// combined with the sanitized prefix (see this header's own comment on
// why a collision would actually matter here, unlike sanitizeHostname()).
static uint16_t fnv1aHash16(const String& s) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < s.length(); i++) {
    h ^= (uint8_t)s[i];
    h *= 16777619u;
  }
  return (uint16_t)(h ^ (h >> 16));
}

String sanitizeCameraDirName(const String& cameraName) {
  String stripped = sanitizeToAlnumHyphen(cameraName);
  char hex[6];
  snprintf(hex, sizeof(hex), "-%04x", (unsigned)fnv1aHash16(cameraName));
  return stripped + String(hex);
}

std::vector<String> filesToPrune(const std::vector<SnapshotFileInfo>& filesOldestFirst,
                                  uint64_t bytesNeeded, size_t maxFiles) {
  std::vector<String> result;
  uint64_t reclaimed = 0;
  for (size_t i = 0; i < filesOldestFirst.size() && result.size() < maxFiles && reclaimed < bytesNeeded; i++) {
    result.push_back(filesOldestFirst[i].name);
    reclaimed += filesOldestFirst[i].size;
  }
  return result;
}
