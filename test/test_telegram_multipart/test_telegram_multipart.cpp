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

// Independently-authored expected multipart body, NOT derived from
// buildMultipart's own m.head/m.tail output - an earlier version of this
// test computed its "expected" Content-Length from those fields directly,
// which only proves internal self-consistency: a mutation that drops a
// \r\n from the head (a real "Telegram rejects this as malformed" bug)
// shrinks m.head.length() and the test's expected value by the exact same
// amount, so it would still pass. This hand-builds the exact expected
// head/tail strings from the documented multipart format instead (the
// boundary is a fixed literal buildMultipart always uses, not read back
// from m.boundary either), so a missing/wrong byte anywhere in either
// actually fails the comparison, and the Content-Length is checked
// against the length of THIS independently-built body, not the
// implementation's own.
void test_buildMultipart_head_and_tail_match_expected_bytes_exactly(void) {
  TelegramMultipart m = buildMultipart(1234, "hi", "98765", "T");
  String boundary = "----ESP32Boundary7MA4YWxk"; // the fixed literal buildMultipart always uses

  String expectedHead =
      "--" + boundary + "\r\n" +
      "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n98765\r\n" +
      "--" + boundary + "\r\n" +
      "Content-Disposition: form-data; name=\"caption\"\r\n\r\nhi\r\n" +
      "--" + boundary + "\r\n" +
      "Content-Disposition: form-data; name=\"photo\"; filename=\"snap.jpg\"\r\n" +
      "Content-Type: image/jpeg\r\n\r\n";
  String expectedTail = "\r\n--" + boundary + "--\r\n";

  TEST_ASSERT_EQUAL_STRING(expectedHead.c_str(), m.head.c_str());
  TEST_ASSERT_EQUAL_STRING(expectedTail.c_str(), m.tail.c_str());

  size_t expectedContentLength = expectedHead.length() + 1234 + expectedTail.length();
  TEST_ASSERT_EQUAL_UINT32(expectedContentLength, m.contentLength);
  TEST_ASSERT_TRUE(m.requestLine.indexOf("Content-Length: " + String((unsigned long)expectedContentLength)) >= 0);
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
  RUN_TEST(test_buildMultipart_head_and_tail_match_expected_bytes_exactly);
  RUN_TEST(test_buildMultipart_head_contains_chat_id_and_caption);
  RUN_TEST(test_buildMultipart_boundary_consistent_across_head_tail_and_header);
  RUN_TEST(test_buildMultipart_zero_length_photo);
  return UNITY_END();
}
