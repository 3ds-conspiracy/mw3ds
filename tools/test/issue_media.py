"""Puts screenshots for a GitHub issue on the repo's `issue-media` branch and prints the URLs to embed in a comment
(the GitHub CLI cannot attach images to issue comments). The branch holds only these pictures, one folder per
issue, and is never merged; files are written with git plumbing, so the working tree and the checked-out branch
are not touched.

    python tools/test/issue_media.py 29 build/shots/issue-29/before.png build/shots/issue-29/after.png [--push]

Without --push it only commits to the local branch (prints the URLs the files will have once pushed).
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

BRANCH = "issue-media"
ROOT = Path(__file__).resolve().parents[2]


def git(*args, env=None, inp=None):
    r = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, env=env, input=inp)
    if r.returncode:
        raise SystemExit(f"git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout.strip()


def repo_slug():
    url = git("remote", "get-url", "origin")
    m = re.search(r"github\.com[:/](.+?)(\.git)?$", url)
    if not m:
        raise SystemExit(f"origin is not a GitHub remote: {url}")
    return m.group(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("issue", type=int)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--push", action="store_true")
    a = ap.parse_args()

    # Start from the remote branch when there is one, so pictures other sessions pushed are kept
    subprocess.run(["git", "fetch", "origin", f"{BRANCH}:{BRANCH}"], cwd=ROOT, capture_output=True)
    parent = subprocess.run(["git", "rev-parse", "--verify", "-q", f"refs/heads/{BRANCH}"], cwd=ROOT,
                            capture_output=True, text=True).stdout.strip()
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(tmp) / "index"))
        if parent:
            git("read-tree", parent, env=env)
        names = []
        for f in a.files:
            p = Path(f)
            if p.suffix.lower() not in (".png", ".jpg", ".jpeg", ".gif"):
                raise SystemExit(f"not a picture: {f}")
            blob = git("hash-object", "-w", str(p.resolve()))
            name = f"issue-{a.issue}/{p.name}"
            git("update-index", "--add", "--cacheinfo", f"100644,{blob},{name}", env=env)
            names.append(name)
        tree = git("write-tree", env=env)
    msg = f"Screenshots for issue #{a.issue}"
    commit = git("commit-tree", tree, *(["-p", parent] if parent else []), "-m", msg)
    git("update-ref", f"refs/heads/{BRANCH}", commit)
    if a.push:
        git("push", "origin", f"{BRANCH}:{BRANCH}")
    slug = repo_slug()
    for n in names:
        print(f"https://raw.githubusercontent.com/{slug}/{BRANCH}/{n}")
    if not a.push:
        print("(local only: run again with --push to publish)", file=sys.stderr)


if __name__ == "__main__":
    main()
