# Dependency provenance

The tool statically includes the portable C implementation of [BLAKE3](https://github.com/BLAKE3-team/BLAKE3) version **1.8.7**, source commit `f3149ec5bb5449af877ba20377a11008ff499fa2`.

`third_party/blake3/provenance.json` records immutable upstream URLs and SHA-256 for every vendored source, header, official vector file, and license. CMake verifies those hashes before building. The upstream files are unmodified. SIMD implementations are disabled; no dependency or code is fetched during configure/build/run.

Upstream provides alternative CC0/Apache licensing; the original `LICENSE_CC0`, `LICENSE_A2`, and `LICENSE_A2LLVM` notices are preserved and included under `licenses/` in native distributions. The surrounding WALScope implementation is MIT licensed.

`tests/hash_vectors.h` is generated from the unmodified official vector JSON by `tools/generate_hash_vectors.py`. Python's standard library and SQLite are used for controlled fixture creation and independent integration tests; they are not linked into the native application.
