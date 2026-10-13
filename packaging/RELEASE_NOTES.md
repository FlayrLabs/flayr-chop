Flayr Chop 1.1.0

- More accurate key detection: relative major/minor mix-ups cut roughly in half, and in half again when stems are separated (tempo and key are re-detected from the drums and bass stems).
- Tempo detection fixed for 48 and 96 kHz files, with better handling of triplet-feel loops.
- Click the BPM or key to see ranked alternatives; new TAP button; uncertain detections are marked with "?".
- Stem separation about 1.7x faster on Apple silicon; Windows and Linux use AVX2 automatically on CPUs that have it; memory use is capped by available RAM on every platform; Windows user folders with non-English names work.

The stem model (84 MB) downloads on first use. See the README for install steps.
