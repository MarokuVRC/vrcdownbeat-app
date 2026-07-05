#pragma once

#include <juce_core/juce_core.h>
#include <memory>

namespace bandjam
{
/** Streams PCM audio into an .mp3 file using the MP3 encoder built into
    Windows (Media Foundation, Windows 8+). No external tools (LAME etc.)
    are needed.

    Usage (any single thread):
        Mp3Writer w;
        if (w.open (file, 44100.0, 2, 192, error))
        {
            w.writeInterleaved (samples, numFrames);   // repeat
            w.finish();
        }

    The input must be interleaved float (-1..1) at a rate the Windows MP3
    encoder accepts: 32000, 44100 or 48000 Hz. */
class Mp3Writer
{
public:
    Mp3Writer();
    ~Mp3Writer();

    bool open (const juce::File& file, double sampleRate, int numChannels,
               int bitrateKbps, juce::String& error);

    bool writeInterleaved (const float* interleaved, int numFrames);

    /** Finalises the file (writes headers). Returns false on failure. */
    bool finish();

    static bool isSampleRateSupported (double rate) noexcept
    {
        return rate == 32000.0 || rate == 44100.0 || rate == 48000.0;
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Mp3Writer)
};

} // namespace bandjam
