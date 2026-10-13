#!/usr/bin/env python3
"""Build tests/eval/labels.tsv for the Flayr Chop tempo/key evaluation.

Truth comes from three kinds of source, never from listening or from another detector:
  filename   the file name states the tempo and/or key (sample packs, bounces)
  loopmeta   Apple Loops shipped with macOS: tempo = beat count / length and key + mode from the
             loop's own metadata chunk (written by the loop author). Read from the CAF file.
  user       stated by the project owner for files in their private list
  song       well-known recordings with a commonly agreed tempo and key (listed below with the reason)
  mix        sums of Apple Loops from the same pack with the same tempo and the same key and mode
             (drums + bass/keys/guitar), looped to a common length by the eval runner

The dev/held split is a hash of a group key, so near-duplicates (the same loop series, the same shaker
pattern at another tempo, the same song twice) always land on the same side. Run once; the split is fixed
by the hash, not by a random seed.

Usage: python3 -I tests/eval/make_labels.py > tests/eval/labels.tsv          (public: Apple Loops only)
       with tests/eval/music_local.py present, write to labels.local.tsv instead (git-ignored)
"""
import hashlib
import os
import re
import struct
import sys

APPLE_ROOT = "/Library/Audio/Apple Loops"
MUSIC_ROOT = os.path.expanduser("~/Music")

# Optional private list of files under ~/Music (tests/eval/music_local.py, git-ignored):
# MUSIC = [(path under ~/Music, bpm or None, key or None, kind, source, group), ...]
# It may also set SHAKE, a folder of loops named "<name> <N>bpm.wav".
try:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from music_local import MUSIC
except ImportError:
    MUSIC = []
try:
    from music_local import SHAKE
except ImportError:
    SHAKE = None


def caf_meta(path):
    out = {}
    with open(path, "rb") as f:
        d = f.read()
    p = 8
    while p + 12 <= len(d):
        typ = d[p:p + 4]
        size = struct.unpack(">q", d[p + 4:p + 12])[0]
        body = d[p + 12:p + 12 + size] if size >= 0 else d[p + 12:]
        if typ in (b"info", b"uuid"):
            b = body if typ == b"info" else body[16:]
            if len(b) >= 4:
                n = struct.unpack(">I", b[:4])[0]
                parts = b[4:].split(b"\0")
                if 0 < n < 64:
                    for i in range(0, 2 * n, 2):
                        if i + 1 < len(parts):
                            out[parts[i].decode("utf8", "replace")] = parts[i + 1].decode("utf8", "replace")
        elif typ == b"desc":
            out["_sr"] = struct.unpack(">d", body[:8])[0]
        elif typ == b"pakt":
            out["_frames"] = struct.unpack(">q", body[8:16])[0]
        if size < 0:
            break
        p += 12 + size
    return out


PCS = {"C": 0, "C#": 1, "Db": 1, "D": 2, "D#": 3, "Eb": 3, "E": 4, "F": 5, "F#": 6, "Gb": 6, "G": 7, "G#": 8, "Ab": 8,
       "A": 9, "A#": 10, "Bb": 10, "B": 11}
NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def key_label(sig, kt):
    if sig not in PCS or kt not in ("major", "minor"):
        return None
    return NAMES[PCS[sig]] + ("m" if kt == "minor" else "")


def split_of(group):
    return "dev" if hashlib.md5(group.encode()).digest()[0] % 2 == 0 else "held"


def series(name):
    return re.sub(r"[\s_]*\d+$", "", os.path.splitext(name)[0]).strip()


def fmt_bpm(b):
    return "-" if b is None else ("%.3f" % b).rstrip("0").rstrip(".")


def main():
    rows = []
    for path, bpm, key, kind, src, group in MUSIC:
        rows.append((split_of("music:" + (group or path)), "music", path, fmt_bpm(bpm), key or "-", kind, src))
    shake_dir = os.path.join(MUSIC_ROOT, SHAKE) if SHAKE else None
    for f in sorted(os.listdir(shake_dir)) if shake_dir and os.path.isdir(shake_dir) else []:
        m = re.search(r"(\d+)bpm", f)
        if not m:
            continue
        group = re.sub(r"\s*\d+bpm", "", f)
        rows.append((split_of("shake:" + group), "music", SHAKE + "/" + f, m.group(1), "-", "perc", "filename"))

    apple = []
    for dp, _dn, fn in os.walk(APPLE_ROOT):
        for f in sorted(fn):
            if f.endswith(".caf"):
                p = os.path.join(dp, f)
                apple.append((os.path.relpath(p, APPLE_ROOT), caf_meta(p)))
    apple.sort()
    groups = {}
    drum_i = 0
    for rel, m in apple:
        if "_frames" not in m or "beat count" not in m or m.get("time signature") != "4/4":
            continue
        dur = m["_frames"] / m["_sr"]
        bpm = round(float(m["beat count"]) * 60.0 / dur, 2)
        folder = rel.split("/")[1]
        desc = m.get("descriptors", "")
        key = key_label(m.get("key signature"), m.get("key type"))
        cat = m.get("category", "")
        is_drum = cat in ("Drums", "Percussion") and not m.get("key signature")
        grp = "apple:" + folder + "/" + series(os.path.basename(rel))
        if "Arrhythmic" in desc:
            continue
        if key:
            rows.append((split_of(grp), "apple", rel, fmt_bpm(bpm), key, "loop-" + cat.lower().replace("/", "-"), "loopmeta"))
        elif is_drum:
            drum_i += 1
            if drum_i % 2 == 0:  # half the drum loops is plenty for tempo
                rows.append((split_of(grp), "apple", rel, fmt_bpm(bpm), "-", "loop-drums", "loopmeta"))
        # Mix material: drums, or tonal loops whose root is known (mode may be "both" for bass lines).
        if is_drum:
            groups.setdefault((folder, bpm), {}).setdefault("drums", []).append(rel)
        elif m.get("key signature") in PCS and m.get("key type") in ("major", "minor", "both"):
            groups.setdefault((folder, bpm), {}).setdefault((PCS[m["key signature"]], m["key type"]), []).append((rel, cat))

    # Mixes: drums + 2..3 tonal loops sharing pack, tempo and key, at least one with a stated mode.
    nmix = 0
    for (folder, bpm), g in sorted(groups.items()):
        drums = g.get("drums", [])
        if not drums:
            continue
        for pc in range(12):
            for mode in ("major", "minor"):
                tonal = g.get((pc, mode), [])
                if not tonal:
                    continue
                both = g.get((pc, "both"), [])
                bass = [r for r, c in both + tonal if c == "Bass"]
                other = [r for r, c in tonal if c != "Bass"]
                if not other:
                    continue
                parts = [drums[(pc + nmix) % len(drums)]]
                if bass:
                    parts.append(bass[0])
                parts += [r for r in other if r not in parts][: 1 if bass else 2]
                if len(parts) < 3:
                    continue
                key = NAMES[pc] + ("m" if mode == "minor" else "")
                grp = "mix:" + folder + "/" + fmt_bpm(bpm) + "/" + key
                rows.append((split_of(grp), "mix", " + ".join(parts), fmt_bpm(bpm), key, "mix", "mix"))
                nmix += 1

    print("# split\troot\tpath\tbpm\tkey\tkind\tsource")
    print("# roots: music = ~/Music, apple = /Library/Audio/Apple Loops, mix = ' + '-joined Apple Loops paths")
    for r in rows:
        print("\t".join(r))
    sys.stderr.write("%d rows, %d mixes\n" % (len(rows), nmix))


if __name__ == "__main__":
    main()
