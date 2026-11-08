#pragma once
#include <Arduino.h>
#include <type_traits>
#include <vector>

// Stored records as a table of fields, shared by the versioned serializers.
// Each row says which schema version added it, and the same table writes and
// reads, so a new setting is one appended row. A record of version V holds
// exactly the rows added in V or earlier; rows are never inserted or reordered.

// Separates fields (ASCII unit separator - never in real data, so no escaping).
static const char RECORD_FIELD_SEP = '\x1F';

inline String stripFieldSeparator(const String& s) {
  String out = s;
  out.replace(String(RECORD_FIELD_SEP), "");
  return out;
}

inline std::vector<String> splitRecordFields(const String& record) {
  std::vector<String> fields;
  int fieldStart = 0;
  for (int i = 0; i <= (int)record.length(); i++) {
    if (i == (int)record.length() || record[i] == RECORD_FIELD_SEP) {
      fields.push_back(record.substring(fieldStart, i));
      fieldStart = i + 1;
    }
  }
  return fields;
}

template <class T>
struct RecordField {
  uint16_t addedInVersion;
  void (*write)(const T&, String&);
  void (*read)(T&, const String&);
};

template <class Member>
struct RecordMemberOwner;
template <class T, class F>
struct RecordMemberOwner<F T::*> {
  using type = T;
};
template <auto Member>
using RecordOwner = typename RecordMemberOwner<decltype(Member)>::type;

using RecordTextFilter = String (*)(const String&);

template <auto Member, RecordTextFilter Filter>
void writeRecordText(const RecordOwner<Member>& o, String& out) { out += Filter(o.*Member); }
template <auto Member>
void readRecordText(RecordOwner<Member>& o, const String& v) { o.*Member = v; }

template <auto Member>
void writeRecordFlag(const RecordOwner<Member>& o, String& out) { out += (o.*Member ? "1" : "0"); }
template <auto Member>
void readRecordFlag(RecordOwner<Member>& o, const String& v) { o.*Member = v == "1"; }
template <auto Member>
void readRecordFlagOrDefault(RecordOwner<Member>& o, const String& v) { if (v.length() > 0) o.*Member = v == "1"; }

template <auto Member>
void writeRecordNumber(const RecordOwner<Member>& o, String& out) { out += String(o.*Member); }
template <auto Member>
void readRecordNumber(RecordOwner<Member>& o, const String& v) {
  if (v.length() == 0) return;
  auto& field = o.*Member;
  field = (std::remove_reference_t<decltype(field)>)v.toInt();
}

template <auto Member, RecordTextFilter Filter = stripFieldSeparator>
constexpr RecordField<RecordOwner<Member>> recordText(uint16_t version) {
  return {version, writeRecordText<Member, Filter>, readRecordText<Member>};
}
template <auto Member>
constexpr RecordField<RecordOwner<Member>> recordFlag(uint16_t version) {
  return {version, writeRecordFlag<Member>, readRecordFlag<Member>};
}
// Empty keeps the struct's default, for flags that default to true.
template <auto Member>
constexpr RecordField<RecordOwner<Member>> recordFlagOrDefault(uint16_t version) {
  return {version, writeRecordFlag<Member>, readRecordFlagOrDefault<Member>};
}
// Empty keeps the struct's default.
template <auto Member>
constexpr RecordField<RecordOwner<Member>> recordNumber(uint16_t version) {
  return {version, writeRecordNumber<Member>, readRecordNumber<Member>};
}

template <class T, size_t N>
size_t recordFieldsInVersion(const RecordField<T> (&table)[N], uint16_t version) {
  size_t count = 0;
  while (count < N && table[count].addedInVersion <= version) count++;
  return count;
}

template <class T, size_t N>
String writeRecord(const RecordField<T> (&table)[N], const T& value) {
  String s;
  for (size_t i = 0; i < N; i++) {
    if (i > 0) s += RECORD_FIELD_SEP;
    table[i].write(value, s);
  }
  return s;
}

// A version newer than the table knows (likely a downgrade) reads as the newest
// layout, since every row qualifies. Version 0 predates exact counts: its last
// rows were only ever appended, so any count from v0MinFields up is valid; from
// V1 on it must match exactly. Malformed records come back default-constructed.
template <class T, size_t N>
T readRecord(const RecordField<T> (&table)[N], const String& record, uint16_t version, size_t v0MinFields) {
  std::vector<String> fields = splitRecordFields(record);
  size_t expected = recordFieldsInVersion(table, version);

  T value;
  if (version == 0 ? fields.size() < v0MinFields : fields.size() != expected) return value;
  for (size_t i = 0; i < fields.size() && i < expected; i++) table[i].read(value, fields[i]);
  return value;
}
