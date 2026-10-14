# Third-party notices

Flayr Chop is built with the following projects.

| Component | License | Source | How it's included |
|---|---|---|---|
| JUCE 8.0.16-security, with Flayr Chop's security patches (cmake/PatchJuce.cmake) | AGPLv3 (used here) or the commercial JUCE licence | https://github.com/juce-framework/JUCE | downloaded at build time |
| Steinberg VST3 SDK (bundled with JUCE) | MIT | https://github.com/steinbergmedia/vst3sdk | downloaded at build time |
| LV2 (bundled with JUCE, Linux builds) | ISC | https://lv2plug.in | downloaded at build time |
| Signalsmith Stretch 1.4.0 and Signalsmith Linear | MIT | https://github.com/Signalsmith-Audio/signalsmith-stretch | downloaded at build time |
| demucs.cpp (modified) | MIT | https://github.com/sevagh/demucs.cpp | vendored in `Source/stems/third_party/demucs.cpp` |
| Eigen | MPL 2.0 | https://gitlab.com/libeigen/eigen | downloaded at build time, unmodified |
| Demucs v4 (htdemucs) model weights | MIT, Meta Platforms | https://github.com/facebookresearch/demucs | downloaded on first use, not distributed with the plug-in |

VST is a registered trademark of Steinberg Media Technologies GmbH. Audio Units and Logic Pro
are trademarks of Apple Inc.
