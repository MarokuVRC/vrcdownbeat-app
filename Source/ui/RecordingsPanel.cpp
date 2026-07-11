#include "RecordingsPanel.h"
#include "common/AppPaths.h"
#include "common/Settings.h"
#include "common/Mp3Encoder.h"
#include "ui/Style.h"
#include <cmath>
#include <thread>

namespace bandjam
{
namespace
{
    juce::String formatClock (double seconds)
    {
        if (seconds < 0.0 || std::isnan (seconds))
            seconds = 0.0;
        const int total = (int) seconds;
        return juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2);
    }

    juce::String kindLabel (const juce::String& kind)
    {
        if (kind == "stem")     return "Backing";
        if (kind == "musician") return "Musician";
        if (kind == "host")     return "Host";
        return kind;
    }
}

//==============================================================================
RecordingsPanel::RecordingsPanel (Options optionsToUse)
    : options (std::move (optionsToUse))
{
    style::styleSectionLabel (listCaption, "Recorded jams");
    addAndMakeVisible (listCaption);

    list.setRowHeight (44);
    addAndMakeVisible (list);

    refreshButton.setButtonText ("Refresh");
    refreshButton.onClick = [this] { refreshList(); };
    addAndMakeVisible (refreshButton);

    loadButton.setButtonText ("Load");
    loadButton.onClick = [this] { loadClicked(); };
    addAndMakeVisible (loadButton);

    deleteButton.setButtonText ("Delete");
    deleteButton.onClick = [this] { deleteClicked(); };
    addAndMakeVisible (deleteButton);

    openFolderButton.setButtonText ("Open folder");
    openFolderButton.onClick = [this]
    {
        const int row = list.getSelectedRow();
        if (juce::isPositiveAndBelow (row, (int) entries.size()))
            entries[(size_t) row].dir.revealToUser();
        else
            settings::recordingsFolder().revealToUser();
    };
    addAndMakeVisible (openFolderButton);

    if (options.onSendRecording)
    {
        sendButton.setButtonText ("Send to...");
        sendButton.setEnabled (false);
        sendButton.onClick = [this]
        {
            const int row = list.getSelectedRow();
            if (juce::isPositiveAndBelow (row, (int) entries.size()))
                options.onSendRecording (entries[(size_t) row].dir, entries[(size_t) row].song, sendButton);
        };
        addAndMakeVisible (sendButton);
    }

    if (options.showAutoReceiveToggle)
    {
        autoReceiveToggle.setButtonText ("Receive recordings from the host automatically");
        autoReceiveToggle.setColour (juce::ToggleButton::textColourId, style::textPrimary());
        autoReceiveToggle.setColour (juce::ToggleButton::tickColourId, style::accent());
        if (options.getAutoReceive)
            autoReceiveToggle.setToggleState (options.getAutoReceive(), juce::dontSendNotification);
        autoReceiveToggle.onClick = [this]
        {
            if (options.onAutoReceiveChanged)
                options.onAutoReceiveChanged (autoReceiveToggle.getToggleState());
        };
        addAndMakeVisible (autoReceiveToggle);
    }

    style::styleSectionLabel (loadedTitle, "No recording loaded");
    addAndMakeVisible (loadedTitle);

    statusLabel.setFont (style::normalFont());
    statusLabel.setColour (juce::Label::textColourId, style::textDim());
    statusLabel.setText (options.showAutoReceiveToggle
                             ? "Recordings you receive from the host show up here."
                             : "Record a jam (enable \"Record\") or a song (auto-record in Settings > Audio), then load it here.",
                         juce::dontSendNotification);
    addAndMakeVisible (statusLabel);

    playButton.setButtonText ("Play");
    playButton.setEnabled (false);
    playButton.onClick = [this]
    {
        if (! loadedIsActive())
            return;
        if (options.player.isPlaying()) options.player.pause();
        else                            options.player.play();
    };
    addAndMakeVisible (playButton);

    stopButton.setButtonText ("Stop");
    stopButton.setEnabled (false);
    stopButton.onClick = [this] { if (loadedIsActive()) options.player.stop(); };
    addAndMakeVisible (stopButton);

    posSlider.setRange (0.0, 1.0, 0.01);
    posSlider.onValueChange = [this]
    {
        if (loadedIsActive() && posSlider.isMouseButtonDown())
            options.player.seek (posSlider.getValue());
    };
    addAndMakeVisible (posSlider);

    timeLabel.setFont (style::normalFont());
    timeLabel.setColour (juce::Label::textColourId, style::textDim());
    timeLabel.setJustificationType (juce::Justification::centredRight);
    timeLabel.setText ("0:00 / 0:00", juce::dontSendNotification);
    addAndMakeVisible (timeLabel);

    stripsViewport.setViewedComponent (&stripsContainer, false);
    stripsViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (stripsViewport);

    exportCaption.setFont (style::normalFont());
    exportCaption.setColour (juce::Label::textColourId, style::textDim());
    exportCaption.setText ("Export the mix (current volumes/mutes):", juce::dontSendNotification);
    addAndMakeVisible (exportCaption);

    bitrateBox.addItem ("128 kbps", 128);
    bitrateBox.addItem ("192 kbps", 192);
    bitrateBox.addItem ("320 kbps", 320);
    bitrateBox.setSelectedId (192, juce::dontSendNotification);
    addAndMakeVisible (bitrateBox);

    exportButton.setButtonText ("Export MP3");
    exportButton.setEnabled (false);
    exportButton.onClick = [this] { exportClicked(); };
    addAndMakeVisible (exportButton);

    refreshList();
    startTimerHz (15);
}

