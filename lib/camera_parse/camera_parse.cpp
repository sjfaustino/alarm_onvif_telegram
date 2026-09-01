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

// PullMessages is called with MessageLimit=20 (camera.cpp), so a single
// response routinely batches several <wsnt:NotificationMessage> blocks -
// including, on some cameras, more than one for the SAME topic in one
// batch (e.g. a stale/trailing "false" from before this poll followed by
// a genuine new "true", or plain debounce flicker). The original version
// of this function only ever inspected the FIRST block mentioning
// topicKeyword and stopped there - a real motion event reported in a
// later block was silently invisible whenever an earlier block in the
// same batch happened to mention the same topic as false. Now scans every
// block that mentions the topic and returns "true" as soon as any of them
// report it - a topic firing true anywhere in the batch counts as fired,
// full stop, regardless of what an earlier or later block in the same
// batch said. Falls back to the last non-empty value found if none was
// "true" (preserves the original single-block behavior/return value when
// there's genuinely only one relevant block, or every block agrees).
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

    // Advance to (at least) this block's end so the next iteration looks
    // at a later block, not the same occurrence again - blockEnd rather
    // than topicPos+1 skips the whole block in one step when it's found;
    // falls back to a plain +1 only if this block's closing tag was never
    // found at all (already exhausted the document via blockEnd==length()
    // in that case, so the next indexOf simply returns -1 and the loop ends).
    searchFrom = (blockEnd > topicPos) ? blockEnd : topicPos + 1;
  }
  return lastValue;
}

CameraEventClassification classifyCameraEvent(const String& xml) {
  CameraEventClassification ev;
  ev.anyTrue = xml.indexOf("Value=\"true\"") >= 0;
  ev.motionAlarm  = xml.indexOf("MotionAlarm") >= 0;
  ev.cellMotion   = xml.indexOf("CellMotionDetector") >= 0;
  ev.peopleDetect = xml.indexOf("PeopleDetect") >= 0;
  ev.signalLoss   = xml.indexOf("SignalLoss") >= 0;
  ev.tamper       = xml.indexOf("TamperDetector") >= 0;
  return ev;
}

bool topicReportedTrue(const String& xml, const String& topicKeyword) {
  return extractEventStateValue(xml, topicKeyword) == "true";
}

bool motionEventFired(const String& xml, const CameraEventClassification& ev) {
  if (ev.motionAlarm && topicReportedTrue(xml, "MotionAlarm")) return true;
  if (ev.cellMotion && topicReportedTrue(xml, "CellMotionDetector")) return true;
  if (ev.peopleDetect && topicReportedTrue(xml, "PeopleDetect")) return true;
  return false;
}

String firstTopic(const String& xml) {
  // Deliberately NOT findElementByLocalName (xml_helpers.h): that
  // function's exact/suffix matching is built for attribute-free elements
  // (Uri, Address, XAddr) - a real <wsnt:Topic Dialect="...">...</wsnt:Topic>
  // almost always carries a Dialect attribute per the ONVIF WS-Topics
  // spec, which breaks both of findElementByLocalName's search strategies
  // (neither "<Topic>" nor the ":Topic>" suffix appears in an opening tag
  // that has attributes before its '>'). Anchored the same two ways
  // (":Topic" or "<Topic") to require an actual tag name, not incidental
  // text elsewhere.
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
