#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace bandjam
{
/** Converts a continuous mono stream from one sample rate to another,
    carrying leftover input between calls. Allocation-free after prepare(),
    so it is safe on the audio thread. Pass-through when the rates match. */
class StreamingResampler
{
public:
    void prepare (double sourceRate, double targetRate, int maxChunkSamples)
    {
        ratio       = sourceRate / targetRate; // input samples per output sample
        passThrough = juce::approximatelyEqual (sourceRate, targetRate);
        capacity    = juce::jmax (1024, maxChunkSamples * 4 + 1024);
        buffer.allocate ((size_t) capacity, true);
        reset();
    }

    void reset()
    {
        interpolator.reset();
        size = 0;
    }

    bool isPassThrough() const noexcept { return passThrough; }

    /** Appends input samples; returns how many were stored (drops on overflow). */
    int pushInput (const float* input, int numSamples)
    {
        const int n = juce::jmin (numSamples, capacity - size);
        if (n > 0)
        {
            std::memcpy (buffer.getData() + size, input, (size_t) n * sizeof (float));
            size += n;
        }
        return n;
    }

    int availableOutput() const noexcept
    {
        if (passThrough)
            return size;
        return (int) std::floor ((double) juce::jmax (0, size - 8) / ratio);
    }

    /** Writes up to @p maxOut converted samples into @p output, returns count. */
    int pullOutput (float* output, int maxOut)
    {
        const int n = juce::jmin (maxOut, availableOutput());
        if (n <= 0)
            return 0;

        if (passThrough)
        {
            std::memcpy (output, buffer.getData(), (size_t) n * sizeof (float));
            consume (n);
            return n;
        }

        const int used = interpolator.process (ratio, buffer.getData(), output, n);
        consume (juce::jlimit (0, size, used));
        return n;
    }

private:
    void consume (int n)
    {
        if (n <= 0) return;
        std::memmove (buffer.getData(), buffer.getData() + n, (size_t) (size - n) * sizeof (float));
        size -= n;
    }

    juce::LagrangeInterpolator interpolator;
    juce::HeapBlock<float> buffer;
    int    capacity { 0 };
    int    size     { 0 };
    double ratio    { 1.0 };
    bool   passThrough { true };
};

} // namespace bandjam
