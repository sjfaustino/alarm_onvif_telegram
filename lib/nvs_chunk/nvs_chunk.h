#pragma once
#include <Arduino.h> // explicit, not chained - see camera_serialize.h's comment
#include <vector>

// NVS entries top out around 4000 bytes. Storing a whole record list as one
// string once silently dropped the last cameras of a ~10-camera list, so lists
// are split across keys "list0", "list1", ...

// Empty input gives zero chunks (no pointless empty write).
std::vector<String> splitIntoChunks(const String& data, size_t maxChunkSize);

// joinChunks(splitIntoChunks(s, n)) == s for any n > 0.
String joinChunks(const std::vector<String>& chunks);
