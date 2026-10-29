# Prefix validation and page reconstruction

The parser reads explicitly supplied regular-file copies. It first computes a BLAKE3 content identity and checks the byte budget. It validates the header, then streams frames in physical order with a single page buffer. The checksum chain, salt epoch, and page-number constraints determine a contiguous accepted prefix. A second whole-file digest check rejects observed input changes. Checksums are structural consistency checks and are not authentication.

The implementation follows SQLite's official [WAL file format and checksum description](https://sqlite.org/fileformat2.html#walformat). Multi-byte fields are decoded explicitly. Checksum input word order comes from the WAL magic; stored metadata remains big-endian. The header checksum seeds the frame chain, and each frame extends it with its initial page/commit fields and page body. Both supported checksum orders have independent fixtures.

Each valid frame contributes bounded metadata: page number, byte offset, commit-size marker, transaction ordinal, and page-content BLAKE3. A nonzero commit-size marker closes the preceding frame run. Sorting that transaction's page IDs gives distinct writes without a quadratic scan. Remaining valid frames after the final marker are pending and excluded from selected committed states. Ordinals are physical ordering labels, not timestamps or SQL transaction IDs.

## Snapshot source selection

For a selected commit, build a dense last-frame index bounded by the final image size. Record each earlier shrinking commit as a suffix invalidation boundary. A prefix-maximum pass determines the last truncation affecting each page. A WAL version is eligible only after that boundary; a base page is eligible only if no such truncation invalidated it. Thus later growth cannot resurrect truncated bytes from an old base or earlier WAL version.

Every final page must have an eligible WAL frame or an explicitly assumed pre-WAL base page. Missing pages are errors; zero filling is refused. The page-one magic, page size, supported header fields, and valid in-header size are checked against the chosen commit. These checks do not validate all B-tree structures or prove that the base belongs to the WAL.

Recovery stages the image in an exclusively created sibling file. WAL page bodies are rehashed against parsed frame identities while reading. Both input identities are rechecked before the new destination is exclusively created and populated. Owned staging files are closed and removed on success or failure. A destination write failure can still leave an incomplete new destination; success requires the exit status and receipt, not file existence alone. No header fields are rewritten.

## Snapshot comparison

Build both complete source indexes, then compare page-content identities across the larger page range. Count added, removed, modified, and unchanged pages. The reported rows retain each side's WAL frame or base source, while aggregate counts cover every page regardless of row truncation. This is a byte/page comparison and does not infer SQL-row or application semantics.

## Implementation boundaries

The core and CLI use C11. Vendored BLAKE3 uses its portable C path, with SIMD variants disabled for reproducibility across the supported builds. Input integers use checked bounds and explicit endianness; no file bytes are cast to packed host structs. Reports omit page payloads and paths; HTML uses native disclosure controls, escaped evidence, no script, and no external assets. Windows paths are converted through UTF-8/UTF-16 APIs rather than the active ANSI code page.

Budgets bound input bytes, frame descriptors, image-index size, displayed records, and rendered output. They do not make arbitrary files safe against every implementation flaw or bound OS-level blocking and disk performance. Use an isolated process and suitable filesystem permissions for unfamiliar evidence.
