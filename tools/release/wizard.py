"""The release converter's window: pick your Morrowind folder, console, language and plugins, then it converts the
game and packs the CIA by running tools/build/build_cia.py --prebuilt, exactly as a developer's build does.
    python\\pythonw.exe app\\tools\\release\\wizard.py     (what MW3DS Converter.exe runs)
    python tools/release/wizard.py --dry-run             in a repo checkout: the window only, Start prints the command
The release folder (tools/release/make_release.py) holds python\\, app\\ (this file's repo layout) and bin\\
(tex3ds, makerom). The work is done in a work folder: app\\ is copied there and the conversion writes out\\ (the
converted data and its caches, several GB) and build\\ (the CIA) inside it. Keeping the same work folder across
releases keeps the caches, so a later conversion is much faster.
"""
import argparse
import hashlib
import json
import os
import queue
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import tkinter as tk
import zipfile
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mwtools                                         # (tools/mwtools.py: the downloads)

APP = Path(__file__).resolve().parents[2]              # app\ in the release, the repo root in a checkout
RELEASE = APP.parent
BIN = RELEASE / "bin"
SETTINGS = RELEASE / "settings.json"
FFMPEG_LOCK = APP / "ffmpeg.lock.json"                 # (only in a release: tools/release/make_release.py ffmpeg_lock)
TITLE = "MW3DS Converter"

# The code pages Morrowind's releases use, by language (tools/convert/mwfiles.py ENCODINGS; LANGUAGES.md)
LANGUAGES = [
    ("English", "win1252"), ("German", "win1252"), ("French", "win1252"), ("Italian", "win1252"),
    ("Spanish", "win1252"), ("Polish", "win1250"), ("Czech", "win1250"), ("Hungarian", "win1250"),
    ("Russian", "win1251"), ("Ukrainian", "win1251"),
]
CODECS = {"win1252": "cp1252", "win1250": "cp1250", "win1251": "cp1251"}
# Masters this project leaves out (mwfiles.out_of_scope(); the wizard has no Tribunal option yet): a plugin needing one is
# refused by the converter
OUT_OF_SCOPE = {"tribunal.esm", "bloodmoon.esm"}
NEED_GB = 5                                            # whole island: mods 0.6 + data 1.1 + caches 0.2 + CIA 1.1, and room
NEED_RAM_GB = 4


# ---- Finding Morrowind

def registry_value(root, key, name):
    try:
        import winreg
        for view in (winreg.KEY_WOW64_32KEY, winreg.KEY_WOW64_64KEY):
            try:
                with winreg.OpenKey(root, key, 0, winreg.KEY_READ | view) as k:
                    return str(winreg.QueryValueEx(k, name)[0])
            except OSError:
                pass
    except ImportError:
        pass
    return None


