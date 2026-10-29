# Security boundaries

WALScope is an offline binary-format inspection/reconstruction preview. It opens explicitly named regular input copies read-only, never loads extensions or executes SQL, and creates only explicitly requested new reports/images plus an owned sibling staging file. It has no target discovery, network client, live database attachment, or source modification path.

Use captures you are authorized to review. Reports omit raw pages and paths, but reveal page identities, sizes, and ordering. Reconstructed images contain database contents and can include unused/free-page data. Protect the output directory. POSIX created files use mode 0600; Windows inherits directory ACLs and denies sharing while a file is being written. Do not publish real captures as examples.

Checksums and salts do not authenticate data or bind a WAL to a base. A supplied base requires an explicit pre-WAL-state assertion; unknown checkpoint state can invalidate historical reconstruction. File identities are BLAKE3 content hashes, not provenance signatures. Use stable offline copies and independent acquisition records.

Application limits bound input bytes, frame metadata, image indexes, displayed records, and report size. They are not an OS memory/time sandbox. Inspect unfamiliar evidence in a constrained process. Whole-file pre/post checks detect observed changes; they are not a locking/acquisition protocol or a guarantee against an adversary modifying and restoring files concurrently.

Parsing stops at the first invalid frame. Prefix acceptance is explicit and keeps an incomplete status. Missing pages are never zero-filled or resurrected from before a truncation. Source byte changes observed during recovery abort publication. A destination disk-write failure can leave a new partial destination; check exit status and receipts before treating it as evidence. Shell redirection controls its own overwrite behavior outside the tool.

Report suspected defects through the repository's private reporting option when available, or the maintainer contact on the GitHub profile. Keep production database contents, tokens, and private paths out of public issues.
