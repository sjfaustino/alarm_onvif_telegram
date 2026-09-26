#pragma once
#include <Arduino.h> // explicit, not just via network_store.h - see camera_serialize.h's comment
#include "network_store.h"

// WifiCredentials <-> one-line record, for config export/import only
// (network_store keeps separate NVS keys). Tested natively; versioned like
// camera_serialize.h.
static const uint16_t NETWORK_SCHEMA_VERSION = 1;

// WiFi passwords are never written.
String serializeNetworkConfig(const WifiCredentials& creds);

// Returns an empty-hostname struct if malformed. Passwords are always blank;
// the user must re-enter them.
WifiCredentials deserializeNetworkConfig(const String& record, uint16_t recordVersion);