def steam_libraries():
    import winreg
    steam = registry_value(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath")
    if not steam:
        return []
    libs = [Path(steam)]
    vdf = Path(steam) / "steamapps" / "libraryfolders.vdf"
    try:
        for p in re.findall(r'"path"\s+"([^"]+)"', vdf.read_text(encoding="utf-8", errors="replace")):
            libs.append(Path(p.replace("\\\\", "\\")))
    except OSError:
        pass
    return libs


def find_morrowind():
    """Install folders that hold Data Files\\Morrowind.esm: the original installer's and GOG's registry keys, Steam's
    libraries, then the usual places."""
    cands = []
    try:
        import winreg
        lm = winreg.HKEY_LOCAL_MACHINE
        cands.append(registry_value(lm, r"SOFTWARE\Bethesda Softworks\Morrowind", "Installed Path"))
        cands.append(registry_value(lm, r"SOFTWARE\GOG.com\Games\1435828767", "path"))
        cands += [str(lib / "steamapps" / "common" / "Morrowind") for lib in steam_libraries()]
    except ImportError:
        pass
    for drive in ("C:", "D:", "E:"):
        cands += [rf"{drive}\Program Files (x86)\Steam\steamapps\common\Morrowind", rf"{drive}\GOG Games\Morrowind",
                  rf"{drive}\Program Files (x86)\GOG Galaxy\Games\Morrowind",
                  rf"{drive}\Program Files (x86)\Bethesda Softworks\Morrowind"]
    seen = []
    for c in cands:
        if c and morrowind_problem(c) is None and Path(c).resolve() not in seen:
            seen.append(Path(c).resolve())
    return [str(p) for p in seen]


def morrowind_problem(folder):
    """Why `folder` is not a Morrowind install the converter can use, or None."""
    data = Path(folder) / "Data Files"
    if not (data / "Morrowind.esm").is_file():
        return "There is no Data Files\\Morrowind.esm in this folder: pick the folder holding Morrowind.exe."
    if not (data / "Morrowind.bsa").is_file():
        return "There is no Data Files\\Morrowind.bsa in this folder."
    return None


# ---- Reading the game files (just enough for the choices: no converter import, it wants its settings first)

def records(path, stop_after=None):
    """(tag, subrecords) of an ESM / ESP: subrecords as [(tag, bytes)]. Stops after the records of `stop_after`
    once a different type follows them."""
    with open(path, "rb") as f:
        seen = False
        while True:
            head = f.read(16)
            if len(head) < 16:
                return
            tag, size = head[:4].decode("latin-1"), struct.unpack_from("<I", head, 4)[0]
            if stop_after and seen and tag != stop_after:
                return
            seen = seen or tag == stop_after
            body = f.read(size)
            subs, i = [], 0
            while i + 8 <= len(body):
                stag, ssize = body[i:i + 4].decode("latin-1"), struct.unpack_from("<I", body, i + 4)[0]
                subs.append((stag, body[i + 8:i + 8 + ssize]))
                i += 8 + ssize
            yield tag, subs
            if tag == "TES3" and stop_after == "TES3":
                return


def masters(path):
    for tag, subs in records(path, "TES3"):
        return [d.split(b"\0", 1)[0].decode("latin-1") for t, d in subs if t == "MAST"]
    return []


def game_strings(esm):
    """The GMST text settings of Morrowind.esm, as bytes."""
    out = []
    for tag, subs in records(esm, "GMST"):
        if tag == "GMST":
            out += [d.split(b"\0", 1)[0] for t, d in subs if t == "STRV"]
    return out


def guess_encoding(strings):
    """The code page a Morrowind.esm's text is most likely in: Cyrillic text is mostly bytes 0xC0-0xFF; Polish and
    Czech use bytes cp1250 gives their own letters (ą ł ś ź ż ę ...) and Western text hardly ever has."""
    blob = b"".join(strings)
    letters = sum(1 for b in blob if b >= 0x41)
    high = sum(1 for b in blob if b >= 0xC0)
    if letters and high / letters > 0.3:
        return "win1251"
    central = sum(blob.count(bytes([b])) for b in (0xB9, 0xB3, 0x9C, 0x9F, 0xBF, 0xEA, 0x8C, 0xA5, 0x9E, 0x8A))
    if central > 20:
        return "win1250"
    return "win1252"


def sample_lines(strings, encoding, n=3):
    """A few game strings with letters outside ASCII (they show a wrong code page at once), else any."""
    pick = [s for s in strings if any(b >= 0x80 for b in s) and len(s) < 60][:n] or [s for s in strings if len(s) < 60][:n]
    return [s.decode(CODECS[encoding], errors="replace") for s in pick]


def plugin_files(folder):
    """(name, refusal or None) of each plugin in Data Files, in Morrowind.ini's load order first."""
    data = Path(folder) / "Data Files"
    names = [p.name for p in data.iterdir() if p.suffix.lower() in (".esp", ".esm")
             and p.name.lower() not in ("morrowind.esm", "tribunal.esm", "bloodmoon.esm")]
    order = []
    try:
        ini = (Path(folder) / "Morrowind.ini").read_text(encoding="latin-1", errors="replace")
        order = [m.strip().lower() for m in re.findall(r"^GameFile\d+=(.+)$", ini, re.M)]
    except OSError:
        pass
    names.sort(key=lambda n: (order.index(n.lower()) if n.lower() in order else len(order), n.lower()))
    have = {n.lower() for n in names} | {"morrowind.esm"}
    out = []
    for n in names:
        try:
            need = [m.lower() for m in masters(data / n)]
        except OSError:
            out.append((n, "cannot be read"))
            continue
        bad = [m for m in need if m in OUT_OF_SCOPE]
        missing = [m for m in need if m not in have and m not in OUT_OF_SCOPE]
        out.append((n, f"needs {bad[0]} (not supported)" if bad else f"needs {missing[0]} (missing)" if missing else None))
    return out, order


# ---- The machine

def fetch_ffmpeg(work, say):
    """ffmpeg for the game's movies. The release does not ship it (a GPL build: tools/release/make_release.py
    ffmpeg_lock); it is downloaded once from PyPI, in the imageio-ffmpeg wheel the release pins, checked against the
    pinned SHA-256, and kept in the work folder."""
    lock = json.loads(FFMPEG_LOCK.read_text(encoding="utf-8"))
    exe = work / "ffmpeg" / f"ffmpeg-{lock['sha256'][:12]}.exe"
    if exe.is_file():
        return exe
    say(f"downloading ffmpeg for the movies from PyPI ({lock['size'] / 1048576:.0f} MB, the first time only)")
    wheel = work / "ffmpeg" / "imageio-ffmpeg.whl"
    try:
        mwtools.download(lock["url"], wheel)
    except OSError as e:
        raise RuntimeError(f"could not download ffmpeg ({e}): check the network (a firewall or antivirus may block "
                           f"Python)")
    if hashlib.sha256(wheel.read_bytes()).hexdigest() != lock["sha256"]:
        wheel.unlink()
        raise RuntimeError("the ffmpeg download does not match its checksum: try again later")
    with zipfile.ZipFile(wheel) as z:
        name = next(n for n in z.namelist() if re.fullmatch(r"imageio_ffmpeg/binaries/ffmpeg[^/]*\.exe", n))
        part = exe.with_suffix(".part")
        part.write_bytes(z.read(name))
        part.replace(exe)
    wheel.unlink()
    return exe


def free_ram_gb():
    try:
        import psutil
        return psutil.virtual_memory().total / 2 ** 30
    except Exception:
        return None


def free_disk_gb(folder):
    p = Path(folder)
    while not p.exists() and p.parent != p:
        p = p.parent
    try:
        return shutil.disk_usage(p).free / 2 ** 30
    except OSError:
        return None


# ---- The window

class Wizard:
    def __init__(self, root, dry_run):
        self.root, self.dry_run = root, dry_run
        self.proc = None
        self.lines = queue.Queue()
        self.strings = []
        self.settings = self.load_settings()
        root.title(TITLE)
        root.minsize(640, 560)
        pad = {"padx": 8, "pady": 3}
        f = ttk.Frame(root, padding=10)
        f.pack(fill="both", expand=True)
        f.columnconfigure(1, weight=1)
        row = 0
        ttk.Label(f, text="Makes a CIA of Morrowind for your 3DS from your own copy of the game (Steam, GOG or "
                          "disc). Nothing from the game is in this program.", wraplength=820,
                  font=("Segoe UI", 10, "bold")).grid(row=row, column=0, columnspan=3, sticky="w", padx=8, pady=(0, 8))
        row += 1

        ttk.Label(f, text="Morrowind folder").grid(row=row, column=0, sticky="w", **pad)
        self.morrowind = tk.StringVar(value=self.settings.get("morrowind", ""))
        self.mw_box = ttk.Combobox(f, textvariable=self.morrowind)
        self.mw_box.grid(row=row, column=1, sticky="ew", **pad)
        ttk.Button(f, text="Browse...", command=self.browse_morrowind).grid(row=row, column=2, **pad)
        self.mw_box.bind("<<ComboboxSelected>>", lambda e: self.morrowind_changed())
        self.mw_box.bind("<FocusOut>", lambda e: self.morrowind_changed())
        row += 1
        self.mw_note = ttk.Label(f, text="", foreground="#a33")
        self.mw_note.grid(row=row, column=1, columnspan=2, sticky="w", padx=8)
        row += 1

        ttk.Label(f, text="Console").grid(row=row, column=0, sticky="w", **pad)
        self.console = tk.StringVar(value=self.settings.get("console", "new"))
        c = ttk.Frame(f)
        c.grid(row=row, column=1, columnspan=2, sticky="w", **pad)
        ttk.Radiobutton(c, text="New 3DS / New 2DS", variable=self.console, value="new").pack(side="left")
        ttk.Radiobutton(c, text="Old 3DS / 2DS (smaller textures)", variable=self.console, value="old").pack(side="left", padx=12)
        row += 1

        ttk.Label(f, text="Language of your game").grid(row=row, column=0, sticky="w", **pad)
        self.language = tk.StringVar(value=self.settings.get("language", "English"))
        lang = ttk.Combobox(f, textvariable=self.language, state="readonly", values=[n for n, _ in LANGUAGES])
        lang.grid(row=row, column=1, sticky="w", **pad)
        lang.bind("<<ComboboxSelected>>", lambda e: self.show_sample())
        row += 1
        self.sample = ttk.Label(f, text="", foreground="#555", wraplength=520, justify="left")
        self.sample.grid(row=row, column=1, columnspan=2, sticky="w", padx=8)
        row += 1

        ttk.Label(f, text="Plugins (.esp)").grid(row=row, column=0, sticky="nw", **pad)
        pf = ttk.Frame(f)
        pf.grid(row=row, column=1, columnspan=2, sticky="nsew", **pad)
        self.plugins = tk.Listbox(pf, selectmode="multiple", height=5, exportselection=False, activestyle="none")
        self.plugins.bind("<<ListboxSelect>>", lambda e: self.show_plugin_count())
        self.plugins.pack(side="left", fill="both", expand=True)
        sb = ttk.Scrollbar(pf, command=self.plugins.yview)
        sb.pack(side="right", fill="y")
        self.plugins.config(yscrollcommand=sb.set)
        self.plugin_names = []
        row += 1
        self.plugin_note = ttk.Label(f, text="", foreground="#555", wraplength=640)
        self.plugin_note.grid(row=row, column=1, columnspan=2, sticky="w", padx=8)
        row += 1

        self.mods = tk.BooleanVar(value=self.settings.get("mods", True))
        ttk.Checkbutton(f, text="Use the optimization mods: faster on the 3DS (Morrowind Optimization Patch; "
                                "Project Atlas too for the Old 3DS).", variable=self.mods).grid(row=row, column=1,
                                                                                            columnspan=2, sticky="w", **pad)
        row += 1
        ttk.Label(f, text="Downloaded from their own pages the first time (about 550 MB).", foreground="#555").grid(
            row=row, column=1, columnspan=2, sticky="w", padx=28)
        row += 1

        ttk.Label(f, text="Output").grid(row=row, column=0, sticky="nw", **pad)
        self.output_kind = tk.StringVar(value=self.settings.get("output_kind", "cia"))
        o = ttk.Frame(f)
        o.grid(row=row, column=1, columnspan=2, sticky="w", **pad)
        ttk.Radiobutton(o, text="One CIA with the game inside (about 1 GB; install with FBI)",
                        variable=self.output_kind, value="cia").pack(anchor="w")
        ttk.Radiobutton(o, text="Small CIA + a 3ds\\mw3ds\\data folder to copy to the SD card (faster to install)",
                        variable=self.output_kind, value="sd").pack(anchor="w")
        row += 1
        ttk.Label(f, text="Save to").grid(row=row, column=0, sticky="w", **pad)
        self.out_dir = tk.StringVar(value=self.settings.get("out_dir", str(RELEASE / "output")))
        ttk.Entry(f, textvariable=self.out_dir).grid(row=row, column=1, sticky="ew", **pad)
        ttk.Button(f, text="Browse...", command=lambda: self.browse_dir(self.out_dir)).grid(row=row, column=2, **pad)
        row += 1
        ttk.Label(f, text="Work folder").grid(row=row, column=0, sticky="w", **pad)
        self.work = tk.StringVar(value=self.settings.get("work", str(RELEASE / "work")))
        ttk.Entry(f, textvariable=self.work).grid(row=row, column=1, sticky="ew", **pad)
        ttk.Button(f, text="Browse...", command=lambda: self.browse_dir(self.work)).grid(row=row, column=2, **pad)
        row += 1
        ttk.Label(f, text=f"Needs about {NEED_GB} GB free. Keep the same work folder for later versions: "
                          "what it has converted is reused. The first run also downloads ffmpeg (about 30 MB, "
                          "from PyPI) for the game's movies.", foreground="#555", wraplength=520).grid(
            row=row, column=1, columnspan=2, sticky="w", padx=8)
        row += 1

        b = ttk.Frame(f)
        b.grid(row=row, column=0, columnspan=3, sticky="ew", pady=(10, 4))
        self.start_btn = ttk.Button(b, text="Start", command=self.start)
        self.start_btn.pack(side="left", padx=8)
        self.stop_btn = ttk.Button(b, text="Stop", command=self.stop, state="disabled")
        self.stop_btn.pack(side="left")
        self.progress = ttk.Progressbar(b, mode="determinate", value=0)
        self.progress.pack(side="left", fill="x", expand=True, padx=8)
        self.status = ttk.Label(b, text="")
        self.status.pack(side="left", padx=8)
        row += 1
        f.rowconfigure(row, weight=1)
        lf = ttk.Frame(f)
        lf.grid(row=row, column=0, columnspan=3, sticky="nsew", padx=8)
        self.log = tk.Text(lf, height=10, wrap="none", state="disabled", font=("Consolas", 9))
        self.log.pack(side="left", fill="both", expand=True)
        lsb = ttk.Scrollbar(lf, command=self.log.yview)
        lsb.pack(side="right", fill="y")
        self.log.config(yscrollcommand=lsb.set)

        found = find_morrowind()
        self.mw_box["values"] = found
        if not self.morrowind.get() and found:
            self.morrowind.set(found[0])
        self.morrowind_changed(first=True)
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(100, self.pump)

    # -- settings

    def load_settings(self):
        try:
            return json.loads(SETTINGS.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}

    def save_settings(self):
        s = {"morrowind": self.morrowind.get(), "console": self.console.get(), "language": self.language.get(),
             "plugins": self.chosen_plugins(), "mods": self.mods.get(), "output_kind": self.output_kind.get(),
             "out_dir": self.out_dir.get(), "work": self.work.get()}
        try:
            SETTINGS.write_text(json.dumps(s, indent=2), encoding="utf-8")
        except OSError:
            pass

    # -- choices

    def browse_morrowind(self):
        d = filedialog.askdirectory(title="The Morrowind folder (holding Morrowind.exe and Data Files)")
        if d:
            self.morrowind.set(str(Path(d)))
            self.morrowind_changed()

    def browse_dir(self, var):
        d = filedialog.askdirectory(initialdir=var.get() or str(RELEASE))
        if d:
            var.set(str(Path(d)))

    def morrowind_changed(self, first=False):
        folder = self.morrowind.get().strip()
        self.plugins.delete(0, "end")
        self.plugin_names, self.strings = [], []
        if not folder:
            self.mw_note.config(text="Pick your Morrowind folder (Steam, GOG or disc install).")
            self.sample.config(text="")
            return
        problem = morrowind_problem(folder)
        self.mw_note.config(text=problem or "")
        if problem:
            self.sample.config(text="")
            return
        try:
            self.strings = game_strings(Path(folder) / "Data Files" / "Morrowind.esm")
        except OSError as e:
            self.mw_note.config(text=f"Morrowind.esm cannot be read: {e}")
        # A saved choice stays on the first look; a newly picked folder gets its own guess
        if not (first and self.settings.get("language")):
            guess = guess_encoding(self.strings)
            if dict(LANGUAGES).get(self.language.get()) != guess:
                self.language.set(next(n for n, e in LANGUAGES if e == guess))
        self.show_sample()
        chosen = {p.lower() for p in self.settings.get("plugins", [])} if first else set()
        try:
            listed, _ = plugin_files(folder)
        except OSError:
            listed = []
        for i, (name, refusal) in enumerate(listed):
            self.plugins.insert("end", name + (f"   ({refusal})" if refusal else ""))
            self.plugin_names.append((name, refusal))
            if refusal:
                self.plugins.itemconfig(i, foreground="#999", selectforeground="#999")
            elif name.lower() in chosen:
                self.plugins.selection_set(i)
        self.show_plugin_count()

    def show_plugin_count(self):
        # (the converter refuses a plugin it cannot load: a refused one cannot be picked)
        for i in self.plugins.curselection():
            if i < len(self.plugin_names) and self.plugin_names[i][1]:
                self.plugins.selection_clear(i)
        n = len(self.chosen_plugins())
        picked = f"{n} picked: " + ", ".join(self.chosen_plugins()) if n else "None picked: the game as it shipped."
        self.plugin_note.config(text=f"{picked}  Click a plugin to pick it or drop it; load order is Morrowind.ini's. "
                                     "Greyed ones need Tribunal or Bloodmoon, or a missing master.")

    def show_sample(self):
        enc = dict(LANGUAGES).get(self.language.get(), "win1252")
        lines = sample_lines(self.strings, enc) if self.strings else []
        self.sample.config(text=("Your game's text reads as: " + "  |  ".join(lines) +
                                 "\nIf this looks garbled, pick your game's language.") if lines else "")

    def chosen_plugins(self):
        return [self.plugin_names[i][0] for i in self.plugins.curselection()
                if i < len(self.plugin_names) and not self.plugin_names[i][1]]

    # -- running

    def write(self, text):
        self.log.config(state="normal")
        self.log.insert("end", text + "\n")
        self.log.see("end")
        self.log.config(state="disabled")

    def start(self):
        folder = self.morrowind.get().strip()
        problem = morrowind_problem(folder) if folder else "Pick your Morrowind folder first."
        if problem:
            messagebox.showerror(TITLE, problem)
            return
        work, out = Path(self.work.get().strip()), Path(self.out_dir.get().strip())
        if not work.is_absolute() or not out.is_absolute():
            messagebox.showerror(TITLE, "The work folder and the output folder need full paths (Browse...).")
            return
        disk = free_disk_gb(work)
        if disk is not None and disk < NEED_GB and not messagebox.askyesno(
                TITLE, f"The work folder's drive has {disk:.0f} GB free; the conversion needs about {NEED_GB} GB "
                       "(less if the work folder already holds an earlier conversion). Start anyway?"):
            return
        ram = free_ram_gb()
        if ram is not None and ram < NEED_RAM_GB and not messagebox.askyesno(
                TITLE, f"This PC has {ram:.0f} GB of memory; the conversion uses about {NEED_RAM_GB} GB and may fail "
                       "or be very slow. Start anyway?"):
            return
        self.save_settings()
        self.start_btn.config(state="disabled")
        self.stop_btn.config(state="normal")
        self.progress.config(mode="indeterminate")
        self.progress.start(15)
        self.t0 = time.time()
        self.log.config(state="normal")
        self.log.delete("1.0", "end")
        self.log.config(state="disabled")
        job = dict(morrowind=folder, work=work, out=out, old=self.console.get() == "old",
                   encoding=dict(LANGUAGES).get(self.language.get(), "win1252"), plugins=self.chosen_plugins(),
                   mods=self.mods.get(), sd=self.output_kind.get() == "sd")
        threading.Thread(target=self.run, args=(job,), daemon=True).start()

    def run(self, job):
        try:
            ok = self.convert(job)
        except Exception as e:                         # (shown, never lost in pythonw's missing console)
            self.lines.put(f"error: {e!r}")
            ok = False
        self.lines.put(("done", ok, job))

    def convert(self, job):
        work = job["work"]
        say = self.lines.put
        say(f"MW3DS Converter {(APP / 'VERSION.txt').read_text().strip() if (APP / 'VERSION.txt').is_file() else 'dev'}")
        if (APP / ".git").exists():
            work = APP                                 # (a repo checkout: run in place, as build_cia.py always does)
        say(f"work folder: {work}")
        if self.dry_run:
            pass
        elif work.resolve() != APP.resolve():
            work.mkdir(parents=True, exist_ok=True)
            # The repo layout the build scripts expect (their ROOT is the folder two up from them)
            for p in APP.rglob("*"):
                if p.is_file() and "__pycache__" not in p.parts:
                    d = work / p.relative_to(APP)
                    d.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(p, d)
            (work / "build").mkdir(exist_ok=True)
            env_file = work / ".env"
            if not env_file.exists():
                env_file.write_text("# The converter passes its settings directly; this file only stops a warning.\n")

        env = dict(os.environ)
        env.update(MW3DS_MORROWIND_DIR=job["morrowind"], MW3DS_ENCODING=job["encoding"],
                   MW3DS_PLUGINS=";".join(job["plugins"]))
        if (BIN / "makerom.exe").is_file():
            env.update(TEX3DS=str(BIN / "tex3ds.exe"), MW3DS_CTR_TOOLS_DIR=str(BIN))
        for k in ("MW3DS_OUT", "MW3DS_OVERLAY_DIRS"):
            env.pop(k, None)
        if FFMPEG_LOCK.is_file() and not self.dry_run:
            env["IMAGEIO_FFMPEG_EXE"] = str(fetch_ffmpeg(work, say))
        python = Path(sys.executable).with_name("python.exe")
        cmd = [str(python if python.is_file() else sys.executable), str(work / "tools" / "build" / "build_cia.py"),
               "--prebuilt"]
        cmd += ["--mods"] if job["mods"] else []
        cmd += ["--old3ds"] if job["old"] else []
        cmd += ["--no-data"] if job["sd"] else []
        cmd += ["--small"] if job.get("small") else []        # (only the Balmora area: for trying the converter out)
        say(f"language: {job['encoding']}, plugins: {', '.join(job['plugins']) or 'none'}")
        say("running: " + " ".join(cmd[1:]))
        if self.dry_run:
            say("(dry run: nothing started)")
            return True
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        self.proc = subprocess.Popen(cmd, cwd=work, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, text=True, encoding="utf-8", errors="replace",
                                     creationflags=flags)
        with open(work / "converter-log.txt", "w", encoding="utf-8") as logfile:
            for line in self.proc.stdout:
                line = line.rstrip()
                logfile.write(line + "\n")
                say(line)
        code = self.proc.wait()
        self.proc = None
        if code != 0:
            say(f"the conversion stopped (exit code {code}). The whole log is in {work / 'converter-log.txt'}.")
            return False

        # Hand over the results
        out = job["out"]
        out.mkdir(parents=True, exist_ok=True)
        cia = work / "build" / ("mw3ds-o3ds.cia" if job["old"] else "mw3ds.cia")
        # (moved, not copied: a second gigabyte for nothing; the next run packs a new one)
        shutil.move(cia, out / "mw3ds.cia")
        say(f"CIA: {out / 'mw3ds.cia'}")
        if job["sd"]:
            data = work / "out" / ("o3ds" if job["old"] else "data")
            dest = out / "3ds" / "mw3ds" / "data"
            say(f"copying the data to {dest} ...")
            if dest.exists():
                shutil.rmtree(dest)
            shutil.copytree(data, dest)
            say("data copied: put the 3ds folder on the root of the SD card (it merges with the one there).")
        return True

    def stop(self):
        if self.proc and messagebox.askyesno(TITLE, "Stop the conversion? What is done so far is kept for next time."):
            kill_tree(self.proc.pid)

    def close(self):
        if self.proc:
            if not messagebox.askyesno(TITLE, "The conversion is still running. Stop it and close?"):
                return
            kill_tree(self.proc.pid)
        self.save_settings()
        self.root.destroy()

    def pump(self):
        try:
            while True:
                item = self.lines.get_nowait()
                if isinstance(item, tuple):
                    self.finished(*item[1:])
                else:
                    self.write(item)
                    if not item.startswith(" "):
                        self.status.config(text=f"{(time.time() - self.t0) / 60:.0f} min")
        except queue.Empty:
            pass
        self.root.after(100, self.pump)

    def finished(self, ok, job):
        self.progress.stop()
        self.progress.config(mode="determinate", value=100 if ok else 0)
        self.start_btn.config(state="normal")
        self.stop_btn.config(state="disabled")
        mins = (time.time() - self.t0) / 60
        self.status.config(text=f"done in {mins:.0f} min" if ok else "stopped")
        if ok and not self.dry_run:
            how = ("Copy the 3ds folder to the root of your SD card, then install mw3ds.cia with FBI."
                   if job["sd"] else "Install mw3ds.cia with FBI (or another CIA installer).")
            messagebox.showinfo(TITLE, f"Done in {mins:.0f} minutes.\n\n{how}\n\nThe CIA holds your own copy of the "
                                       "game: it is for your console only.")
            if os.name == "nt":
                os.startfile(job["out"])
        elif not ok:
            messagebox.showerror(TITLE, "The conversion did not finish: see the log in the window. To report a bug, "
                                        "attach converter-log.txt from the work folder.")


def kill_tree(pid):
    """The build and every converter process under it."""
    if os.name == "nt":
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(pid)], capture_output=True,
                       creationflags=subprocess.CREATE_NO_WINDOW)
    else:
        os.kill(pid, 9)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true", help="show the command Start would run, run nothing")
    a = ap.parse_args()
    root = tk.Tk()
    try:
        Wizard(root, a.dry_run)
    except Exception as e:
        messagebox.showerror(TITLE, f"The converter could not start: {e!r}")
        raise
    root.mainloop()


if __name__ == "__main__":
    main()
