Flayr Chop 1.2.1

Security update. Please update: 1.2.0 can be crashed, and in one case made to overwrite memory, by a crafted audio file or project.

- Audio files: a crafted WAV/RF64 header could overflow memory, and files that claim impossible sample rates or lengths could exhaust memory. Every file now goes through one checked reader, with limits on rate, length and memory.
- Projects and presets: hostile saved state (XML tricks, huge allocations, NaN values, VST3 trailer data) is refused before it can crash or freeze your DAW.
- Network paths stored in a project (Windows shares, /net, smb:) are never touched when the project opens; the sample shows as missing until you relink it.
- The stem model's SHA-256 is now checked every time it loads, not only after the download, and its file parser is hardened.
- Several bugs in JUCE's MP3, CAF, WAV, AIFF and XML readers found by fuzzing are patched. Built on JUCE 8.0.16-security.
- Logic Pro: projects whose samples live in places Logic's plug-in sandbox can't read (like ~/Music/Logic or Downloads) now reopen with sound. Chop keeps a copy of each loaded sample in Music/Flayr Chop/Samples.
- All eight auto-chop modes now restore with your project.

The stem model (84 MB) downloads on first use. See the README for install steps and SECURITY.md for how to verify your download.
