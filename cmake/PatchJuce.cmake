# Security fixes applied to the fetched JUCE sources (FetchContent PATCH_COMMAND), idempotent:
#   cmake -DJUCE_DIR=<juce source dir> -P cmake/PatchJuce.cmake
# All found by fuzzing Flayr Chop's file loading (tests/fuzz). Each edit is marked "Flayr Chop:" in
# the patched file; CMakeLists.txt refuses to configure against a JUCE checkout without them.
#
# 1. MP3 decoder, intensity stereo (MP3Decoder::Layer3SideInfo::Info::doIStereo): for a long block
#    whose last non-zero band is band 21, maxBandl is 22 and the loop that copies band 20 into band
#    21 started at sample 576, writing past the 2 x 576 sample buffer into the rest of the decoder
#    object. JUCE decodes MP3 itself on Windows and Linux, and on macOS when Core Audio refuses a
#    file. Same fix as mpg123's for CVE-2017-11126: nothing to do above band 21.
#
# 2. CAF metadata (CoreAudioFormatMetatdata, macOS): a chunk with a negative size moved the stream
#    backwards and the chunk loop never ended, and the 'info' and 'uuid' chunk parsers looped up to a
#    32-bit entry count from the file after the stream ran out. A 79-byte file hung the loader.

if(NOT JUCE_DIR)
    message(FATAL_ERROR "PatchJuce.cmake: pass -DJUCE_DIR=<JUCE source dir>")
endif()

# Replaces `old` with `new` (each must appear exactly as written, line endings aside) unless `marker`
# is already in the file.
function(flayr_chop_patch file marker old new)
    set(_path "${JUCE_DIR}/${file}")
    file(READ "${_path}" _src)
    # Windows checkouts may have CRLF line endings (core.autocrlf): match and write with LF.
    string(REPLACE "\r" "" _src "${_src}")
    string(FIND "${_src}" "${marker}" _done)
    if(NOT _done EQUAL -1)
        return()
    endif()
    string(REPLACE "\r" "" old "${old}")
    string(REPLACE "\r" "" new "${new}")
    string(FIND "${_src}" "${old}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "PatchJuce.cmake: ${file} doesn't match this JUCE version's code (${marker}); update the patch")
    endif()
    string(REPLACE "${old}" "${new}" _src "${_src}")
    file(WRITE "${_path}" "${_src}")
    message(STATUS "Patched JUCE: ${marker}")
endfunction()

# 1
flayr_chop_patch("modules/juce_audio_formats/codecs/juce_MP3AudioFormat.cpp" "Flayr Chop: maxBandl"
"            else
            {
                int index = bi.longIndex[maxBandl];

                for (uint32 sfb = maxBandl; sfb < 21; ++sfb)"
"            else
            {
                // Flayr Chop: maxBandl 22 means band 21 holds data: no intensity stereo to apply (and band 20's
                // copy below would start past the end of xr). Same fix as mpg123's for CVE-2017-11126.
                if (maxBandl > 21)
                    return;

                int index = bi.longIndex[maxBandl];

                for (uint32 sfb = maxBandl; sfb < 21; ++sfb)")

# 2
flayr_chop_patch("modules/juce_audio_formats/codecs/juce_CoreAudioFormat.cpp" "Flayr Chop: negative chunk size"
"                const ChunkHeader chunkHeader (input);
"
"                const ChunkHeader chunkHeader (input);

                // Flayr Chop: negative chunk sizes (other than the data chunk's 'unknown', -1) would move
                // the stream backwards and loop forever.
                if (chunkHeader.chunkSize < -1 || (chunkHeader.chunkSize == -1 && chunkHeader.chunkType != chunkName (\"data\")))
                    break;
")
flayr_chop_patch("modules/juce_audio_formats/codecs/juce_CoreAudioFormat.cpp" "Flayr Chop: info entries"
"        for (uint32 i = 0; i < numEntries; ++i)
            infoStrings.set (input.readString(), input.readString());"
"        for (uint32 i = 0; i < numEntries && ! input.isExhausted(); ++i) // Flayr Chop: info entries end with the stream
            infoStrings.set (input.readString(), input.readString());")
flayr_chop_patch("modules/juce_audio_formats/codecs/juce_CoreAudioFormat.cpp" "Flayr Chop: uuid entries"
"            for (uint32 i = 0; i < numEntries && input.getPosition() < originalPosition + size; ++i)"
"            for (uint32 i = 0; i < numEntries && input.getPosition() < originalPosition + size && ! input.isExhausted(); ++i) // Flayr Chop: uuid entries end with the stream")
