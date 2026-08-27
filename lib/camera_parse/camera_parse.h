#pragma once
#include <Arduino.h> // explicit, not just via xml_helpers.h - see camera_serialize.h's comment
#include <vector>
#include "xml_helpers.h"

// Pure parsing of ONVIF Media/Events XML bodies, split out of camera.cpp so
// it can be unit-tested natively (test/test_camera_parse) without pulling
// in WiFi/HTTPClient, which only exist on-device.

struct ProfileInfo {
  String token;
  String name;
};

// Extracts every <trt:Profiles token="..."><tt:Name>...</tt:Name>...
// element from a GetProfilesResponse body. Restricted to actual
// "...Profiles " opening tags (":Profiles " or "<Profiles "), not any
// "token=" in the document - GetProfiles responses also carry tokens on
// nested VideoEncoderConfiguration/VideoSourceConfiguration elements.
std::vector<ProfileInfo> parseProfiles(const String& xml);

// Finds the State/IsMotion boolean value inside the NotificationMessage
// block containing topicKeyword (not the whole response - otherwise a
// batch with several topics would return whichever State/IsMotion
// happened to appear first, regardless of which topic was asked for).
// Recognizes Name="State", Name="state", and Name="IsMotion" - different
// camera stacks name the boolean differently for the same kind of event.
// Returns "" if topicKeyword, the block, or a recognized Name isn't found.
String extractEventStateValue(const String& xml, const String& topicKeyword);

// Which alarm topics are present in a PullMessages response body, and
// whether any of them actually fired (Value="true" anywhere in the body -
// PullMessages batches can report changes back to false too, which this
// distinguishes from an actual event).
struct CameraEventClassification {
  bool anyTrue = false;
  bool motionAlarm = false;
  bool cellMotion = false;
  bool signalLoss = false;
  bool tamper = false;
};
CameraEventClassification classifyCameraEvent(const String& xml);
