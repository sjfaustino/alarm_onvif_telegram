#pragma once
#include <Arduino.h>

// Security panel content (dashboard login status + change form). Split
// out of webserver.cpp - see webserver_network.h's comment for why. The
// actual login-checking/updating (the auth middleware) stays in
// webserver.cpp, since it's tied to the PsychicHttpServer instance there.
String renderSecurityPanel();

// Plain-text dump of every camera, Telegram user, and network setting
// currently in NVS, for manual disaster recovery (see this panel's own
// "there's no recovery flow if [the login is] lost - forgetting it means
// erasing the board's NVS entirely" warning - this is the other half of
// that story: recovering everything ELSE that erasing NVS would also
// wipe). Deliberately excludes every password (camera and WiFi both) -
// re-enter those manually after a restore; everything else here is
// exactly what's tedious to reconstruct from memory. Served as a
// downloadable file by webserver.cpp's /export route.
String buildConfigExport();
