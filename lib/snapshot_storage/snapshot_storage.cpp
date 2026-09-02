#include "snapshot_storage.h"
#include <cctype>
#include <cstdio>
#include <ctime>

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

time_t parseSnapshotTimestamp(const String& filename) {
  if (filename.length() < 15) return (time_t)-1;
  for (int i = 0; i < 8; i++) {
    if (!isdigit((unsigned char)filename[i])) return (time_t)-1;
  }
  if (filename[8] != '-') return (time_t)-1;
  for (int i = 9; i < 15; i++) {
    if (!isdigit((unsigned char)filename[i])) return (time_t)-1;
  }

  struct tm tmStruct = {};
  tmStruct.tm_year = filename.substring(0, 4).toInt() - 1900;
  tmStruct.tm_mon  = filename.substring(4, 6).toInt() - 1;
  tmStruct.tm_mday = filename.substring(6, 8).toInt();
  tmStruct.tm_hour = filename.substring(9, 11).toInt();
  tmStruct.tm_min  = filename.substring(11, 13).toInt();
  tmStruct.tm_sec  = filename.substring(13, 15).toInt();
  tmStruct.tm_isdst = -1; // let mktime figure out DST, same as the localtime_r that wrote it
  return mktime(&tmStruct); // -1 on failure too - already our own "unparseable" sentinel
}

std::vector<String> filesToExpire(const std::vector<SnapshotFileInfo>& filesOldestFirst,
                                   uint16_t retentionDays, time_t nowEpoch) {
  std::vector<String> result;
  if (retentionDays == 0) return result; // keep forever

  time_t cutoff = nowEpoch - (time_t)retentionDays * 24 * 60 * 60;
  for (auto& f : filesOldestFirst) {
    time_t ts = parseSnapshotTimestamp(f.name);
    if (ts == (time_t)-1) continue; // can't determine age - leave it alone, don't guess
    if (ts < cutoff) result.push_back(f.name);
  }
  return result;
}
