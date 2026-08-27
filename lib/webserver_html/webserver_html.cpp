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
