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


def lrclib_candidates(artist: str, title: str, duration: float) -> list[dict]:
    """All LRCLIB entries that might be this song (exact match first, then searches)."""
    def get(path: str, params: dict):
        url = f"https://lrclib.net/api/{path}?{urllib.parse.urlencode(params)}"
        try:
            return json.loads(fetch(url))
        except Exception:  # noqa: BLE001 - not found / network: try the next strategy
            return None

    found, seen = [], set()
    def add(r):
        if isinstance(r, dict) and r.get("id") not in seen and (r.get("syncedLyrics") or r.get("plainLyrics")):
            seen.add(r.get("id"))
            found.append(r)

    if artist and title:
        add(get("get", {"artist_name": artist, "track_name": title, "duration": int(round(duration))}))
        for r in get("search", {"artist_name": artist, "track_name": title}) or []:
            add(r)
    for r in get("search", {"q": f"{artist} {title}".strip()}) or []:
        add(r)
    if title:
        for r in get("search", {"track_name": title}) or []:
            add(r)
    return found


def plain_lines(r: dict) -> list[str]:
    text = r.get("plainLyrics") or re.sub(r"\[[^\]]*\]", "", r.get("syncedLyrics") or "")
    return [l.strip() for l in text.splitlines() if l.strip()]


# ---------------------------------------------------------------- AI sync (Whisper on the lead vocal)
WHISPER_ALIGN_MODEL = "small"   # enough to time known lyrics
WHISPER_TRANSCRIBE_MODEL = "medium"  # used when there are no lyrics at all


def whisper_words(audio: Path, model_name: str, prompt: str = "", language: str | None = None) -> tuple[list[dict], str]:
    import torch
    import whisper

    ffmpeg_on_path()
    device = "cuda" if torch.cuda.is_available() else "cpu"
    model = whisper.load_model(model_name, device=device, download_root=str(MODELS_DIR / "whisper"))
    result = model.transcribe(
        str(audio), word_timestamps=True, verbose=False, language=language,
        fp16=False,  # GTX 10-series (Pascal) is slow at fp16
        initial_prompt=prompt[:600] or None, condition_on_previous_text=False)
    words = [{"w": w["word"].strip(), "t": float(w["start"]), "end": float(w["end"])}
             for seg in result.get("segments", []) for w in seg.get("words", []) if w["word"].strip()]
    return words, result.get("language", "")


def norm_token(w: str) -> str:
    import unicodedata

    w = unicodedata.normalize("NFKD", w.lower())
    return "".join(c for c in w if c.isalnum())


def align_lines(lines: list[str], heard: list[dict]) -> tuple[list[dict], float]:
    """Give each known lyric word the time Whisper heard it; interpolate the words it missed."""
    import difflib

    tokens = []  # (line index, display word, normalised)
    for li, line in enumerate(lines):
        for w in line.split():
            n = norm_token(w)
            if n:
                tokens.append([li, w, n, None, None])
    heard_norm = [norm_token(h["w"]) for h in heard]
    sm = difflib.SequenceMatcher(None, [t[2] for t in tokens], heard_norm, autojunk=False)
    matched = 0
    for block in sm.get_matching_blocks():
        for k in range(block.size):
            t, h = tokens[block.a + k], heard[block.b + k]
            t[3], t[4] = h["t"], h["end"]
            matched += 1
    # Fuzzy pass: pair up remaining words inside unmatched gaps when they sound alike.
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "replace":
            for k in range(min(i2 - i1, j2 - j1)):
                if difflib.SequenceMatcher(None, tokens[i1 + k][2], heard_norm[j1 + k]).ratio() >= 0.6:
                    tokens[i1 + k][3], tokens[i1 + k][4] = heard[j1 + k]["t"], heard[j1 + k]["end"]
                    matched += 1

    # Interpolate missing times between known neighbours.
    known = [i for i, t in enumerate(tokens) if t[3] is not None]
    if not known:
        return [], 0.0
    for i, t in enumerate(tokens):
        if t[3] is not None:
            continue
        prev = max((k for k in known if k < i), default=None)
        nxt = min((k for k in known if k > i), default=None)
        if prev is None:
            t[3] = max(0.0, tokens[nxt][3] - 0.4 * (nxt - i)); t[4] = t[3] + 0.3
        elif nxt is None:
            t[3] = tokens[prev][4] + 0.35 * (i - prev - 1); t[4] = t[3] + 0.3
        else:
            span = (tokens[nxt][3] - tokens[prev][4]) / (nxt - prev)
            t[3] = tokens[prev][4] + span * (i - prev - 1) + 0.01
            t[4] = t[3] + max(0.1, span * 0.8)

    out = []
    for li, line in enumerate(lines):
        ws = [t for t in tokens if t[0] == li]
        if not ws:
            continue
        words = [{"t": round(t[3], 3), "end": round(t[4], 3), "w": t[1]} for t in ws]
        out.append({"t": words[0]["t"], "end": words[-1]["end"], "text": line, "words": words})
    out.sort(key=lambda l: l["t"])
    return out, matched / max(1, len(tokens))


