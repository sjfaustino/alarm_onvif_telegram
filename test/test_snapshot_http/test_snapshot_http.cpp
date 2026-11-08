#include <unity.h>
#include <deque>
#include <vector>
#include "snapshot_http.h"

void setUp(void) {}
void tearDown(void) {}

// Reproduces what HTTPClient does (arduino-esp32 3.3.11):
// - end() clears added headers but keeps the Basic credentials;
// - while those are set, GET sends them, and addHeader("Authorization") is
//   silently dropped;
// - header() only returns headers registered with collectHeaders().
class FakeHttp {
 public:
  struct Response { int code; const char* wwwAuthenticate; };
  struct Request { String url; String authorization; };

  std::deque<Response> responses;
  std::vector<Request> requests;
  int ends = 0;

  bool begin(const String& url) { url_ = url; return true; }
  void setTimeout(uint16_t) {}
  void collectHeaders(const char* keys[], size_t count) {
    collected_.clear();
    for (size_t i = 0; i < count; i++) collected_.push_back(keys[i]);
  }
  void setAuthorization(const char* user, const char* pass) {
    if (user && pass) basic_ = "Basic " + String(user) + ":" + String(pass);
  }
  void setAuthorization(const char* auth) { if (auth) basic_ = auth; }
  void addHeader(const String& name, const String& value) {
    if (name.equalsIgnoreCase("Authorization") && basic_.length() > 0) return;
    if (name.equalsIgnoreCase("Authorization")) added_ = value;
  }
  int GET() {
    requests.push_back({url_, basic_.length() > 0 ? basic_ : added_});
    TEST_ASSERT_FALSE_MESSAGE(responses.empty(), "more requests than expected");
    Response r = responses.front();
    responses.pop_front();
    wwwAuth_ = r.wwwAuthenticate;
    return r.code;
  }
  String header(const char* name) {
    for (const String& k : collected_) {
      if (k.equalsIgnoreCase(name) && String(name).equalsIgnoreCase("WWW-Authenticate")) return wwwAuth_;
    }
    return "";
  }
  void end() { added_ = ""; ends++; }

 private:
  String url_, basic_, added_, wwwAuth_;
  std::vector<String> collected_;
};

static const char* kUrl = "http://10.0.0.5/cgi-bin/snapshot.cgi?a=1";
static const char* kChallenge = "Digest qop=\"auth\", realm=\"CAMERA\", nonce=\"abc123\"";
static const char* kNewChallenge = "Digest qop=\"auth\", realm=\"CAMERA\", nonce=\"fresh99\"";
static String fixedCnonce() { return "cnonce0"; }

static String digestFor(const char* challenge) {
  return digestAuthorization(challenge, "GET", "/cgi-bin/snapshot.cgi?a=1", "admin", "pw", "cnonce0");
}

static SnapshotGetResult get(FakeHttp& http, const String& cached = "") {
  return snapshotGetWithLogin(http, kUrl, 1000, "admin", "pw", cached, fixedCnonce);
}

void test_basic_camera_takes_one_request(void) {
  FakeHttp http;
  http.responses.push_back({200, ""});
  SnapshotGetResult r = get(http);
  TEST_ASSERT_EQUAL(200, r.code);
  TEST_ASSERT_FALSE(r.usedDigest);
  TEST_ASSERT_FALSE(r.challengeChanged);
  TEST_ASSERT_EQUAL(1, (int)http.requests.size());
  TEST_ASSERT_EQUAL_STRING("Basic admin:pw", http.requests[0].authorization.c_str());
}

// The bug that shipped: the retry went out with the Basic credentials again,
// so the camera answered 401 for a correct password.
void test_digest_retry_really_sends_the_digest_header(void) {
  FakeHttp http;
  http.responses.push_back({401, kChallenge});
  http.responses.push_back({200, ""});
  SnapshotGetResult r = get(http);
  TEST_ASSERT_EQUAL(200, r.code);
  TEST_ASSERT_TRUE(r.usedDigest);
  TEST_ASSERT_EQUAL(2, (int)http.requests.size());
  TEST_ASSERT_EQUAL_STRING(digestFor(kChallenge).c_str(), http.requests[1].authorization.c_str());
  TEST_ASSERT_EQUAL(1, http.ends); // the refused connection was closed first
}