RecordingsPanel::~RecordingsPanel() = default;

//==============================================================================
void RecordingsPanel::refreshList()
{
    entries.clear();

    for (const auto& dir : settings::recordingsFolder().findChildFiles (juce::File::findDirectories, false))
    {
        if (dir.getFileName().endsWithIgnoreCase (".part"))
            continue;   // half-received transfer

        const auto metaFile = dir.getChildFile ("meta.json");
        if (! metaFile.existsAsFile())
            continue;

        const auto meta = juce::JSON::parse (metaFile.loadFileAsString());
        if (! meta.isObject())
            continue;

        Entry e;
        e.dir  = dir;
        e.song = meta.getProperty ("song", dir.getFileName()).toString();
        e.date = meta.getProperty ("date", juce::String()).toString();

        if (auto* tracks = meta.getProperty ("tracks", juce::var()).getArray())
            e.numTracks = tracks->size();

        const double rate = (double) meta.getProperty ("sampleRate", 44100.0);
        const auto   len  = (juce::int64) meta.getProperty ("lengthSamples", 0);
        e.lengthSeconds   = rate > 0.0 ? (double) len / rate : 0.0;

        entries.push_back (std::move (e));
    }

    // Newest first (the folder names embed the timestamp).
    std::sort (entries.begin(), entries.end(),
               [] (const Entry& a, const Entry& b) { return a.dir.getFileName() > b.dir.getFileName(); });

    list.updateContent();
    list.repaint();
    if (! entries.empty() && list.getSelectedRow() < 0)
        list.selectRow (0);
    updateButtons();
}

void RecordingsPanel::showStatus (const juce::String& text, bool isError)
{
    setStatus (text, isError ? style::bad() : style::textDim());
}

int RecordingsPanel::getNumRows() { return (int) entries.size(); }

void RecordingsPanel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    juce::ignoreUnused (height);
    if (! juce::isPositiveAndBelow (row, (int) entries.size()))
        return;

    const auto& e = entries[(size_t) row];

    if (selected)
        g.fillAll (style::accentDark().withAlpha (0.35f));

    g.setColour (style::textPrimary());
    g.setFont (style::normalFont());
    g.drawText (e.song, 10, 4, width - 16, 18, juce::Justification::centredLeft);

    g.setColour (style::textDim());
    g.setFont (juce::Font (juce::FontOptions (12.5f)));
    g.drawText (e.date + "   -   " + juce::String (e.numTracks) + " stems   -   " + formatClock (e.lengthSeconds),
                10, 23, width - 16, 15, juce::Justification::centredLeft);
}

void RecordingsPanel::selectedRowsChanged (int) { updateButtons(); }

void RecordingsPanel::listBoxItemDoubleClicked (int, const juce::MouseEvent&) { loadClicked(); }

//==============================================================================
std::vector<RecordingsPanel::TrackInfo> RecordingsPanel::parseTracks (const juce::var& meta)
{
    std::vector<TrackInfo> result;
    if (auto* tracks = meta.getProperty ("tracks", juce::var()).getArray())
    {
        for (const auto& t : *tracks)
        {
            TrackInfo info;
            info.fileName = t.getProperty ("file", juce::String()).toString();
            info.name     = t.getProperty ("name", juce::String()).toString();
            info.kind     = t.getProperty ("kind", "stem").toString();
            if (info.fileName.isNotEmpty())
                result.push_back (std::move (info));
        }
    }
    return result;
}

