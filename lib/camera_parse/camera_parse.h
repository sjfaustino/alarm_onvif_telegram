#pragma once
#include <Arduino.h> // explicit, not just via xml_helpers.h - see camera_serialize.h's comment
#include <vector>
#include "xml_helpers.h"

// Pure ONVIF XML parsing, split from camera.cpp so it can be tested natively.

struct ProfileInfo {
  String token;
  String name;
  // This profile's video codec ("JPEG", "H264", ...), "" if none. Scoped to
  // the VideoEncoderConfiguration so an audio codec can't make an H.264-only
  // camera look MJPEG-capable.
  String encoding;
};

// Profiles from a GetProfilesResponse. Only real Profiles tags count; nested
// configurations carry tokens too.
std::vector<ProfileInfo> parseProfiles(const String& xml);

// The State/IsMotion value inside topicKeyword's own NotificationMessage (not
// the first one in the batch). Accepts "State", "state" and "IsMotion"; "" if
// not found.
String extractEventStateValue(const String& xml, const String& topicKeyword);

// Topics present in a PullMessages body. anyTrue is body-wide and can be set
// by an unrelated topic in the same batch - use motionEventFired to decide on
// a motion alert.
struct CameraEventClassification {
  bool anyTrue = false;
  bool motionAlarm = false;
  bool cellMotion = false;
  // Vendor RuleEngine topics (".../MyRuleDetector/PeopleDetect",
  // "VehicleDetect", "DogCatDetect") seen in the field, matched by name.
  bool peopleDetect = false;
  bool vehicleDetect = false;
  // Not part of motionEventFired: pet alerts are opt-in per camera, checked in
  // parseEvents.
  bool dogCatDetect = false;
  bool signalLoss = false;
  bool tamper = false;
};
CameraEventClassification classifyCameraEvent(const String& xml);

// Whether topicKeyword's own message reported true (unlike the body-wide
// anyTrue).
bool topicReportedTrue(const String& xml, const String& topicKeyword);

// Whether a motion topic (MotionAlarm, CellMotionDetector, PeopleDetect,
// VehicleDetect) itself reported true. Checking anyTrue alone fired motion
// alerts off unrelated topics like SignalLoss.
bool motionEventFired(const String& xml, const CameraEventClassification& ev);

// First <tt:Topic> text, for logging unrecognized events. Never used to
// classify.
String firstTopic(const String& xml);
