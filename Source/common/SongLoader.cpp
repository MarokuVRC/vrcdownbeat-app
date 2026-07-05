#include "SongLoader.h"
#include <juce_events/juce_events.h>
#include <atomic>
#include <cmath>
#include <mutex>

namespace bandjam::songloader
{
namespace
{
    juce::AudioFormatManager& sharedFormatManager()
    {
        static juce::AudioFormatManager manager;
        static const bool initialised = [] { manager.registerBasicFormats(); return true; }();
        juce::ignoreUnused (initialised);
        return manager;
    }

    juce::ThreadPool& loaderPool()
    {
        static juce::ThreadPool pool (juce::jmax (2, juce::SystemStats::getNumCpus() - 1));
        return pool;
    }

    struct SharedState
    {
        std::vector<StemRequest> requests;
        std::shared_ptr<Result>  result;
        std::function<void (std::shared_ptr<Result>)> onDone;
        std::mutex mutex;
        std::atomic<int> remaining { 0 };
    };

    void decodeOne (SharedState& state, int index)
    {
        const auto& request = state.requests[(size_t) index];
        const double targetRate = state.result->targetRate;

        DecodedStem out;
        out.name   = request.name;
        out.gainDb = request.gainDb;
        out.mute   = request.mute;

        juce::String error;
        std::unique_ptr<juce::AudioFormatReader> reader (sharedFormatManager().createReaderFor (request.file));

        if (reader == nullptr || reader->lengthInSamples <= 0)
        {
            error = "Can't read stem: " + request.name;
        }
        else
        {
            const int srcLen = (int) reader->lengthInSamples;
            const int numCh  = juce::jmin (2, (int) reader->numChannels);

            juce::AudioBuffer<float> src (numCh, srcLen);
            reader->read (&src, 0, srcLen, 0, true, numCh > 1);

            if (juce::approximatelyEqual (reader->sampleRate, targetRate))
            {
                out.buffer = std::move (src);
            }
            else
            {
                const double ratio  = reader->sampleRate / targetRate;
                const int    outLen = (int) std::floor (srcLen / ratio);
                out.buffer.setSize (numCh, outLen);
                for (int ch = 0; ch < numCh; ++ch)
                {
                    juce::LagrangeInterpolator interpolator;
                    interpolator.process (ratio, src.getReadPointer (ch), out.buffer.getWritePointer (ch), outLen);
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock (state.mutex);
            if (error.isNotEmpty())
            {
                if (state.result->error.isEmpty())
                    state.result->error = error;
            }
            else
            {
                state.result->stems[(size_t) index] = std::move (out);
            }
        }

        if (state.remaining.fetch_sub (1) == 1)
        {
            // Last stem done - finalise and hand over on the message thread.
            auto result = state.result;
            result->ok = result->error.isEmpty();
            for (const auto& stem : result->stems)
                result->maxLengthSamples = juce::jmax (result->maxLengthSamples,
                                                       (juce::int64) stem.buffer.getNumSamples());
            if (! result->ok)
                result->stems.clear();

            juce::MessageManager::callAsync ([onDone = state.onDone, result] { onDone (result); });
        }
    }
}

void decodeAsync (std::vector<StemRequest> stems, double targetRate,
                  std::function<void (std::shared_ptr<Result>)> onDone)
{
    auto result = std::make_shared<Result>();
    result->targetRate = targetRate;

    if (stems.empty() || targetRate <= 0.0)
    {
        result->error = stems.empty() ? "The song has no stems." : "No audio device active.";
        juce::MessageManager::callAsync ([onDone, result] { onDone (result); });
        return;
    }

    auto state = std::make_shared<SharedState>();
    state->requests = std::move (stems);
    state->result   = result;
    state->onDone   = std::move (onDone);
    state->result->stems.resize (state->requests.size());
    state->remaining.store ((int) state->requests.size());

    for (int i = 0; i < (int) state->requests.size(); ++i)
        loaderPool().addJob ([state, i] { decodeOne (*state, i); });
}

} // namespace bandjam::songloader
