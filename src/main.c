#include "walscope.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

static const char *help =
  "Rudy WALScope - offline SQLite WAL inspection and committed-page recovery\n"
  "walscope inspect CAPTURE.wal [--format text|json|html] [--fail-on pending]\n"
  "walscope page CAPTURE.wal --page NUMBER [--format text|json|html]\n"
  "walscope diff CAPTURE.wal --from COMMIT --to COMMIT|latest [--fail-on changed]\n"
  "walscope recover CAPTURE.wal --commit COMMIT|latest --output NEW_DATABASE\n"
  "\nDiff/recover: --base PRE_WAL_DATABASE --assume-pre-wal-base (both required together)\n"
  "             --accept-tail explicitly uses only a validated prefix and returns 1\n"
  "Common: --format text|json|html --rows 1..1000 --max-frames 1..1000000\n"
  "        --max-input-mib 1..4096 --max-output-mib 1..2048\n"
  "Report files also use --output; recovery writes its receipt to standard output.\n"
  "No SQLite connection, target probes, source writes, or automatic base pairing.\n";

static bool numeric(const char *s, uint64_t maximum, uint64_t *value) {
  if (!s || !*s) return false;
  uint64_t n = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
    uint64_t digit = (uint64_t)(*p - '0');
    if (n > maximum / 10 || (n == maximum / 10 && digit > maximum % 10)) return false;
    n = n * 10 + digit;
  }
  if (n == 0) return false;
  *value = n; return true;
}
static bool commit_value(const char *s, uint32_t *value) {
  if (strcmp(s, "latest") == 0) { *value = 0; return true; }
  uint64_t n; if (!numeric(s, 1000000, &n)) return false; *value = (uint32_t)n; return true;
}
static int run_cli(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "--version") == 0) { printf("walscope %s\n", WS_VERSION); return 0; }
  if (argc < 2 || (argc == 2 && strcmp(argv[1], "--help") == 0)) { fputs(help, stdout); return argc < 2 ? 2 : 0; }
  WsError error = {{0}}; WsWal wal; WsBase base; WsSnapshot snapshot; WsDiff diff; WsBuffer json = {0}, rendered = {0};
  memset(&wal, 0, sizeof(wal)); memset(&base, 0, sizeof(base)); memset(&snapshot, 0, sizeof(snapshot)); memset(&diff, 0, sizeof(diff));
  int result = 2; const char *command = argv[1], *input = NULL, *output = NULL, *base_path = NULL, *format = "text", *gate = "none";
  bool inspect = strcmp(command, "inspect") == 0, history = strcmp(command, "page") == 0, recovery = strcmp(command, "recover") == 0, comparison = strcmp(command, "diff") == 0;
  bool accept_tail = false, assume_base = false; uint32_t page = 0, selected = 0, from = 0, to = 0;
  bool have_commit = false, have_from = false, have_to = false; WsLimits limits = ws_limits(); uint32_t seen = 0;
  static const char *names[] = {"--output", "--base", "--format", "--rows", "--max-frames", "--max-input-mib", "--max-output-mib", "--page", "--commit", "--from", "--to", "--fail-on", "--accept-tail", "--assume-pre-wal-base"};
  if (!inspect && !history && !recovery && !comparison) { ws_error(&error, "Unknown command; use --help"); goto done; }
  for (int i = 2; i < argc; ++i) {
    if (strncmp(argv[i], "--", 2) != 0) { if (input) { ws_error(&error, "Exactly one WAL path is required"); goto done; } input = argv[i]; continue; }
    size_t option;
    for (option = 0; option < sizeof(names) / sizeof(names[0]); ++option) if (strcmp(argv[i], names[option]) == 0) break;
    if (option == sizeof(names) / sizeof(names[0]) || (seen & (UINT32_C(1) << option))) { ws_error(&error, "Unknown or repeated option"); goto done; }
    seen |= UINT32_C(1) << option;
    if (option == 12) { accept_tail = true; continue; }
    if (option == 13) { assume_base = true; continue; }
    if (++i >= argc || strncmp(argv[i], "--", 2) == 0) { ws_error(&error, "Option requires a value"); goto done; }
    const char *value = argv[i]; uint64_t n;
    if (option == 0) output = value;
    else if (option == 1) base_path = value;
    else if (option == 2) format = value;
    else if (option == 11) gate = value;
    else if (option >= 8 && option <= 10) {
      uint32_t *target = option == 8 ? &selected : option == 9 ? &from : &to;
      if (!commit_value(value, target)) { ws_error(&error, "Commit must be a positive ordinal or latest"); goto done; }
      if (option == 8) have_commit = true; else if (option == 9) have_from = true; else have_to = true;
    } else {
      uint64_t maximum = option == 3 ? 1000 : option == 4 ? 1000000 : option == 5 ? 4096 : option == 6 ? 2048 : UINT32_MAX - 1U;
      if (!numeric(value, maximum, &n)) { ws_error(&error, "Numeric option is outside its documented range"); goto done; }
      if (option == 3) limits.rows = (uint32_t)n;
      else if (option == 4) limits.max_frames = (uint32_t)n;
      else if (option == 5) limits.max_input = n * 1024 * 1024;
      else if (option == 6) limits.max_output = n * 1024 * 1024;
      else page = (uint32_t)n;
    }
  }
  if (!input) { ws_error(&error, "A captured WAL path is required"); goto done; }
  if (!input[0] || (output && !output[0]) || (base_path && !base_path[0])) { ws_error(&error, "File paths must be nonempty"); goto done; }
  if (strcmp(format, "text") != 0 && strcmp(format, "json") != 0 && strcmp(format, "html") != 0) { ws_error(&error, "Format must be text, json, or html"); goto done; }
  if ((history != (page != 0)) || (recovery != have_commit) || (comparison != (have_from && have_to)) || (!comparison && (have_from || have_to))) { ws_error(&error, "Page and commit selectors do not match the selected command"); goto done; }
  if ((inspect || history) && (base_path || assume_base || accept_tail)) { ws_error(&error, "Base and tail-acceptance options require diff or recover"); goto done; }
  if ((base_path != NULL) != assume_base) { ws_error(&error, "A base requires --assume-pre-wal-base; pairing and checkpoint state cannot be inferred"); goto done; }
  if (recovery && !output) { ws_error(&error, "Recovery requires a new output database path"); goto done; }
  if ((strcmp(gate, "none") != 0) && !((inspect || history) && strcmp(gate, "pending") == 0) && !(comparison && strcmp(gate, "changed") == 0)) { ws_error(&error, "Failure gate does not match the selected command"); goto done; }
  if (!ws_scan(input, &limits, &wal, &error)) goto done;
  if ((recovery || comparison) && wal.tail != WS_TAIL_CLEAN && !accept_tail) { ws_error(&error, "WAL tail is outside the validated prefix; explicit --accept-tail is required for prefix recovery or comparison"); goto done; }
  if (base_path && !ws_open_base(base_path, &limits, wal.page_size, &base, &error)) goto done;
  WsBase *base_pointer = base_path ? &base : NULL;
  bool accepted = wal.tail != WS_TAIL_CLEAN && accept_tail;
  if (inspect || history) ws_report_inspect(&wal, page, limits.rows, &json);
  else if (comparison) {
    if (!ws_diff(&wal, base_pointer, from, to, &limits, &diff, &error)) goto done;
    ws_report_diff(&wal, base_pointer, &diff, &json);
  } else {
    uint8_t digest[32];
    if (!ws_snapshot(&wal, base_pointer, selected, &limits, &snapshot, &error) || !ws_recover(&wal, base_pointer, &snapshot, output, digest, &error)) goto done;
    ws_report_recover(&wal, base_pointer, &snapshot, digest, accepted, &json);
  }
  if (strcmp(format, "json") == 0) { rendered = json; memset(&json, 0, sizeof(json)); }
  else if (strcmp(format, "html") == 0) ws_render_html(&wal, command, limits.rows, &json, &rendered);
  else ws_render_text(&wal, command, limits.rows, &json, &rendered);
  if (!ws_write_report(recovery ? NULL : output, &rendered, &error)) goto done;
  if ((inspect || history) && wal.tail != WS_TAIL_CLEAN) result = 2;
  else result = accepted || ((inspect || history) && strcmp(gate, "pending") == 0 && wal.frame_count > wal.last_commit_frame) || (comparison && strcmp(gate, "changed") == 0 && (diff.added || diff.removed || diff.modified)) ? 1 : 0;
done:
  if (error.text[0]) fprintf(stderr, "walscope: %s\n", error.text);
  ws_buffer_free(&json); ws_buffer_free(&rendered); ws_snapshot_free(&snapshot); ws_diff_free(&diff); ws_close_input(&base.input); ws_wal_free(&wal);
  return result;
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
  char **utf8 = (char **)calloc((size_t)argc, sizeof(char *));
  if (!utf8) return 2;
  int result = 2;
  for (int i = 0; i < argc; ++i) {
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, NULL, 0, NULL, NULL);
    if (size <= 0) goto done;
    utf8[i] = (char *)malloc((size_t)size); if (!utf8[i]) goto done;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, utf8[i], size, NULL, NULL) != size) goto done;
  }
  result = run_cli(argc, utf8);
done:
  for (int i = 0; i < argc; ++i) free(utf8[i]);
  free(utf8); return result;
}
#else
int main(int argc, char **argv) { return run_cli(argc, argv); }
#endif
