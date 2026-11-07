#pragma once
#include <Arduino.h>

// Security panel content (dashboard login status + change form). The login
// middleware is in webserver.cpp; config export/import is in config_backup.h.
String renderSecurityPanel();
