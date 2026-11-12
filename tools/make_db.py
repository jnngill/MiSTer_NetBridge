#!/usr/bin/env python3
"""Writes db.json, the MiSTer Downloader database for NetBridge.

    python3 tools/make_db.py           write db.json
    python3 tools/make_db.py --check   fail if db.json does not match the files

Run it after changing anything under Scripts/ and commit db.json with it:
the database pins each file's MD5, and Downloader refuses a file whose hash
does not match.
"""
import hashlib
import json
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DB_ID = "mister_netbridge"
BASE_URL = "https://raw.githubusercontent.com/jnngill/MiSTer_NetBridge/main/"
FILES = ["Scripts/netbridge.sh", "Scripts/.config/netbridge/netbridge"]
FOLDERS = ["Scripts", "Scripts/.config", "Scripts/.config/netbridge"]


def entries():
    files = {}
    for path in FILES:
        data = open(os.path.join(ROOT, path), "rb").read()
        files[path] = {"hash": hashlib.md5(data).hexdigest(), "size": len(data)}
    return files


def main():
    db_path = os.path.join(ROOT, "db.json")
    files = entries()
    if "--check" in sys.argv:
        db = json.load(open(db_path))
        if db.get("files") != files:
            sys.exit("db.json is stale: run python3 tools/make_db.py and commit it")
        print("db.json matches")
        return
    db = {
        "v": 1,
        "db_id": DB_ID,
        "timestamp": int(time.time()),
        "base_files_url": BASE_URL,
        "files": files,
        "folders": {f: {} for f in FOLDERS},
    }
    with open(db_path, "w", newline="\n") as f:
        json.dump(db, f, indent=2)
        f.write("\n")
    print("wrote", db_path)


if __name__ == "__main__":
    main()
