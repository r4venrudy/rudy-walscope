#include "walscope.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

uint32_t ws_u32(const uint8_t *p, bool big) {
  if (big) return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
  return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}
void ws_checksum(const uint8_t *data, size_t size, bool big, uint32_t state[2]) {
  for (size_t i = 0; i < size; i += 8) {
    state[0] += ws_u32(data + i, big) + state[1];
    state[1] += ws_u32(data + i + 4, big) + state[0];
  }
}
static bool page_size_valid(uint32_t n) { return n >= 512 && n <= 65536 && (n & (n - 1)) == 0; }
static int page_compare(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return (x > y) - (x < y);
}
static void *grow(void *array, uint32_t *capacity, uint32_t count, size_t element, uint32_t maximum, WsError *error) {
  if (count < *capacity) return array;
  uint32_t next = *capacity ? *capacity * 2 : 64;
  if (next > maximum) next = maximum;
  if (next <= count || (size_t)next > SIZE_MAX / element) { ws_error(error, "Frame or allocation budget exhausted"); return NULL; }
  void *p = realloc(array, (size_t)next * element);
  if (!p) { ws_error(error, "Cannot allocate bounded frame metadata"); return NULL; }
  *capacity = next; return p;
}
const char *ws_tail_name(WsTail tail) {
  switch (tail) {
    case WS_TAIL_CLEAN: return "clean";
    case WS_TAIL_PARTIAL: return "partial-frame";
    case WS_TAIL_SALT: return "salt-mismatch";
    case WS_TAIL_CHECKSUM: return "checksum-mismatch";
    case WS_TAIL_PAGE: return "invalid-page-number";
  }
  return "unknown";
}
void ws_wal_free(WsWal *wal) { ws_close_input(&wal->input); free(wal->frames); free(wal->commits); memset(wal, 0, sizeof(*wal)); }
bool ws_scan(const char *path, const WsLimits *limits, WsWal *wal, WsError *error) {
  memset(wal, 0, sizeof(*wal));
  if (!ws_open_input(path, limits->max_input, &wal->input, error)) return false;
  if (wal->input.size == 0) {
    if (!ws_stable(&wal->input, error)) goto failed;
    return true;
  }
  uint8_t header[32], page[65536], frame[24];
  if (wal->input.size < 32 || !ws_read_at(&wal->input, 0, header, 32, error)) { ws_error(error, "WAL header is truncated"); goto failed; }
  uint32_t magic = ws_u32(header, true);
  if (magic != UINT32_C(0x377f0682) && magic != UINT32_C(0x377f0683)) { ws_error(error, "Unsupported WAL magic"); goto failed; }
  if (ws_u32(header + 4, true) != 3007000) { ws_error(error, "Unsupported WAL format version"); goto failed; }
  wal->page_size = ws_u32(header + 8, true);
  if (!page_size_valid(wal->page_size)) { ws_error(error, "WAL page size must be a power of two from 512 through 65536"); goto failed; }
  wal->checksum_big = (magic & 1U) != 0;
  wal->sequence = ws_u32(header + 12, true);
  wal->salt[0] = ws_u32(header + 16, true); wal->salt[1] = ws_u32(header + 20, true);
  uint32_t state[2] = {0, 0}; ws_checksum(header, 24, wal->checksum_big, state);
  if (state[0] != ws_u32(header + 24, true) || state[1] != ws_u32(header + 28, true)) { ws_error(error, "WAL header checksum does not match"); goto failed; }
  memcpy(wal->checksum, state, sizeof(state));
  uint64_t offset = 32, stride = (uint64_t)wal->page_size + 24;
  while (offset < wal->input.size) {
    wal->tail_offset = offset;
    if (wal->input.size - offset < stride) { wal->tail = WS_TAIL_PARTIAL; break; }
    if (wal->frame_count >= limits->max_frames) { ws_error(error, "Frame budget exhausted; no complete analysis was produced"); goto failed; }
    if (!ws_read_at(&wal->input, offset, frame, 24, error)) goto failed;
    if (ws_u32(frame + 8, true) != wal->salt[0] || ws_u32(frame + 12, true) != wal->salt[1]) { wal->tail = WS_TAIL_SALT; break; }
    uint32_t pg = ws_u32(frame, true), pages = ws_u32(frame + 4, true);
    if (pg == 0 || pg == UINT32_MAX || pages == UINT32_MAX) { wal->tail = WS_TAIL_PAGE; break; }
    if (!ws_read_at(&wal->input, offset + 24, page, wal->page_size, error)) goto failed;
    uint32_t next[2] = {state[0], state[1]};
    ws_checksum(frame, 8, wal->checksum_big, next); ws_checksum(page, wal->page_size, wal->checksum_big, next);
    if (next[0] != ws_u32(frame + 16, true) || next[1] != ws_u32(frame + 20, true)) { wal->tail = WS_TAIL_CHECKSUM; break; }
    void *grown = grow(wal->frames, &wal->frame_capacity, wal->frame_count, sizeof(WsFrame), limits->max_frames, error);
    if (!grown) goto failed;
    wal->frames = (WsFrame *)grown;
    WsFrame *row = &wal->frames[wal->frame_count++];
    row->offset = offset; row->page = pg; row->db_pages = pages; row->transaction = wal->commit_count + 1;
    ws_digest(page, wal->page_size, row->digest); memcpy(state, next, sizeof(state));
    if (pages != 0) {
      grown = grow(wal->commits, &wal->commit_capacity, wal->commit_count, sizeof(WsCommit), limits->max_frames, error);
      if (!grown) goto failed;
      wal->commits = (WsCommit *)grown;
      WsCommit *commit = &wal->commits[wal->commit_count++];
      commit->first = wal->last_commit_frame + 1; commit->last = wal->frame_count; commit->db_pages = pages;
      uint32_t length = commit->last - commit->first + 1;
      uint32_t *numbers = (uint32_t *)malloc((size_t)length * sizeof(uint32_t));
      if (!numbers) { ws_error(error, "Cannot allocate transaction page index"); goto failed; }
      for (uint32_t i = 0; i < length; ++i) numbers[i] = wal->frames[commit->first - 1 + i].page;
      qsort(numbers, length, sizeof(uint32_t), page_compare);
      commit->unique_pages = 0;
      for (uint32_t i = 0; i < length; ++i) if (i == 0 || numbers[i] != numbers[i - 1]) ++commit->unique_pages;
      free(numbers); wal->last_commit_frame = wal->frame_count;
    }
    offset += stride;
  }
  if (wal->tail == WS_TAIL_CLEAN) wal->tail_offset = wal->input.size;
  if (!ws_stable(&wal->input, error)) goto failed;
  return true;
failed:
  ws_wal_free(wal); return false;
}
static bool database_header(const uint8_t *header, uint32_t expected, WsError *error) {
  uint32_t size = ((uint32_t)header[16] << 8) | header[17]; if (size == 1) size = 65536;
  if (memcmp(header, "SQLite format 3\0", 16) != 0 || size != expected || !page_size_valid(size)) { ws_error(error, "Database magic or page size is incompatible with the WAL"); return false; }
  if ((header[18] != 1 && header[18] != 2) || (header[19] != 1 && header[19] != 2) || header[21] != 64 || header[22] != 32 || header[23] != 32 || expected - header[20] < 480) {
    ws_error(error, "Unsupported or invalid SQLite database header"); return false;
  }
  return true;
}
bool ws_open_base(const char *path, const WsLimits *limits, uint32_t size, WsBase *base, WsError *error) {
  memset(base, 0, sizeof(*base));
  if (!page_size_valid(size)) { ws_error(error, "An empty WAL has no page size for a base database"); return false; }
  if (!ws_open_input(path, limits->max_input, &base->input, error)) return false;
  base->page_size = size;
  if (base->input.size == 0) return true;
  uint8_t header[100];
  if (base->input.size % size != 0 || base->input.size / size > UINT32_MAX - 1U || base->input.size < 100) { ws_error(error, "Base database must contain complete bounded SQLite pages"); goto failed; }
  if (!ws_read_at(&base->input, 0, header, 100, error) || !database_header(header, size, error)) goto failed;
  base->pages = (uint32_t)(base->input.size / size);
  return true;
failed:
  ws_close_input(&base->input); return false;
}
void ws_snapshot_free(WsSnapshot *snapshot) { free(snapshot->source); memset(snapshot, 0, sizeof(*snapshot)); }
bool ws_snapshot(const WsWal *wal, const WsBase *base, uint32_t ordinal, const WsLimits *limits, WsSnapshot *snapshot, WsError *error) {
  memset(snapshot, 0, sizeof(*snapshot));
  if (ordinal == 0) ordinal = wal->commit_count;
  if (ordinal == 0 || ordinal > wal->commit_count) { ws_error(error, "Selected commit is not in the validated WAL prefix"); return false; }
  const WsCommit *commit = &wal->commits[ordinal - 1];
  if ((uint64_t)commit->db_pages * wal->page_size > limits->max_output) { ws_error(error, "Selected snapshot exceeds the image byte budget"); return false; }
  size_t count = (size_t)commit->db_pages + 1;
  if (count > SIZE_MAX / sizeof(uint32_t)) { ws_error(error, "Snapshot index exceeds addressable memory"); return false; }
  snapshot->source = (uint32_t *)calloc(count, sizeof(uint32_t));
  uint32_t *cutoff = (uint32_t *)calloc(count, sizeof(uint32_t));
  if (!snapshot->source || !cutoff) { ws_error(error, "Cannot allocate bounded snapshot indexes"); free(cutoff); ws_snapshot_free(snapshot); return false; }
  snapshot->pages = commit->db_pages; snapshot->ordinal = ordinal; snapshot->last_frame = commit->last;
  for (uint32_t i = 0; i < commit->last; ++i) {
    uint32_t page = wal->frames[i].page;
    if (page <= snapshot->pages) snapshot->source[page] = i + 1;
    uint32_t size = wal->frames[i].db_pages;
    if (size != 0 && size < snapshot->pages) cutoff[size + 1] = i + 1;
  }
  uint32_t invalidated = 0;
  for (uint32_t page = 1; page <= snapshot->pages; ++page) {
    if (cutoff[page] > invalidated) invalidated = cutoff[page];
    if (snapshot->source[page] > invalidated) continue;
    if (invalidated == 0 && base && page <= base->pages) { snapshot->source[page] = 0; continue; }
    ws_error(error, "Snapshot page %" PRIu32 " has no valid source after truncation or growth; zero filling is refused", page);
    free(cutoff); ws_snapshot_free(snapshot); return false;
  }
  free(cutoff); return true;
}
bool ws_page_bytes(WsWal *wal, WsBase *base, const WsSnapshot *snapshot, uint32_t page, uint8_t *buffer, WsError *error) {
  if (page == 0 || page > snapshot->pages) { ws_error(error, "Snapshot page is outside the selected database size"); return false; }
  uint32_t source = snapshot->source[page];
  if (source) {
    const WsFrame *frame = &wal->frames[source - 1];
    if (!ws_read_at(&wal->input, frame->offset + 24, buffer, wal->page_size, error)) return false;
    uint8_t digest[32]; ws_digest(buffer, wal->page_size, digest);
    if (memcmp(digest, frame->digest, 32) != 0) { ws_error(error, "WAL page changed after prefix validation"); return false; }
    return true;
  }
  if (!base) { ws_error(error, "A required pre-WAL base page is unavailable"); return false; }
  return ws_read_at(&base->input, (uint64_t)(page - 1) * wal->page_size, buffer, wal->page_size, error);
}
static bool page_hash(WsWal *wal, WsBase *base, const WsSnapshot *snapshot, uint32_t page, uint8_t digest[32], WsError *error) {
  uint32_t source = snapshot->source[page];
  if (source) { memcpy(digest, wal->frames[source - 1].digest, 32); return true; }
  uint8_t buffer[65536];
  if (!ws_page_bytes(wal, base, snapshot, page, buffer, error)) return false;
  ws_digest(buffer, wal->page_size, digest); return true;
}
void ws_diff_free(WsDiff *diff) { ws_snapshot_free(&diff->before); ws_snapshot_free(&diff->after); free(diff->rows); memset(diff, 0, sizeof(*diff)); }
bool ws_diff(WsWal *wal, WsBase *base, uint32_t before, uint32_t after, const WsLimits *limits, WsDiff *diff, WsError *error) {
  memset(diff, 0, sizeof(*diff));
  if (!ws_snapshot(wal, base, before, limits, &diff->before, error) || !ws_snapshot(wal, base, after, limits, &diff->after, error)) goto failed;
  diff->rows = (WsChange *)calloc(limits->rows, sizeof(WsChange));
  if (!diff->rows) { ws_error(error, "Cannot allocate displayed difference rows"); goto failed; }
  uint32_t maximum = diff->before.pages > diff->after.pages ? diff->before.pages : diff->after.pages;
  for (uint32_t page = 1; page <= maximum; ++page) {
    WsChange change; memset(&change, 0, sizeof(change)); change.page = page;
    bool have_before = page <= diff->before.pages, have_after = page <= diff->after.pages;
    if (have_before) { change.before_frame = diff->before.source[page]; if (!page_hash(wal, base, &diff->before, page, change.before_hash, error)) goto failed; }
    if (have_after) { change.after_frame = diff->after.source[page]; if (!page_hash(wal, base, &diff->after, page, change.after_hash, error)) goto failed; }
    if (!have_before) { change.kind = 1; ++diff->added; }
    else if (!have_after) { change.kind = 2; ++diff->removed; }
    else if (memcmp(change.before_hash, change.after_hash, 32) != 0) { change.kind = 3; ++diff->modified; }
    else { ++diff->unchanged; continue; }
    if (diff->row_count < limits->rows) diff->rows[diff->row_count++] = change;
  }
  if (!ws_stable(&wal->input, error) || (base && !ws_stable(&base->input, error))) goto failed;
  return true;
failed:
  ws_diff_free(diff); return false;
}
bool ws_recover(WsWal *wal, WsBase *base, const WsSnapshot *snapshot, const char *output, uint8_t digest[32], WsError *error) {
  uint8_t page[65536];
  if (!ws_page_bytes(wal, base, snapshot, 1, page, error) || !database_header(page, wal->page_size, error)) return false;
  if (ws_u32(page + 24, true) == ws_u32(page + 92, true) && ws_u32(page + 28, true) != 0 && ws_u32(page + 28, true) != snapshot->pages) {
    ws_error(error, "Selected page-one database size disagrees with the commit marker"); return false;
  }
  size_t length = strlen(output);
  if (length > 32700) { ws_error(error, "Output path is too long for a staging suffix"); return false; }
  char *stage_path = (char *)malloc(length + 80);
  if (!stage_path) { ws_error(error, "Cannot allocate staging path"); return false; }
#ifdef _WIN32
  unsigned long pid = GetCurrentProcessId();
#else
  unsigned long pid = (unsigned long)getpid();
#endif
  FILE *stage = NULL, *published = NULL; bool ok = false;
  for (unsigned attempt = 0; attempt < 128 && !stage; ++attempt) {
    (void)snprintf(stage_path, length + 80, "%s.walscope-stage-%lu-%u", output, pid, attempt);
    stage = ws_create(stage_path, true, error);
  }
  if (!stage) { ws_error(error, "Cannot create exclusive sibling staging file"); free(stage_path); return false; }
  blake3_hasher hash; blake3_hasher_init(&hash);
  for (uint32_t number = 1; number <= snapshot->pages; ++number) {
    if (!ws_page_bytes(wal, base, snapshot, number, page, error)) goto done;
    if (fwrite(page, 1, wal->page_size, stage) != wal->page_size) { ws_error(error, "Cannot stage complete snapshot image"); goto done; }
    blake3_hasher_update(&hash, page, wal->page_size);
  }
  if (fflush(stage) != 0) { ws_error(error, "Cannot flush staged image"); goto done; }
  if (!ws_stable(&wal->input, error) || (base && !ws_stable(&base->input, error))) goto done;
  published = ws_create(output, false, error);
  if (!published) goto done;
  if (!ws_copy_file(stage, published, (uint64_t)snapshot->pages * wal->page_size, error)) goto done;
  if (fclose(published) != 0) { published = NULL; ws_error(error, "Cannot finish image output; new file may be incomplete"); goto done; }
  published = NULL; blake3_hasher_finalize(&hash, digest, 32); ok = true;
done:
  if (published) (void)fclose(published);
  (void)fclose(stage); ws_remove_own(stage_path); free(stage_path); return ok;
}
