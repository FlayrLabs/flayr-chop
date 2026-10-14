# Security Policy

## Supported versions

Security fixes go into the latest release. Please update to it before reporting.

| Version | Supported |
|---|---|
| Latest release | Yes |
| Older releases | No |

## Reporting a vulnerability

**Please don't open a public issue for security problems.**

Report it privately through GitHub instead:
**[Report a vulnerability](https://github.com/FlayrLabs/flayr-chop/security/advisories/new)**

Include what you found, the version, OS and DAW, and the steps or a file that reproduces it.

What to expect:
- We reply within 3 business days.
- We aim to release a fix within 30 days for confirmed issues, sooner for serious ones.
- We credit you in the advisory and release notes, unless you'd rather stay anonymous.
- We'll coordinate with you on when the details are made public.

## What's in scope

Flayr Chop runs inside your DAW, so anything that could affect your computer or your projects is in scope, for example:
- A project, saved plug-in state or audio file that crashes the DAW, uses unbounded memory, or runs code when loaded.
- The stem model download: anything that could make the plug-in accept a file other than the published model.
- Problems in the installers or install scripts.
- Release files that don't match this source code or aren't signed as described below.

Audio quality bugs and crashes that need no untrusted input are regular bugs: please open a normal issue.

## What Flayr Chop does and doesn't do

- **One network request, only when you ask for it.** Flayr Chop touches the network in exactly one
  place: when you press SEPARATE STEMS and the stem model isn't on your computer yet. It then
  downloads `https://github.com/FlayrLabs/flayr-chop/releases/download/model-htdemucs-4s-v1/ggml-model-htdemucs-4s-f16.bin`
  over HTTPS (GitHub redirects release downloads to its own file servers; at most 8 redirects are
  followed), refuses anything that isn't exactly the expected size, and keeps the file only if its
  SHA-256 matches the value pinned in the source (`Source/stems/Stems.cpp`); the model is checked
  against that SHA-256 again every time it's loaded. Loading samples or projects, playback, analysis
  and everything else never connect: no telemetry, no analytics, no licence check, no auto-update,
  and project paths on other machines are never opened automatically. The plug-in declares no
  sandbox entitlements or AU resource-usage grants of its own; it runs with your DAW's permissions.
- **Files it reads:** the audio file you load (projects store its path and its library copy's path,
  not the audio).
- **Files it writes:** a copy of each sample you load in `Music/Flayr Chop/Samples` (so projects
  reopen in sandboxed hosts; see the README for clearing it); the stem model
  (`Flayr Chop/models` in `~/Library/Application Support` on macOS, `%APPDATA%` on Windows,
  `~/.local/share` on Linux) and a per-track stem cache (`Flayr Chop/stems` in
  `~/Library/Application Support`, `%APPDATA%`, or `~/.config` on Linux); WAV files you export; and
  slices you drag out of the plug-in, saved in `Music/Flayr Chop/Slices`. Deleting any of these
  Flayr Chop folders is always safe.
- **No elevated privileges.** The plug-in runs with your DAW's permissions. The installers only copy the plug-in into the standard plug-in folders.
- Saved state from a project is size-limited, and every value is range-checked before use.

## Verifying your download

Every release includes `SHA256SUMS.txt`. Check your file against it:

```sh
shasum -a 256 FlayrChop-*-macOS.dmg                    # macOS
certutil -hashfile FlayrChop-*-Windows-Setup.exe SHA256  # Windows
sha256sum FlayrChop-*-Linux-x64.tar.gz                   # Linux
```

**macOS:** the DMG and plug-ins are signed with Flayr Labs' Apple Developer ID (team `NRNU83UJ68`) and notarized by Apple. To check:

```sh
spctl --assess --type open --context context:primary-signature -v FlayrChop-*-macOS.dmg
codesign -dv --verbose=2 "/Library/Audio/Plug-Ins/Components/Flayr Chop.component"
```

You should see `Notarized Developer ID` and `Developer ID Application: Flayr Labs LLC (NRNU83UJ68)`.

**Windows and Linux:** these files are built from the tagged source by GitHub Actions, in public, and carry a signed build provenance attestation. With the [GitHub CLI](https://cli.github.com):

```sh
gh attestation verify FlayrChop-*-Windows-Setup.exe --repo FlayrLabs/flayr-chop
```

The Windows installer isn't Authenticode-signed yet, so Windows SmartScreen may warn about it.
