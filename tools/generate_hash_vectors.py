"""Regenerate the C vector table from the unmodified vendored upstream BLAKE3 vectors."""
from pathlib import Path
import json
root = Path(__file__).resolve().parent.parent
cases = json.loads((root / "third_party/blake3/test_vectors.json").read_text())["cases"]
rows = ["/* Generated from the vendored official BLAKE3 test vectors; see THIRD_PARTY.md. */", "static const struct { size_t length; const char *hash; } vectors[] = {"]
rows += ['  {%d, "%s"},' % (case["input_len"], case["hash"][:64]) for case in cases]
rows += ["};", ""]
(root / "tests/hash_vectors.h").write_text("\n".join(rows), encoding="utf-8")