void test_successful_challenge_is_remembered(void) {
  FakeHttp http;
  http.responses.push_back({401, kChallenge});
  http.responses.push_back({200, ""});
  SnapshotGetResult r = get(http);
  TEST_ASSERT_TRUE(r.challengeChanged);
  TEST_ASSERT_EQUAL_STRING(kChallenge, r.challenge.c_str());
}

void test_remembered_challenge_skips_the_refused_round_trip(void) {
  FakeHttp http;
  http.responses.push_back({200, ""});
  SnapshotGetResult r = get(http, kChallenge);
  TEST_ASSERT_EQUAL(200, r.code);
  TEST_ASSERT_TRUE(r.usedDigest);
  TEST_ASSERT_FALSE(r.challengeChanged);
  TEST_ASSERT_EQUAL(1, (int)http.requests.size());
  TEST_ASSERT_EQUAL_STRING(digestFor(kChallenge).c_str(), http.requests[0].authorization.c_str());
}

void test_expired_challenge_is_replaced_by_the_fresh_one(void) {
  FakeHttp http;
  http.responses.push_back({401, kNewChallenge});
  http.responses.push_back({200, ""});
  SnapshotGetResult r = get(http, kChallenge);
  TEST_ASSERT_EQUAL(200, r.code);
  TEST_ASSERT_EQUAL_STRING(digestFor(kNewChallenge).c_str(), http.requests[1].authorization.c_str());
  TEST_ASSERT_EQUAL_STRING(kNewChallenge, r.challenge.c_str());
}

void test_wrong_password_forgets_the_challenge_and_stops(void) {
  FakeHttp http;
  http.responses.push_back({401, kChallenge});
  http.responses.push_back({401, kChallenge});
  SnapshotGetResult r = get(http);
  TEST_ASSERT_EQUAL(401, r.code);
  TEST_ASSERT_EQUAL(2, (int)http.requests.size()); // one retry, no more
  TEST_ASSERT_TRUE(r.challengeChanged);
  TEST_ASSERT_EQUAL_STRING("", r.challenge.c_str());
}

void test_basic_refusal_is_not_retried(void) {
  FakeHttp http;
  http.responses.push_back({401, "Basic realm=\"cam\""});
  SnapshotGetResult r = get(http);
  TEST_ASSERT_EQUAL(401, r.code);
  TEST_ASSERT_FALSE(r.usedDigest);
  TEST_ASSERT_FALSE(r.challengeChanged);
  TEST_ASSERT_EQUAL(1, (int)http.requests.size());
}

void test_unsupported_digest_algorithm_sends_no_second_request(void) {
  FakeHttp http;
  http.responses.push_back({401, "Digest realm=\"x\", nonce=\"n\", algorithm=SHA-256"});
  SnapshotGetResult r = get(http);
  TEST_ASSERT_EQUAL(401, r.code);
  TEST_ASSERT_EQUAL(1, (int)http.requests.size());
  TEST_ASSERT_EQUAL_STRING("", r.challenge.c_str());
}

void test_other_errors_pass_through_untouched(void) {
  FakeHttp http;
  http.responses.push_back({500, ""});
  SnapshotGetResult r = get(http);
  TEST_ASSERT_EQUAL(500, r.code);
  TEST_ASSERT_EQUAL(0, http.ends); // the caller still owns the connection
}

// ---- reading the body ----

class FakeClock {
 public:
  uint32_t now = 0;
  uint32_t nowMs() { return now; }
  void sleepMs(uint32_t ms) { now += ms; }
};

// `arrivals` are {time, bytes}: bytes that become readable at that time.
class FakeConn {
 public:
  FakeConn(FakeClock& clock, std::vector<std::pair<uint32_t, size_t>> arrivals, bool staysOpen)
      : clock_(clock), arrivals_(arrivals), staysOpen_(staysOpen) {}
  bool connected() { return staysOpen_ || available() > 0 || future(); }
  size_t available() {
    size_t ready = 0;
    for (auto& a : arrivals_) if (a.first <= clock_.now) ready += a.second;
    return ready - consumed_;
  }
  size_t read(uint8_t* buf, size_t n) {
    TEST_ASSERT_TRUE(n <= available());
    for (size_t i = 0; i < n; i++) buf[i] = 0xAB;
    consumed_ += n;
    return n;
  }
  size_t consumed() const { return consumed_; }

 private:
  bool future() {
    for (auto& a : arrivals_) if (a.first > clock_.now) return true;
    return false;
  }
  FakeClock& clock_;
  std::vector<std::pair<uint32_t, size_t>> arrivals_;
  size_t consumed_ = 0;
  bool staysOpen_;
};

