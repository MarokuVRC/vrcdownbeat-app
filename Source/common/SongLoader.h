#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <functional>
#include <memory>
#include <vector>

namespace bandjam::songloader
{
struct StemRequest
{
    juce::File   file;
    juce::String name;
    float        gainDb { 0.0f };
    bool         mute   { false };
};

struct DecodedStem
{
    juce::String name;
    float        gainDb { 0.0f };
    bool         mute   { false };
    juce::AudioBuffer<float> buffer;   ///< 1-2 channels, at the requested target rate
};

struct Result
{
    bool         ok { false };
    juce::String error;
    std::vector<DecodedStem> stems;    ///< same order as the requests
    juce::int64  maxLengthSamples { 0 };
    double       targetRate { 0.0 };
};

/** Decodes (and, if needed, resamples) all stems in parallel - one thread-pool
    job per stem - and delivers the result on the message thread. This keeps
    the UI responsive and cuts load times roughly by the core count, which
    fixes the multi-second "freeze" the old blocking decode caused.

    The jobs only touch the shared result (kept alive by shared_ptr), never
    the caller, so the caller may be destroyed while a load is in flight -
    guard the callback with a SafePointer / generation counter. */
void decodeAsync (std::vector<StemRequest> stems, double targetRate,
                  std::function<void (std::shared_ptr<Result>)> onDone);

} // namespace bandjam::songloader