def vocal_track(song_dir: Path) -> Path:
    """Lead + backing vocals combined: ad-libs in the lyric text then have a voice to align to."""
    import numpy as np
    import soundfile as sf

    parts = [song_dir / f"{s}.wav" for s in ("lead", "backing", "vocals") if (song_dir / f"{s}.wav").exists()]
    if not parts:
        return song_dir / "source.wav"
    mix, sr = sf.read(str(parts[0]), dtype="float32", always_2d=True)
    for extra in parts[1:]:
        b, _ = sf.read(str(extra), dtype="float32", always_2d=True)
        n = min(len(mix), len(b))
        mix[:n] += b[:n]
    out = song_dir / "align_vocals.wav"
    sf.write(str(out), mix, sr, subtype="FLOAT")
    return out


def forced_align(audio: Path, lines: list[str]) -> tuple[list[dict], float]:
    """CTC forced alignment (torchaudio MMS_FA, multilingual): walks the known lyrics through the
    audio in order, so every word gets a time and repeated choruses can't be mixed up."""
    import numpy as np
    import soundfile as sf
    import torch
    import torchaudio
    import torchaudio.functional as F

    os.environ.setdefault("TORCH_HOME", str(MODELS_DIR / "torch"))
    bundle = torchaudio.pipelines.MMS_FA
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    # The "star" wildcard token soaks up sung sounds that aren't in the lyric text
    # (oohs, breaths, ad-libs), so they don't drag the real words out of place.
    model = bundle.get_model(with_star=True).to(device).eval()
    vocab = bundle.get_dict(star="*")

    wav, sr = sf.read(str(audio), dtype="float32", always_2d=True)
    wave = torch.from_numpy(wav.mean(axis=1)).unsqueeze(0)
    wave = F.resample(wave, sr, bundle.sample_rate)

    # Emissions in 30 s chunks (whole-song attention would not fit in 8 GB).
    chunk = bundle.sample_rate * 30
    emissions = []
    with torch.inference_mode():
        for i in range(0, wave.shape[1], chunk):
            part = wave[:, i:i + chunk]
            if part.shape[1] < 400:
                continue
            em, _ = model(part.to(device))
            emissions.append(torch.log_softmax(em, dim=-1).cpu())
            progress("lyrics", 0.94 + 0.04 * min(1.0, (i + chunk) / wave.shape[1]), "Syncing lyrics to the vocal (forced alignment)")
    emission = torch.cat(emissions, dim=1)
    sec_per_frame = wave.shape[1] / bundle.sample_rate / emission.shape[1]

    # Words -> romanised letters the aligner knows (accents stripped, e.g. "coração" -> "coracao").
    words = []  # (line index, display word, tokens)
    for li, line in enumerate(lines):
        for w in line.split():
            letters = "".join(c for c in norm_token(w).replace("0", "o") if c in vocab)
            if letters:
                words.append((li, w, [vocab[c] for c in letters]))
    if not words:
        return [], 0.0
    star = vocab["*"]
    flat, word_pos, prev_line = [star], [], words[0][0]
    for li, w, toks in words:
        if li != prev_line:
            flat.append(star)  # wildcard between lyric lines
            prev_line = li
        word_pos.append(len(flat))
        flat.extend(toks)
    flat.append(star)
    targets = torch.tensor([flat], dtype=torch.int32)
    if targets.shape[1] >= emission.shape[1]:
        return [], 0.0
    ali, scores = F.forced_align(emission, targets, blank=0)
    spans = F.merge_tokens(ali[0], scores[0].exp())

    out, conf = {}, []
    for (li, w, toks), k in zip(words, word_pos):
        ws = spans[k:k + len(toks)]
        t0, t1 = ws[0].start * sec_per_frame, ws[-1].end * sec_per_frame
        conf.append(float(np.mean([s.score for s in ws])))
        out.setdefault(li, []).append({"t": round(t0, 3), "end": round(t1, 3), "w": w})
    result = [{"t": ws[0]["t"], "end": ws[-1]["end"], "text": lines[li], "words": ws} for li, ws in sorted(out.items())]
    return result, float(np.mean(conf))


def add_word_timing(lrc: list[dict], aligned: list[dict]) -> list[dict]:
    """Merge AI word times into LRCLIB lines: LRCLIB's line start is trusted, the AI supplies the
    rhythm of the words inside the line (anchored to that start, compressed if it would overrun)."""
    out = []
    for i, (ref, ai) in enumerate(zip(lrc, aligned)):
        words = ai["words"]
        next_t = lrc[i + 1]["t"] if i + 1 < len(lrc) else ref["t"] + 8.0
        span_ai = max(0.3, words[-1]["end"] - words[0]["t"])
        if abs(ai["t"] - ref["t"]) > 1.0:
            span_ai = min(span_ai, 0.35 * len(words) + 0.5)  # AI misplaced this line: don't trust its length
        span = min(span_ai, max(0.5, next_t - ref["t"] - 0.1))
        scale = span / max(0.3, words[-1]["end"] - words[0]["t"])
        words = [{"t": round(ref["t"] + (w["t"] - words[0]["t"]) * scale, 3),
                  "end": round(ref["t"] + (w["end"] - words[0]["t"]) * scale, 3), "w": w["w"]} for w in words]
        out.append({"t": ref["t"], "end": ref["end"], "text": ref["text"], "words": words})
    return out


