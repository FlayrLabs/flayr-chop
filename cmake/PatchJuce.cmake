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
#
# 3. XML entities (XmlDocument::readEntity): a hexadecimal character reference cut off by the end of
#    the text ("&#x41" as the last bytes) stepped past the string's terminator, and the parser kept
#    reading heap memory after it. Reached through any project state (setStateInformation).
#
# 4. WAV and AIFF metadata chunks (bext, smpl, inst, cue, INST): the readers allocated (and zeroed)
#    whatever size the chunk header claimed, up to 4 GB for a 100-byte file, before reading it; where
#    that allocation fails the null buffer is then used. The size is now limited to what the file
#    holds.
#
# 5. AIFF 'cate' chunk (AiffFileHelpers::CATEChunk::read): each tag check read three bytes even when
#    fewer were left in the chunk, a heap over-read past the chunk's buffer.
#
# 6. VST3 wrapper (JuceVST3Component::setStateInformation, before the plug-in sees the state): the
#    "JUCEPrivateData" trailer's 64-bit size came from the project and was trusted, so a lying size
#    read far outside the state (and wrapped the size passed on to the plug-in), and the trailer was
#    parsed with ValueTree::readFromData, which recurses per child and sizes arrays from the data (a
#    deeply nested trailer overflowed the host thread's stack; a child count of 2^31 asked for 16 GB).
#    Found by an independent review of the sister plug-in Flayr Tune. The size must now fit in the
#    state, the trailer is at most 4 KB, and it is read without recursion for the one value JUCE
#    stores there (Bypass).
#
# 7. MemoryBlock::fromBase64Encoding: the decimal size before the '.' came from the text and was
#    allocated (zeroed) as given, up to 2 GB for a few bytes, and getIntValue overflowed on long digit
#    strings; ValueTree::fromXml decodes attributes named "base64:..." with it, and the LV2 wrapper
#    decodes the whole saved state with it. Plain digits only, at most INT_MAX, and no more than the
#    encoded characters can hold: the same change as Flayr Tune's juce-base64-declared-size.patch.
#
# 8. XML nesting (XmlDocument::readNextElement and entity expansion): the parser recursed once per
#    level with no limit, so 2,000 nested elements in a 16 KB WAV's iXML chunk overflowed the 512 KB
#    stack of the thread that loads samples (found by an independent review). Parsing now fails past
#    64 levels, for every parseXML caller.
#
# 9. WAV reader: the iXML and axml chunks (free-form XML metadata Flayr Chop never uses) are no
#    longer parsed at all, so an audio file never feeds JUCE's XML parser.

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

