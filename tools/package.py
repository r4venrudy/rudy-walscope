"""Exercise an installed native layout and archive only controlled package contents."""
from pathlib import Path
from contextlib import closing
import argparse
import importlib.util
import json
import os
import sqlite3
import subprocess
import sys
import tarfile
import zipfile
sys.dont_write_bytecode = True

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("stage",type=Path)
parser.add_argument("target",choices=["linux-x64","windows-x64","macos-arm64"])
parser.add_argument("--version",required=True)
args=parser.parse_args()
stage=args.stage.resolve()
binary=stage/"bin"/("walscope.exe" if os.name=="nt" else "walscope")
def run(*arguments):
    return subprocess.check_output([binary,*map(str,arguments)],text=True,encoding="utf-8",timeout=30)
assert run("--version").strip()=="walscope "+args.version
spec=importlib.util.spec_from_file_location("synthetic",stage/"examples/make_capture.py")
generator=importlib.util.module_from_spec(spec);spec.loader.exec_module(generator)
capture=stage/"examples/synthetic"
generator.create_capture(capture)
wal,base=capture/"capture.wal",capture/"base.sqlite"
run("inspect",wal,"--format","html","--output",capture/"timeline.html")
run("inspect",wal,"--format","json","--output",capture/"inspection.json")
run("diff",wal,"--base",base,"--assume-pre-wal-base","--from",1,"--to","latest","--format","html","--output",capture/"comparison.html")
image=capture/"recovered.sqlite"
receipt=json.loads(run("recover",wal,"--base",base,"--assume-pre-wal-base","--commit","latest","--output",image,"--format","json"))
(capture/"recovery-receipt.json").write_text(json.dumps(receipt,indent=2)+"\n",encoding="utf-8")
expected=json.loads((capture/"expected.json").read_text())
with closing(sqlite3.connect(image.as_uri()+"?mode=ro&immutable=1",uri=True)) as connection:
    assert connection.execute("PRAGMA integrity_check").fetchall()==[("ok",)]
    assert [list(r) for r in connection.execute("SELECT id,payload FROM events ORDER BY id")]==expected["states"][-1]
assert not list(stage.rglob("*.walscope-stage-*"))
name="walscope-"+args.version+"-"+args.target
destination=Path("dist");destination.mkdir(exist_ok=True)
files=sorted(p for p in stage.rglob("*") if "__pycache__" not in p.relative_to(stage).parts and p.suffix != ".pyc")
assert all(not p.is_symlink() for p in files)
archive=destination/(name+(".zip" if args.target=="windows-x64" else ".tar.gz"))
assert not archive.exists(),"Refusing to replace an existing archive"
if archive.suffix==".zip":
    with zipfile.ZipFile(archive,"x",compression=zipfile.ZIP_DEFLATED) as output:
        for file in files:output.write(file,file.relative_to(stage).as_posix())
else:
    with tarfile.open(archive,"x:gz") as output:
        for file in files:output.add(file,arcname=file.relative_to(stage).as_posix(),recursive=False)
print("Verified installed image and created",archive)
