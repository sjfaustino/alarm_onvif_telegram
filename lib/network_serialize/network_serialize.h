#pragma once
#include <Arduino.h> // explicit, not just via network_store.h - see camera_serialize.h's comment
#include "network_store.h"

// Pure (de)serialization between WifiCredentials and a single-line record -
// used only by the config export/import feature (webserver_security.cpp),
// not by network_store.cpp's own NVS persistence (which stores each field
// as its own Preferences key, not a joined record). Split out so it can be
// unit-tested natively (test/test_network_serialize) without <Preferences.h>,
// which only exists on-device.
//
// Schema-versioned the same way as camera_serialize.h - see that header's
// CAMERA_SCHEMA_VERSION comment for the full rationale. Bump this (and add
// a new deserializeNetworkConfig branch, never edit an existing one) any
// time serializeNetworkConfig()'s field layout changes.
static const uint16_t NETWORK_SCHEMA_VERSION = 1;

// Always writes NETWORK_SCHEMA_VERSION's current field layout. Deliberately
// excludes primary.password/backup.password - same "never exported"
// convention as camera_serialize.h's serializeCamera (c.pass).
String serializeNetworkConfig(const WifiCredentials& creds);

// `recordVersion` is the schema version `record` was exported under.
// Returns a default-constructed WifiCredentials (hostname.length()==0) if
// `record` is malformed for that version - config_import_parse.cpp treats
// that as "no valid Network section". Both .password fields are always
// left blank (never serialized) - the caller must prompt for them again.
WifiCredentials deserializeNetworkConfig(const String& record, uint16_t recordVersion);
