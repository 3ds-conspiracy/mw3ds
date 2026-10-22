"""Blood demo: OpenMW's side of a dagger blow on a person (tools/test/blooddemo/ is its Lua mod), filmed.

    python tools/test/blooddemo.py [--seed 1] [--out build/blooddemo]

OpenMW 0.51 (as castdemo.py runs it) starts in Seyda Neen; the player draws an iron dagger and strikes a Fargoth made 90 units ahead
and held still. ffmpeg films OpenMW's window (30 pictures a second) from the moment he is there; the log's "hit" line says when his
health first dropped. Writes <out>/blooddemo.mp4 and <out>/frames/fNNN.png and prints the frames around the hit (a sheet of them:
<out>/sheet.png, 400 x 240 each, 0.07 s apart).
"""
import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import castdemo
import playfight
import sidebyside as sbs
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
castdemo.MOD = Path(__file__).resolve().parent / "blooddemo"
castdemo.CD = ROOT / "build" / "openmw-bd"
CD = castdemo.CD


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", default=str(ROOT / "build" / "blooddemo"))
    ap.add_argument("--timeout", type=int, default=90)
    a = ap.parse_args()
    user = castdemo.setup()
    # (800 x 480: a blood spray is small at the 3DS's 400 x 240)
    cfg = CD / "settings.cfg"
    cfg.write_text(cfg.read_text().replace("resolution x = 400", "resolution x = 800").replace("resolution y = 240", "resolution y = 480"))
    for f in (CD / "openmw.log", user / "openmw.log"):
        if f.exists():
            f.unlink()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    video = out / "blooddemo.mp4"
    proc = subprocess.Popen([str(sbs.OPENMW / "openmw.exe"), "--replace=config", f"--config={CD}", "--skip-menu", "--start=Seyda Neen",
                             "--no-grab", "--no-sound", f"--random-seed={a.seed}"], cwd=sbs.OPENMW)
    cap, cap_t, lines, done = None, 0.0, [], False
    try:
        end = time.time() + a.timeout
        while time.time() < end and not done and proc.poll() is None:
            logs = [f for f in (CD / "openmw.log", user / "openmw.log") if f.exists()]
            if logs:
                text = logs[0].read_text(errors="replace").splitlines()
                lines = [ln for ln in text if "MW3DS-CD" in ln]
                done = any(ln.endswith("MW3DS-CD done") for ln in lines)
                if not cap and any("MW3DS-CD begin" in ln for ln in lines):
                    cap = playfight.start_capture(proc, video)
                    cap_t = time.time()
            time.sleep(0.1)
    finally:
        if cap:
            try:
                cap.communicate(b"q", timeout=20)
            except subprocess.TimeoutExpired:
                cap.kill()
        if proc.poll() is None:
            proc.kill()
        proc.wait()
    for ln in lines:
        print(ln.split("MW3DS-CD", 1)[1].strip())

    def wall(ln):
        m = re.match(r"\[(\d+):(\d+):(\d+)\.(\d+)", ln)
        now = time.localtime(cap_t)
        return time.mktime((now.tm_year, now.tm_mon, now.tm_mday, int(m[1]), int(m[2]), int(m[3]), 0, 0, -1)) + int(m[4]) / 1000.0

    hit = next((wall(ln) for ln in lines if "hit health" in ln), None)
    if not (video.exists() and hit):
        print("no film or no hit")
        return
    t_hit = hit - cap_t - 0.3
    frames = out / "frames"
    frames.mkdir(exist_ok=True)
    for old in frames.glob("*.png"):
        old.unlink()
    start = max(0.0, t_hit - 0.4)
    subprocess.run([playfight.find_ffmpeg(), "-v", "error", "-y", "-ss", f"{start:.2f}", "-t", "1.6", "-i", str(video), "-vf", "scale=800:480,fps=15",
                    str(frames / "f%03d.png")], check=True)
    pngs = sorted(frames.glob("f*.png"))
    sheet = Image.new("RGB", (1200, 240 * ((len(pngs[:12]) + 2) // 3)))
    for i, p in enumerate(pngs[:12]):
        sheet.paste(Image.open(p).convert("RGB"), ((i % 3) * 400, (i // 3) * 240))
    sheet.save(out / "sheet.png")
    print(f"the hit is {t_hit - start:.2f} s into the sheet's first frame; {len(pngs)} frames; {out / 'sheet.png'}")


if __name__ == "__main__":
    main()