void RecordingsPanel::loadClicked()
{
    const int row = list.getSelectedRow();
    if (! juce::isPositiveAndBelow (row, (int) entries.size()))
        return;

    if (! options.player.canLoad())
    {
        setStatus ("A jam is running - stop it before loading a recording.", style::warn());
        return;
    }

    const auto entry = entries[(size_t) row];   // copy: async completion outlives the list
    const auto meta  = juce::JSON::parse (entry.dir.getChildFile ("meta.json").loadFileAsString());
    auto tracks      = parseTracks (meta);
    if (tracks.empty())
    {
        setStatus ("This recording has no stems.", style::bad());
        return;
    }

    const double targetRate = options.player.getDeviceSampleRate();
    if (targetRate <= 0.0)
    {
        setStatus ("No audio device active - check Settings > Audio.", style::bad());
        return;
    }

    std::vector<songloader::StemRequest> requests;
    for (const auto& t : tracks)
        requests.push_back ({ entry.dir.getChildFile (t.fileName), t.name, 0.0f, false });

    setStatus ("Loading \"" + entry.song + "\"...", style::textDim());
    loadButton.setEnabled (false);

    const auto id = "Recording: " + entry.song + " (" + entry.date + ")";
    const double metaRate = (double) meta.getProperty ("sampleRate", 44100.0);
    const auto   metaLen  = (juce::int64) meta.getProperty ("lengthSamples", 0);
    const int generation = ++loadGeneration;
    juce::Component::SafePointer<RecordingsPanel> safe (this);

    songloader::decodeAsync (std::move (requests), targetRate,
        [safe, generation, entry, tracks = std::move (tracks), id, metaRate, metaLen]
        (std::shared_ptr<songloader::Result> result) mutable
    {
        if (safe == nullptr || generation != safe->loadGeneration)
            return;

        juce::String error = result->ok ? juce::String() : result->error;
        if (error.isEmpty())
            safe->options.player.adopt (id, entry.song, metaRate, metaLen,
                                        std::move (result->stems), error);

        if (error.isEmpty())
        {
            safe->loadedDir    = entry.dir;
            safe->loadedName   = id;
            safe->loadedTracks = std::move (tracks);
            safe->gainsDb.assign (safe->loadedTracks.size(), 0.0f);
            safe->mutes.assign (safe->loadedTracks.size(), 0);

            style::styleSectionLabel (safe->loadedTitle, entry.song + "  (" + entry.date + ")");
            safe->posSlider.setRange (0.0, juce::jmax (0.1, safe->options.player.getLengthSeconds()), 0.01);
            safe->rebuildStrips();
            safe->setStatus ("Loaded - press Play, adjust the stems, then export.", style::good());
            if (safe->onPreviewChanged) safe->onPreviewChanged();
        }
        else
        {
            safe->setStatus ("Load failed: " + error, style::bad());
        }
        safe->updateButtons();
    });
}

void RecordingsPanel::deleteClicked()
{
    const int row = list.getSelectedRow();
    if (! juce::isPositiveAndBelow (row, (int) entries.size()))
        return;

    const auto entry = entries[(size_t) row];
    juce::Component::SafePointer<RecordingsPanel> safe (this);

    juce::NativeMessageBox::showOkCancelBox (juce::MessageBoxIconType::WarningIcon,
        "Delete recording",
        "Delete \"" + entry.song + " (" + entry.date + ")\" and all its stems?",
        this, juce::ModalCallbackFunction::create ([safe, entry] (int okPressed)
    {
        if (okPressed == 0 || safe == nullptr)
            return;

        if (safe->loadedDir == entry.dir)
        {
            safe->options.player.unload();
            safe->loadedDir = juce::File();
            safe->loadedName.clear();
            safe->loadedTracks.clear();
            safe->strips.clear (true);
            style::styleSectionLabel (safe->loadedTitle, "No recording loaded");
            if (safe->onPreviewChanged) safe->onPreviewChanged();
        }

        entry.dir.deleteRecursively();
        safe->refreshList();
    }));
}

//==============================================================================
void RecordingsPanel::rebuildStrips()
{
    strips.clear (true);

    for (int i = 0; i < (int) loadedTracks.size(); ++i)
    {
        const auto& t = loadedTracks[(size_t) i];
        auto* strip = new ChannelStrip (t.name);
        strip->setInfoText (kindLabel (t.kind));
        strip->setValues (gainsDb[(size_t) i], mutes[(size_t) i] != 0);
        strip->onGain = [this, i] (float db)
        {
            gainsDb[(size_t) i] = db;
            options.player.setStemGainDb (i, db);
        };
        strip->onMute = [this, i] (bool mute)
        {
            mutes[(size_t) i] = mute ? 1 : 0;
            options.player.setStemMute (i, mute);
        };
        stripsContainer.addAndMakeVisible (strip);
        strips.add (strip);
    }

    layoutStrips();
}

