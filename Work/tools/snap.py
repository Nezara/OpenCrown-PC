#!/usr/bin/env python3
"""Copy the current save into Work/story/snap_NN_<label>/ (next free NN).  usage: snap.py <label>"""
import os, shutil, sys, pathlib
root = pathlib.Path(__file__).resolve().parents[1] / "story"
src = pathlib.Path(os.environ.get("DCP_SAVE_ROOT") or pathlib.Path.home() / "Documents/Dragons Crown Pro/saves") / "SAVEDATA00/SAVE0.DAT"
n = 1 + max([int(p.name[5:7]) for p in root.glob("snap_[0-9][0-9]_*")] or [0])
dst = root / f"snap_{n:02d}_{sys.argv[1]}"
dst.mkdir(parents=True)
shutil.copy2(src, dst)
print(dst)
