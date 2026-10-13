"""BetterVoiceMeeter stem worker.

Searches YouTube, downloads a song, splits it into stems (lead vocal, backing vocals/ad-libs,
drums, bass, instruments) and fetches synced lyrics. Talks to BetterVoiceMeeter through
JSON lines on stdout:

    {"type": "results", "items": [...]}
    {"type": "progress", "stage": "separate", "pct": 0.4, "message": "..."}
    {"type": "done", "dir": "...", "meta": {...}}
    {"type": "error", "message": "..."}

Usage:
    stemworker.py search "<query>" [--limit 8]
    stemworker.py process <youtube id or url> --songs <dir> [--engine local|musicai]
                  [--musicai-key KEY --musicai-workflow SLUG] [--stems 3|5]
    stemworker.py lyrics <song dir>          # re-fetch lyrics only
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
import time
import traceback
import urllib.parse
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
MODELS_DIR = Path(os.environ.get("LOCALAPPDATA", HERE)) / "BetterVoiceMeeter" / "stem-models"

# Local engine models (audio-separator / UVR model zoo). Chosen for quality on an 8 GB GPU.
VOCALS_MODEL = "model_bs_roformer_ep_317_sdr_12.9755.ckpt"                  # vocals vs instrumental
KARAOKE_MODEL = "mel_band_roformer_karaoke_aufr33_viperx_sdr_10.1956.ckpt"  # lead vs backing vocals
DRUMS_BASS_MODEL = "htdemucs.yaml"                                             # drums / bass / other

# Speed vs quality. The RoFormer configs default to overlap 8 (each sample processed ~8x);
# 2 is ~4x faster with a barely audible difference. Demucs: one shift instead of two.
MDXC_PARAMS = {"segment_size": 256, "override_model_segment_size": False, "batch_size": 2, "overlap": 2, "pitch_shift": 0}
DEMUCS_PARAMS = {"segment_size": "Default", "shifts": 1, "overlap": 0.25, "segments_enabled": True}

USER_AGENT = "BetterVoiceMeeter (https://github.com/LuigiHard/BetterVoiceMeeter)"


# --------------------------------------------------------------------------- output
def emit(kind: str, **fields) -> None:
    print(json.dumps({"type": kind, **fields}, ensure_ascii=False), flush=True)


_last_pct = [0.0]


def progress(stage: str, pct: float, message: str) -> None:
    # Never move backwards (some models report several passes).
    _last_pct[0] = max(_last_pct[0], max(0.0, min(1.0, pct)))
    emit("progress", stage=stage, pct=round(_last_pct[0], 3), message=message)


def ffmpeg_on_path() -> str:
    """Make sure an ffmpeg binary is reachable (bundled via the static-ffmpeg package)."""
    if shutil.which("ffmpeg"):
        return shutil.which("ffmpeg")
    import static_ffmpeg  # noqa: WPS433 (optional import)

    static_ffmpeg.add_paths(weak=True)
    path = shutil.which("ffmpeg")
    if not path:
        raise RuntimeError("ffmpeg not found")
    return path


# --------------------------------------------------------------------------- YouTube
def search(query: str, limit: int) -> None:
    import yt_dlp

    opts = {"quiet": True, "skip_download": True, "extract_flat": True, "noplaylist": True}
    with yt_dlp.YoutubeDL(opts) as ydl:
        info = ydl.extract_info(f"ytsearch{limit}:{query}", download=False)
    items = []
    for e in info.get("entries") or []:
        if not e or not e.get("id"):
            continue
        thumbs = e.get("thumbnails") or []
        items.append({
            "id": e["id"],
            "title": e.get("title") or "",
            "channel": e.get("channel") or e.get("uploader") or "",
            "duration": e.get("duration") or 0,
            "thumbnail": thumbs[-1]["url"] if thumbs else "",
            "url": f"https://www.youtube.com/watch?v={e['id']}",
        })
    emit("results", items=items)


def video_id_from(arg: str) -> str:
    m = re.search(r"(?:v=|youtu\.be/|shorts/)([A-Za-z0-9_-]{11})", arg)
    return m.group(1) if m else arg


def download(video_id: str, song_dir: Path) -> dict:
    import yt_dlp

    ffmpeg = ffmpeg_on_path()

    def hook(d):
        if d.get("status") == "downloading":
            total = d.get("total_bytes") or d.get("total_bytes_estimate") or 0
            if total:
                progress("download", 0.02 + 0.13 * d.get("downloaded_bytes", 0) / total, "Downloading audio from YouTube")

    opts = {
        "quiet": True,
        "noprogress": True,
        "noplaylist": True,
        "format": "bestaudio/best",
        "outtmpl": str(song_dir / "download.%(ext)s"),
        "ffmpeg_location": str(Path(ffmpeg).parent),
        "progress_hooks": [hook],
        "postprocessors": [{"key": "FFmpegExtractAudio", "preferredcodec": "wav"}],
    }
    with yt_dlp.YoutubeDL(opts) as ydl:
        info = ydl.extract_info(f"https://www.youtube.com/watch?v={video_id}", download=True)
    # Normalise to 44.1 kHz stereo, the rate the separation models output.
    to_wav(song_dir / "download.wav", song_dir / "source.wav")
    (song_dir / "download.wav").unlink(missing_ok=True)
    return info


def guess_artist_title(info: dict) -> tuple[str, str]:
    """YouTube Music uploads carry artist/track; otherwise parse 'Artist - Title (Official Video)'."""
    artist, track = info.get("artist") or info.get("creator") or "", info.get("track") or ""
    if artist and track:
        return artist.split(",")[0].strip(), track.strip()
    title = info.get("title") or ""
    clean = re.sub(r"[\(\[][^\)\]]*(official|video|audio|lyric|visualizer|hd|4k|remaster|mv|live)[^\)\]]*[\)\]]", "", title, flags=re.I)
    clean = re.sub(r"\s+(ft\.?|feat\.?)\s.*$", "", clean, flags=re.I).strip(" -|")
    for sep in (" - ", " – ", " — ", " | "):
        if sep in clean:
            a, t = clean.split(sep, 1)
            return a.strip(), t.strip()
    channel = re.sub(r"\s*-\s*Topic$|VEVO$", "", info.get("channel") or "", flags=re.I).strip()
    return channel, clean


# --------------------------------------------------------------------------- local separation
def separate_local(song_dir: Path, five_stems: bool) -> list[str]:
    import torch
    from audio_separator.separator import Separator

    ffmpeg_on_path()
    MODELS_DIR.mkdir(parents=True, exist_ok=True)
    work = song_dir / "work"
    work.mkdir(exist_ok=True)
    gpu = torch.cuda.is_available()
    progress("separate", 0.16, f"Separating on {'GPU: ' + torch.cuda.get_device_name(0) if gpu else 'CPU (slow)'}")

    def run(model: str, src: Path, label: str, pct: float, pct_end: float) -> dict[str, Path]:
        progress("separate", pct, label)
        report_tqdm(lambda frac: progress("separate", pct + (pct_end - pct) * frac, label))
        sep = Separator(output_dir=str(work), output_format="WAV", model_file_dir=str(MODELS_DIR), log_level=40,
                        mdxc_params=MDXC_PARAMS, demucs_params=DEMUCS_PARAMS)
        sep.load_model(model_filename=model)
        outs = sep.separate(str(src))
        stems = {}
        for o in outs:
            p = Path(o) if Path(o).is_absolute() else work / o
            # Names accumulate tags across steps, e.g. "source_(Vocals)_m1_(Instrumental)_m2.wav":
            # the last tag is the one this model produced.
            tags = re.findall(r"_\(([^)]+)\)", p.name)
            stems[(tags[-1] if tags else p.stem).lower()] = p
        return stems

    first = run(VOCALS_MODEL, song_dir / "source.wav", "Step 1/3: vocals vs music", 0.18, 0.5 if five_stems else 0.6)
    vocals = next(p for k, p in first.items() if "vocal" in k and "instrument" not in k)
    music = next(p for k, p in first.items() if "instrument" in k)

    second = run(KARAOKE_MODEL, vocals, "Step 2/3: lead vocal vs backing vocals / ad-libs", 0.5 if five_stems else 0.6, 0.78 if five_stems else 0.9)
    # On a vocals-only input, the karaoke model's "instrumental" is everything except the lead: backing + ad-libs.
    backing = next(p for k, p in second.items() if "instrument" in k)
    lead = next(p for k, p in second.items() if "vocal" in k)
    shutil.move(str(lead), song_dir / "lead.wav")
    shutil.move(str(backing), song_dir / "backing.wav")
    stems = ["lead", "backing"]

    if five_stems:
        third = run(DRUMS_BASS_MODEL, music, "Step 3/3: drums, bass and instruments", 0.78, 0.9)
        for name in ("drums", "bass", "other"):
            src = next((p for k, p in third.items() if k == name), None)
            if src:
                shutil.move(str(src), song_dir / f"{name}.wav")
                stems.append(name)
        # Demucs' own "vocals" output here is just separation residue: fold it into "other".
        residue = next((p for k, p in third.items() if k == "vocals"), None)
        if residue and (song_dir / "other.wav").exists():
            mix_into(song_dir / "other.wav", residue)
    else:
        shutil.move(str(music), song_dir / "instrumental.wav")
        stems.append("instrumental")

    shutil.rmtree(work, ignore_errors=True)
    return stems


def report_tqdm(callback) -> None:
    """Forward the separators' internal tqdm progress bars to our JSON progress (throttled)."""
    import tqdm.std

    original = getattr(tqdm.std.tqdm, "_bvm_original_display", tqdm.std.tqdm.display)
    tqdm.std.tqdm._bvm_original_display = original
    last = [0.0]

    def display(self, *args, **kwargs):
        if self.total and time.time() - last[0] > 0.5:
            last[0] = time.time()
            callback(min(1.0, self.n / self.total))
        return original(self, *args, **kwargs)

    tqdm.std.tqdm.display = display


