#include "firmware_signature.h"
#include "firmware_signing_key.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <string.h>

static mbedtls_sha256_context g_hash;

void firmwareHashStart() {
  mbedtls_sha256_free(&g_hash);
  mbedtls_sha256_init(&g_hash);
  mbedtls_sha256_starts(&g_hash, 0);
}

void firmwareHashUpdate(const uint8_t* data, size_t len) { mbedtls_sha256_update(&g_hash, data, len); }

// One check at a time: the web server handles one upload at a time.
static struct {
  uint8_t digest[32];
  const uint8_t* signature;
  size_t signatureLen;
  bool valid;
  SemaphoreHandle_t done;
} g_check;

static void verifyTask(void*) {
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  // The PEM parser counts the terminating NUL.
  bool valid = mbedtls_pk_parse_public_key(&key, (const unsigned char*)FIRMWARE_PUBLIC_KEY_PEM,
                                           strlen(FIRMWARE_PUBLIC_KEY_PEM) + 1) == 0 &&
               mbedtls_pk_can_do(&key, MBEDTLS_PK_ECDSA) &&
               mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, g_check.digest, sizeof(g_check.digest), g_check.signature,
                                 g_check.signatureLen) == 0;
  mbedtls_pk_free(&key);
  g_check.valid = valid;
  xSemaphoreGive(g_check.done);
  vTaskDelete(nullptr);
}

bool firmwareHashMatchesSignature(const uint8_t* signature, size_t signatureLen) {
  if (mbedtls_sha256_finish(&g_hash, g_check.digest) != 0) return false;
  mbedtls_sha256_free(&g_hash);

  if (!g_check.done) g_check.done = xSemaphoreCreateBinary();
  if (!g_check.done) return false;
  g_check.signature = signature;
  g_check.signatureLen = signatureLen;
  g_check.valid = false;
  // Its own task: ECDSA needs more stack than the web server's task can spare.
  if (xTaskCreate(verifyTask, "otaVerify", 10240, nullptr, tskIDLE_PRIORITY + 1, nullptr) != pdPASS) return false;
  xSemaphoreTake(g_check.done, portMAX_DELAY);
  return g_check.valid;
}
