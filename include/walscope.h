#ifndef RUDY_WALSCOPE_H
#define RUDY_WALSCOPE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "blake3.h"

#define WS_VERSION "0.1.0-preview"
#define WS_ERROR_SIZE 256
#define WS_REPORT_MAX (16U * 1024U * 1024U)
typedef struct { char text[WS_ERROR_SIZE]; } WsError;
typedef struct { uint64_t max_input, max_output; uint32_t max_frames, rows; } WsLimits;
typedef struct { FILE *file; uint64_t size; uint8_t digest[32]; } WsInput;
typedef struct {
  uint64_t offset;
  uint32_t page, db_pages, transaction;
  uint8_t digest[32];
} WsFrame;
typedef struct { uint32_t first, last, db_pages, unique_pages; } WsCommit;
typedef enum { WS_TAIL_CLEAN, WS_TAIL_PARTIAL, WS_TAIL_SALT, WS_TAIL_CHECKSUM, WS_TAIL_PAGE } WsTail;
typedef struct {
  WsInput input;
  uint32_t page_size, sequence, salt[2], checksum[2];
  bool checksum_big;
  WsFrame *frames; WsCommit *commits;
  uint32_t frame_count, commit_count, last_commit_frame;
  uint32_t frame_capacity, commit_capacity;
  WsTail tail;
  uint64_t tail_offset;
} WsWal;
typedef struct { WsInput input; uint32_t page_size, pages; } WsBase;
typedef struct { uint32_t *source, pages, ordinal, last_frame; } WsSnapshot;
typedef struct { uint32_t page, before_frame, after_frame; int kind; uint8_t before_hash[32], after_hash[32]; } WsChange;
typedef struct {
  WsSnapshot before, after;
  uint32_t added, removed, modified, unchanged;
  WsChange *rows; uint32_t row_count;
} WsDiff;
typedef struct { char *data; size_t size, capacity; bool failed; } WsBuffer;

void ws_error(WsError *, const char *, ...);
WsLimits ws_limits(void);
uint32_t ws_u32(const uint8_t *, bool);
void ws_checksum(const uint8_t *, size_t, bool, uint32_t [2]);
void ws_digest(const void *, size_t, uint8_t [32]);
void ws_hex(const uint8_t [32], char [65]);
bool ws_open_input(const char *, uint64_t, WsInput *, WsError *);
bool ws_stable(WsInput *, WsError *);
void ws_close_input(WsInput *);
bool ws_seek(FILE *, uint64_t, WsError *);
bool ws_read_at(WsInput *, uint64_t, void *, size_t, WsError *);
FILE *ws_create(const char *, bool, WsError *);
void ws_remove_own(const char *);
bool ws_copy_file(FILE *, FILE *, uint64_t, WsError *);
bool ws_write_report(const char *, const WsBuffer *, WsError *);
void ws_buffer_free(WsBuffer *);
void ws_append(WsBuffer *, const char *, ...);
void ws_json_string(WsBuffer *, const char *);

bool ws_scan(const char *, const WsLimits *, WsWal *, WsError *);
void ws_wal_free(WsWal *);
const char *ws_tail_name(WsTail);
bool ws_open_base(const char *, const WsLimits *, uint32_t, WsBase *, WsError *);
bool ws_snapshot(const WsWal *, const WsBase *, uint32_t, const WsLimits *, WsSnapshot *, WsError *);
void ws_snapshot_free(WsSnapshot *);
bool ws_page_bytes(WsWal *, WsBase *, const WsSnapshot *, uint32_t, uint8_t *, WsError *);
bool ws_diff(WsWal *, WsBase *, uint32_t, uint32_t, const WsLimits *, WsDiff *, WsError *);
void ws_diff_free(WsDiff *);
bool ws_recover(WsWal *, WsBase *, const WsSnapshot *, const char *, uint8_t [32], WsError *);

void ws_report_inspect(const WsWal *, uint32_t, uint32_t, WsBuffer *);
void ws_report_diff(const WsWal *, const WsBase *, const WsDiff *, WsBuffer *);
void ws_report_recover(const WsWal *, const WsBase *, const WsSnapshot *, const uint8_t [32], bool, WsBuffer *);
void ws_render_html(const WsWal *, const char *, uint32_t, const WsBuffer *, WsBuffer *);
void ws_render_text(const WsWal *, const char *, uint32_t, const WsBuffer *, WsBuffer *);
#endif
