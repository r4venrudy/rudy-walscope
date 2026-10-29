# Acquisition contract

Use stable offline evidence copies and retain original acquisition records. This tool does not connect to applications, lock a running database, or produce a live snapshot. Copying files independently while a writer is active can mix states; whole-file digest checks during this tool's run do not repair an already inconsistent acquisition.

Inspection and page history need only the captured WAL. Historical image reconstruction additionally needs either every selected page in the WAL itself, or a known base that belongs to the same database and predates the WAL generation. `--assume-pre-wal-base` records that operator assertion; it is not inferred or verified by the tool. A quiesced database/WAL pair can still have checkpointed pages newer than an earlier selected commit. Its physical readability and matching page size are not proof of historical suitability.

If the base state is unknown, keep the result at WAL-prefix and page-version inspection. A later base can otherwise introduce future bytes into an earlier image. There is deliberately no automatic pairing, base guessing, or implicit zero filling.

An excluded tail can represent an interrupted write, corruption, or stale bytes from a previous WAL epoch. Reports identify the first failed condition without assigning a cause. Explicit `--accept-tail` allows only committed states inside the validated prefix and returns a qualified status. It cannot establish what later data might have contained.

Recovered output preserves selected page bytes. Keep that image as evidence and query a separate copy with an independently trusted SQLite reader. For controlled test images, read-only immutable URI mode avoids sidecar writes; do not use immutable mode to claim that a live database is stable. Validate logical integrity and expected application data independently.

No WAL field supplies an authenticated application identity, actor, wall-clock timestamp, or acquisition chain of custody. BLAKE3 identities identify captured bytes; salts and rolling checksums are not digital signatures. Recovered databases can contain private data, including unused page contents. Handle exports and receipts under the evidence owner's access policy.
