// The VST3 plug-in as a host loads it: project states that reach JUCE's VST3 wrapper before
// FlayrChopProcessor sees them (the "JUCEPrivateData" trailer after the plug-in's own state).
//
//   vst3_wrapper_test "path/to/Flayr Chop.vst3"
//
// Each crafted state must return normally: a trailer whose size lies (it used to be trusted and read
// far outside the state), a deeply nested trailer (ValueTree::readFromData recursed per level), and a
// trailer claiming 2^31 children (sized an array from the data). A genuine state still restores.
#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdio>

using namespace juce;

static int failures = 0, checks = 0;
#define CHECK(cond, ...)                                    \
    do {                                                    \
        ++checks;                                           \
        if (! (cond)) {                                     \
            ++failures;                                     \
            std::printf ("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf (__VA_ARGS__);                      \
            std::printf ("\n");                             \
        }                                                   \
    } while (0)

// The host-side state (XML around the base64 IComponent stream) with the component stream replaced.
static MemoryBlock withComponentStream (const XmlElement& hostState, const MemoryBlock& component)
{
    XmlElement copy (hostState);
    auto* ic = copy.getChildByName ("IComponent");
    ic->deleteAllTextElements();
    ic->addTextElement (component.toBase64Encoding());
    MemoryBlock out;
    AudioProcessor::copyXmlToBinary (copy, out);
    return out;
}

// Plug-in state, then the wrapper's trailer: int64 0, private data, int64 size, identifier.
static MemoryBlock trailer (const MemoryBlock& pluginState, const MemoryBlock& privateData, int64 claimedSize)
{
    MemoryOutputStream c;
    c.write (pluginState.getData(), pluginState.getSize());
    c.writeInt64 (0);
    c.write (privateData.getData(), privateData.getSize());
    c.writeInt64 (claimedSize);
    c << "JUCEPrivateData";
    return c.getMemoryBlock();
}

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    ScopedJuceInitialiser_GUI gui;
    if (argc < 2)
    {
        std::printf ("usage: vst3_wrapper_test path/to/plugin.vst3\n");
        return 2;
    }

    VST3PluginFormat format;
    OwnedArray<PluginDescription> types;
    format.findAllTypesForFile (types, argv[1]);
    CHECK (! types.isEmpty(), "no plug-in in %s", argv[1]);
    if (types.isEmpty())
        return 1;
    String error;
    auto plugin = format.createInstanceFromDescription (*types[0], 48000.0, 512, error);
    CHECK (plugin != nullptr, "load failed: %s", error.toRawUTF8());
    if (plugin == nullptr)
        return 1;

    // A genuine state with a parameter moved, to tell a restore from no restore.
    auto* volume = plugin->getParameters()[0];
    volume->setValueNotifyingHost (0.25f);
    const auto expected = volume->getValue(); // as the parameter stores it
    MemoryBlock genuine;
    plugin->getStateInformation (genuine);
    const auto hostState = AudioProcessor::getXmlFromBinary (genuine.getData(), (int) genuine.getSize());
    CHECK (hostState != nullptr && hostState->getChildByName ("IComponent") != nullptr, "unexpected host state format");
    if (hostState == nullptr || hostState->getChildByName ("IComponent") == nullptr)
        return 1;
    MemoryBlock component;
    component.fromBase64Encoding (hostState->getChildByName ("IComponent")->getAllSubText());
    // The plug-in's own part: copyXmlToBinary's magic, length, text and NUL.
    const auto* bytes = static_cast<const uint8*> (component.getData());
    CHECK (component.getSize() > 8 && ByteOrder::littleEndianInt (bytes) == 0x21324356, "component stream isn't ours");
    const auto ownLength = (size_t) ByteOrder::littleEndianInt (bytes + 4) + 9;
    CHECK (ownLength <= component.getSize(), "component stream too short");
    const MemoryBlock pluginState (bytes, jmin (ownLength, component.getSize()));

    std::printf ("-- genuine state\n");
    volume->setValueNotifyingHost (0.9f);
    plugin->setStateInformation (genuine.getData(), (int) genuine.getSize());
    CHECK (std::abs (volume->getValue() - expected) < 1e-3f, "genuine state not restored (volume %f)", volume->getValue());

    std::printf ("-- trailer size beyond the state\n");
    for (const int64 lie : { (int64) 0x40000000, (int64) -1, (int64) 0x7fffffffffffffffLL, (int64) (pluginState.getSize() + 1) })
    {
        const auto evil = withComponentStream (*hostState, trailer (pluginState, {}, lie));
        plugin->setStateInformation (evil.getData(), (int) evil.getSize());
        CHECK (true, "survived a lying trailer size");
    }

    std::printf ("-- deeply nested trailer\n");
    {
        MemoryOutputStream p;
        for (int i = 0; i < 400000; ++i)
        {
            p.write ("a", 2); // type name "a" and its NUL
            p.writeCompressedInt (0);
            p.writeCompressedInt (1);
        }
        const auto evil = withComponentStream (*hostState, trailer (pluginState, p.getMemoryBlock(), (int64) p.getDataSize()));
        plugin->setStateInformation (evil.getData(), (int) evil.getSize());
        CHECK (true, "survived a deeply nested trailer");
        // Small enough to be parsed (under 4 KB), still nested: read without recursion.
        MemoryOutputStream q;
        for (int i = 0; i < 1000; ++i)
        {
            q.write ("a", 2);
            q.writeCompressedInt (0);
            q.writeCompressedInt (1);
        }
        const auto small = withComponentStream (*hostState, trailer (pluginState, q.getMemoryBlock(), (int64) q.getDataSize()));
        plugin->setStateInformation (small.getData(), (int) small.getSize());
        CHECK (true, "survived a nested trailer under 4 KB");
    }

    std::printf ("-- trailer with 2^31 children\n");
    {
        MemoryOutputStream p;
        p << "JUCEPrivateData";
        p.writeByte (0);
        p.writeCompressedInt (0);
        p.writeCompressedInt (0x7fffffff);
        const auto evil = withComponentStream (*hostState, trailer (pluginState, p.getMemoryBlock(), (int64) p.getDataSize()));
        plugin->setStateInformation (evil.getData(), (int) evil.getSize());
        CHECK (true, "survived a trailer claiming 2^31 children");
    }

    std::printf ("-- genuine state again\n");
    volume->setValueNotifyingHost (0.9f);
    plugin->setStateInformation (genuine.getData(), (int) genuine.getSize());
    CHECK (std::abs (volume->getValue() - expected) < 1e-3f, "genuine state not restored after the crafted ones");

    plugin.reset();
    std::printf ("vst3_wrapper_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
