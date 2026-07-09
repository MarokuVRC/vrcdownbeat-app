#pragma once

#include <juce_core/juce_core.h>

namespace bandjam::paths
{
/** %APPDATA%/BandJam - all persistent app data lives below this. */
inline juce::File appDataRoot()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("BandJam");
    dir.createDirectory();
    return dir;
}

inline juce::File libraryRoot()    { auto d = appDataRoot().getChildFile ("library");    d.createDirectory(); return d; }
inline juce::File downloadsRoot()  { auto d = appDataRoot().getChildFile ("downloads");  d.createDirectory(); return d; }
inline juce::File recordingsRoot() { auto d = appDataRoot().getChildFile ("recordings"); d.createDirectory(); return d; }
inline juce::File localSongsRoot() { auto d = appDataRoot().getChildFile ("mysongs");    d.createDirectory(); return d; }
inline juce::File incomingRoot()   { auto d = appDataRoot().getChildFile ("incoming");   d.createDirectory(); return d; }
inline juce::File settingsFile()  { return appDataRoot().getChildFile ("settings.json"); }

/** Keeps only characters that are safe in file names. */
inline juce::String safeFileName (const juce::String& s)
{
    auto cleaned = juce::File::createLegalFileName (s.trim());
    return cleaned.isEmpty() ? "unnamed" : cleaned;
}

} // namespace bandjam::paths
