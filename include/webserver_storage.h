#pragma once
#include <Arduino.h>

// Storage panel content (SD card status, enable/disable toggle, a
// readability check, and an erase-all-history action). Split out of
// webserver.cpp - see webserver_network.h's comment for why. Backed by
// sd_store.h for the actual SD mechanics; this file only renders/parses
// the form and the two maintenance-action buttons - webserver.cpp's
// routes call sd_store.h directly for the actions themselves and pass
// the result back through renderShell's banner, same pattern every other
// panel already uses.
String renderStoragePanel();
