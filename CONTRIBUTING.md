# Contributing

Run CTest with the independent Python/SQLite suite enabled before proposing a change. For checksum/offset/snapshot changes, include a real SQLite oracle case and a counterexample that fails the previous behavior. Preserve strict budgets, contiguous-prefix validation, explicit tail qualification, source identities, first-owner page provenance, and truncation barriers.

Never weaken the base-state assertion or silently infer pairing from matching page sizes. Do not zero-fill missing pages, apply pending transactions, resynchronize past bad frames, or rewrite page headers without introducing an explicit separate contract. New formats must document unsupported variants and byte-order rules.

Keep fixtures synthetic. Generated captures, exported images, credentials, runtime state, and build artifacts do not belong in source commits. Tests should not touch live databases or network targets. Windows/MSVC and Unix/sanitizer checks must pass. The C header is internal and has no current ABI/source-compatibility guarantee; report-schema changes require explicit versioning.
