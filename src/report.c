#include "walscope.h"
#include <inttypes.h>
#include <string.h>

static void common(const WsWal *wal, const char *kind, WsBuffer *out) {
  char digest[65]; ws_hex(wal->input.digest, digest);
  ws_append(out, "{\n  \"schema\":\"rudy-walscope/report-v1\",\"version\":\"%s\",\"kind\":\"%s\",\"complete\":%s,\n", WS_VERSION, kind, wal->tail == WS_TAIL_CLEAN ? "true" : "false");
  ws_append(out, "  \"wal\":{\"blake3\":\"%s\",\"bytes\":%" PRIu64 ",\"headerPresent\":%s,\"pageSize\":%" PRIu32 ",\"checksumByteOrder\":\"%s\",\"checkpointSequence\":%" PRIu32 ",\"salts\":[%" PRIu32 ",%" PRIu32 "]},\n", digest, wal->input.size, wal->input.size ? "true" : "false", wal->page_size, wal->input.size ? (wal->checksum_big ? "big" : "little") : "none", wal->sequence, wal->salt[0], wal->salt[1]);
  ws_append(out, "  \"summary\":{\"validFrames\":%" PRIu32 ",\"commits\":%" PRIu32 ",\"lastCommitFrame\":%" PRIu32 ",\"pendingFrames\":%" PRIu32 "},\n", wal->frame_count, wal->commit_count, wal->last_commit_frame, wal->frame_count - wal->last_commit_frame);
  ws_append(out, "  \"tail\":{\"status\":\"%s\",\"offset\":%" PRIu64 ",\"ignoredBytes\":%" PRIu64 "},\n", ws_tail_name(wal->tail), wal->tail_offset, wal->input.size - wal->tail_offset);
}
static void base_info(const WsBase *base, WsBuffer *out) {
  if (base) {
    char digest[65]; ws_hex(base->input.digest, digest);
    ws_append(out, "  \"base\":{\"blake3\":\"%s\",\"bytes\":%" PRIu64 ",\"pages\":%" PRIu32 ",\"pairing\":\"operator-asserted-pre-wal\"},\n", digest, base->input.size, base->pages);
  } else ws_append(out, "  \"base\":null,\n");
}
void ws_report_inspect(const WsWal *wal, uint32_t page, uint32_t rows, WsBuffer *out) {
  common(wal, page ? "page-history" : "inspection", out);
  ws_append(out, "  \"requestedPage\":%" PRIu32 ",\n  \"commits\":[\n", page);
  uint32_t shown = wal->commit_count < rows ? wal->commit_count : rows;
  for (uint32_t i = 0; i < shown; ++i) {
    const WsCommit *c = &wal->commits[i];
    ws_append(out, "    %s{\"ordinal\":%" PRIu32 ",\"firstFrame\":%" PRIu32 ",\"lastFrame\":%" PRIu32 ",\"databasePages\":%" PRIu32 ",\"uniquePagesWritten\":%" PRIu32 "}\n", i ? "," : "", i + 1, c->first, c->last, c->db_pages, c->unique_pages);
  }
  ws_append(out, "  ],\"omittedCommits\":%" PRIu32 ",\n  \"frames\":[\n", wal->commit_count - shown);
  uint32_t matched = 0, displayed = 0;
  for (uint32_t i = 0; i < wal->frame_count; ++i) {
    const WsFrame *f = &wal->frames[i]; if (page && f->page != page) continue;
    ++matched; if (displayed >= rows) continue;
    char digest[65]; ws_hex(f->digest, digest);
    ws_append(out, "    %s{\"frame\":%" PRIu32 ",\"page\":%" PRIu32 ",\"offset\":%" PRIu64 ",\"transaction\":%" PRIu32 ",\"committed\":%s,\"commitMarkerPages\":%" PRIu32 ",\"pageBlake3\":\"%s\"}\n", displayed ? "," : "", i + 1, f->page, f->offset, f->transaction, i < wal->last_commit_frame ? "true" : "false", f->db_pages, digest);
    ++displayed;
  }
  ws_append(out, "  ],\"matchedFrames\":%" PRIu32 ",\"omittedFrames\":%" PRIu32 "\n}\n", matched, matched - displayed);
}
static void source_info(const WsWal *wal, uint32_t frame, const uint8_t hash[32], WsBuffer *out) {
  char digest[65]; ws_hex(hash, digest);
  ws_append(out, "{\"kind\":\"%s\",\"frame\":%" PRIu32 ",\"pageBlake3\":\"%s\"", frame ? "wal" : "base", frame, digest);
  if (frame) ws_append(out, ",\"walOffset\":%" PRIu64, wal->frames[frame - 1].offset);
  ws_append(out, "}");
}
void ws_report_diff(const WsWal *wal, const WsBase *base, const WsDiff *diff, WsBuffer *out) {
  common(wal, "snapshot-diff", out); base_info(base, out);
  uint32_t changed = diff->added + diff->removed + diff->modified;
  ws_append(out, "  \"fromCommit\":%" PRIu32 ",\"toCommit\":%" PRIu32 ",\"beforePages\":%" PRIu32 ",\"afterPages\":%" PRIu32 ",\n", diff->before.ordinal, diff->after.ordinal, diff->before.pages, diff->after.pages);
  ws_append(out, "  \"differences\":{\"added\":%" PRIu32 ",\"removed\":%" PRIu32 ",\"modified\":%" PRIu32 ",\"unchanged\":%" PRIu32 ",\"equivalentPageBytes\":%s},\n  \"pages\":[\n", diff->added, diff->removed, diff->modified, diff->unchanged, changed ? "false" : "true");
  for (uint32_t i = 0; i < diff->row_count; ++i) {
    const WsChange *r = &diff->rows[i];
    ws_append(out, "    %s{\"page\":%" PRIu32 ",\"change\":\"%s\",\"before\":", i ? "," : "", r->page, r->kind == 1 ? "added" : r->kind == 2 ? "removed" : "modified");
    if (r->kind == 1) ws_append(out, "null"); else source_info(wal, r->before_frame, r->before_hash, out);
    ws_append(out, ",\"after\":");
    if (r->kind == 2) ws_append(out, "null"); else source_info(wal, r->after_frame, r->after_hash, out);
    ws_append(out, "}\n");
  }
  ws_append(out, "  ],\"omittedDifferences\":%" PRIu32 "\n}\n", changed - diff->row_count);
}
void ws_report_recover(const WsWal *wal, const WsBase *base, const WsSnapshot *snapshot, const uint8_t digest[32], bool accepted, WsBuffer *out) {
  common(wal, "recovery-receipt", out); base_info(base, out);
  uint32_t base_pages = 0;
  for (uint32_t p = 1; p <= snapshot->pages; ++p) if (snapshot->source[p] == 0) ++base_pages;
  char hash[65]; ws_hex(digest, hash);
  ws_append(out, "  \"snapshot\":{\"commit\":%" PRIu32 ",\"lastFrame\":%" PRIu32 ",\"pages\":%" PRIu32 ",\"bytes\":%" PRIu64 ",\"blake3\":\"%s\",\"basePagesUsed\":%" PRIu32 ",\"walPagesUsed\":%" PRIu32 "},\n", snapshot->ordinal, snapshot->last_frame, snapshot->pages, (uint64_t)snapshot->pages * wal->page_size, hash, base_pages, snapshot->pages - base_pages);
  ws_append(out, "  \"acceptedInvalidTail\":%s,\"framesAfterSelectionIgnored\":%" PRIu32 ",\"zeroFilledPages\":0,\"headerRewritten\":false\n}\n", accepted ? "true" : "false", wal->frame_count - snapshot->last_frame);
}
static void html_escape(const char *s, WsBuffer *out) {
  for (; *s; ++s) {
    if (*s == '<') ws_append(out, "&lt;"); else if (*s == '>') ws_append(out, "&gt;");
    else if (*s == '&') ws_append(out, "&amp;"); else if (*s == '"') ws_append(out, "&quot;");
    else ws_append(out, "%c", *s);
  }
}
void ws_render_html(const WsWal *wal, const char *kind, uint32_t rows, const WsBuffer *evidence, WsBuffer *out) {
  if (evidence->failed) { out->failed = true; return; }
  ws_append(out, "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'\"><title>Rudy WALScope / capture investigation</title><style>"
    ":root{color-scheme:dark}*{box-sizing:border-box}body{margin:0;background:#090f17;color:#eaf3ff;font:15px/1.6 system-ui,sans-serif}main{max-width:1150px;margin:36px auto;padding:24px}header{border-left:4px solid #55e0b6;background:linear-gradient(110deg,#101e2b,#0a111b);padding:18px 28px}small{font:12px monospace;color:#55e0b6;letter-spacing:2px}h1{font-size:clamp(38px,6vw,64px);line-height:1.1;letter-spacing:-2px;margin:16px 0}h2{font-size:24px}p,footer{color:#9db0c5}.cards{display:flex;gap:16px;flex-wrap:wrap;margin:28px 0}.card{flex:1;min-width:200px;padding:20px;background:#101b28;border:1px solid #293c51;border-radius:10px}.value{font-size:28px;color:#55e0b6}.notice{border:1px solid #84613d;background:#302318;padding:14px 18px;border-radius:8px;color:#ffd1a2}.scroll{overflow:auto}table{border-collapse:collapse;width:100%;font-size:14px}td,th{padding:12px;text-align:left;border-bottom:1px solid #26384b}th{color:#91a9bf}details{border:1px solid #2b3c4f;padding:14px 18px;border-radius:8px;margin:20px 0}summary{cursor:pointer;font-weight:650}pre{white-space:pre-wrap;overflow-wrap:anywhere;max-height:580px;overflow:auto;font:12px/1.6 ui-monospace,monospace;background:#070c12;padding:16px}footer{margin-top:32px;border-top:1px solid #26384b;padding-top:20px;font-size:13px}</style><main><header><small>RUDY SYSTEMS / SQLITE WAL FORENSICS</small><h1>Rudy WALScope</h1><p>Validate the chain. Follow the commits. Preserve the evidence.</p></header>");
  ws_append(out, "<h2>%s</h2><div class=\"cards\"><div class=\"card\">Validated frames<div class=\"value\">%" PRIu32 "</div></div><div class=\"card\">Commit markers<div class=\"value\">%" PRIu32 "</div></div><div class=\"card\">Page size<div class=\"value\">%" PRIu32 " bytes</div></div><div class=\"card\">Pending frames<div class=\"value\">%" PRIu32 "</div></div></div>", kind, wal->frame_count, wal->commit_count, wal->page_size, wal->frame_count - wal->last_commit_frame);
  if (wal->tail != WS_TAIL_CLEAN) ws_append(out, "<div class=\"notice\">Capture validation stopped at byte %" PRIu64 ": %s. %" PRIu64 " remaining bytes are outside the validated prefix. A stale WAL generation or interrupted acquisition can also cause this condition.</div>", wal->tail_offset, ws_tail_name(wal->tail), wal->input.size - wal->tail_offset);
  ws_append(out, "<h2>Commit timeline</h2><div class=\"scroll\"><table><thead><tr><th>Commit</th><th>First frame</th><th>Commit frame</th><th>Database pages</th><th>Unique page writes</th></tr></thead><tbody>");
  uint32_t shown = wal->commit_count < rows ? wal->commit_count : rows;
  for (uint32_t i = 0; i < shown; ++i) {
    const WsCommit *c = &wal->commits[i]; ws_append(out, "<tr><td>%" PRIu32 "</td><td>%" PRIu32 "</td><td>%" PRIu32 "</td><td>%" PRIu32 "</td><td>%" PRIu32 "</td></tr>", i + 1, c->first, c->last, c->db_pages, c->unique_pages);
  }
  ws_append(out, "</tbody></table></div><p>Omitted timeline rows: %" PRIu32 ". Summary counts cover the complete validated prefix.</p><details><summary>Structured evidence and page provenance</summary><pre>", wal->commit_count - shown);
  html_escape(evidence->data ? evidence->data : "", out);
  ws_append(out, "</pre></details><details><summary>Interpretation boundaries</summary><p>Commit ordinals reflect WAL marker order, not wall-clock time. Page writes do not identify SQL statements, users, or row-level changes. Checksums are not authentication. A base database's pairing and pre-WAL state require an operator assertion; the WAL does not prove them.</p></details><footer>C11 / offline / developer preview %s. Raw page bytes and file paths are omitted from this report. Recovery preserves page bytes, excludes uncommitted frames, and refuses missing-page zero filling. No source database is opened through SQLite.</footer></main></html>", WS_VERSION);
}
void ws_render_text(const WsWal *wal, const char *kind, uint32_t rows, const WsBuffer *evidence, WsBuffer *out) {
  if (evidence->failed) { out->failed = true; return; }
  ws_append(out, "RUDY WALSCOPE %s / %s\nPage size: %" PRIu32 " | Valid frames: %" PRIu32 " | Commits: %" PRIu32 " | Pending frames: %" PRIu32 "\nTail: %s | Validated prefix ends at byte %" PRIu64 "\n", WS_VERSION, kind, wal->page_size, wal->frame_count, wal->commit_count, wal->frame_count - wal->last_commit_frame, ws_tail_name(wal->tail), wal->tail_offset);
  uint32_t shown = wal->commit_count < rows ? wal->commit_count : rows;
  for (uint32_t i = 0; i < shown; ++i) {
    const WsCommit *c = &wal->commits[i]; ws_append(out, "Commit %" PRIu32 ": frames %" PRIu32 "..%" PRIu32 ", %" PRIu32 " database pages, %" PRIu32 " unique page writes\n", i + 1, c->first, c->last, c->db_pages, c->unique_pages);
  }
  ws_append(out, "\nStructured evidence:\n%s", evidence->data ? evidence->data : "");
}
