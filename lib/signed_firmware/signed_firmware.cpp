#include "signed_firmware.h"

static const char kMagic[8] = {'A', 'O', 'T', 'S', 'I', 'G', '1', '\n'};
// Shortest DER encoding worth accepting: a real P-256 signature is 68-72 bytes.
static const size_t kMinSignatureBytes = 8;

void SignedFirmwareReader::reset(bool allowUnsigned) {
  state_ = State::Start;
  allowUnsigned_ = allowUnsigned;
  headerBytes_ = 0;
}

void SignedFirmwareReader::validateHeader() {
  bool ok = memcmp(header_, kMagic, sizeof(kMagic)) == 0 && header_[8] >= kMinSignatureBytes &&
            header_[8] <= SIGNED_FIRMWARE_MAX_SIGNATURE_BYTES;
  for (size_t i = 9; ok && i < 16; i++) ok = header_[i] == 0;
  state_ = ok ? State::Image : State::Rejected;
}

void SignedFirmwareReader::feed(const uint8_t* data, size_t len, const uint8_t*& image, size_t& imageLen) {
  image = nullptr;
  imageLen = 0;
  if (len == 0 || state_ == State::Rejected) return;

  if (state_ == State::Start) {
    // An app image starts with 0xE9, never the magic's first byte.
    if (data[0] != (uint8_t)kMagic[0]) {
      state_ = allowUnsigned_ ? State::Unsigned : State::Rejected;
      if (state_ == State::Rejected) return;
    } else {
      state_ = State::Header;
    }
  }

  if (state_ == State::Header) {
    size_t take = SIGNED_FIRMWARE_HEADER_BYTES - headerBytes_;
    if (take > len) take = len;
    memcpy(header_ + headerBytes_, data, take);
    headerBytes_ += take;
    data += take;
    len -= take;
    if (headerBytes_ < SIGNED_FIRMWARE_HEADER_BYTES) return;
    validateHeader();
    if (state_ == State::Rejected) return;
  }

  image = data;
  imageLen = len;
}
