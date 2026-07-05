#include "DownloadStore.h"
#include "common/AppPaths.h"

namespace bandjam
{
void DownloadStore::load()
{
    songs.clear();

    for (const auto& entry : juce::RangedDirectoryIterator (paths::downloadsRoot(), false, "*", juce::File::findDirectories))
    {
        const auto manifest = entry.getFile().getChildFile ("song.json");
        if (! manifest.existsAsFile())
            continue;

        const auto v = juce::JSON::parse (manifest.loadFileAsString());

        LocalSong song;
        song.folder        = entry.getFile();
        song.id            = v.getProperty ("id", juce::String()).toString();
        song.name          = v.getProperty ("name", entry.getFile().getFileName()).toString();
        song.sampleRate    = (double) v.getProperty ("sampleRate", 44100.0);
        song.lengthSamples = (juce::int64) v.getProperty ("lengthSamples", 0);

        if (auto* stems = v.getProperty ("stems", juce::var()).getArray())
        {
            for (const auto& sv : *stems)
            {
                LocalStem stem;
                stem.id       = sv.getProperty ("id", juce::String()).toString();
                stem.name     = sv.getProperty ("name", "Stem").toString();
                stem.fileName = sv.getProperty ("fileName", juce::String()).toString();
                stem.fileSize = (juce::int64) sv.getProperty ("fileSize", 0);
                stem.gainDb   = (float) (double) sv.getProperty ("gainDb", 0.0);
                stem.mute     = (bool) sv.getProperty ("mute", false);
                if (stem.id.isNotEmpty())
                    song.stems.add (stem);
            }
        }

        if (song.id.isNotEmpty())
            songs.add (song);
    }
}

const LocalSong* DownloadStore::findSong (const juce::String& songId) const
{
    const int i = indexOf (songId);
    return i >= 0 ? &songs.getReference (i) : nullptr;
}

int DownloadStore::indexOf (const juce::String& songId) const
{
    for (int i = 0; i < songs.size(); ++i)
        if (songs.getReference (i).id == songId)
            return i;
    return -1;
}

void DownloadStore::saveManifest (const LocalSong& song) const
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
        so->setProperty ("gainDb",   (double) stem.gainDb);
        so->setProperty ("mute",     stem.mute);
        stemArr.add (juce::var (so));
    }
    obj->setProperty ("stems", stemArr);

    song.folder.createDirectory();
    song.folder.getChildFile ("song.json")
        .replaceWithText (juce::JSON::toString (juce::var (obj)));
}

void DownloadStore::syncFromHostList (const juce::var& songListJson)
{
    auto* arr = songListJson.getArray();
    if (arr == nullptr)
        return;

    for (const auto& v : *arr)
    {
        const auto songId = v.getProperty ("id", juce::String()).toString();
        if (songId.isEmpty())
            continue;

        const int existing = indexOf (songId);
        LocalSong song = existing >= 0 ? songs.getReference (existing) : LocalSong();

        song.id            = songId;
        song.name          = v.getProperty ("name", "Song").toString();
        song.sampleRate    = (double) v.getProperty ("sampleRate", 44100.0);
        song.lengthSamples = (juce::int64) v.getProperty ("lengthSamples", 0);
        if (song.folder == juce::File())
            song.folder = paths::downloadsRoot().getChildFile (
                              paths::safeFileName (song.name) + "_" + songId.retainCharacters ("0123456789abcdefABCDEF").substring (0, 8));

        juce::Array<LocalStem> merged;
        if (auto* stems = v.getProperty ("stems", juce::var()).getArray())
        {
            for (const auto& sv : *stems)
            {
                LocalStem stem;
                stem.id       = sv.getProperty ("id", juce::String()).toString();
                stem.name     = sv.getProperty ("name", "Stem").toString();
                stem.fileName = paths::safeFileName (sv.getProperty ("fileName", stem.name + ".wav").toString());
                stem.fileSize = (juce::int64) sv.getProperty ("fileSize", 0);

                // Keep the musician's mix settings across syncs.
                for (const auto& old : song.stems)
                    if (old.id == stem.id)
                    {
                        stem.gainDb = old.gainDb;
                        stem.mute   = old.mute;
                        break;
                    }

                if (stem.id.isNotEmpty())
                    merged.add (stem);
            }
        }
        song.stems = std::move (merged);

        saveManifest (song);
        if (existing >= 0)
            songs.getReference (existing) = song;
        else
            songs.add (song);
    }
}

juce::File DownloadStore::stemDestFile (const juce::String& songId, const juce::String& stemId) const
{
    if (const auto* song = findSong (songId))
        for (const auto& stem : song->stems)
            if (stem.id == stemId)
                return song->folder.getChildFile (stem.fileName);
    return {};
}

void DownloadStore::setStemMix (const juce::String& songId, const juce::String& stemId,
                                float gainDb, bool mute)
{
    const int i = indexOf (songId);
    if (i < 0)
        return;

    auto& song = songs.getReference (i);
    for (auto& stem : song.stems)
    {
        if (stem.id == stemId)
        {
            stem.gainDb = gainDb;
            stem.mute   = mute;
            saveManifest (song);
            return;
        }
    }
}

void DownloadStore::deleteLocal (const juce::String& songId)
{
    const int i = indexOf (songId);
    if (i < 0)
        return;

    songs.getReference (i).folder.deleteRecursively();
    songs.remove (i);
}

} // namespace bandjam
