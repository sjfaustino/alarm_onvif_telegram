#include "nvs_chunk.h"

std::vector<String> splitIntoChunks(const String& data, size_t maxChunkSize) {
  std::vector<String> chunks;
  if (data.length() == 0 || maxChunkSize == 0) return chunks;

  for (size_t pos = 0; pos < data.length(); pos += maxChunkSize) {
    size_t len = data.length() - pos;
    if (len > maxChunkSize) len = maxChunkSize;
    chunks.push_back(data.substring(pos, pos + len));
  }
  return chunks;
}

String joinChunks(const std::vector<String>& chunks) {
  String out;
  for (const String& c : chunks) out += c;
  return out;
}
