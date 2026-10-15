"""Shared helpers for the build and test scripts in tools/build and tools/test: the repo root, .env settings,
running make (inside MSYS2's devkitPro on Windows, plain make elsewhere), links, and process handling.
Import it from a script with:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    import mwtools
"""
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IS_WINDOWS = os.name == "nt"
NATIVE_EXE = "build/native/mw3ds-native.exe"          # (build.py names it .exe on every system)
TEST_DIR = ROOT / "tools" / "test"
CASES = TEST_DIR / "cases"

_loaded = False


def load_env():
    """Reads the repo's .env (see .env.example) into os.environ; variables already set win over the file."""
    global _loaded
    if _loaded:
        return
    _loaded = True
    env = ROOT / ".env"
    if not env.is_file():
        hint = (" Found '.env.txt': rename it to '.env' (Explorer hides the .txt; turn on View > File name extensions)."
                if (ROOT / ".env.txt").is_file() else " Copy .env.example to .env in that folder and fill it in.")
        print(f"warning: no .env file at {env}.{hint}", file=sys.stderr)
        return
    for line in env.read_text(encoding="utf-8-sig").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        value = value.strip().strip('"').strip("'")
        if value:
            os.environ.setdefault(key.strip(), value)


def get_env(name, what):
    """The value of environment variable `name` (from the shell or .env), or exit saying how to set it."""
    load_env()
    value = os.environ.get(name)
    if not value:
        raise SystemExit(f"{name} is not set: {what}. Put it in .env (copy .env.example) or the environment.")
    return value


def optional_env(name):
    load_env()
    return os.environ.get(name) or ""


def msys_path(path):
    """C:\\a\\b -> /c/a/b (the path as MSYS2's bash sees it)."""
    p = str(Path(path).resolve()).replace("\\", "/")
    m = re.match(r"^([A-Za-z]):(.*)$", p)
    return "/" + m.group(1).lower() + m.group(2) if m else p


def _msys_bash():
    return Path(get_env("MW3DS_MSYS2_DIR", "your MSYS2 folder")) / "usr" / "bin" / "bash.exe"


def devkitpro_bash(command, capture=False):
    """Runs a shell command line in the devkitPro environment: MSYS2's bash on Windows, sh elsewhere.
    Returns the CompletedProcess (stdout and stderr together when `capture`)."""
    kw = {"stdout": subprocess.PIPE, "stderr": subprocess.STDOUT, "text": True, "errors": "replace"} if capture else {}
    if IS_WINDOWS:
        env = dict(os.environ, MSYSTEM="MSYS")
        return subprocess.run([str(_msys_bash()), "-lc", command], env=env, **kw)
    return subprocess.run(["sh", "-c", command], **kw)


def project_dir_for_shell():
    return msys_path(ROOT) if IS_WINDOWS else str(ROOT)


def make(args, capture=False):
    """Runs make in the repo for the 3DS build (every hardware thread compiles)."""
    jobs = os.cpu_count() or 2
    cmd = f"cd {shlex.quote(project_dir_for_shell())} && make -j{jobs} {' '.join(shlex.quote(a) for a in args)} 2>&1"
    return devkitpro_bash(cmd, capture)


def devkitarm_bin():
    """The folder of the arm-none-eabi tools as the shell sees it."""
    return (os.environ.get("DEVKITARM") or "/opt/devkitpro/devkitARM") + "/bin"


def kill_tree(proc):
    """Stops a started process and everything it started."""
    try:
        import psutil
        try:
            parent = psutil.Process(proc.pid)
        except psutil.NoSuchProcess:
            return
        for child in parent.children(recursive=True):
            try:
                child.kill()
            except psutil.NoSuchProcess:
                pass
        parent.kill()
    except ImportError:
        proc.kill()


def hidden_flags():
    """Popen creationflags for a background process without a console window (Windows only)."""
    return getattr(subprocess, "CREATE_NO_WINDOW", 0) if IS_WINDOWS else 0


def kill_processes_under(directory, names):
    """Stops running programs called one of `names` whose executable lives under `directory`."""
    import psutil
    base = os.path.normcase(str(Path(directory).resolve()))
    for p in psutil.process_iter(["name", "exe"]):
        try:
            exe = p.info["exe"]
            if p.info["name"] and p.info["name"].lower() in names and exe and os.path.normcase(exe).startswith(base):
                p.kill()
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass


def link_target(path):
    """Where a link (symlink, or a junction on Windows) points, or None when `path` is not a link."""
    try:
        t = os.readlink(path)
    except (OSError, ValueError):
        return None
    return t[4:] if t.startswith("\\\\?\\") else t


def remove_link(path):
    """Removes a link itself, never the folder it points at."""
    if IS_WINDOWS:
        os.rmdir(path)
    else:
        os.unlink(path)


def make_link(link, target):
    """A directory link (a junction on Windows, which needs no special rights): no second copy of the data."""
    target = str(Path(target).resolve())
    if IS_WINDOWS:
        import _winapi
        _winapi.CreateJunction(target, str(link))
    else:
        os.symlink(target, link, target_is_directory=True)


def link_data_dir(link, target):
    """Makes `link` point at `target`, replacing a different link or an old real copy."""
    link = Path(link)
    cur = link_target(link)
    want = str(Path(target).resolve())
    if cur is not None and os.path.normcase(cur.rstrip("\\/")) == os.path.normcase(want):
        return
    if cur is not None:
        remove_link(link)
    elif link.exists():
        shutil.rmtree(link)
    make_link(link, want)


def copy_tree_linked(src, dst):
    """Copies a folder, hard-linking the files where the disk allows (no second copy of a gigabyte)."""
    src, dst = Path(src), Path(dst)
    if dst.exists():
        shutil.rmtree(dst)

    def copy(s, d):
        try:
            os.link(s, d)
        except OSError:
            shutil.copy2(s, d)
    shutil.copytree(src, dst, copy_function=copy)


def read_lines(path):
    return Path(path).read_text(encoding="utf-8", errors="replace").splitlines()


def write_lines(path, lines):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text("\n".join(lines) + "\n", encoding="ascii", errors="replace", newline="\n")


def grep(path, pattern):
    """Lines of a file matching a regular expression (case-insensitive)."""
    rx = re.compile(pattern, re.IGNORECASE)
    return [ln for ln in read_lines(path) if rx.search(ln)] if Path(path).exists() else []


def tail_lines(path, count):
    return read_lines(path)[-count:]


def emulator_exe(emu_dir):
    return Path(emu_dir) / ("azahar.exe" if IS_WINDOWS else "azahar")
