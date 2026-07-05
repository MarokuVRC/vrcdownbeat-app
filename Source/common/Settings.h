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

} // namespace bandjam::settings
