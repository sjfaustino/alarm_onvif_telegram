#pragma once
#include <Arduino.h> // explicit, not chained - see camera_serialize.h's comment
#include <vector>
#include <utility> // std::pair

// Submitted form fields. The dashboard wraps a PsychicRequest
// (request_params.h); tests use a map.
class FormParams {
 public:
  virtual ~FormParams() = default;
  virtual bool has(const char* name) const = 0;
  virtual String get(const char* name, const char* fallback) const = 0;
};

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

// Labelled form inputs. `label` is trusted HTML (fixed text); `value` is raw
// and escaped here. `attrs` is appended inside the tag verbatim (e.g.
// " required"). `name` is escaped too (it can carry user data, e.g.
// "cam_<camera name>").
String htmlTextInput(const String& label, const String& name, const String& value, const char* attrs = "");
String htmlTimeInput(const String& label, const String& name, const String& value);
// Never pre-filled, so a stored password can't leak into the page.
String htmlPasswordInput(const String& label, const String& name, const char* attrs = "");
String htmlCheckbox(const String& label, const String& name, bool checked);
String htmlHiddenInput(const String& name, const String& value);
// <select> with one option per (value, text) pair; `selected` marks the
// matching value. Option text is trusted HTML.
String htmlSelect(const String& label, const String& name,
                  const std::vector<std::pair<String, String>>& options, const String& selected);
