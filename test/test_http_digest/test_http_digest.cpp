#include <unity.h>
#include "http_digest.h"

void setUp(void) {}
void tearDown(void) {}

// RFC 1321 test suite.
void test_md5_known_vectors(void) {
  TEST_ASSERT_EQUAL_STRING("d41d8cd98f00b204e9800998ecf8427e", md5Hex("").c_str());
  TEST_ASSERT_EQUAL_STRING("0cc175b9c0f1b6a831c399e269772661", md5Hex("a").c_str());
  TEST_ASSERT_EQUAL_STRING("900150983cd24fb0d6963f7d28e17f72", md5Hex("abc").c_str());
  TEST_ASSERT_EQUAL_STRING("f96b697d7cb7938d525a2f31aaf161d0", md5Hex("message digest").c_str());
  TEST_ASSERT_EQUAL_STRING("57edf4a22be3c955ac49da2e2107b67a",
                           md5Hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890").c_str());
}

// RFC 2617 section 3.5 example.
void test_digest_rfc2617_example(void) {
  String challenge = "Digest realm=\"testrealm@host.com\", qop=\"auth,auth-int\", "
                     "nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\", opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"";
  String auth = digestAuthorization(challenge, "GET", "/dir/index.html", "Mufasa", "Circle Of Life", "0a4f113b");
  TEST_ASSERT_TRUE(auth.indexOf("response=\"6629fae49393a05397450978507c4ef1\"") >= 0);
  TEST_ASSERT_TRUE(auth.indexOf("qop=auth, nc=00000001, cnonce=\"0a4f113b\"") >= 0);
  TEST_ASSERT_TRUE(auth.indexOf("opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"") >= 0);
  TEST_ASSERT_TRUE(auth.startsWith("Digest username=\"Mufasa\""));
}

// Old style without qop: response = MD5(HA1:nonce:HA2).
void test_digest_without_qop(void) {
  String auth = digestAuthorization("Digest realm=\"r\", nonce=\"n\"", "GET", "/x", "u", "p", "c");
  String expected = md5Hex(md5Hex("u:r:p") + ":n:" + md5Hex("GET:/x"));
  TEST_ASSERT_TRUE(auth.indexOf("response=\"" + expected + "\"") >= 0);
  TEST_ASSERT_EQUAL(-1, auth.indexOf("qop="));
}

void test_digest_rejects_non_digest_and_other_algorithms(void) {
  TEST_ASSERT_EQUAL_STRING("", digestAuthorization("Basic realm=\"x\"", "GET", "/", "u", "p", "c").c_str());
  TEST_ASSERT_EQUAL_STRING("",
                           digestAuthorization("Digest realm=\"x\", nonce=\"n\", algorithm=SHA-256", "GET", "/", "u", "p", "c").c_str());
  TEST_ASSERT_EQUAL_STRING("",
                           digestAuthorization("Digest realm=\"x\", nonce=\"n\", qop=\"auth-int\"", "GET", "/", "u", "p", "c").c_str());
}

void test_digest_parses_unquoted_and_cnonce_safe_values(void) {
  String auth = digestAuthorization("digest realm=r, cnonce=\"bad\", nonce=good, algorithm=MD5", "GET", "/", "u", "p", "c");
  TEST_ASSERT_TRUE(auth.indexOf("nonce=\"good\"") >= 0);
  TEST_ASSERT_TRUE(auth.indexOf("algorithm=MD5") >= 0);
}

void test_request_uri_from_url(void) {
  TEST_ASSERT_EQUAL_STRING("/cgi-bin/snapshot.cgi?a=1", requestUriFromUrl("http://10.0.0.1:81/cgi-bin/snapshot.cgi?a=1").c_str());
  TEST_ASSERT_EQUAL_STRING("/", requestUriFromUrl("http://10.0.0.1").c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_md5_known_vectors);
  RUN_TEST(test_digest_rfc2617_example);
  RUN_TEST(test_digest_without_qop);
  RUN_TEST(test_digest_rejects_non_digest_and_other_algorithms);
  RUN_TEST(test_digest_parses_unquoted_and_cnonce_safe_values);
  RUN_TEST(test_request_uri_from_url);
  return UNITY_END();
}
