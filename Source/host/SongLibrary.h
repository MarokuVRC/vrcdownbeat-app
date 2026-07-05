#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace bandjam
{
struct LibraryStem
{
    juce::String id;
    juce::String name;          ///< display name (derived from the file name)
    juce::String fileName;      ///< file inside the song folder
    juce::int64  fileSize      { 0 };
    double       sampleRate    { 0.0 };
    juce::int64  lengthSamples { 0 };
};

struct LibrarySong
{
    juce::String id;
    juce::String name;
    double       sampleRate    { 44100.0 };  ///< the jam's timebase (first stem's rate)
    juce::int64  lengthSamples { 0 };        ///< longest stem, in song sample rate
    juce::Array<LibraryStem> stems;
    juce::File   folder;
};

/** The host's song library: each song is a folder under appdata/library with
    the stem audio files and a song.json manifest. Message-thread only. */
class SongLibrary
{
public:
    SongLibrary();

    const juce::Array<LibrarySong>& getSongs() const noexcept { return songs; }
    const LibrarySong* findSong (const juce::String& songId) const;

    /** Copies the given audio files into a new song folder, reading sample
        rate / length from each. Returns the new song id ("" + error on failure). */
    juce::String addSong (const juce::String& name,
                          const juce::Array<juce::File>& stemFiles,
                          juce::String& error);

    bool removeSong (const juce::String& songId);

    /** The client-facing JSON for the songList message. */
    juce::var toJson() const;

    /** Resolves a stem's audio file for transfer ({} if unknown). */
    juce::File getStemFile (const juce::String& songId, const juce::String& stemId) const;

    /** Fired after any change (message thread). */
    std::function<void()> onChanged;

private:
    void load();
    void saveManifest (const LibrarySong& song) const;

    juce::AudioFormatManager formatManager;
    juce::Array<LibrarySong> songs;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SongLibrary)
};

} // namespace bandjam