void RecordingsPanel::layoutStrips()
{
    const int stripHeight = 34;
    const int width = juce::jmax (100, stripsViewport.getWidth() - 14);
    stripsContainer.setSize (width, juce::jmax (1, (int) strips.size() * (stripHeight + 4)));

    int y = 0;
    for (auto* strip : strips)
    {
        strip->setBounds (0, y, width, stripHeight);
        y += stripHeight + 4;
    }
}

//==============================================================================
void RecordingsPanel::exportClicked()
{
    if (exporting || loadedDir == juce::File() || loadedTracks.empty())
        return;

    const auto meta = juce::JSON::parse (loadedDir.getChildFile ("meta.json").loadFileAsString());
    const double metaRate = (double) meta.getProperty ("sampleRate", 44100.0);
    const double exportRate = Mp3Writer::isSampleRateSupported (metaRate) ? metaRate : 44100.0;
    const int bitrate = juce::jmax (128, bitrateBox.getSelectedId());

    auto outFile = loadedDir.getChildFile (paths::safeFileName (
                        meta.getProperty ("song", "mix").toString()) + " mix.mp3")
                      .getNonexistentSibling();

    std::vector<songloader::StemRequest> requests;
    for (const auto& t : loadedTracks)
        requests.push_back ({ loadedDir.getChildFile (t.fileName), t.name, 0.0f, false });

    exporting = true;
    setStatus ("Exporting MP3...", style::textDim());
    updateButtons();

    // Snapshot the mix parameters now; the render must not race the sliders.
    auto gains = gainsDb;
    auto muted = mutes;
    const int generation = ++loadGeneration;   // a new Load cancels the status updates
    juce::Component::SafePointer<RecordingsPanel> safe (this);

    auto finish = [safe, generation] (bool ok, juce::String message)
    {
        juce::MessageManager::callAsync ([safe, generation, ok, message]
        {
            if (safe == nullptr)
                return;
            safe->exporting = false;
            if (generation == safe->loadGeneration)
                safe->setStatus (message, ok ? style::good() : style::bad());
            safe->updateButtons();
        });
    };

    // Decode fresh from disk (independent of the live player buffers),
    // then mix + encode on a worker thread.
    songloader::decodeAsync (std::move (requests), exportRate,
        [gains = std::move (gains), muted = std::move (muted),
         exportRate, bitrate, outFile, finish] (std::shared_ptr<songloader::Result> result)
    {
        if (! result->ok)
        {
            finish (false, "Export failed: " + result->error);
            return;
        }

        std::thread ([result, gains, muted, exportRate, bitrate, outFile, finish]
        {
            Mp3Writer writer;
            juce::String error;
            if (! writer.open (outFile, exportRate, 2, bitrate, error))
            {
                finish (false, "Export failed: " + error);
                return;
            }

            juce::int64 total = 0;
            for (const auto& stem : result->stems)
                total = juce::jmax (total, (juce::int64) stem.buffer.getNumSamples());

            constexpr int chunk = 4096;
            std::vector<float> inter ((size_t) chunk * 2);

            bool ok = total > 0;
            for (juce::int64 pos = 0; ok && pos < total; pos += chunk)
            {
                const int n = (int) juce::jmin ((juce::int64) chunk, total - pos);
                std::fill (inter.begin(), inter.begin() + (size_t) n * 2, 0.0f);

                for (size_t s = 0; s < result->stems.size(); ++s)
                {
                    if (s < muted.size() && muted[s] != 0)
                        continue;

                    const auto& buf = result->stems[s].buffer;
                    const auto  len = (juce::int64) buf.getNumSamples();
                    if (pos >= len)
                        continue;

                    const int m = (int) juce::jmin ((juce::int64) n, len - pos);
                    const float gain = juce::Decibels::decibelsToGain (s < gains.size() ? gains[s] : 0.0f);

                    const float* srcL = buf.getReadPointer (0, (int) pos);
                    const float* srcR = buf.getNumChannels() > 1 ? buf.getReadPointer (1, (int) pos) : srcL;

                    for (int j = 0; j < m; ++j)
                    {
                        inter[(size_t) j * 2]     += srcL[j] * gain;
                        inter[(size_t) j * 2 + 1] += srcR[j] * gain;
                    }
                }

                ok = writer.writeInterleaved (inter.data(), n);
            }

            ok = writer.finish() && ok;

            if (ok)
                finish (true, "Exported: " + outFile.getFullPathName());
            else
                finish (false, "Export failed while encoding.");
        }).detach();
    });
}

