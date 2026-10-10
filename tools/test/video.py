r"""Record a native test case as an MP4 (video and sound).

Usage:
  python tools\test\video.py <case> [--every N] [--fps F] [--frames A-B] [--out file.mp4] [--wait SECS] [--exe E] [--sd D]
                              [--data out\world] [--start 'Seyda Neen'] [--no-audio]

Runs tools\test\run-test.ps1 -Native with NATIVE_VIDEO / NATIVE_AUDIO set (tools/test/native/native_gpu.cpp and
native_stubs.cpp). The game draws every Nth frame (30 / N frames a second in the video) and pipes it to ffmpeg;
the DSP stand-in mixes sound 1/30 s a frame into a WAV; this script joins the two. Frames are a fixed 1/30 s, so
sound and picture stay in step whatever the PC's speed. ffmpeg: WinGet Gyan.FFmpeg (or NATIVE_FFMPEG).
"""
import argparse, os, shutil, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def findFfmpeg():
    exe = os.environ.get("NATIVE_FFMPEG") or shutil.which("ffmpeg")
    if exe:
        return exe
    for p in (Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft/WinGet/Packages").glob("Gyan.FFmpeg*/*/bin/ffmpeg.exe"):
        return str(p)
    sys.exit("ffmpeg not found (winget install Gyan.FFmpeg, or set NATIVE_FFMPEG)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("case")
    ap.add_argument("--every", type=int, default=2, help="draw every Nth frame (default 2: 15 fps)")
    ap.add_argument("--frames", default="", help="record only frames A-B")
    ap.add_argument("--out", default="")
    ap.add_argument("--wait", type=int, default=3600)
    ap.add_argument("--data", default="out/world")
    ap.add_argument("--start", default="Seyda Neen")
    ap.add_argument("--no-audio", action="store_true")
    ap.add_argument("--fps", default="", help="the video's frame rate (e.g. 24; default 30 / every)")
    ap.add_argument("--exe", default="", help="another native build (run-test.ps1 -Exe)")
    ap.add_argument("--sd", default="", help="its own SD folder (run-test.ps1 -Sd), so it can run beside other tests")
    a = ap.parse_args()
    ffmpeg = findFfmpeg()
    outDir = ROOT / "build" / "video"
    outDir.mkdir(parents=True, exist_ok=True)
    mp4 = Path(a.out) if a.out else outDir / (a.case + ".mp4")
    silent = outDir / (a.case + "-silent.mp4")
    wav = outDir / (a.case + ".wav")
    env = dict(os.environ, NATIVE_VIDEO="%s:%d" % (silent, a.every), NATIVE_FFMPEG=ffmpeg)
    if a.frames:
        env["NATIVE_VIDEO_FRAMES"] = a.frames
    if a.fps:
        env["NATIVE_VIDEO_FPS"] = a.fps
    if not a.no_audio:
        env["NATIVE_AUDIO"] = str(wav)
    t0 = time.time()
    cmd = ["powershell", "-NoProfile", "-File", str(ROOT / "tools/test/run-test.ps1"), a.case, "-Native",
           "-Data", a.data, "-Start", a.start, "-Wait", str(a.wait)]
    if a.exe:
        cmd += ["-Exe", a.exe]
    if a.sd:
        cmd += ["-Sd", a.sd]
    r = subprocess.run(cmd, env=env, cwd=ROOT, capture_output=True, text=True)
    t1 = time.time()
    verdict = [l for l in r.stdout.splitlines() if l.startswith("RESULT") or "did not reach" in l]
    print("\n".join(verdict))
    print("run: %.0f s" % (t1 - t0))
    if not silent.exists():
        sys.exit("no video written")
    if a.no_audio or not wav.exists():
        shutil.move(silent, mp4)
    else:
        # Audio covers every frame; the video only the recorded range, so trim the sound to match
        args = [ffmpeg, "-y", "-loglevel", "error", "-i", str(silent)]
        if a.frames:
            first = int(a.frames.split("-")[0] or 0)
            args += ["-ss", "%.3f" % (first / 30.0)]
        args += ["-i", str(wav), "-c:v", "copy", "-c:a", "aac", "-b:a", "96k", "-shortest", str(mp4)]
        subprocess.run(args, check=True)
        silent.unlink()
    if wav.exists():
        print("wav: %.0f MB (deleted)" % (wav.stat().st_size / 1e6))
        wav.unlink()
    print("mp4: %s  %.1f MB  (mux %.0f s)" % (mp4, mp4.stat().st_size / 1e6, time.time() - t1))


main()
