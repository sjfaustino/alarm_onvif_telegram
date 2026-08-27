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

    if (token.length() > 0) profiles.push_back({token, name});
    pos = tagEnd + 1;
  }
  return profiles;
}

String extractEventStateValue(const String& xml, const String& topicKeyword) {
  int topicPos = xml.indexOf(topicKeyword);
  if (topicPos < 0) return "";
  int blockEnd = xml.indexOf("</wsnt:NotificationMessage>", topicPos);
  if (blockEnd < 0) blockEnd = xml.length();

  int p = xml.indexOf("Name=\"State\"", topicPos);
  if (p < 0 || p >= blockEnd) p = xml.indexOf("Name=\"state\"", topicPos);
  if (p < 0 || p >= blockEnd) p = xml.indexOf("Name=\"IsMotion\"", topicPos);
  if (p < 0 || p >= blockEnd) return "";

  int valuePos = xml.indexOf("Value=", p);
  if (valuePos < 0 || valuePos >= blockEnd) return "";
  int start = valuePos + strlen("Value=");
  if (start >= (int)xml.length()) return "";
  char quote = xml[start];
  if (quote != '"' && quote != '\'') return "";
  start++;
  int end = xml.indexOf(quote, start);
  if (end < 0) return "";

  String state = xml.substring(start, end);
  state.trim();
  return state;
}

CameraEventClassification classifyCameraEvent(const String& xml) {
  CameraEventClassification ev;
  ev.anyTrue = xml.indexOf("Value=\"true\"") >= 0;
  ev.motionAlarm = xml.indexOf("MotionAlarm") >= 0;
  ev.cellMotion  = xml.indexOf("CellMotionDetector") >= 0;
  ev.signalLoss  = xml.indexOf("SignalLoss") >= 0;
  ev.tamper      = xml.indexOf("TamperDetector") >= 0;
  return ev;
}

bool motionEventFired(const String& xml, const CameraEventClassification& ev) {
  if (ev.motionAlarm && extractEventStateValue(xml, "MotionAlarm") == "true") return true;
  if (ev.cellMotion && extractEventStateValue(xml, "CellMotionDetector") == "true") return true;
  return false;
}
