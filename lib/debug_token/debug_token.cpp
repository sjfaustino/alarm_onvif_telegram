#include "debug_token.h"
#include <cstring>

bool debugTokenValid(const char* configured, const char* supplied) {
  if (!configured || !supplied) return false;
  size_t len = strlen(configured);
  if (len == 0 || strlen(supplied) != len) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < len; i++) diff |= (unsigned char)(configured[i] ^ supplied[i]);
  return diff == 0;
}
