#include "walscope.h"
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <share.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

void ws_error(WsError *e, const char *format, ...) {
  va_list args; va_start(args, format);
  (void)vsnprintf(e->text, sizeof(e->text), format, args); va_end(args);
  e->text[sizeof(e->text) - 1] = '\0';
}
WsLimits ws_limits(void) {
  WsLimits l = {UINT64_C(512) * 1024 * 1024, UINT64_C(512) * 1024 * 1024, 200000, 100}; return l;
}
void ws_digest(const void *data, size_t size, uint8_t digest[32]) {
  blake3_hasher hash; blake3_hasher_init(&hash); blake3_hasher_update(&hash, data, size); blake3_hasher_finalize(&hash, digest, 32);
}
void ws_hex(const uint8_t digest[32], char result[65]) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; ++i) { result[i * 2] = hex[digest[i] >> 4]; result[i * 2 + 1] = hex[digest[i] & 15]; }
  result[64] = '\0';
}
#ifdef _WIN32
static wchar_t *wide_path(const char *path) {
  int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
  if (count <= 0 || count > 32768) return NULL;
  wchar_t *result = (wchar_t *)malloc((size_t)count * sizeof(wchar_t));
  if (!result) return NULL;
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, result, count) != count) { free(result); return NULL; }
  return result;
}
#endif
bool ws_seek(FILE *file, uint64_t offset, WsError *error) {
  if (offset > INT64_MAX) { ws_error(error, "File offset exceeds signed 64-bit range"); return false; }
#ifdef _WIN32
  if (_fseeki64(file, (int64_t)offset, SEEK_SET) != 0) {
#else
  if (fseeko(file, (off_t)offset, SEEK_SET) != 0) {
#endif
    ws_error(error, "Cannot seek input or staging file"); return false;
  }
  return true;
}
static bool file_size(FILE *file, uint64_t *size, WsError *error) {
#ifdef _WIN32
  struct _stat64 st;
  if (_fstat64(_fileno(file), &st) != 0 || (st.st_mode & _S_IFMT) != _S_IFREG || st.st_size < 0) {
#else
  struct stat st;
  if (fstat(fileno(file), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
#endif
    ws_error(error, "Input must be a readable regular file"); return false;
  }
  *size = (uint64_t)st.st_size; return true;
}
static bool hash_file(FILE *file, uint64_t size, uint8_t digest[32], WsError *error) {
  if (!ws_seek(file, 0, error)) return false;
  blake3_hasher hash; blake3_hasher_init(&hash);
  uint8_t buffer[65536]; uint64_t left = size;
  while (left) {
    size_t want = left < sizeof(buffer) ? (size_t)left : sizeof(buffer);
    if (fread(buffer, 1, want, file) != want) { ws_error(error, "Input changed or became unreadable while hashing"); return false; }
    blake3_hasher_update(&hash, buffer, want); left -= want;
  }
  if (fgetc(file) != EOF || ferror(file)) { ws_error(error, "Input size changed while hashing"); return false; }
  blake3_hasher_finalize(&hash, digest, 32); return true;
}
bool ws_open_input(const char *path, uint64_t maximum, WsInput *input, WsError *error) {
  memset(input, 0, sizeof(*input));
#ifdef _WIN32
  wchar_t *wide = wide_path(path);
  if (wide) { input->file = _wfsopen(wide, L"rb", _SH_DENYWR); free(wide); }
#else
  int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd >= 0) { input->file = fdopen(fd, "rb"); if (!input->file) (void)close(fd); }
#endif
  if (!input->file) { ws_error(error, "Cannot open selected input file"); return false; }
  if (!file_size(input->file, &input->size, error)) { ws_close_input(input); return false; }
  if (input->size > maximum) { ws_error(error, "Input byte budget exceeded"); ws_close_input(input); return false; }
  if (!hash_file(input->file, input->size, input->digest, error)) { ws_close_input(input); return false; }
  return true;
}
bool ws_stable(WsInput *input, WsError *error) {
  uint8_t digest[32]; uint64_t size;
  if (!file_size(input->file, &size, error)) return false;
  if (size != input->size || !hash_file(input->file, input->size, digest, error)) { ws_error(error, "Input size or content changed during analysis"); return false; }
  if (memcmp(digest, input->digest, 32) != 0) { ws_error(error, "Input content changed during analysis"); return false; }
  return true;
}
void ws_close_input(WsInput *input) { if (input->file) (void)fclose(input->file); memset(input, 0, sizeof(*input)); }
bool ws_read_at(WsInput *input, uint64_t offset, void *buffer, size_t size, WsError *error) {
  if (offset > input->size || (uint64_t)size > input->size - offset) { ws_error(error, "Read extends outside captured input"); return false; }
  if (!ws_seek(input->file, offset, error)) return false;
  if (fread(buffer, 1, size, input->file) != size) { ws_error(error, "Input became unreadable"); return false; }
  return true;
}
FILE *ws_create(const char *path, bool readwrite, WsError *error) {
  FILE *file = NULL;
#ifdef _WIN32
  wchar_t *wide = wide_path(path); int fd = -1;
  if (wide) {
    int flags = _O_CREAT | _O_EXCL | _O_BINARY | (readwrite ? _O_RDWR : _O_WRONLY);
    if (_wsopen_s(&fd, wide, flags, _SH_DENYRW, _S_IREAD | _S_IWRITE) == 0) {
      file = _fdopen(fd, readwrite ? "w+b" : "wb"); if (!file) (void)_close(fd);
    }
    free(wide);
  }
#else
  int fd = open(path, O_CREAT | O_EXCL | O_CLOEXEC | (readwrite ? O_RDWR : O_WRONLY), 0600);
  if (fd >= 0) { file = fdopen(fd, readwrite ? "w+b" : "wb"); if (!file) (void)close(fd); }
#endif
  if (!file) ws_error(error, "Output must be a new writable file; existing files and links are refused");
  return file;
}
void ws_remove_own(const char *path) {
#ifdef _WIN32
  wchar_t *wide = wide_path(path); if (wide) { (void)_wremove(wide); free(wide); }
#else
  (void)remove(path);
#endif
}
bool ws_copy_file(FILE *from, FILE *to, uint64_t size, WsError *error) {
  uint8_t buffer[65536];
  if (!ws_seek(from, 0, error)) return false;
  while (size) {
    size_t want = size < sizeof(buffer) ? (size_t)size : sizeof(buffer);
    if (fread(buffer, 1, want, from) != want || fwrite(buffer, 1, want, to) != want) { ws_error(error, "Cannot write complete output; the new file may be incomplete"); return false; }
    size -= want;
  }
  return true;
}
bool ws_write_report(const char *path, const WsBuffer *b, WsError *error) {
  if (b->failed) { ws_error(error, "Report allocation or 16 MiB output budget failed"); return false; }
  FILE *file = path ? ws_create(path, false, error) : stdout;
  if (!file) return false;
  bool ok = fwrite(b->data ? b->data : "", 1, b->size, file) == b->size;
  if (path) { if (fclose(file) != 0) ok = false; }
  else if (fflush(file) != 0) ok = false;
  if (!ok) ws_error(error, "Cannot write complete report; a new file may be incomplete");
  return ok;
}
void ws_buffer_free(WsBuffer *b) { free(b->data); memset(b, 0, sizeof(*b)); }
void ws_append(WsBuffer *b, const char *format, ...) {
  if (b->failed) return;
  va_list args, copy; va_start(args, format); va_copy(copy, args);
  int count = vsnprintf(NULL, 0, format, copy); va_end(copy);
  if (count < 0 || (size_t)count > WS_REPORT_MAX - b->size) { b->failed = true; va_end(args); return; }
  size_t required = b->size + (size_t)count + 1;
  if (required > b->capacity) {
    size_t capacity = b->capacity ? b->capacity : 1024;
    while (capacity < required) capacity *= 2;
    if (capacity > (size_t)WS_REPORT_MAX + 1) capacity = (size_t)WS_REPORT_MAX + 1;
    char *data = (char *)realloc(b->data, capacity);
    if (!data) { b->failed = true; va_end(args); return; }
    b->data = data; b->capacity = capacity;
  }
  (void)vsnprintf(b->data + b->size, b->capacity - b->size, format, args); va_end(args); b->size += (size_t)count;
}
void ws_json_string(WsBuffer *b, const char *s) {
  ws_append(b, "\"");
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    if (*p == '"' || *p == '\\') ws_append(b, "\\%c", *p);
    else if (*p < 32 || *p >= 127) ws_append(b, "\\u%04x", (unsigned)*p);
    else ws_append(b, "%c", *p);
  }
  ws_append(b, "\"");
}
