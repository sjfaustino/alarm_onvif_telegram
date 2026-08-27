#pragma once
#include <Arduino.h>

// Firmware panel content (build info, running partition, upload form).
// Split out of webserver.cpp - see webserver_network.h's comment for why.
// The actual OTA upload handling (Update.begin/write/end, the reboot task,
// upload-in-progress state) stays in webserver.cpp: it's tightly coupled
// to route registration on the PsychicHttpServer instance living there,
// not page content.
String renderFirmwarePanel();
