#pragma once
#include <Arduino.h> // explicit, not chained - see camera_serialize.h's comment
#include <vector>
#include <utility> // std::pair

// Pure HTML builders shared by the dashboard's list panels. All inputs are
// raw; escaping happens here, in one place.

// "Edit | Delete" cell: an edit link (editRouteBase + encoded name) and a POST
// delete form with a confirm(). deleteRoute is used as-is (cameras and users
// differ).
String renderEditDeleteActions(const String& editRouteBase, const String& deleteRoute, const String& itemName);

// One row of a discovery results table (camera search, WiFi scan): display
// cells plus query params for the Add link.
struct DiscoveryResultRow {
  std::vector<String> cells;                       // one per columnHeaders entry, in order
  std::vector<std::pair<String, String>> addParams; // Add link's query params, name -> raw value
};

// Results table with an Add link per row: addPath + "?" + addParams.
String renderDiscoveryResultsTable(const std::vector<String>& columnHeaders, const String& addPath,
                                    const std::vector<DiscoveryResultRow>& rows);

// Read-only table with no action column (e.g. Test all results).
String renderDataTable(const std::vector<String>& columnHeaders, const std::vector<std::vector<String>>& rows);
