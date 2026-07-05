#pragma once

#include <juce_core/juce_core.h>

namespace bandjam
{
struct LocalStem
{
    juce::String id;
    juce::String name;
    juce::String fileName;
    juce::int64  fileSize { 0 };   ///< expected size (from the host's song list)
    float        gainDb   { 0.0f };
    bool         mute     { false };

    bool isDownloaded (const juce::File& songFolder) const
    {
        const auto f = songFolder.getChildFile (fileName);
        return fileName.isNotEmpty() && f.existsAsFile() && f.getSize() == fileSize;
    }
};

struct LocalSong
{
    juce::String id;
    juce::String name;
    double       sampleRate    { 44100.0 };
    juce::int64  lengthSamples { 0 };
    juce::Array<LocalStem> stems;
    juce::File   folder;

    int numDownloaded() const
    {
        int n = 0;
        for (const auto& s : stems)
            if (s.isDownloaded (folder)) ++n;
        return n;
    }
    bool isComplete() const { return ! stems.isEmpty() && numDownloaded() == stems.size(); }
};

/** The musician's local mirror of the host's library: one folder per song under
    appdata/downloads with the stem files, plus mix settings (gain/mute) that
    survive reconnects. Message-thread only. */
class DownloadStore
{
public:
    DownloadStore() { load(); }

    void load();
    const juce::Array<LocalSong>& getSongs() const noexcept { return songs; }
    const LocalSong* findSong (const juce::String& songId) const;

    /** Merges the host's songList JSON: creates/updates local manifests while
        keeping existing files and mix settings. Removes nothing. */
    void syncFromHostList (const juce::var& songListJson);

    /** Destination file for an incoming stem download. */
    juce::File stemDestFile (const juce::String& songId, const juce::String& stemId) const;

    void setStemMix (const juce::String& songId, const juce::String& stemId,
                     float gainDb, bool mute);

    /** Deletes the song's local files (folder incl. stems + manifest) and
        removes it from the list. Re-sync with the host's songList to get a
        fresh (file-less) entry back. */
    void deleteLocal (const juce::String& songId);

private:
    int  indexOf (const juce::String& songId) const;
    void saveManifest (const LocalSong& song) const;

    juce::Array<LocalSong> songs;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DownloadStore)
};

} // namespace bandjam
