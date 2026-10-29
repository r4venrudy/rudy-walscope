#include "walscope.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hash_vectors.h"

static unsigned checks = 0;
static void check(bool condition, const char *message) {
  ++checks; if (!condition) { fprintf(stderr, "Failed: %s\n", message); exit(1); }
}
static void hashes(void) {
  for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
    size_t length = vectors[i].length;
    uint8_t *input = (uint8_t *)malloc(length ? length : 1); check(input != NULL, "hash allocation");
    for (size_t k = 0; k < length; ++k) input[k] = (uint8_t)(k % 251);
    uint8_t hash[32]; char hex[65]; ws_digest(input, length, hash); ws_hex(hash, hex);
    check(strcmp(hex, vectors[i].hash) == 0, "official BLAKE3 one-shot vector");
    blake3_hasher state; blake3_hasher_init(&state);
    size_t offset = 0;
    while (offset < length) { size_t n = length - offset < 17 ? length - offset : 17; blake3_hasher_update(&state, input + offset, n); offset += n; }
    blake3_hasher_finalize(&state, hash, 32); ws_hex(hash, hex);
    check(strcmp(hex, vectors[i].hash) == 0, "official BLAKE3 segmented vector"); free(input);
  }
}
static void checksums(void) {
  uint8_t words[16]; for (unsigned i = 0; i < 16; ++i) words[i] = (uint8_t)i;
  check(ws_u32(words, true) == 0x00010203U, "big endian word");
  check(ws_u32(words, false) == 0x03020100U, "little endian word");
  uint32_t little[2] = {0, 0}, big[2] = {0, 0};
  ws_checksum(words, 16, false, little); ws_checksum(words, 16, true, big);
  check(little[0] == 0x1814100cU && little[1] == 0x312a231cU, "little endian checksum recurrence");
  check(big[0] == 0x0c101418U && big[1] == 0x1c232a31U, "big endian checksum recurrence");
  uint32_t segmented[2] = {0, 0}; ws_checksum(words, 8, false, segmented); ws_checksum(words + 8, 8, false, segmented);
  check(memcmp(segmented, little, sizeof(little)) == 0, "checksum chaining across record segments");
  uint32_t wrap[2] = {UINT32_MAX, UINT32_MAX}; uint8_t zero[8] = {0}; ws_checksum(zero, 8, false, wrap);
  check(wrap[0] == UINT32_MAX - 1U && wrap[1] == UINT32_MAX - 2U, "checksum wraps modulo 2^32");
}
static void buffers(void) {
  WsBuffer b = {0}; ws_json_string(&b, "quote\"slash\\control\n");
  check(!b.failed && strcmp(b.data, "\"quote\\\"slash\\\\control\\u000a\"") == 0, "JSON escaping");
  ws_buffer_free(&b); ws_append(&b, "%s:%u", "test", 17U);
  check(b.size == 7 && strcmp(b.data, "test:17") == 0, "bounded formatted append"); ws_buffer_free(&b);
  char *large = (char *)malloc((size_t)WS_REPORT_MAX + 2); check(large != NULL, "output cap allocation");
  memset(large, 'a', (size_t)WS_REPORT_MAX + 1); large[WS_REPORT_MAX + 1] = '\0';
  ws_append(&b, "%s", large); check(b.failed, "oversized report refused"); free(large); ws_buffer_free(&b);
}
int main(void) { hashes(); checksums(); buffers(); printf("WALScope native assertions passed: %u\n", checks); return 0; }