def lines_from_words(words: list[dict], max_words: int = 10, gap: float = 0.6) -> list[dict]:
    """Group transcribed words into karaoke lines: break at pauses, after sentence punctuation,
    or before a capitalised word once the line is long enough; never exceed max_words."""
    lines, cur = [], []
    for w in words:
        if cur:
            pause = w["t"] - cur[-1]["end"] > gap
            punct = cur[-1]["w"][-1:] in ".?!" or (cur[-1]["w"][-1:] == "," and len(cur) >= 5)
            new_phrase = w["w"][:1].isupper() and w["w"] not in ("I", "I'm", "I've", "I'll", "I'd") and len(cur) >= 4
            if pause or punct or new_phrase or len(cur) >= max_words:
                lines.append(cur); cur = []
        cur.append(w)
    if cur:
        lines.append(cur)
    return [{"t": l[0]["t"], "end": l[-1]["end"], "text": " ".join(x["w"] for x in l), "words": l} for l in lines]


def matches_recording(r: dict, duration: float) -> bool:
    return bool(r.get("syncedLyrics")) and (not duration or abs((r.get("duration") or 0) - duration) <= 3)


def find_lyrics(song_dir: Path, meta: dict, provided=None, resync: bool = False) -> None:
    """Synced lyrics, best first:
       1. Music.AI's own transcription (cloud engine)
       2. LRCLIB synced lyrics whose length matches this recording
       3. LRCLIB lyrics text (synced for another version, or plain) re-timed with Whisper on the lead vocal
       4. Whisper transcription of the lead vocal"""
    lines, source = [], ""
    if provided:
        lines, source = musicai_lyrics(provided), "Music.AI"

    if not lines:
        progress("lyrics", 0.93, "Searching LRCLIB for lyrics")
        cands = lrclib_candidates(meta.get("artist", ""), meta.get("track", ""), meta.get("duration", 0))
        exact = [r for r in cands if matches_recording(r, meta.get("duration", 0))]
        if exact and not resync:
            lines, source = parse_lrc(exact[0]["syncedLyrics"]), "LRCLIB (synced)"
            try:  # add word-by-word timing, keeping LRCLIB's trusted line starts
                progress("lyrics", 0.94, "Adding word-by-word timing")
                vocals = vocal_track(song_dir)
                aligned, _ = forced_align(vocals, [l["text"] for l in lines])
                if len(aligned) == len(lines):
                    lines, source = add_word_timing(lines, aligned), "LRCLIB (synced) + AI word timing"
                if vocals.name == "align_vocals.wav":
                    vocals.unlink(missing_ok=True)
            except Exception:  # noqa: BLE001 - word timing is a bonus; keep the line timing
                traceback.print_exc(file=sys.stderr)
        else:
            vocals = vocal_track(song_dir)
            text_cands = exact + [r for r in cands if r not in exact]
            text = plain_lines(text_cands[0]) if text_cands else []
            if text:
                progress("lyrics", 0.94, "Syncing lyrics to the vocal (forced alignment)")
                lines, conf = forced_align(vocals, text)
                source = f"LRCLIB + AI sync (confidence {int(conf * 100)}%)"
            if not lines:
                progress("lyrics", 0.94, "No lyrics found online: transcribing the vocal with AI (Whisper)")
                heard, lang = whisper_words(vocals, WHISPER_TRANSCRIBE_MODEL)
                lines = lines_from_words(heard)
                if lines:  # tighten Whisper's word times with forced alignment of its own transcript
                    aligned, _ = forced_align(vocals, [l["text"] for l in lines])
                    lines = aligned or lines
                source = f"AI transcription ({lang})" if lines else ""
            vocals.unlink(missing_ok=True) if vocals.name == "align_vocals.wav" else None

    old_offset = 0.0
    if (song_dir / "lyrics.json").exists():
        try:
            old_offset = json.loads((song_dir / "lyrics.json").read_text(encoding="utf-8")).get("offset", 0.0) if not lines else 0.0
        except Exception:  # noqa: BLE001
            pass
    (song_dir / "lyrics.json").write_text(json.dumps({"source": source, "offset": old_offset, "lines": lines},
                                                     ensure_ascii=False, indent=1), encoding="utf-8")
    meta["lyrics"] = source or "none"


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
    if args.artist is not None:
        meta["artist"] = args.artist.strip()
    if args.title is not None:
        meta["track"] = args.title.strip()
    find_lyrics(song_dir, meta, resync=args.resync)
    (song_dir / "meta.json").write_text(json.dumps(meta, ensure_ascii=False, indent=1), encoding="utf-8")
    progress("done", 1.0, f"Lyrics: {meta['lyrics']}")
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
    ly.add_argument("--artist"); ly.add_argument("--title")
    ly.add_argument("--resync", action="store_true", help="re-time lyrics to this recording even if LRCLIB has synced ones")
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
