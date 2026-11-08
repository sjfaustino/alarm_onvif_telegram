#pragma once
#include <stddef.h>
#include <stdint.h>

// Checks an uploaded image against FIRMWARE_PUBLIC_KEY_PEM. The hash runs over
// the image as it arrives; a failed check means it must not be booted.
void firmwareHashStart();
void firmwareHashUpdate(const uint8_t* data, size_t len);
bool firmwareHashMatchesSignature(const uint8_t* signature, size_t signatureLen);
