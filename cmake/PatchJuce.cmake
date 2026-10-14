# Patches applied to the fetched JUCE sources (FetchContent PATCH_COMMAND), idempotent:
#   cmake -DJUCE_DIR=<juce source dir> -P cmake/PatchJuce.cmake
#
# 1. MP3 decoder, intensity stereo (MP3Decoder::Layer3SideInfo::Info::doIStereo): for a long block
#    whose last non-zero band is band 21, maxBandl is 22 and the loop that copies band 20 into band
#    21 started at sample 576, writing past the 2 x 576 sample buffer into the rest of the decoder
#    object. Found by fuzzing Flayr Chop with a crafted .mp3 (JUCE decodes MP3 itself on Windows and
#    Linux, and on macOS when Core Audio refuses a file). Same fix as mpg123's for CVE-2017-11126:
#    nothing to do above band 21.

if(NOT JUCE_DIR)
    message(FATAL_ERROR "PatchJuce.cmake: pass -DJUCE_DIR=<JUCE source dir>")
endif()

set(_file "${JUCE_DIR}/modules/juce_audio_formats/codecs/juce_MP3AudioFormat.cpp")
file(READ "${_file}" _src)
# Windows checkouts may have CRLF line endings (core.autocrlf): match and write with LF.
string(REPLACE "\r" "" _src "${_src}")
set(_marker "Flayr Chop: maxBandl")
string(FIND "${_src}" "${_marker}" _done)
if(NOT _done EQUAL -1)
    return()
endif()

set(_old "            else
            {
                int index = bi.longIndex[maxBandl];

                for (uint32 sfb = maxBandl; sfb < 21; ++sfb)")
set(_new "            else
            {
                // ${_marker} 22 means band 21 holds data: no intensity stereo to apply (and band 20's
                // copy below would start past the end of xr). Same fix as mpg123's for CVE-2017-11126.
                if (maxBandl > 21)
                    return;

                int index = bi.longIndex[maxBandl];

                for (uint32 sfb = maxBandl; sfb < 21; ++sfb)")
string(REPLACE "\r" "" _old "${_old}")
string(REPLACE "\r" "" _new "${_new}")
string(FIND "${_src}" "${_old}" _at)
if(_at EQUAL -1)
    message(FATAL_ERROR "PatchJuce.cmake: the MP3 decoder in ${_file} doesn't match this JUCE version's code; update the patch")
endif()
string(REPLACE "${_old}" "${_new}" _src "${_src}")
file(WRITE "${_file}" "${_src}")
message(STATUS "Patched JUCE: MP3 intensity stereo bounds (${_file})")
