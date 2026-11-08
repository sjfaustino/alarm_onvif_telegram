#pragma once
#include <Arduino.h> // explicit: PlatformIO's LDF only scans a lib's own includes

// The signed firmware file: a fixed header, then the normal app image. The
// header holds an ECDSA P-256 signature (DER) over the SHA-256 of the image.
//
//   8 bytes  magic "AOTSIG1\n"
//   1 byte   signature length
//   7 bytes  zero
//  72 bytes  signature, zero-padded
//
// scripts/board.py writes this format. Splitting it from the upload chunks is
// pure and tested natively; the signature check itself needs mbedTLS and lives
// in firmware_signature.cpp.

static const size_t SIGNED_FIRMWARE_HEADER_BYTES = 88;
static const size_t SIGNED_FIRMWARE_MAX_SIGNATURE_BYTES = 72;

class SignedFirmwareReader {
 public:
  // allowUnsigned lets a plain app image through, for the build that introduces
  // signing to a board whose uploads weren't signed yet.
  void reset(bool allowUnsigned);

  // Takes the next upload chunk. image/imageLen are the part of it that is
  // firmware (none while the header is still arriving, or once rejected).
  void feed(const uint8_t* data, size_t len, const uint8_t*& image, size_t& imageLen);

  bool rejected() const { return state_ == State::Rejected; }
  bool isUnsigned() const { return state_ == State::Unsigned; }
  bool headerComplete() const { return state_ == State::Image; }

  const uint8_t* signature() const { return header_ + 16; }
  size_t signatureLength() const { return header_[8]; }

 private:
  enum class State { Start, Header, Image, Unsigned, Rejected };
  void validateHeader();

  State state_ = State::Start;
  bool allowUnsigned_ = false;
  size_t headerBytes_ = 0;
  uint8_t header_[SIGNED_FIRMWARE_HEADER_BYTES] = {0};
};
