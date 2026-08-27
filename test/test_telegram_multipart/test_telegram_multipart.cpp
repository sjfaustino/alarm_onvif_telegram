#include <unity.h>
#include <Arduino.h>
#include "telegram_multipart.h"

void setUp(void) {}
void tearDown(void) {}

void test_buildMultipart_requestLine_has_bot_token_and_endpoint(void) {
  TelegramMultipart m = buildMultipart(100, "caption", "12345", "TESTTOKEN");
  TEST_ASSERT_TRUE(m.requestLine.startsWith("POST /botTESTTOKEN/sendPhoto HTTP/1.1\r\n"));
  TEST_ASSERT_TRUE(m.requestLine.indexOf("Host: api.telegram.org\r\n") >= 0);
}

void test_buildMultipart_requestLine_content_length_matches_actual_body(void) {
  TelegramMultipart m = buildMultipart(1234, "caption", "12345", "TESTTOKEN");
  String expected = "Content-Length: " + String(m.head.length() + 1234 + m.tail.length());
  TEST_ASSERT_TRUE(m.requestLine.indexOf(expected) >= 0);
  TEST_ASSERT_EQUAL_UINT32(m.head.length() + 1234 + m.tail.length(), m.contentLength);
}

void test_buildMultipart_head_contains_chat_id_and_caption(void) {
  TelegramMultipart m = buildMultipart(0, "front door - 12:00", "98765", "T");
  TEST_ASSERT_TRUE(m.head.indexOf("name=\"chat_id\"\r\n\r\n98765\r\n") >= 0);
  TEST_ASSERT_TRUE(m.head.indexOf("name=\"caption\"\r\n\r\nfront door - 12:00\r\n") >= 0);
  TEST_ASSERT_TRUE(m.head.indexOf("Content-Type: image/jpeg\r\n\r\n") >= 0);
}

// The boundary string used in the Content-Type header must be the exact
// same one the head/tail were built with - a mismatch here would make
// Telegram reject the whole request as malformed multipart.
void test_buildMultipart_boundary_consistent_across_head_tail_and_header(void) {
  TelegramMultipart m = buildMultipart(0, "c", "1", "T");
  TEST_ASSERT_TRUE(m.head.indexOf("--" + m.boundary + "\r\n") >= 0);
  TEST_ASSERT_TRUE(m.tail.indexOf("--" + m.boundary + "--\r\n") >= 0);
  TEST_ASSERT_TRUE(m.requestLine.indexOf("boundary=" + m.boundary) >= 0);
}

void test_buildMultipart_zero_length_photo(void) {
  TelegramMultipart m = buildMultipart(0, "c", "1", "T");
  TEST_ASSERT_EQUAL_UINT32(m.head.length() + m.tail.length(), m.contentLength);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_buildMultipart_requestLine_has_bot_token_and_endpoint);
  RUN_TEST(test_buildMultipart_requestLine_content_length_matches_actual_body);
  RUN_TEST(test_buildMultipart_head_contains_chat_id_and_caption);
  RUN_TEST(test_buildMultipart_boundary_consistent_across_head_tail_and_header);
  RUN_TEST(test_buildMultipart_zero_length_photo);
  return UNITY_END();
}
