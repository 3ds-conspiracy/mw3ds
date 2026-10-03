"""Compiles every script in Morrowind.esm and lists lines where an expression left tokens unread
(usually a function whose arguments tools/mwscript.py's EXPR_ARITY doesn't know).

  python tools/scriptcheck.py"""
import collections
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import mwscript
from mwfiles import DATA_FILES, read_records


def main():
    path = DATA_FILES / "Morrowind.esm" if not str(DATA_FILES).endswith(".esm") else DATA_FILES
    globals_ = {r.zstr("NAME").lower() for r in read_records(path, {"GLOB"})}
    lines, scripts, failed = collections.Counter(), 0, []
    for r in read_records(path, {"SCPT"}):
        text = (r.get("SCTX") or b"").decode("latin-1")
        c = mwscript.Compiler(globals_)
        try:
            c.compile(text)
        except mwscript.ScriptError as e:
            failed.append(f"script {r.zstr('NAME')}: {e}")      # the game would lose it: say so
            continue
        scripts += 1
        for line in c.leftovers:
            lines[line.strip()] += 1
    # Dialogue result scripts
    results = 0
    for r in read_records(path, {"INFO"}):
        text = (r.get("BNAM") or b"").decode("latin-1")
        if not text.strip():
            continue
        c = mwscript.Compiler(globals_)
        try:
            c.compile(text)
        except mwscript.ScriptError as e:
            failed.append(f"dialogue result {r.zstr('INAM')}: {e}")
            continue
        results += 1
        for line in c.leftovers:
            lines[line.strip()] += 1
    print(f"{scripts} scripts, {results} dialogue results, {len(lines)} lines with unread tokens, "
          f"{len(failed)} that don't compile")
    for f in failed:
        print("  FAILED", f)
    for line, n in lines.most_common():
        print(f"{n:4d}  {line}")


if __name__ == "__main__":
    main()
