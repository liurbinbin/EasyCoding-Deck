#pragma once

#include <cstddef>
#include <cstdint>

namespace s3transport {

inline bool isHex(const char* value, size_t length, size_t expected_length) {
  if (!value || length != expected_length) return false;
  for (size_t index = 0; index < length; ++index) {
    const char character = value[index];
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f') ||
          (character >= 'A' && character <= 'F'))) return false;
  }
  return true;
}

inline bool constantTimeEqual(const uint8_t* left, const uint8_t* right,
                              size_t length) {
  if (!left || !right) return false;
  uint8_t difference = 0;
  for (size_t index = 0; index < length; ++index)
    difference |= left[index] ^ right[index];
  return difference == 0;
}

class SequenceCounter {
 public:
  void reset() { expected_ = 0; }
  bool accept(uint32_t sequence) {
    if (sequence != expected_) return false;
    ++expected_;
    return true;
  }
  uint32_t expected() const { return expected_; }

 private:
  uint32_t expected_ = 0;
};

}  // namespace s3transport
