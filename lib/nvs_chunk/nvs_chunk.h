#pragma once
#include <Arduino.h> // explicit, not chained - see camera_serialize.h's comment
#include <vector>

// NVS (Preferences) has a practical per-entry size ceiling somewhere
// around 4000 bytes for a single putString/putBytes call - not documented
// as a hard, precise number, and large enough that a handful of short
// records never hit it in testing, but real enough that a growing list of
// verbose records (long notes/URL fields, a dozen-plus cameras) can
// silently exceed it. camera_store.cpp/telegram_users.cpp used to store
// their whole record list as one NVS string value; hit in the field, some
// of the last records in a ~10-camera list failed to persist with no
// visible error until the write's return value was actually checked.
//
// splitIntoChunks/joinChunks let a value be stored across several
// small, safely-under-the-ceiling NVS keys ("list0", "list1", ...)
// instead of one that can grow without bound.

// Splits `data` into chunks of at most maxChunkSize bytes each, in order.
// Empty input produces zero chunks (not one empty chunk), so an empty
// stored list needs zero NVS writes instead of one pointless empty one.
std::vector<String> splitIntoChunks(const String& data, size_t maxChunkSize);

// Reassembles chunks produced by splitIntoChunks back into the original
// string, in order. joinChunks(splitIntoChunks(s, n)) == s for any n > 0.
String joinChunks(const std::vector<String>& chunks);
