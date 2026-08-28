#include <unity.h>
#include <Arduino.h>
#include "webserver_html.h"

void setUp(void) {}
void tearDown(void) {}

void test_renderEditDeleteActions_edit_link_uses_route_base_and_encoded_name(void) {
  String html = renderEditDeleteActions("/cameras/edit?name=", "/delete", "D01-FrontDoor");
  TEST_ASSERT_TRUE(html.indexOf("<a href=\"/cameras/edit?name=D01-FrontDoor\">Edit</a>") >= 0);
}

void test_renderEditDeleteActions_percent_encodes_special_characters_in_edit_link(void) {
  // A space (and other non-unreserved characters) in the name must be
  // percent-encoded in the URL, not passed through raw.
  String html = renderEditDeleteActions("/users/edit?name=", "/users/delete", "Front Door");
  TEST_ASSERT_TRUE(html.indexOf("name=Front%20Door") >= 0);
}

void test_renderEditDeleteActions_uses_given_delete_route(void) {
  // The two real callers don't share a delete-route convention (cameras:
  // "/delete", users: "/users/delete") - this must use exactly what's passed.
  String html = renderEditDeleteActions("/cameras/edit?name=", "/delete", "D01");
  TEST_ASSERT_TRUE(html.indexOf("action=\"/delete\"") >= 0);
}

void test_renderEditDeleteActions_confirm_dialog_and_hidden_field_use_escaped_name(void) {
  String html = renderEditDeleteActions("/users/edit?name=", "/users/delete", "A & B");
  TEST_ASSERT_TRUE(html.indexOf("confirm('Delete A &amp; B?')") >= 0);
  TEST_ASSERT_TRUE(html.indexOf("value=\"A &amp; B\"") >= 0);
}

// The bug this exists to catch: the confirm() dialog's text sits inside a
// single-quoted JS string NESTED inside a double-quoted onsubmit="..."
// attribute. htmlEscape() used to leave ' untouched (only "/&/</> were
// escaped), so a name containing a literal ' could break out of that JS
// string and inject arbitrary script that runs the instant an admin
// clicks Delete on that row - a real, confirmed XSS gap, not a
// hypothetical. Verifies the single quote is now escaped to &#39;, not
// left as a literal ' that would terminate the JS string early.
void test_renderEditDeleteActions_confirm_dialog_escapes_single_quote(void) {
  String html = renderEditDeleteActions("/cameras/edit?name=", "/delete", "a');alert(1);//");
  TEST_ASSERT_TRUE(html.indexOf("confirm('Delete a&#39;);alert(1);//?')") >= 0);
  TEST_ASSERT_TRUE(html.indexOf("');alert(1);//") < 0); // the raw breakout sequence must not appear anywhere
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_renderEditDeleteActions_edit_link_uses_route_base_and_encoded_name);
  RUN_TEST(test_renderEditDeleteActions_percent_encodes_special_characters_in_edit_link);
  RUN_TEST(test_renderEditDeleteActions_uses_given_delete_route);
  RUN_TEST(test_renderEditDeleteActions_confirm_dialog_and_hidden_field_use_escaped_name);
  RUN_TEST(test_renderEditDeleteActions_confirm_dialog_escapes_single_quote);
  return UNITY_END();
}
