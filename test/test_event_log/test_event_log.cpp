#include <unity.h>
#include <Arduino.h>
#include "event_log.h"

void setUp(void) {}
void tearDown(void) {}

void test_EventRingBuffer_starts_empty(void) {
  EventRingBuffer buf(3);
  TEST_ASSERT_EQUAL_INT(0, (int)buf.size());
  TEST_ASSERT_EQUAL_INT(3, (int)buf.capacity());
  TEST_ASSERT_TRUE(buf.entries().empty());
}

void test_EventRingBuffer_push_below_capacity_keeps_everything_in_order(void) {
  EventRingBuffer buf(5);
  buf.push(100, "first");
  buf.push(200, "second");
  buf.push(300, "third");
  TEST_ASSERT_EQUAL_INT(3, (int)buf.size());
  TEST_ASSERT_EQUAL_STRING("first", buf.entries()[0].text.c_str());
  TEST_ASSERT_EQUAL_STRING("second", buf.entries()[1].text.c_str());
  TEST_ASSERT_EQUAL_STRING("third", buf.entries()[2].text.c_str());
  TEST_ASSERT_EQUAL_UINT32(100UL, buf.entries()[0].ms);
}

// The core behavior this exists for: once full, only the single oldest
// entry drops per push - not the whole buffer, and not more than one at a
// time even under a burst.
void test_EventRingBuffer_drops_only_the_oldest_entry_once_full(void) {
  EventRingBuffer buf(2);
  buf.push(1, "a");
  buf.push(2, "b");
  buf.push(3, "c"); // "a" should be evicted
  TEST_ASSERT_EQUAL_INT(2, (int)buf.size());
  TEST_ASSERT_EQUAL_STRING("b", buf.entries()[0].text.c_str());
  TEST_ASSERT_EQUAL_STRING("c", buf.entries()[1].text.c_str());
}

void test_EventRingBuffer_survives_many_pushes_past_capacity(void) {
  EventRingBuffer buf(3);
  for (int i = 0; i < 10; i++) buf.push((unsigned long)i, "e" + String(i));
  TEST_ASSERT_EQUAL_INT(3, (int)buf.size());
  // Only the last 3 pushed (e7, e8, e9) should remain, oldest-first.
  TEST_ASSERT_EQUAL_STRING("e7", buf.entries()[0].text.c_str());
  TEST_ASSERT_EQUAL_STRING("e8", buf.entries()[1].text.c_str());
  TEST_ASSERT_EQUAL_STRING("e9", buf.entries()[2].text.c_str());
}

void test_EventRingBuffer_capacity_of_one(void) {
  EventRingBuffer buf(1);
  buf.push(1, "a");
  buf.push(2, "b");
  TEST_ASSERT_EQUAL_INT(1, (int)buf.size());
  TEST_ASSERT_EQUAL_STRING("b", buf.entries()[0].text.c_str());
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_EventRingBuffer_starts_empty);
  RUN_TEST(test_EventRingBuffer_push_below_capacity_keeps_everything_in_order);
  RUN_TEST(test_EventRingBuffer_drops_only_the_oldest_entry_once_full);
  RUN_TEST(test_EventRingBuffer_survives_many_pushes_past_capacity);
  RUN_TEST(test_EventRingBuffer_capacity_of_one);
  return UNITY_END();
}
