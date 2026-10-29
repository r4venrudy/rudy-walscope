"""Independent SQLite SQL-state oracle and adversarial binary-format contracts."""
from pathlib import Path
from contextlib import closing
import hashlib
import importlib.util
import json
import os
import re
import sqlite3
import struct
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True

binary = Path(sys.argv[1]).resolve()
root = Path(sys.argv[2]).resolve()
spec = importlib.util.spec_from_file_location("synthetic", root / "examples/make_capture.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)
checks = 0

def check(condition, label):
    global checks
    checks += 1
    assert condition, label

def run(*args, code=0):
    p = subprocess.run([str(binary), *map(str, args)], capture_output=True, text=True, encoding="utf-8", timeout=30)
    check(p.returncode == code, (args, p.returncode, p.stderr, p.stdout[:300]))
    return p

def report(*args, code=0):
    p = run(*args, "--format", "json", code=code)
    j = json.loads(p.stdout)
    check(j["schema"] == "rudy-walscope/report-v1", "versioned report contract")
    return j

def checksum(data, state=(0, 0), order="little"):
    a, b = state
    assert len(data) % 8 == 0
    for x, y in struct.iter_unpack(("<" if order == "little" else ">") + "II", data):
        a = (a + x + b) & 0xffffffff
        b = (b + y + a) & 0xffffffff
    return a, b

def frames(raw):
    page_size = struct.unpack_from(">I", raw, 8)[0]
    return [(struct.unpack_from(">II", raw, pos)[0], struct.unpack_from(">II", raw, pos)[1], raw[pos+24:pos+24+page_size]) for pos in range(32, len(raw), page_size+24)]

def encode(page_size, records, order="little"):
    magic = 0x377f0682 if order == "little" else 0x377f0683
    salt = (0x11223344, 0xaabbccdd)
    header = struct.pack(">6I", magic, 3007000, page_size, 7, *salt)
    state = checksum(header, order=order)
    out = bytearray(header + struct.pack(">2I", *state))
    for page, size, content in records:
        assert len(content) == page_size
        first = struct.pack(">II", page, size)
        state = checksum(content, checksum(first, state, order), order)
        out += first + struct.pack(">2I", *salt) + struct.pack(">2I", *state) + content
    return bytes(out)

def validate_sql(path, expected):
    with closing(sqlite3.connect(path.resolve().as_uri() + "?mode=ro&immutable=1", uri=True)) as c:
        check(c.execute("PRAGMA integrity_check").fetchall() == [("ok",)], "SQLite structural integrity oracle")
        check([list(r) for r in c.execute("SELECT id,payload FROM events ORDER BY id")] == expected, "SQLite query-state oracle")
        check(c.execute("SELECT note FROM static_notes").fetchall() == [("synthetic unchanged base page",)], "untouched base page retained")

with tempfile.TemporaryDirectory(prefix="walscope-integration-") as directory:
    temp = Path(directory)
    for page_size in (512, 4096, 65536):
        capture = temp / ("captur\u00e9 \u0130 " + str(page_size))
        generator.create_capture(capture, page_size)
        expected = json.loads((capture / "expected.json").read_text())
        wal = capture / "capture.wal"
        base = capture / "base.sqlite"
        original = wal.read_bytes()
        base_original = base.read_bytes()
        j = report("inspect", wal)
        check(j["complete"] and j["summary"]["commits"] == 3 and j["wal"]["pageSize"] == page_size, "real SQLite capture topology")
        check(j["tail"]["status"] == "clean" and j["summary"]["pendingFrames"] == 0, "real capture chain is complete")
        check(all(row["committed"] for row in j["frames"]), "all real frames are committed")
        check(len(j["wal"]["blake3"]) == 64, "BLAKE3 source identity")
        native_rows = j["frames"]
        actual_records = frames(original)
        check(len(actual_records) == j["summary"]["validFrames"], "independent frame-size accounting")
        for ordinal in (1, 2, 3):
            output = capture / f"snapshot-{ordinal}.sqlite"
            recovered = report("recover", wal, "--base", base, "--assume-pre-wal-base", "--commit", ordinal, "--output", output)
            check(recovered["snapshot"]["commit"] == ordinal and recovered["snapshot"]["bytes"] == output.stat().st_size, "recovery receipt matches output")
            check(recovered["snapshot"]["basePagesUsed"] >= 1 and not recovered["headerRewritten"], "explicit base provenance and unchanged page header")
            check(recovered["zeroFilledPages"] == 0, "no fabricated page bytes")
            validate_sql(output, expected["states"][ordinal-1])
            if os.name != "nt":
                check(output.stat().st_mode & 0o777 == 0o600, "POSIX recovery output is private")
        different = report("diff", wal, "--base", base, "--assume-pre-wal-base", "--from", 1, "--to", "latest", "--fail-on", "changed", code=1)
        first_image = (capture / "snapshot-1.sqlite").read_bytes()
        last_image = (capture / "snapshot-3.sqlite").read_bytes()
        before = [first_image[i:i+page_size] for i in range(0, len(first_image), page_size)]
        after = [last_image[i:i+page_size] for i in range(0, len(last_image), page_size)]
        modified = sum(a != b for a, b in zip(before, after))
        unchanged = sum(a == b for a, b in zip(before, after))
        check(different["differences"] == {"added": max(0,len(after)-len(before)), "removed": max(0,len(before)-len(after)), "modified": modified, "unchanged": unchanged, "equivalentPageBytes": False}, "independent raw-page comparison oracle")
        same = report("diff", wal, "--base", base, "--assume-pre-wal-base", "--from", 2, "--to", 2, "--fail-on", "changed")
        check(same["differences"]["equivalentPageBytes"] and not same["pages"], "same selected snapshot is equivalent")
        history = report("page", wal, "--page", 2)
        check(history["matchedFrames"] == sum(r[0] == 2 for r in actual_records) and all(r["page"] == 2 for r in history["frames"]), "page-version provenance is filtered")
        limited = report("inspect", wal, "--rows", 1)
        check(limited["summary"] == j["summary"] and limited["omittedFrames"] > 0, "display limit leaves validation totals intact")
        html = run("inspect", wal, "--format", "html").stdout
        check("<details>" in html and "<script" not in html and "default-src 'none'" in html, "inert self-contained report")
        check(str(wal) not in html and "alpha-1" not in html, "paths and raw page data are omitted")
        big = capture / "big-endian.wal"
        big.write_bytes(encode(page_size, actual_records, "big"))
        bj = report("inspect", big)
        check(bj["wal"]["checksumByteOrder"] == "big" and bj["summary"] == j["summary"], "both checksum byte orders decode identically")
        big_output = capture / "big-endian.sqlite"
        report("recover", big, "--base", base, "--assume-pre-wal-base", "--commit", "latest", "--output", big_output)
        check(big_output.read_bytes() == last_image, "endianness changes no reconstructed page bytes")
        pending = capture / "pending.wal"
        pending_records = actual_records + [(actual_records[-1][0], 0, actual_records[-1][2])]
        pending.write_bytes(encode(page_size, pending_records))
        pj = report("inspect", pending, "--fail-on", "pending", code=1)
        check(pj["complete"] and pj["summary"]["pendingFrames"] == 1 and not pj["frames"][-1]["committed"], "valid uncommitted tail is distinct from damage")
        pending_output = capture / "pending-ignored.sqlite"
        report("recover", pending, "--base", base, "--assume-pre-wal-base", "--commit", "latest", "--output", pending_output)
        check(pending_output.read_bytes() == last_image, "uncommitted pages never enter recovery")
        damaged = capture / "damaged.wal"
        damaged.write_bytes(original + b"torn frame")
        dj = report("inspect", damaged, code=2)
        check(not dj["complete"] and dj["tail"]["status"] == "partial-frame", "damaged tail retains an explicitly incomplete prefix")
        refused = capture / "refused.sqlite"
        run("recover", damaged, "--base", base, "--assume-pre-wal-base", "--commit", "latest", "--output", refused, code=2)
        check(not refused.exists(), "tail acceptance is explicit")
        accepted = report("recover", damaged, "--base", base, "--assume-pre-wal-base", "--accept-tail", "--commit", "latest", "--output", refused, code=1)
        check(accepted["acceptedInvalidTail"] and not accepted["complete"] and refused.read_bytes() == last_image, "accepted prefix has qualified status and identical committed image")
        before_hash = hashlib.sha256(original).hexdigest()
        run("inspect", wal, "--output", wal, code=2)
        run("recover", wal, "--base", base, "--assume-pre-wal-base", "--commit", "latest", "--output", wal, code=2)
        hardlink = capture / "alias.wal"
        hardlink.hardlink_to(wal)
        run("inspect", wal, "--output", hardlink, code=2)
        check(hashlib.sha256(wal.read_bytes()).hexdigest() == before_hash and base.read_bytes() == base_original, "sources survive existing-file and hard-link output aliases")
        check(not list(capture.glob("*.walscope-stage-*")), "all owned staging files are cleaned")

    capture = temp / "captur\u00e9 \u0130 4096"
    wal, base = capture / "capture.wal", capture / "base.sqlite"
    original = wal.read_bytes()
    clean = report("inspect", wal)
    records = frames(original)
    for option, value in [("--max-frames", "0"), ("--max-frames", "-1"), ("--rows", "0"), ("--rows", "1001"), ("--max-input-mib", "4097"), ("--max-output-mib", "2049"), ("--rows", "184467440737095516160"), ("--rows", "+1")]:
        run("inspect", wal, option, value, code=2)
    limited_path = temp / "no-partial-report.json"
    p = run("inspect", wal, "--max-frames", 1, "--output", limited_path, code=2)
    check(not limited_path.exists() and p.stdout == "", "frame limit cannot produce a successful partial report")
    for args in [("inspect", wal, "--rows", 1, "--rows", 2), ("page", wal), ("inspect", wal, "--page", 2), ("diff", wal, "--from", 1), ("recover", wal, "--commit", "latest"), ("recover", wal, "--commit", "latest", "--output", ""), ("inspect", wal, "--format", "csv"), ("inspect", wal, "--base", base)]:
        run(*args, code=2)
    run("recover", wal, "--base", base, "--commit", "latest", "--output", temp / "not-authorized.sqlite", code=2)
    run("recover", wal, "--commit", "latest", "--output", temp / "missing-base.sqlite", code=2)
    run("recover", wal, "--base", base, "--assume-pre-wal-base", "--commit", 99, "--output", temp / "unknown-commit.sqlite", code=2)
    mismatch = temp / "wrong-size.sqlite"
    mismatch.write_bytes((temp / "captur\u00e9 \u0130 512" / "base.sqlite").read_bytes())
    run("recover", wal, "--base", mismatch, "--assume-pre-wal-base", "--commit", "latest", "--output", temp / "wrong-output.sqlite", code=2)
    # Stop at the first invalid full frame, including a generation's leftover bytes.
    for reason, position in [("salt-mismatch", 32 + 8), ("checksum-mismatch", 32 + 24), ("invalid-page-number", 32)]:
        raw = bytearray(original)
        if reason == "invalid-page-number":raw[position:position+4] = b"\0"*4
        else:raw[position] ^= 1
        f = temp / (reason + ".wal"); f.write_bytes(raw)
        result = report("inspect", f, code=2)
        check(result["tail"]["status"] == reason and result["summary"]["validFrames"] == 0, "invalid frame never resynchronizes to later valid-looking commits")
    for length in (1, 7, 16, 31):
        f = temp / f"short-{length}.wal"; f.write_bytes(original[:length])
        check(run("inspect", f, code=2).stdout == "", "truncated header is not a prefix proof")
    empty = temp / "empty.wal"; empty.write_bytes(b"")
    check(report("inspect", empty)["summary"]["commits"] == 0, "zero-byte reset WAL")
    header = temp / "header.wal"; header.write_bytes(original[:32])
    check(report("inspect", header)["summary"]["validFrames"] == 0, "header-only WAL")
    for position in (0, 4, 8, 24):
        raw = bytearray(original); raw[position] ^= 0x80
        f = temp / f"bad-header-{position}.wal"; f.write_bytes(raw)
        check(run("inspect", f, code=2).stdout == "", "invalid header cannot be accepted through tail recovery")
    oversized = temp / "oversized.wal"; oversized.write_bytes(b"x" * (1024*1024+1))
    check("byte budget" in run("inspect", oversized, "--max-input-mib", 1, code=2).stderr, "input limit precedes decoding")
    huge = temp / "huge-image.wal"; huge.write_bytes(encode(4096, [(1,300,records[0][2])]))
    check("image byte budget" in run("recover", huge, "--commit", 1, "--max-output-mib", 1, "--output", temp/"huge.sqlite", code=2).stderr, "image bounds precede page index allocation")
    # A shrink invalidates old base/WAL pages; later growth cannot resurrect them.
    page_one = base.read_bytes()[:4096]
    def sized_page(n):
        p = bytearray(page_one); struct.pack_into(">I",p,28,n); return bytes(p)
    regression = temp / "shrink-regrow.wal"
    regression.write_bytes(encode(4096,[(1,1,sized_page(1)),(1,3,sized_page(3))]))
    rejected = run("recover", regression, "--base", base, "--assume-pre-wal-base", "--commit", "latest", "--output", temp/"resurrected.sqlite", code=2)
    check("no valid source" in rejected.stderr and not (temp/"resurrected.sqlite").exists(), "truncated pages are never silently resurrected from the base")
    # A WAL with a complete selected image needs no operator-provided base.
    first_image = (capture / "snapshot-1.sqlite").read_bytes()
    contents = [first_image[i:i+4096] for i in range(0,len(first_image),4096)]
    full = temp/"self-contained.wal"
    full.write_bytes(encode(4096,[(i+1,len(contents) if i+1==len(contents) else 0,p) for i,p in enumerate(contents)]))
    standalone = temp/"standalone.sqlite"
    receipt = report("recover", full, "--commit", 1, "--output", standalone)
    check(receipt["base"] is None and standalone.read_bytes() == first_image, "complete WAL-only image is reconstructed byte for byte")
    validate_sql(standalone,json.loads((capture/"expected.json").read_text())["states"][0])
    duplicate = temp/"duplicate-page.wal"
    duplicate.write_bytes(encode(4096,[(2,0,contents[1]),(2,0,contents[1]),(1,len(contents),contents[0])]))
    dj = report("inspect",duplicate)
    check(dj["commits"][0]["uniquePagesWritten"] == 2 and dj["summary"]["validFrames"] == 3, "repeated page versions do not inflate distinct writes")
    report_path = temp/"report.json"
    run("inspect",wal,"--format","json","--output",report_path)
    check(json.loads(report_path.read_text())["complete"], "saved JSON is parseable")
    try:
        symlink=temp/"symlink.json";symlink.symlink_to(report_path)
    except OSError:pass
    else:run("inspect",wal,"--output",symlink,code=2)
    check(not list(temp.rglob("*.walscope-stage-*")), "no staging files remain after failures")
    if (root/"README.md").exists():
        links=[s.split("#")[0] for s in re.findall(r"\]\(([^)]+)\)",(root/"README.md").read_text(encoding="utf-8")) if "://" not in s and not s.startswith("#")]
        check(all((root/s).is_file() for s in links), "source and installed documentation links")

print(f"SQLite oracle and adversarial assertions passed: {checks}; three page sizes, both checksum byte orders, all selected commits validated.")