//==============================================================================
bool RecordingsPanel::loadedIsActive() const
{
    return loadedName.isNotEmpty() && options.player.isCurrent (loadedName);
}

void RecordingsPanel::updateButtons()
{
    const bool rowSelected = juce::isPositiveAndBelow (list.getSelectedRow(), (int) entries.size());
    const bool idle = options.player.canLoad();

    loadButton.setEnabled (rowSelected && idle);
    deleteButton.setEnabled (rowSelected);
    if (options.onSendRecording)
        sendButton.setEnabled (rowSelected);

    const bool active = loadedIsActive();
    playButton.setEnabled (active && idle);
    stopButton.setEnabled (active && idle);
    exportButton.setEnabled (loadedDir != juce::File() && ! loadedTracks.empty() && ! exporting);
    bitrateBox.setEnabled (! exporting);
}

void RecordingsPanel::setStatus (const juce::String& text, juce::Colour colour)
{
    statusLabel.setText (text, juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, colour);
}

void RecordingsPanel::timerCallback()
{
    const bool active  = loadedIsActive();
    const bool playing = active && options.player.isPlaying();

    playButton.setButtonText (playing ? "Pause" : "Play");

    if (active)
    {
        const double pos = options.player.getPositionSeconds();
        const double len = options.player.getLengthSeconds();
        if (! posSlider.isMouseButtonDown())
            posSlider.setValue (pos, juce::dontSendNotification);
        timeLabel.setText (formatClock (pos) + " / " + formatClock (len), juce::dontSendNotification);
    }

    for (int i = 0; i < strips.size(); ++i)
        strips.getUnchecked (i)->setLevel (playing ? options.player.getStemLevel (i) : 0.0f);

    updateButtons();
}

//==============================================================================
void RecordingsPanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().reduced (4);
    auto left = r.removeFromLeft (330);
    style::drawPanel (g, left.toFloat());
    r.removeFromLeft (8);
    style::drawPanel (g, r.toFloat());
}

void RecordingsPanel::resized()
{
    auto r = getLocalBounds().reduced (4);

    // -- left column: the list of recorded jams ---------------------------------
    auto left = r.removeFromLeft (330).reduced (10);
    listCaption.setBounds (left.removeFromTop (24));
    left.removeFromTop (6);

    if (options.showAutoReceiveToggle)
    {
        autoReceiveToggle.setBounds (left.removeFromBottom (24));
        left.removeFromBottom (6);
    }

    if (options.onSendRecording)
    {
        auto sendRow = left.removeFromBottom (28);
        sendButton.setBounds (sendRow.removeFromLeft (110));
        left.removeFromBottom (6);
    }

    auto listButtons = left.removeFromBottom (28);
    loadButton.setBounds (listButtons.removeFromLeft (80));
    listButtons.removeFromLeft (6);
    deleteButton.setBounds (listButtons.removeFromLeft (80));
    listButtons.removeFromLeft (6);
    refreshButton.setBounds (listButtons.removeFromLeft (80));
    listButtons.removeFromLeft (6);
    openFolderButton.setBounds (listButtons);
    left.removeFromBottom (8);
    list.setBounds (left);

    r.removeFromLeft (8);

    // -- right column: loaded recording ------------------------------------------
    auto right = r.reduced (10);
    loadedTitle.setBounds (right.removeFromTop (24));
    right.removeFromTop (2);
    statusLabel.setBounds (right.removeFromTop (22));
    right.removeFromTop (8);

    auto transport = right.removeFromTop (28);
    playButton.setBounds (transport.removeFromLeft (80));
    transport.removeFromLeft (6);
    stopButton.setBounds (transport.removeFromLeft (80));
    transport.removeFromLeft (10);
    timeLabel.setBounds (transport.removeFromRight (110));
    transport.removeFromRight (6);
    posSlider.setBounds (transport);
    right.removeFromTop (10);

    auto exportRow = right.removeFromBottom (28);
    exportCaption.setBounds (exportRow.removeFromLeft (250));
    exportRow.removeFromLeft (6);
    bitrateBox.setBounds (exportRow.removeFromLeft (110));
    exportRow.removeFromLeft (6);
    exportButton.setBounds (exportRow.removeFromLeft (110));
    right.removeFromBottom (8);

    stripsViewport.setBounds (right);
    layoutStrips();
}

} // namespace bandjam
