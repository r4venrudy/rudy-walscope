# Evidence contract v1

Reports use `schema: rudy-walscope/report-v1` and a tool `version`. `complete` refers to physical validation through the captured file's end. A valid pending transaction does not make the file physically incomplete, but its frames never enter a committed image.

- `wal`: whole-capture BLAKE3, byte length, page size, checksum word order, sequence, and salts.
- `summary`: validated frames, commit markers, last commit frame, and pending frames.
- `tail`: first excluded byte offset, excluded byte count, and `clean`, `partial-frame`, `salt-mismatch`, `checksum-mismatch`, or `invalid-page-number`.
- Inspection `commits`: 1-based commit ordinal, first/last frame, database size in pages, distinct pages written.
- Inspection/history `frames`: 1-based frame index, page number, frame-header byte offset, transaction ordinal, committed status, commit marker, and page-content BLAKE3.

Displayed commit/frame/difference rows can be truncated independently. Omission counters are explicit and summaries always use the entire validated prefix. A frame after the last commit has a provisional transaction ordinal but `committed: false`; it is not evidence of a durable transaction.

## Recovery receipt

`snapshot` identifies the selected commit/frame boundary, final byte/page size, output BLAKE3, and how many pages came from the base or WAL. `base` records its content identity and the operator-asserted pre-WAL pairing, or is null for a self-contained WAL image. `acceptedInvalidTail` preserves a prefix qualification, and `framesAfterSelectionIgnored` includes both later commits and pending frames. `zeroFilledPages` is always zero and `headerRewritten` is false.

## Difference report

`fromCommit` and `toCommit` resolve `latest` to actual ordinals. Counts describe pages, not SQL rows. A change row has a page number and each side's source frame/base plus content identity. Absent pages have null sources. `equivalentPageBytes` covers all selected page bytes; it is independent of the display limit.

A report is not a signed attestation. Capture, base, image, and page BLAKE3 identities have different scopes. A structurally valid WAL plus a readable output header does not prove a correct acquisition, logical database consistency, or an authentic source. Check process exit status: I/O, allocation, and resource-limit failures are errors even if an output file was partly created.
