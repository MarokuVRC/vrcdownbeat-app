#include "InstrumentHost.h"
#include "common/Settings.h"
#include "ui/Style.h"
#include <cmath>

namespace bandjam
{
//==============================================================================
/** The plugin's own UI in a native window. Closing hides it (the plugin keeps
    running); it is destroyed before the plugin instance. */
class InstrumentHost::EditorWindow : public juce::DocumentWindow
{
public:
    explicit EditorWindow (juce::AudioPluginInstance& p)
        : juce::DocumentWindow (p.getName(), style::background(),
                                juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton)
    {
        setUsingNativeTitleBar (true);

        if (auto* editor = p.createEditorIfNeeded())
            setContentOwned (editor, true);
        else
            setContentOwned (new juce::GenericAudioProcessorEditor (p), true);

        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override { setVisible (false); }
};

//==============================================================================
InstrumentHost::InstrumentHost (const juce::String& settingsPrefix)
    : keyPrefix (settingsPrefix)
{
    formatManager.addFormat (new juce::VST3PluginFormat());
}

InstrumentHost::~InstrumentHost()
{
    unloadPlugin();
}

//==============================================================================
bool InstrumentHost::loadPlugin (const juce::File& vst3File, double sampleRate, int blockSize,
                                 juce::String& error)
{
    unloadPlugin();

    if (! vst3File.exists())
    {
        error = "Plugin file not found: " + vst3File.getFullPathName();
        return false;
    }

    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* format : formatManager.getFormats())
        format->findAllTypesForFile (types, vst3File.getFullPathName());

    const juce::PluginDescription* chosen = nullptr;
    for (auto* type : types)
        if (type->isInstrument)
            { chosen = type; break; }
    if (chosen == nullptr && ! types.isEmpty())
        chosen = types.getFirst();   // some instruments don't flag themselves

    if (chosen == nullptr)
    {
        error = "No VST3 plugin found in this file.";
        return false;
    }

    const double rate  = sampleRate > 0.0 ? sampleRate : 44100.0;
    const int    block = juce::jmax (256, blockSize);

    juce::String createError;
    plugin = formatManager.createPluginInstance (*chosen, rate, block, createError);
    if (plugin == nullptr)
    {
        error = "Could not load plugin: " + createError;
        return false;
    }

    pluginFile = vst3File;

    // Restore the saved state if it belongs to this same plugin file.
    if (settings::get (key ("instrumentPlugin"), "").toString() == vst3File.getFullPathName())
    {
        juce::MemoryBlock state;
        if (state.fromBase64Encoding (settings::get (key ("instrumentState"), "").toString())
            && state.getSize() > 0)
            plugin->setStateInformation (state.getData(), (int) state.getSize());
    }

    prepare (rate, block);
    setMidiInput (settings::get (key ("midiInput"), "").toString());

