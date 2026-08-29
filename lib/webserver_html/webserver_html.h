#pragma once
#include <Arduino.h> // explicit, not chained - see camera_serialize.h's comment
#include <vector>
#include <utility> // std::pair

// Small pure HTML-snippet builders shared across the dashboard's list
// panels (Cameras, Telegram Users) - split out because webserver_cameras.cpp
// and webserver_users.cpp each had their own byte-identical copy of this
// exact block, differing only in the routes and the item's name.

// The "Edit | Delete" actions cell at the end of a list table row - an
// edit link plus a POST form with a JS confirm() dialog, both keyed by
// itemName. editRouteBase is the edit link's URL up to (not including) the
// name itself, e.g. "/cameras/edit?name=" - itemName is percent-encoded
// and appended to it. deleteRoute is the delete form's action, used as-is
// (the two current callers don't actually share a naming convention here -
// cameras delete via plain "/delete", users via "/users/delete" - so this
// takes the exact route rather than assuming one).
String renderEditDeleteActions(const String& editRouteBase, const String& deleteRoute, const String& itemName);

// One row in a "discovered resource" results table (renderDiscoveryResultsTable
// below) - the Cameras page's "Search network for cameras" and the Network
// page's "Search WiFi networks" both produce this same shape: a few
// descriptive columns plus an Add link that prefills another page's form
// via query params, differing only in which columns are shown and which
// page/params Add points at. cells and addParams are raw, NOT
// pre-escaped/encoded - renderDiscoveryResultsTable does that itself, one
// escaping point, same as renderEditDeleteActions above.
struct DiscoveryResultRow {
  std::vector<String> cells;                       // one per columnHeaders entry, in order
  std::vector<std::pair<String, String>> addParams; // Add link's query params, name -> raw value
};

// Renders a "<column headers>...Add" results table. addPath is the Add
// link's target page (e.g. "/network") - each row's addParams are appended
// to it as "?name=value&...". Shared by the two callers described above,
// which used to each hand-roll this exact table shape independently.
String renderDiscoveryResultsTable(const std::vector<String>& columnHeaders, const String& addPath,
                                    const std::vector<DiscoveryResultRow>& rows);

// Renders a plain "<column headers>...<rows>" table with no per-row action
// column - for a read-only results table like "Test all cameras" (no
// Add/Edit/Delete per row). A sibling of renderDiscoveryResultsTable above
// rather than one function covering both shapes with an optional Add
// column: that would trade this file's small duplication for a worse
// one - a function whose meaning changes based on which optional
// arguments happen to be empty. Cells are raw, NOT pre-escaped - escaped
// internally, same one-escaping-point rule as renderDiscoveryResultsTable.
String renderDataTable(const std::vector<String>& columnHeaders, const std::vector<std::vector<String>>& rows);
