#pragma once

#include <juce_core/juce_core.h>
#include "AppPaths.h"

namespace bandjam::settings
{
/** Tiny JSON settings store (message thread only). */
inline juce::var loadAll()
{
    return juce::JSON::parse (paths::settingsFile().loadFileAsString());
}

inline void saveAll (const juce::var& v)
{
    paths::settingsFile().replaceWithText (juce::JSON::toString (v));
}

inline juce::var get (const juce::String& key, const juce::var& fallback = {})
{
    const auto all = loadAll();
    if (auto* obj = all.getDynamicObject())
        if (obj->hasProperty (key))
            return obj->getProperty (key);
    return fallback;
}

inline void set (const juce::String& key, const juce::var& value)
{
    auto all = loadAll();
    auto* obj = all.getDynamicObject();
    if (obj == nullptr)
    {
        all = juce::var (new juce::DynamicObject());
        obj = all.getDynamicObject();
    }
    obj->setProperty (key, value);
    saveAll (all);
}

inline juce::File deviceStateFile()     { return paths::appDataRoot().getChildFile ("audio_device.xml"); }
inline juce::File hostDeviceStateFile() { return paths::appDataRoot().getChildFile ("host_audio_device.xml"); }

/** Where recordings are saved and where the Recordings window looks for
    them. Users can point this anywhere; falls back to the app-data default
    when unset or not creatable. */
inline juce::File recordingsFolder()
{
    const auto custom = get ("recordingsFolder", "").toString();
    if (custom.isNotEmpty())
    {
        juce::File folder (custom);
        if (folder.isDirectory() || folder.createDirectory())
            return folder;
    }
    return paths::recordingsRoot();
}

/** Expands the user's file-name pattern for a new recording folder.
    Tokens: {song}, {date} (YYYY-MM-DD), {time} (HH-MM-SS). */
inline juce::String makeRecordingFolderName (const juce::String& songName)
{
    auto pattern = get ("recordingNamePattern", "").toString();
    if (pattern.trim().isEmpty())
        pattern = "{song}_{date}_{time}";

    const auto now = juce::Time::getCurrentTime();
    pattern = pattern.replace ("{song}", songName)
                     .replace ("{date}", now.formatted ("%Y-%m-%d"))
                     .replace ("{time}", now.formatted ("%H-%M-%S"));
    return paths::safeFileName (pattern);
}

} // namespace bandjam::settings
