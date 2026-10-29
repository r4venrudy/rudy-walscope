Rudy WALScope is a native C11 tool for offline SQLite WAL prefix validation, commit/page provenance, selected committed-image reconstruction, and page-level snapshot comparison.

- Rolling checksums in both word orders, salt epochs, first invalid offsets, pending frames, and BLAKE3 identities.
- Explicit known-base assumptions, self-contained WAL support, truncation barriers, and missing-page refusal.
- New-file-only recovery with staged bytes and source rechecks; versioned JSON and expandable offline HTML.
- Real-SQLite query/integrity oracles, three page sizes, adversarial captures, and native cross-platform/sanitizer CI.

Verify SHA256SUMS.txt before extracting the native archive. Packages include `bin/`, generated synthetic captures and expected states, example reports/image, documentation, and licenses. No language runtime or SQLite library installation is required by the native executable. Binaries target Linux x64 (Ubuntu 24.04 build), Windows x64, and macOS ARM64 (macOS 15 build); older OS compatibility is unverified.

Developer preview. Use stable authorized evidence copies. The WAL cannot prove base pairing/checkpoint state, and structural checksum validation is not SQL integrity or authentication. Read the acquisition contract before reconstructing historical images.
