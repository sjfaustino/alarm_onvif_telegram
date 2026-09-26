#include "camera_parse.h"
#include <cstring>

std::vector<ProfileInfo> parseProfiles(const String& xml) {
  std::vector<ProfileInfo> profiles;
  int pos = 0;
  while (true) {
    int p = xml.indexOf("Profiles ", pos);
    if (p < 0) break;
    bool isOpeningTag = (p > 0) && (xml[p - 1] == ':' || xml[p - 1] == '<');
    if (!isOpeningTag) { pos = p + 9; continue; }

    int tagEnd = xml.indexOf(">", p);
    if (tagEnd < 0) break;
    String tag = xml.substring(p, tagEnd);

    String token = findAttributeInTag(tag, "token"); // tolerates single- or double-quoted attributes

    String name;
    int namePos = xml.indexOf("Name>", tagEnd);
    if (namePos >= 0 && namePos - tagEnd < 300) {
      int cs = namePos + 5;
      int ce = xml.indexOf("</", cs);
      if (ce > cs) name = xml.substring(cs, ce);
    }

    // Search only this profile's block, so an audio Encoding or a later
    // profile's video Encoding isn't picked up.
    String encoding;
    int nextProfilePos = xml.indexOf("Profiles ", tagEnd);
    int profileBlockEnd = (nextProfilePos > 0) ? nextProfilePos : xml.length();
    int vecPos = xml.indexOf("VideoEncoderConfiguration", tagEnd);
    if (vecPos >= 0 && vecPos < profileBlockEnd) {
      encoding = findElementByLocalName(xml.substring(vecPos, profileBlockEnd), "Encoding");
    }

    if (token.length() > 0) profiles.push_back({token, name, encoding});
    pos = tagEnd + 1;
  }
  return profiles;
}

// A PullMessages batch (MessageLimit=20) can hold several messages for the
// same topic, e.g. a stale "false" then a real "true". Checking only the first
// made later events invisible. Returns "true" if any block says so, else the
// last non-empty value.
String extractEventStateValue(const String& xml, const String& topicKeyword) {
  String lastValue;
  int searchFrom = 0;
  while (true) {
    int topicPos = xml.indexOf(topicKeyword, searchFrom);
    if (topicPos < 0) break;

    int blockEnd = xml.indexOf("</wsnt:NotificationMessage>", topicPos);
    if (blockEnd < 0) blockEnd = xml.length();

    int p = xml.indexOf("Name=\"State\"", topicPos);
    if (p < 0 || p >= blockEnd) p = xml.indexOf("Name=\"state\"", topicPos);
    if (p < 0 || p >= blockEnd) p = xml.indexOf("Name=\"IsMotion\"", topicPos);

    if (p >= 0 && p < blockEnd) {
      int valuePos = xml.indexOf("Value=", p);
      if (valuePos >= 0 && valuePos < blockEnd) {
        int start = valuePos + strlen("Value=");
        if (start < (int)xml.length()) {
          char quote = xml[start];
          if (quote == '"' || quote == '\'') {
            start++;
            int end = xml.indexOf(quote, start);
            if (end >= 0) {
              String state = xml.substring(start, end);
              state.trim();
              if (state == "true") return "true"; // any block reporting true wins outright
              lastValue = state;
            }
          }
        }
      }
    }

    // Skip past this whole block (or +1 if it has no closing tag).
    searchFrom = (blockEnd > topicPos) ? blockEnd : topicPos + 1;
  }
  return lastValue;
}

CameraEventClassification classifyCameraEvent(const String& xml) {
  CameraEventClassification ev;
  // Both quote styles: missing Value='true' would make anyTrue false and
  // silently kill all detection for that camera.
  ev.anyTrue = xml.indexOf("Value=\"true\"") >= 0 || xml.indexOf("Value='true'") >= 0;
  ev.motionAlarm   = xml.indexOf("MotionAlarm") >= 0;
  ev.cellMotion    = xml.indexOf("CellMotionDetector") >= 0;
  ev.peopleDetect  = xml.indexOf("PeopleDetect") >= 0;
  ev.vehicleDetect = xml.indexOf("VehicleDetect") >= 0;
  ev.dogCatDetect  = xml.indexOf("DogCatDetect") >= 0;
  ev.signalLoss    = xml.indexOf("SignalLoss") >= 0;
  ev.tamper        = xml.indexOf("TamperDetector") >= 0;
  return ev;
}

bool topicReportedTrue(const String& xml, const String& topicKeyword) {
  return extractEventStateValue(xml, topicKeyword) == "true";
}

bool motionEventFired(const String& xml, const CameraEventClassification& ev) {
  if (ev.motionAlarm && topicReportedTrue(xml, "MotionAlarm")) return true;
  if (ev.cellMotion && topicReportedTrue(xml, "CellMotionDetector")) return true;
  if (ev.peopleDetect && topicReportedTrue(xml, "PeopleDetect")) return true;
  if (ev.vehicleDetect && topicReportedTrue(xml, "VehicleDetect")) return true;
  // DogCatDetect is gated per camera in parseEvents instead.
  return false;
}

String firstTopic(const String& xml) {
  // Not findElementByLocalName: <wsnt:Topic> usually carries a Dialect
  // attribute, which that function's matching can't handle.
  int p = xml.indexOf(":Topic");
  if (p < 0) p = xml.indexOf("<Topic");
  if (p < 0) return "";

  int tagEnd = xml.indexOf('>', p);
  if (tagEnd < 0) return "";
  int contentEnd = xml.indexOf("</", tagEnd);
  if (contentEnd < 0 || contentEnd <= tagEnd + 1) return "";

  String result = xml.substring(tagEnd + 1, contentEnd);
  result.trim();
  return result;
}
