#include "SongLibrary.h"
#include "common/AppPaths.h"

namespace bandjam
{
SongLibrary::SongLibrary()
{
    formatManager.registerBasicFormats();
    load();
}

const LibrarySong* SongLibrary::findSong (const juce::String& songId) const
{
    for (const auto& s : songs)
        if (s.id == songId)
            return &s;
    return nullptr;
}

void SongLibrary::load()
{
    songs.clear();

    for (const auto& entry : juce::RangedDirectoryIterator (paths::libraryRoot(), false, "*", juce::File::findDirectories))
    {
        const auto manifest = entry.getFile().getChildFile ("song.json");
        if (! manifest.existsAsFile())
            continue;

        const auto v = juce::JSON::parse (manifest.loadFileAsString());

        LibrarySong song;
        song.folder        = entry.getFile();
        song.id            = v.getProperty ("id", juce::String()).toString();
        song.name          = v.getProperty ("name", entry.getFile().getFileName()).toString();
        song.sampleRate    = (double) v.getProperty ("sampleRate", 44100.0);
        song.lengthSamples = (juce::int64) v.getProperty ("lengthSamples", 0);

        if (auto* stems = v.getProperty ("stems", juce::var()).getArray())
        {
            for (const auto& sv : *stems)
            {
                LibraryStem stem;
                stem.id            = sv.getProperty ("id", juce::String()).toString();
                stem.name          = sv.getProperty ("name", "Stem").toString();
                stem.fileName      = sv.getProperty ("fileName", juce::String()).toString();
                stem.fileSize      = (juce::int64) sv.getProperty ("fileSize", 0);
                stem.sampleRate    = (double) sv.getProperty ("sampleRate", song.sampleRate);
                stem.lengthSamples = (juce::int64) sv.getProperty ("lengthSamples", 0);

                if (stem.id.isNotEmpty() && song.folder.getChildFile (stem.fileName).existsAsFile())
                    song.stems.add (stem);
            }
        }

        if (song.id.isNotEmpty() && ! song.stems.isEmpty())
            songs.add (song);
    }
}

void SongLibrary::saveManifest (const LibrarySong& song) const
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("id",            song.id);
    obj->setProperty ("name",          song.name);
    obj->setProperty ("sampleRate",    song.sampleRate);
    obj->setProperty ("lengthSamples", song.lengthSamples);

    juce::Array<juce::var> stemArr;
    for (const auto& stem : song.stems)
    {
        auto* so = new juce::DynamicObject();
        so->setProperty ("id",            stem.id);
        so->setProperty ("name",          stem.name);
        so->setProperty ("fileName",      stem.fileName);
        so->setProperty ("fileSize",      stem.fileSize);
        so->setProperty ("sampleRate",    stem.sampleRate);
        so->setProperty ("lengthSamples", stem.lengthSamples);
        stemArr.add (juce::var (so));
    }
    obj->setProperty ("stems", stemArr);

    song.folder.getChildFile ("song.json")
        .replaceWithText (juce::JSON::toString (juce::var (obj)));
}

juce::String SongLibrary::addSong (const juce::String& name,
                                   const juce::Array<juce::File>& stemFiles,
                                   juce::String& error)
{
    if (stemFiles.isEmpty())
    {
        error = "No files selected.";
        return {};
    }

    LibrarySong song;
    song.id     = juce::Uuid().toString();
    song.name   = name.trim().isEmpty() ? stemFiles[0].getFileNameWithoutExtension() : name.trim();
    song.folder = paths::libraryRoot().getChildFile (paths::safeFileName (song.name) + "_" + song.id.substring (0, 8));

    if (! song.folder.createDirectory())
    {
        error = "Could not create the song folder.";
        return {};
    }

    for (const auto& file : stemFiles)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
        if (reader == nullptr || reader->lengthInSamples <= 0)
        {
            error = "Not a readable audio file: " + file.getFileName();
            song.folder.deleteRecursively();
            return {};
        }

        LibraryStem stem;
        stem.id            = juce::Uuid().toString();
        stem.name          = file.getFileNameWithoutExtension();
        stem.fileName      = paths::safeFileName (file.getFileName());
        stem.sampleRate    = reader->sampleRate;
        stem.lengthSamples = reader->lengthInSamples;

        // First stem defines the song's timebase.
        if (song.stems.isEmpty())
            song.sampleRate = reader->sampleRate;

        const auto dest = song.folder.getChildFile (stem.fileName);
        if (dest.existsAsFile() || ! file.copyFileTo (dest))
        {
            error = "Could not copy the file: " + file.getFileName();
            song.folder.deleteRecursively();
            return {};
        }
        stem.fileSize = dest.getSize();

        // Song length in the song's timebase (stems may have differing rates).
        const auto scaled = (juce::int64) std::llround ((double) stem.lengthSamples
                                                        * song.sampleRate / stem.sampleRate);
        song.lengthSamples = juce::jmax (song.lengthSamples, scaled);

        song.stems.add (stem);
    }

    saveManifest (song);
    songs.add (song);

    if (onChanged) onChanged();
    return song.id;
}

bool SongLibrary::removeSong (const juce::String& songId)
{
    for (int i = 0; i < songs.size(); ++i)
    {
        if (songs.getReference (i).id == songId)
        {
            songs.getReference (i).folder.deleteRecursively();
            songs.remove (i);
            if (onChanged) onChanged();
            return true;
        }
    }
    return false;
}

juce::var SongLibrary::toJson() const
{
    juce::Array<juce::var> arr;
    for (const auto& song : songs)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("id",            song.id);
        obj->setProperty ("name",          song.name);
        obj->setProperty ("sampleRate",    song.sampleRate);
        obj->setProperty ("lengthSamples", song.lengthSamples);

        juce::Array<juce::var> stemArr;
        for (const auto& stem : song.stems)
        {
            auto* so = new juce::DynamicObject();
            so->setProperty ("id",       stem.id);
            so->setProperty ("name",     stem.name);
            so->setProperty ("fileName", stem.fileName);
            so->setProperty ("fileSize", stem.fileSize);
            stemArr.add (juce::var (so));
        }
        obj->setProperty ("stems", stemArr);
        arr.add (juce::var (obj));
    }
    return juce::var (arr);
}

juce::File SongLibrary::getStemFile (const juce::String& songId, const juce::String& stemId) const
{
    if (const auto* song = findSong (songId))
        for (const auto& stem : song->stems)
            if (stem.id == stemId)
                return song->folder.getChildFile (stem.fileName);
    return {};
}

} // namespace bandjam