def balance_stems(song_dir: Path, stems: list[str]) -> None:
    """The separators peak-normalise each stage independently, which changes the balance between
    stems. Fit one gain per stem (least squares) so that the stems add back up to the original
    mix, then save them as float WAV (corrected stems may exceed full scale)."""
    import numpy as np
    import soundfile as sf

    src, sr = sf.read(str(song_dir / "source.wav"), dtype="float32", always_2d=True)
    if any(sf.info(str(song_dir / f"{s}.wav")).samplerate != sr for s in stems):
        progress("balance", 0.92, "Skipping rebalance (sample rates differ)")
        return
    parts = [sf.read(str(song_dir / f"{s}.wav"), dtype="float32", always_2d=True)[0] for s in stems]
    n = min(len(src), *(len(p) for p in parts))
    a = np.stack([p[:n:8].ravel() for p in parts], axis=1)
    gains, *_ = np.linalg.lstsq(a, src[:n:8].ravel(), rcond=None)
    gains = np.clip(gains, 0.25, 4.0)
    for s, g, p in zip(stems, gains, parts):
        sf.write(str(song_dir / f"{s}.wav"), p * np.float32(g), sr, subtype="FLOAT")
    residual = src[:n] - sum(np.float32(g) * p[:n] for g, p in zip(gains, parts))
    ratio_db = 10 * np.log10((np.mean(residual ** 2) + 1e-12) / (np.mean(src[:n] ** 2) + 1e-12))
    progress("balance", 0.92, f"Stems rebalanced (rebuild error {ratio_db:.0f} dB)")