    settings::set (key ("instrumentPlugin"), vst3File.getFullPathName());
    active.store (true);
    return true;
}

void InstrumentHost::unloadPlugin()
{
    active.store (false);
    level.store (0.0f);

    if (midiInput != nullptr)
    {
        midiInput->stop();
        midiInput.reset();
    }

    if (plugin != nullptr)
    {
        saveState();
        editorWindow.reset();      // editor must go before the plugin
        plugin->releaseResources();
        plugin.reset();
    }
    else
    {
        editorWindow.reset();
    }
}

void InstrumentHost::saveState() const
{
    if (plugin == nullptr)
        return;

    juce::MemoryBlock state;
    plugin->getStateInformation (state);
    settings::set (key ("instrumentPlugin"), pluginFile.getFullPathName());
    settings::set (key ("instrumentState"), state.toBase64Encoding());
}

juce::File InstrumentHost::getSavedPluginFile() const
{
    const auto path = settings::get (key ("instrumentPlugin"), "").toString();
    return path.isNotEmpty() ? juce::File (path) : juce::File();
}

juce::String InstrumentHost::getPluginName() const
{
    return plugin != nullptr ? plugin->getName() : juce::String();
}

void InstrumentHost::showEditor()
{
    if (plugin == nullptr)
        return;

    if (editorWindow == nullptr)
        editorWindow = std::make_unique<EditorWindow> (*plugin);

    editorWindow->setVisible (true);
    editorWindow->toFront (true);
}

void InstrumentHost::closeEditor()
{
    editorWindow.reset();
}

//==============================================================================
juce::Array<juce::MidiDeviceInfo> InstrumentHost::getMidiInputs()
{
    return juce::MidiInput::getAvailableDevices();
}

void InstrumentHost::setMidiInput (const juce::String& identifier)
{
    if (midiInput != nullptr)
    {
        midiInput->stop();
        midiInput.reset();
    }

    midiIdentifier = identifier;
    settings::set (key ("midiInput"), identifier);
    midiFailed.store (false);

    if (identifier.isEmpty())
        return;

    // Fails when another program (e.g. the plugin's standalone app) holds
    // the port exclusively - surface that instead of failing silently.
    midiInput = juce::MidiInput::openDevice (identifier, this);
    if (midiInput != nullptr)
        midiInput->start();
    else
        midiFailed.store (true);
}

void InstrumentHost::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& message)
{
    ++midiCount;
    if (message.isNoteOn())
        lastNote.store (message.getNoteNumber());
    midiCollector.addMessageToQueue (message);
}

//==============================================================================
void InstrumentHost::prepare (double sampleRate, int maxBlockSize)
{
    preparedRate  = sampleRate > 0.0 ? sampleRate : 44100.0;
    preparedBlock = juce::jmax (256, maxBlockSize);

    midiCollector.reset (preparedRate);

    // Keep the plugin's default bus layout (forcing a layout can fail and
    // leave some plugins deactivated) - just prepare it and size our buffer
    // to whatever channel count it ended up with.
    if (plugin != nullptr)
    {
        plugin->releaseResources();
        plugin->prepareToPlay (preparedRate, preparedBlock);
    }

    const int channels = plugin != nullptr
        ? juce::jmax (2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels())
        : 2;
    procBuffer.setSize (channels, preparedBlock);
    procBuffer.clear();
}

void InstrumentHost::process (int numSamples)
{
    lastRendered = 0;

    if (! active.load() || plugin == nullptr)
        return;

    const int n = juce::jmin (numSamples, procBuffer.getNumSamples());
    if (n <= 0)
        return;

    midiBuffer.clear();
    midiCollector.removeNextBlockOfMessages (midiBuffer, n);

    procBuffer.clear();
    juce::AudioBuffer<float> slice (procBuffer.getArrayOfWritePointers(),
                                    procBuffer.getNumChannels(), 0, n);
    plugin->processBlock (slice, midiBuffer);
    lastRendered = n;

    float peak = 0.0f;
    const float* left = procBuffer.getReadPointer (0);
    for (int i = 0; i < n; ++i)
        peak = juce::jmax (peak, std::abs (left[i]));
    level.store (peak);
}

void InstrumentHost::addToOutput (float* const* out, int numOut, int numSamples) const
{
    if (lastRendered <= 0 || numOut <= 0)
        return;

    const int n = juce::jmin (numSamples, lastRendered);
    const float* left  = procBuffer.getReadPointer (0);
    const float* right = procBuffer.getNumChannels() > 1 ? procBuffer.getReadPointer (1) : left;

    if (out[0] != nullptr)
        juce::FloatVectorOperations::add (out[0], left, n);
    if (numOut > 1 && out[1] != nullptr)
        juce::FloatVectorOperations::add (out[1], right, n);
}

void InstrumentHost::mixMono (float* dest, int numSamples) const
{
    juce::FloatVectorOperations::clear (dest, numSamples);

    if (lastRendered <= 0)
        return;

    const int n = juce::jmin (numSamples, lastRendered);
    const float* left  = procBuffer.getReadPointer (0);
    const float* right = procBuffer.getNumChannels() > 1 ? procBuffer.getReadPointer (1) : left;

    for (int i = 0; i < n; ++i)
        dest[i] = 0.5f * (left[i] + right[i]);
}

} // namespace bandjam
