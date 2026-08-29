#include "webserver_html.h"
#include "format_utils.h"

String renderEditDeleteActions(const String& editRouteBase, const String& deleteRoute, const String& itemName) {
  String html;
  html += "<a href=\"" + editRouteBase + urlEncode(itemName) + "\">Edit</a> ";
  html += "<form class=\"inline\" method=\"POST\" action=\"" + deleteRoute + "\" "
          "onsubmit=\"return confirm('Delete " + htmlEscape(itemName) + "?');\">";
  html += "<input type=\"hidden\" name=\"name\" value=\"" + htmlEscape(itemName) + "\">";
  html += "<button type=\"submit\">Delete</button></form>";
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
