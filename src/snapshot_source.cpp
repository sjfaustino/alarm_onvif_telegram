#include "snapshot_source.h"

const char* snapshotSourceLabel(SnapshotSource source) {
  switch (source) {
    case SnapshotSource::Motion:    return "motion";
    case SnapshotSource::Person:    return "person";
    case SnapshotSource::Vehicle:   return "vehicle";
    case SnapshotSource::Pet:       return "pet";
    case SnapshotSource::Tamper:    return "tamper";
    case SnapshotSource::Timelapse: return "timelapse";
    case SnapshotSource::Test:      return "test";
    case SnapshotSource::Manual:    return "manual";
  }
  return "motion"; // unreachable if every enumerator above is handled
}

SnapshotSource snapshotSourceFromLabel(const String& label) {
  if (label == "person") return SnapshotSource::Person;
  if (label == "vehicle") return SnapshotSource::Vehicle;
  if (label == "pet") return SnapshotSource::Pet;
  if (label == "tamper") return SnapshotSource::Tamper;
  if (label == "timelapse") return SnapshotSource::Timelapse;
  if (label == "test") return SnapshotSource::Test;
  if (label == "manual") return SnapshotSource::Manual;
  return SnapshotSource::Motion; // covers "motion" itself and any unrecognized/missing label
}