def mix_into(target: Path, extra: Path) -> None:
    import soundfile as sf

    a, sr = sf.read(str(target), dtype="float32", always_2d=True)
    b, _ = sf.read(str(extra), dtype="float32", always_2d=True)
    n = min(len(a), len(b))
    a[:n] += b[:n]
    sf.write(str(target), a, sr, subtype="FLOAT")


# --------------------------------------------------------------------------- Music.AI separation
def musicai_request(method: str, url: str, key: str, body: dict | None = None) -> dict:
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers={"Authorization": key, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        raw = r.read()
    return json.loads(raw) if raw else {}


def separate_musicai(song_dir: Path, key: str, workflow: str) -> tuple[list[str], object]:
    """Runs a Music.AI workflow you created in their dashboard. Outputs are matched by name:
    lead / back(ing) / drums / bass / other|instrument|accompaniment, plus any lyrics output."""
    api = "https://api.music.ai/v1"
    progress("upload", 0.16, "Uploading to Music.AI")
    up = musicai_request("GET", f"{api}/upload", key)
    with open(song_dir / "source.wav", "rb") as f:
        req = urllib.request.Request(up["uploadUrl"], data=f.read(), method="PUT", headers={"Content-Type": "audio/wav"})
        urllib.request.urlopen(req, timeout=600).read()

    job = musicai_request("POST", f"{api}/job", key, {
        "name": f"BetterVoiceMeeter {song_dir.name}", "workflow": workflow, "params": {"inputUrl": up["downloadUrl"]}})
    started = time.time()
    while True:
        state = musicai_request("GET", f"{api}/job/{job['id']}", key)
        status = state.get("status")
        if status == "SUCCEEDED":
            break
        if status in ("FAILED", "CANCELED", "CANCELLED"):
            raise RuntimeError(f"Music.AI job {status.lower()}: {state.get('error') or ''}")
        progress("separate", min(0.85, 0.2 + (time.time() - started) / 240), f"Music.AI is processing ({status})")
        time.sleep(5)

    stems, lyrics = [], None
    names = {"lead": ("lead",), "backing": ("back",), "drums": ("drum",), "bass": ("bass",),
             "instrumental": ("instrument", "accompan", "other", "no_vocal")}
    for out_name, value in (state.get("result") or {}).items():
        low = out_name.lower()
        if "lyric" in low or "transcri" in low or "align" in low:
            lyrics = value if not (isinstance(value, str) and value.startswith("http")) else json.loads(fetch(value))
            continue
        if not (isinstance(value, str) and value.startswith("http")):
            continue
        stem = next((s for s, keys in names.items() if any(k in low for k in keys)), None)
        if stem is None and "vocal" in low and "lead" not in stems:
            stem = "lead"  # workflow without a lead/backing split: treat all vocals as lead
        if stem is None or stem in stems:
            continue
        progress("download", 0.88, f"Downloading {stem} stem")
        tmp = song_dir / f"{stem}.download"
        tmp.write_bytes(fetch(value))
        to_wav(tmp, song_dir / f"{stem}.wav")
        tmp.unlink(missing_ok=True)
        stems.append(stem)
    try:
        musicai_request("DELETE", f"{api}/job/{job['id']}", key)
    except Exception:  # noqa: BLE001 - cleanup is best effort
        pass
    if not stems:
        raise RuntimeError("The Music.AI workflow returned no audio stems")
    return stems, lyrics


def fetch(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=300) as r:
        return r.read()


def to_wav(src: Path, dst: Path) -> None:
    import subprocess

    subprocess.run([ffmpeg_on_path(), "-y", "-loglevel", "error", "-i", str(src), "-ar", "44100", "-ac", "2", str(dst)], check=True)


# --------------------------------------------------------------------------- lyrics
def parse_lrc(text: str) -> list[dict]:
    lines = []
    for raw in text.splitlines():
        stamps = re.findall(r"\[(\d+):(\d+(?:\.\d+)?)\]", raw)
        words = re.sub(r"\[[^\]]*\]", "", raw).strip()
        for mm, ss in stamps:
            lines.append({"t": int(mm) * 60 + float(ss), "text": words})
    lines.sort(key=lambda l: l["t"])
    for i, line in enumerate(lines):
        line["end"] = lines[i + 1]["t"] if i + 1 < len(lines) else line["t"] + 5.0
    return [l for l in lines if l["text"]]


def lrclib(artist: str, title: str, duration: float) -> tuple[list[dict], str] | None:
    def get(path: str, params: dict):
        url = f"https://lrclib.net/api/{path}?{urllib.parse.urlencode(params)}"
        try:
            return json.loads(fetch(url))
        except Exception:  # noqa: BLE001 - not found / network: try the next strategy
            return None

    exact = get("get", {"artist_name": artist, "track_name": title, "duration": int(round(duration))}) if artist else None
    if exact and exact.get("syncedLyrics"):
        return parse_lrc(exact["syncedLyrics"]), "LRCLIB"
    found = get("search", {"q": f"{artist} {title}".strip()}) or []
    synced = [r for r in found if r.get("syncedLyrics")]
    if not synced:
        return None
    # Prefer the version whose length matches this recording (YouTube intros/outros shift timing).
    best = min(synced, key=lambda r: abs((r.get("duration") or 0) - duration))
    if duration and abs((best.get("duration") or 0) - duration) > 15:
        return None
    return parse_lrc(best["syncedLyrics"]), "LRCLIB"


def musicai_lyrics(value) -> list[dict]:
    """Best-effort conversion of a Music.AI lyrics output into our line format."""
    items = value.get("lines") or value.get("segments") or value.get("words") or [] if isinstance(value, dict) else value or []
    lines = []
    for it in items:
        if not isinstance(it, dict):
            continue
        start = it.get("start") or it.get("startTime") or it.get("t") or 0
        end = it.get("end") or it.get("endTime") or start + 2
        text = it.get("text") or it.get("line") or it.get("word") or ""
        words = [{"t": w.get("start", start), "end": w.get("end", end), "w": w.get("text") or w.get("word") or ""}
                 for w in (it.get("words") or []) if isinstance(w, dict)]
        lines.append({"t": float(start), "end": float(end), "text": text.strip(), **({"words": words} if words else {})})
    return [l for l in lines if l["text"]]


def find_lyrics(song_dir: Path, meta: dict, provided=None) -> None:
    progress("lyrics", 0.93, "Finding synced lyrics")
    lines, source = [], ""
    if provided:
        lines, source = musicai_lyrics(provided), "Music.AI"
    if not lines:
        got = lrclib(meta.get("artist", ""), meta.get("track", ""), meta.get("duration", 0))
        if got:
            lines, source = got
    (song_dir / "lyrics.json").write_text(json.dumps({"source": source, "offset": 0.0, "lines": lines}, ensure_ascii=False, indent=1), encoding="utf-8")
    meta["lyrics"] = source or "none"


# --------------------------------------------------------------------------- main commands
def process(args) -> None:
    video_id = video_id_from(args.video)
    song_dir = Path(args.songs) / video_id
    song_dir.mkdir(parents=True, exist_ok=True)

    progress("download", 0.01, "Fetching from YouTube")
    if not (song_dir / "source.wav").exists():
        info = download(video_id, song_dir)
        (song_dir / "youtube.json").write_text(json.dumps({k: info.get(k) for k in (
            "id", "title", "channel", "duration", "artist", "track", "creator", "thumbnail")}, ensure_ascii=False), encoding="utf-8")
    info = json.loads((song_dir / "youtube.json").read_text(encoding="utf-8"))
    artist, track = guess_artist_title(info)
    meta = {"id": video_id, "title": info.get("title"), "artist": artist, "track": track,
            "duration": info.get("duration") or 0, "thumbnail": info.get("thumbnail") or "", "engine": args.engine}

    for old in song_dir.glob("*.wav"):
        if old.name != "source.wav":
            old.unlink()

    provided_lyrics = None
    if args.engine == "musicai":
        if not (args.musicai_key and args.musicai_workflow):
            raise RuntimeError("Music.AI needs an API key and a workflow ID (Sing Studio > Settings)")
        meta["stems"], provided_lyrics = separate_musicai(song_dir, args.musicai_key, args.musicai_workflow)
    else:
        meta["stems"] = separate_local(song_dir, five_stems=args.stems == 5)
    balance_stems(song_dir, meta["stems"])

    find_lyrics(song_dir, meta, provided_lyrics)
    (song_dir / "meta.json").write_text(json.dumps(meta, ensure_ascii=False, indent=1), encoding="utf-8")
    progress("done", 1.0, "Ready")
    emit("done", dir=str(song_dir), meta=meta)


def relyrics(args) -> None:
    song_dir = Path(args.dir)
    meta = json.loads((song_dir / "meta.json").read_text(encoding="utf-8"))
    find_lyrics(song_dir, meta)
    (song_dir / "meta.json").write_text(json.dumps(meta, ensure_ascii=False, indent=1), encoding="utf-8")
    emit("done", dir=str(song_dir), meta=meta)


def main() -> int:
    # Keep stdout clean for the JSON protocol; library chatter goes to stderr.
    sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("search"); s.add_argument("query"); s.add_argument("--limit", type=int, default=8)
    pr = sub.add_parser("process"); pr.add_argument("video"); pr.add_argument("--songs", required=True)
    pr.add_argument("--engine", choices=("local", "musicai"), default="local")
    pr.add_argument("--musicai-key", default=os.environ.get("MUSICAI_API_KEY", ""))
    pr.add_argument("--musicai-workflow", default="")
    pr.add_argument("--stems", type=int, choices=(3, 5), default=5)
    ly = sub.add_parser("lyrics"); ly.add_argument("dir")
    args = p.parse_args()
    try:
        {"search": search, "process": process, "lyrics": relyrics}[args.cmd](*(
            (args.query, args.limit) if args.cmd == "search" else (args,)))
        return 0
    except Exception as e:  # noqa: BLE001 - report every failure to the app
        traceback.print_exc(file=sys.stderr)
        emit("error", message=str(e) or type(e).__name__)
        return 1


if __name__ == "__main__":
    sys.exit(main())
