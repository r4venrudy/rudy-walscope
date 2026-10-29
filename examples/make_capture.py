"""Produce an invented SQLite workload and stable offline copies. Never open user databases."""
from pathlib import Path
import argparse
import json
import sqlite3
import tempfile

def create_capture(destination, page_size=4096):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(prefix="walscope-synthetic-") as temp:
        database = Path(temp) / "work.sqlite"
        connection = sqlite3.connect(database)
        try:
            connection.execute(f"PRAGMA page_size={page_size}")
            assert connection.execute("PRAGMA journal_mode=WAL").fetchone()[0] == "wal"
            connection.execute("PRAGMA wal_autocheckpoint=0")
            connection.execute("CREATE TABLE events(id INTEGER PRIMARY KEY, payload TEXT NOT NULL)")
            connection.execute("CREATE TABLE static_notes(note TEXT NOT NULL)")
            connection.execute("INSERT INTO static_notes VALUES ('synthetic unchanged base page')")
            connection.execute("INSERT INTO events VALUES (0, 'seed')")
            connection.commit()
            connection.execute("PRAGMA wal_checkpoint(TRUNCATE)")
            base = database.read_bytes()
            states = []
            connection.executemany("INSERT INTO events VALUES (?,?)", [(i, f"alpha-{i}:" + "x" * 200) for i in range(1, 21)])
            connection.commit()
            states.append(connection.execute("SELECT id,payload FROM events ORDER BY id").fetchall())
            connection.execute("UPDATE events SET payload='updated synthetic value' WHERE id=1")
            connection.execute("DELETE FROM events WHERE id=2")
            connection.commit()
            states.append(connection.execute("SELECT id,payload FROM events ORDER BY id").fetchall())
            connection.execute("INSERT INTO events VALUES (21,'last synthetic record')")
            connection.execute("CREATE INDEX events_payload ON events(payload)")
            connection.commit()
            states.append(connection.execute("SELECT id,payload FROM events ORDER BY id").fetchall())
            wal = Path(str(database) + "-wal").read_bytes()
        finally:
            connection.close()
    (destination / "base.sqlite").write_bytes(base)
    (destination / "capture.wal").write_bytes(wal)
    expected = {"schema": "rudy-walscope/synthetic-v1", "pageSize": page_size, "states": states, "staticNotes": [["synthetic unchanged base page"]]}
    (destination / "expected.json").write_text(json.dumps(expected, indent=2) + "\n", encoding="utf-8")
    return expected

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("new_directory", help="New output directory; existing directories are refused")
    parser.add_argument("--page-size", type=int, choices=[512, 4096, 65536], default=4096)
    args = parser.parse_args()
    create_capture(args.new_directory, args.page_size)
    print("Created only synthetic base.sqlite, capture.wal, and expected.json.")
