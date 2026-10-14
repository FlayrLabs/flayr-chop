# Contributing to Flayr Chop

Thanks for helping. A few notes so changes land smoothly.

## Reporting bugs

For security problems, please report privately instead: see [SECURITY.md](SECURITY.md).

Please include:
- Your OS and DAW, with versions.
- The plug-in format (AU, VST3 or LV2).
- What you loaded (format, length, sample rate) and whether stems were separated.
- A short audio clip, if the bug is about how it sounds.

## Where things live

| File | What it is |
|---|---|
| `Source/Sampler.h` | Pads, voices, stem mixing and the filter. Framework-free; runs on the audio thread. |
| `Source/Renderer.*` | HQ Stretch (key and tempo) renders on a worker thread. |
| `Source/Analysis.h` | Tempo, beat grid, key, transients, auto-chop. Framework-free. |
| `Source/stems/` | Stem separation (Demucs v4 via a modified demucs.cpp). |
| `Source/SampleLoader.*` | Decoding, resampling, the stem cache, WAV export. |
| `Source/PluginProcessor.*`, `Source/PluginEditor.*` | The plug-in and its UI. |

## Making changes

- **The audio thread must never allocate, lock or wait.** Audio data reaches it through
  `Published<T>` snapshots (see `Sampler.h`); follow that pattern.
- Add or update a check in `tests/` for any behaviour change, and run `ctest --test-dir build -C Release`.
- **Parameter IDs and saved-state property names are permanent.** Saved projects depend on them.
  Rename the display name instead.
- **UI changes:** build with `-DFLAYR_CHOP_BUILD_TOOLS=ON` and attach a `FlayrChopSnapshot`
  render to the pull request.
- **Validate the plug-in** with [pluginval](https://github.com/Tracktion/pluginval) at strictness 8 or higher.
- Only describe features in your own words; don't use other products' names or trademarks in the UI or docs.
- **Changes to anything that parses files or project state** (audio decoding, `setStateInformation`,
  the stem model or stem cache): run the fuzz targets for a while, in a separate build directory
  (never a release build):

  ```sh
  cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DFLAYR_CHOP_COPY_AFTER_BUILD=OFF -DFLAYR_CHOP_BUILD_TESTS=OFF -DFLAYR_CHOP_BUILD_FUZZERS=ON
  cmake --build build-fuzz --target fuzz_sample_load fuzz_state_restore fuzz_model_parser fuzz_stem_cache
  scripts/fuzz.sh build-fuzz 900          # 15 minutes per target
  ```

  libFuzzer is used when the compiler has it; Apple's clang doesn't, so on macOS the targets use the
  small coverage-guided engine in `tests/fuzz/FuzzDriver.cpp` instead (both with ASan and UBSan). A
  finding stops the run and leaves the input in the reported `artifacts` folder; replay it with
  `fuzz_<target> <file>`. Fix it, then commit the input under `tests/fuzz/regressions/<target>/` and add
  a check for it to `tests/hardening_test.cpp` (or `tests/stems_test.cpp`).

## Releases (maintainers)

1. Bump `project(FlayrChop VERSION x.y.z)` in `CMakeLists.txt`, commit, then tag and push `vx.y.z`.
   GitHub Actions builds the Windows installer and the Linux package and puts them on a draft release.
2. On a Mac with the Flayr Labs Developer ID, check out the tag (clean tree) and run
   `scripts/release-macos.sh --upload` to add the signed and notarized DMG. The script refuses to
   run anywhere else and builds from a fresh clone of the tag.
3. Publish the draft.

