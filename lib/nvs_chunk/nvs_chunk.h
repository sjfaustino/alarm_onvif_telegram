#pragma once
#include <Arduino.h> // explicit: PlatformIO's LDF only scans a lib's own includes
#include <vector>

// NVS entries top out around 4000 bytes. A whole record list stored as one
// string would silently lose its tail, so lists
// are split across keys "list0", "list1", ...

// Empty input gives zero chunks (no pointless empty write).
std::vector<String> splitIntoChunks(const String& data, size_t maxChunkSize);

// joinChunks(splitIntoChunks(s, n)) == s for any n > 0.
String joinChunks(const std::vector<String>& chunks);