# 3
flayr_chop_patch("modules/juce_core/xml/juce_XmlDocument.cpp" "Flayr Chop: hex entity at the end"
"            while (input[0] != ';')
            {
                auto hexValue = CharacterFunctions::getHexDigitValue (input[0]);
"
"            while (input[0] != ';')
            {
                if (input[0] == 0) // Flayr Chop: hex entity at the end of the text: don't step past it
                {
                    setLastError (\"unexpected end of input\", true);
                    return;
                }

                auto hexValue = CharacterFunctions::getHexDigitValue (input[0]);
")

# 4
set(_clamp "length = (uint32) jmin ((int64) length, jmax ((int64) 0, input->getTotalLength() - input->getPosition()));")
foreach(_chunk "BWAVChunk bwav" "SMPLChunk smpl" "InstChunk inst" "CueChunk cue")
    string(REPLACE " " ";" _parts "${_chunk}")
    list(GET _parts 0 _type)
    list(GET _parts 1 _var)
    flayr_chop_patch("modules/juce_audio_formats/codecs/juce_WavAudioFormat.cpp" "Flayr Chop: ${_type} size"
"                    HeapBlock<${_type}> ${_var};
"
"                    ${_clamp} // Flayr Chop: ${_type} size within the file
                    HeapBlock<${_type}> ${_var};
")
endforeach()
flayr_chop_patch("modules/juce_audio_formats/codecs/juce_AiffAudioFormat.cpp" "Flayr Chop: InstChunk size"
"                        HeapBlock<InstChunk> inst;
"
"                        ${_clamp} // Flayr Chop: InstChunk size within the file
                        HeapBlock<InstChunk> inst;
")

# 5
flayr_chop_patch("modules/juce_audio_formats/codecs/juce_AiffAudioFormat.cpp" "Flayr Chop: cate tag at the end"
"                if (isValidTag (data))
"
"                if (dataEnd - data >= 3 && isValidTag (data)) // Flayr Chop: cate tag at the end: needs 3 bytes
")

# 6
flayr_chop_patch("modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp" "Flayr Chop: private data size"
"                privateDataSize = ByteOrder::swapIfBigEndian (privateDataSize);
                size -= privateDataSize + jucePrivDataIdentifierSize + sizeof (uint64);
"
"                privateDataSize = ByteOrder::swapIfBigEndian (privateDataSize);

                // Flayr Chop: private data size from the project: it must fit in the state (with the
                // leading zero and the size field), or the state is refused.
                if ((size_t) size < jucePrivDataIdentifierSize + 2 * sizeof (uint64)
                    || privateDataSize > size - jucePrivDataIdentifierSize - 2 * sizeof (uint64))
                    return;

                size -= privateDataSize + jucePrivDataIdentifierSize + sizeof (uint64);
")
flayr_chop_patch("modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp" "Flayr Chop: private data parse"
"            if (comPluginInstance->getBypassParameter() != nullptr)
            {
                auto privateData = ValueTree::readFromData (data, static_cast<size_t> (sizeInBytes));
                setBypassed (static_cast<bool> (privateData.getProperty (\"Bypass\", var (false))));
            }"
"            if (comPluginInstance->getBypassParameter() != nullptr)
            {
                // Flayr Chop: private data parse without ValueTree::readFromData (recursive, and it sizes
                // arrays from the data): only the shape writeJucePrivateStateInformation writes, one
                // tree of scalar properties, at most 4 KB.
                if (sizeInBytes <= 0 || sizeInBytes > 4096)
                    return;

                MemoryInputStream in (data, static_cast<size_t> (sizeInBytes), false);

                if (in.readString() != kJucePrivateDataIdentifier)
                    return;

                const auto numProps = in.readCompressedInt();
                bool bypass = false;

                for (int i = 0; i < numProps && i < 16 && ! in.isExhausted(); ++i)
                {
                    const auto name = in.readString();
                    const auto numBytes = in.readCompressedInt();

                    if (numBytes <= 0 || numBytes > in.getNumBytesRemaining())
                        return;

                    const auto next = in.getPosition() + numBytes;
                    const auto marker = in.readByte(); // var::writeToStream: 1 int, 2 true, 3 false

                    if (name == \"Bypass\")
                        bypass = marker == 2 || (marker == 1 && numBytes >= 5 && in.readInt() != 0);

                    in.setPosition (next);
                }

                setBypassed (bypass);
            }")

# 7 (identical to Flayr Tune's patches/juce-base64-declared-size.patch)
flayr_chop_patch("modules/juce_core/memory/juce_MemoryBlock.cpp" "FLAYR_PATCH juce-base64-declared-size"
"    auto numBytesNeeded = String (s.text, dot).getIntValue();

    setSize ((size_t) numBytesNeeded, true);

    auto srcChars = dot + 1;
    size_t pos = 0;
"
"    // FLAYR_PATCH juce-base64-declared-size: the byte count comes from the encoded text and used to be allocated as
    // given (\"-1.\" asked for SIZE_MAX and threw std::bad_alloc, \"2000000000.\" for 2 GB,
    // and getIntValue overflowed on longer numbers). Accept only plain digits, and only a
    // count the characters after the dot can actually fill.
    size_t numBytesNeeded = 0;

    for (auto p = s.text; p != dot; ++p)
    {
        const auto c = *p;

        if (c < '0' || c > '9')
            return false;

        numBytesNeeded = numBytesNeeded * 10 + (size_t) (c - '0');

        if (numBytesNeeded > (size_t) std::numeric_limits<int>::max())
            return false;
    }

    auto srcChars = dot + 1;

    if (numBytesNeeded > srcChars.length() * 6 / 8 + 1)
        return false;

    setSize (numBytesNeeded, true);

    size_t pos = 0;
")

# 8
flayr_chop_patch("modules/juce_core/xml/juce_XmlDocument.cpp" "Flayr Chop: XML depth limit"
"XmlElement* XmlDocument::readNextElement (const bool alsoParseSubElements)
{
    XmlElement* node = nullptr;
"
"XmlElement* XmlDocument::readNextElement (const bool alsoParseSubElements)
{
    // Flayr Chop: XML depth limit. This function recurses once per nesting level, so untrusted text
    // could exhaust the stack (2,000 levels overflow a 512 KB thread): fail past 64.
    struct DepthGuard
    {
        explicit DepthGuard (int& d) : depth (d) { ++depth; }
        ~DepthGuard() { --depth; }
        int& depth;
    };
    static thread_local int nesting = 0;
    const DepthGuard guard (nesting);

    if (nesting > 64)
    {
        setLastError (\"too deeply nested\", false);
        outOfData = true;
        return nullptr;
    }

    XmlElement* node = nullptr;
")
flayr_chop_patch("modules/juce_core/xml/juce_XmlDocument.cpp" "Flayr Chop: entity depth limit"
"String XmlDocument::expandExternalEntity (const String& entity)
{
"
"String XmlDocument::expandExternalEntity (const String& entity)
{
    // Flayr Chop: entity depth limit (entities that refer to entities recurse here).
    struct DepthGuard
    {
        explicit DepthGuard (int& d) : depth (d) { ++depth; }
        ~DepthGuard() { --depth; }
        int& depth;
    };
    static thread_local int nesting = 0;
    const DepthGuard guard (nesting);

    if (nesting > 64)
    {
        setLastError (\"entities too deeply nested\", false);
        outOfData = true;
        return {};
    }

")

# 9
flayr_chop_patch("modules/juce_audio_formats/codecs/juce_WavAudioFormat.cpp" "Flayr Chop: no iXML/axml parsing"
"                else if (chunkType == chunkName (\"axml\"))
                {
                    MemoryBlock axml;
                    input->readIntoMemoryBlock (axml, (ssize_t) length);
                    AXMLChunk::addToMetadata (dict, axml.toString());
                }
                else if (chunkType == chunkName (\"iXML\"))
                {
                    MemoryBlock ixml;
                    input->readIntoMemoryBlock (ixml, (ssize_t) length);
                    IXMLChunk::addToMetadata (dict, ixml.toString());
                }"
"                else if (chunkType == chunkName (\"axml\") || chunkType == chunkName (\"iXML\"))
                {
                    // Flayr Chop: no iXML/axml parsing: free-form XML metadata from the file, never used by
                    // Flayr Chop, would otherwise go through the XML parser. Skipped like any unknown chunk.
                }")
