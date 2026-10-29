# Preview validation

The local Windows Clang 23.1.3 build passes four CTest targets. Native tests contain 115 assertions, including 35 upstream BLAKE3 vectors evaluated in one-shot and segmented modes, checksum recurrence/overflow cases, and report bounds.

The independent standard-library Python suite checks real SQLite-generated captures at page sizes 512, 4096, and 65536. It validates every selected commit through SQLite's `integrity_check` and exact expected SQL rows, checks untouched base pages, and independently compares exported page bytes. It rewrites valid captures into the other checksum byte order and requires byte-identical reconstructed images.

Adversarial scenarios cover pending frames, partial/checksum/salt/page-number tails, invalid headers, no resynchronization beyond the first bad frame, frame/input/image limits, repeated/invalid options, missing/incompatible bases, unavailable commits, missing-page refusal across shrink/regrowth, self-contained WAL-only recovery, duplicate page versions, Unicode paths, existing-file and hard-link aliases, and staging cleanup. Symbolic-link and POSIX permission cases run where the host supports them. Raw paths and payload values must remain absent from HTML reports.

CI adds Linux GCC, Windows MSVC, macOS Clang, and Linux AddressSanitizer/UndefinedBehaviorSanitizer. Tagged native packaging depends on that workflow, exercises the installed layout, and queries the generated recovered image independently. Publication is complete only after checking the actual workflow result and downloaded archive bytes.

This is regression evidence, not a formal proof, production performance benchmark, exhaustive fuzzing campaign, or validation of every SQLite version/extension. There is no live acquisition, encrypted/SQLCipher support claim, SQL-row recovery, or proof of base pairing/checkpoint state. No real user databases are used in fixtures or releases.
