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
// whether *anything* in the whole body reported Value="true" - anyTrue is
// NOT scoped to any one topic (PullMessages batches can report changes back
// to false too, which this distinguishes from "some event in this body
// actually fired", but not from *which* one). A batch carrying several
// different topics is normal (MessageLimit=20 in camera.cpp's PullMessages
// call), so anyTrue can be set entirely by a topic unrelated to
// motionAlarm/cellMotion - see motionEventFired() below for the check that
// actually matters for deciding whether to send a motion alert.
struct CameraEventClassification {
  bool anyTrue = false;
  bool motionAlarm = false;
  bool cellMotion = false;
  bool signalLoss = false;
  bool tamper = false;
};
CameraEventClassification classifyCameraEvent(const String& xml);

// Whether topicKeyword's own NotificationMessage block (via
// extractEventStateValue's per-block scoping - NOT ev.anyTrue, which is a
// body-wide flag a same-batch, unrelated topic can set even while this
// one's own state is actually false) reported State/IsMotion="true". The
// building block motionEventFired below, and camera.cpp's tamper/signal-
// loss firing checks, are built from.
bool topicReportedTrue(const String& xml, const String& topicKeyword);

// Whether a motion-relevant topic (MotionAlarm or CellMotionDetector) that
// classifyCameraEvent found present in this batch *itself* reported
// State/IsMotion="true" - NOT ev.anyTrue, which is a body-wide flag that a
// same-batch, unrelated topic (SignalLoss, TamperDetector) can set to true
// while the motion topic's own state is actually false (e.g. "motion just
// ended"). Checking ev.anyTrue alone - what camera.cpp's parseEvents used
// to do - fires a motion alert off an unrelated topic's true value, with
// the log line for the motion topic itself confusingly showing "State =
// false" right before it.
bool motionEventFired(const String& xml, const CameraEventClassification& ev);

// Best-effort extraction of the first <tt:Topic>...</tt:Topic> element's
// content in a PullMessages response - "" if none is found. For logging an
// event this project doesn't otherwise recognize (camera.cpp's parseEvents
// unrecognized-topic fallback) - not used for any classification decision,
// just to tell a human what topic string a camera actually sent so support
// for it can be added deliberately, rather than the event just vanishing.
String firstTopic(const String& xml);