static uint8_t g_buf[4096];

// A Reolink keeps the connection open after the image; reading toward a larger
// cap waited out the whole stall and failed the send.
void test_stops_at_content_length_without_waiting_for_a_kept_alive_connection(void) {
  FakeClock clock;
  FakeConn conn(clock, {{0, 1000}}, true);
  SnapshotBodyRead r = readSnapshotBody(conn, clock, g_buf, 1000, 5000);
  TEST_ASSERT_EQUAL(1000, (int)r.total);
  TEST_ASSERT_FALSE(r.stalled);
  TEST_ASSERT_EQUAL_UINT32(0, clock.now);
}

void test_never_reads_past_what_was_asked_for(void) {
  FakeClock clock;
  FakeConn conn(clock, {{0, 3000}}, true);
  SnapshotBodyRead r = readSnapshotBody(conn, clock, g_buf, 1000, 5000);
  TEST_ASSERT_EQUAL(1000, (int)r.total);
  TEST_ASSERT_EQUAL(1000, (int)conn.consumed());
}

void test_slow_data_is_collected_across_arrivals(void) {
  FakeClock clock;
  FakeConn conn(clock, {{0, 100}, {4000, 100}, {8000, 100}}, true);
  SnapshotBodyRead r = readSnapshotBody(conn, clock, g_buf, 300, 5000);
  TEST_ASSERT_EQUAL(300, (int)r.total); // each arrival restarts the stall timer
  TEST_ASSERT_FALSE(r.stalled);
}

void test_stall_gives_up_with_what_arrived(void) {
  FakeClock clock;
  FakeConn conn(clock, {{0, 100}}, true);
  SnapshotBodyRead r = readSnapshotBody(conn, clock, g_buf, 1000, 5000);
  TEST_ASSERT_EQUAL(100, (int)r.total);
  TEST_ASSERT_TRUE(r.stalled);
  TEST_ASSERT_TRUE(clock.now > 5000 && clock.now < 5010);
}

void test_closed_connection_ends_the_read_without_a_stall(void) {
  FakeClock clock;
  FakeConn conn(clock, {{0, 100}}, false);
  SnapshotBodyRead r = readSnapshotBody(conn, clock, g_buf, 1000, 5000);
  TEST_ASSERT_EQUAL(100, (int)r.total);
  TEST_ASSERT_FALSE(r.stalled);
  TEST_ASSERT_EQUAL_UINT32(0, clock.now);
}

void test_read_cap_uses_content_length_when_there_is_one(void) {
  TEST_ASSERT_EQUAL(85000, (int)snapshotReadCap(85000, 2000000));
  TEST_ASSERT_EQUAL(2000000, (int)snapshotReadCap(-1, 2000000));
  TEST_ASSERT_EQUAL(2000000, (int)snapshotReadCap(0, 2000000));
}

void test_chunked_detection_ignores_case(void) {
  TEST_ASSERT_TRUE(snapshotBodyIsChunked("Chunked"));
  TEST_ASSERT_TRUE(snapshotBodyIsChunked("gzip, chunked"));
  TEST_ASSERT_FALSE(snapshotBodyIsChunked("identity"));
  TEST_ASSERT_FALSE(snapshotBodyIsChunked(""));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_basic_camera_takes_one_request);
  RUN_TEST(test_digest_retry_really_sends_the_digest_header);
  RUN_TEST(test_successful_challenge_is_remembered);
  RUN_TEST(test_remembered_challenge_skips_the_refused_round_trip);
  RUN_TEST(test_expired_challenge_is_replaced_by_the_fresh_one);
  RUN_TEST(test_wrong_password_forgets_the_challenge_and_stops);
  RUN_TEST(test_basic_refusal_is_not_retried);
  RUN_TEST(test_unsupported_digest_algorithm_sends_no_second_request);
  RUN_TEST(test_other_errors_pass_through_untouched);
  RUN_TEST(test_stops_at_content_length_without_waiting_for_a_kept_alive_connection);
  RUN_TEST(test_never_reads_past_what_was_asked_for);
  RUN_TEST(test_slow_data_is_collected_across_arrivals);
  RUN_TEST(test_stall_gives_up_with_what_arrived);
  RUN_TEST(test_closed_connection_ends_the_read_without_a_stall);
  RUN_TEST(test_read_cap_uses_content_length_when_there_is_one);
  RUN_TEST(test_chunked_detection_ignores_case);
  return UNITY_END();
}
