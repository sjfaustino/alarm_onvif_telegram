#pragma once
#include <Arduino.h>

// Security panel content (dashboard login status + change form). Split
// out of webserver.cpp - see webserver_network.h's comment for why. The
// actual login-checking/updating (the auth middleware) stays in
// webserver.cpp, since it's tied to the PsychicHttpServer instance there.
String renderSecurityPanel();
