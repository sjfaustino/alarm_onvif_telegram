#pragma once
#include <Arduino.h>

// Activity panel content (recent-events log). Split out of webserver.cpp -
// see webserver_network.h's comment for why. Backed by event_log_store.h's
// global ring buffer, which every event-producing call site (camera.cpp,
// telegram.cpp, main.cpp, webserver.cpp's OTA handler) feeds via logEvent().
String renderActivityPanel();
