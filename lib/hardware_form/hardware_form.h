#pragma once
#include <Arduino.h> // explicit: PlatformIO's LDF only scans a lib's own includes
#include "bridge_watchdog.h"
#include "net_watchdog.h"
#include "power_monitor.h"
#include "webserver_html.h" // FormParams

// Hardware page forms: parsing and validation, tested natively.

NetWatchdogSettings parseNetWatchdogForm(const FormParams& params);
BridgeWatchdogSettings parseBridgeWatchdogForm(const FormParams& params);
PowerMonitorSettings parsePowerMonitorForm(const FormParams& params);

// Each returns the banner explaining why the settings can't be saved, or "" if
// they can. The other two features' saved settings are checked for pin clashes.
String validateNetWatchdogSettings(const NetWatchdogSettings& s, const BridgeWatchdogSettings& bridge,
                                   const PowerMonitorSettings& power);
String validateBridgeWatchdogSettings(const BridgeWatchdogSettings& s, const NetWatchdogSettings& net,
                                      const PowerMonitorSettings& power);
String validatePowerMonitorSettings(const PowerMonitorSettings& s, const NetWatchdogSettings& net,
                                    const BridgeWatchdogSettings& bridge);
