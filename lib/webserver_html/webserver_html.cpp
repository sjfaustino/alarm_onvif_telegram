#include "webserver_html.h"
#include "format_utils.h"

String renderEditDeleteActions(const String& editRouteBase, const String& deleteRoute, const String& itemName) {
  String html = "<div class=\"row-actions\">";
  html += "<a class=\"icon-btn secondary\" href=\"" + editRouteBase + urlEncode(itemName) +
          "\" title=\"Edit\" aria-label=\"Edit\">&#9998;</a>";
  html += "<form class=\"inline\" method=\"POST\" action=\"" + deleteRoute + "\" "
          "onsubmit=\"return confirm('Delete " + htmlEscape(itemName) + "?');\">";
  html += "<input type=\"hidden\" name=\"name\" value=\"" + htmlEscape(itemName) + "\">";
  html += "<button type=\"submit\" class=\"danger icon-btn\" title=\"Delete\" aria-label=\"Delete\">&#128465;</button></form>";
  html += "</div>";
  return html;
}

String renderDiscoveryResultsTable(const std::vector<String>& columnHeaders, const String& addPath,
                                    const std::vector<DiscoveryResultRow>& rows) {
  String html = "<table><tr>";
  for (auto& h : columnHeaders) html += "<th>" + h + "</th>"; // static English column labels, not user data
  html += "<th></th></tr>";

  for (auto& row : rows) {
    html += "<tr>";
    for (auto& cell : row.cells) html += "<td>" + htmlEscape(cell) + "</td>";

    String href = addPath;
    for (size_t i = 0; i < row.addParams.size(); i++) {
      href += (i == 0 ? "?" : "&") + row.addParams[i].first + "=" + urlEncode(row.addParams[i].second);
    }
    html += "<td><a href=\"" + href + "\">Add</a></td></tr>";
  }
  html += "</table>";
  return html;
}

String renderDataTable(const std::vector<String>& columnHeaders, const std::vector<std::vector<String>>& rows) {
  String html = "<table><tr>";
  for (auto& h : columnHeaders) html += "<th>" + h + "</th>";
  html += "</tr>";
  for (auto& row : rows) {
    html += "<tr>";
    for (auto& cell : row) html += "<td>" + htmlEscape(cell) + "</td>";
    html += "</tr>";
  }
  html += "</table>";
  return html;
}

static String valueInput(const String& label, const char* type, const char* name, const String& value,
                         const char* attrs) {
  return "<label>" + label + "<input type=\"" + type + "\" name=\"" + name + "\" value=\"" + htmlEscape(value) +
         "\"" + attrs + "></label>";
}

String htmlTextInput(const String& label, const char* name, const String& value, const char* attrs) {
  return valueInput(label, "text", name, value, attrs);
}

String htmlTimeInput(const String& label, const char* name, const String& value) {
  return valueInput(label, "time", name, value, "");
}

String htmlPasswordInput(const String& label, const char* name, const char* attrs) {
  return "<label>" + label + "<input type=\"password\" name=\"" + name + "\"" + attrs + "></label>";
}

String htmlCheckbox(const String& label, const char* name, bool checked) {
  return String("<label class=\"checkbox\"><input type=\"checkbox\" name=\"") + name + "\"" +
         (checked ? " checked" : "") + "> " + label + "</label>";
}

String htmlHiddenInput(const char* name, const String& value) {
  return String("<input type=\"hidden\" name=\"") + name + "\" value=\"" + htmlEscape(value) + "\">";
}
