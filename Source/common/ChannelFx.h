#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <vector>
#include <memory>

namespace bandjam
{
//==============================================================================
/** The catalog of available effect types: ids, display names and parameter
    descriptors. The FX editor builds its sliders from this, and the DSP
    chain reads parameters by id - adding a new effect means adding a
    descriptor here plus a Node in ChannelFx. */
namespace fx
{
    struct ParamDesc
    {
        const char* id;
        const char* label;
        float minValue, maxValue, defaultValue;
        const char* suffix;
        float skewMidPoint;   ///< 0 = linear
    };

    struct EffectDesc
    {
        const char* type;     ///< stable id used in JSON
        const char* name;     ///< shown in the UI
        std::vector<ParamDesc> params;
    };

    inline const std::vector<EffectDesc>& catalog()
    {
        static const std::vector<EffectDesc> list =
        {
            { "lowcut",  "Low Cut",
              { { "freq", "Frequency", 20.0f, 1000.0f, 80.0f, " Hz", 200.0f } } },

            { "highcut", "High Cut",
              { { "freq", "Frequency", 1000.0f, 20000.0f, 12000.0f, " Hz", 6000.0f } } },

            { "eq",      "Equalizer",
              { { "low",  "Low",  -12.0f, 12.0f, 0.0f, " dB", 0.0f },
                { "mid",  "Mid",  -12.0f, 12.0f, 0.0f, " dB", 0.0f },
                { "high", "High", -12.0f, 12.0f, 0.0f, " dB", 0.0f } } },

            { "gate",    "Noise Gate",
              { { "threshold", "Threshold", -80.0f, 0.0f, -50.0f, " dB", 0.0f },
                { "ratio",     "Ratio",       1.0f, 20.0f, 10.0f, ":1",   0.0f },
                { "attack",    "Attack",      0.1f, 100.0f, 1.0f, " ms",  10.0f },
                { "release",   "Release",    10.0f, 1000.0f, 100.0f, " ms", 200.0f } } },

            { "comp",    "Compressor",
              { { "threshold", "Threshold", -60.0f, 0.0f, -18.0f, " dB", 0.0f },
                { "ratio",     "Ratio",       1.0f, 20.0f,  3.0f, ":1",   0.0f },
                { "attack",    "Attack",      0.1f, 200.0f, 10.0f, " ms", 20.0f },
                { "release",   "Release",    10.0f, 1000.0f, 150.0f, " ms", 200.0f },
                { "makeup",    "Makeup",      0.0f, 24.0f,  0.0f, " dB",  0.0f } } },

            { "dist",    "Distortion",
              { { "drive", "Drive",  0.0f, 40.0f, 12.0f, " dB", 0.0f },
                { "level", "Level", -24.0f, 6.0f,  0.0f, " dB", 0.0f } } },

            { "chorus",  "Chorus",
              { { "rate",     "Rate",     0.05f, 5.0f,  1.0f,  " Hz", 1.0f },
                { "depth",    "Depth",    0.0f,  1.0f,  0.25f, "",    0.0f },
                { "delay",    "Delay",    1.0f,  60.0f, 7.0f,  " ms", 15.0f },
                { "feedback", "Feedback", -0.95f, 0.95f, 0.0f, "",    0.0f },
                { "mix",      "Mix",      0.0f,  1.0f,  0.5f,  "",    0.0f } } },

            { "phaser",  "Phaser",
              { { "rate",     "Rate",     0.05f, 5.0f,   0.5f,   " Hz", 1.0f },
                { "depth",    "Depth",    0.0f,  1.0f,   0.5f,   "",    0.0f },
                { "freq",     "Centre",   100.0f, 5000.0f, 1000.0f, " Hz", 800.0f },
                { "feedback", "Feedback", -0.95f, 0.95f, 0.0f,   "",    0.0f },
                { "mix",      "Mix",      0.0f,  1.0f,   0.5f,   "",    0.0f } } },

            { "delay",   "Delay / Echo",
              { { "time",     "Time",     10.0f, 2000.0f, 350.0f, " ms", 400.0f },
                { "feedback", "Feedback", 0.0f,  0.9f,    0.35f,  "",    0.0f },
                { "mix",      "Mix",      0.0f,  1.0f,    0.3f,   "",    0.0f } } },

            { "reverb",  "Reverb",
              { { "room",    "Room",    0.0f, 1.0f, 0.55f, "", 0.0f },
                { "damping", "Damping", 0.0f, 1.0f, 0.45f, "", 0.0f },
                { "width",   "Width",   0.0f, 1.0f, 1.0f,  "", 0.0f },
                { "mix",     "Mix",     0.0f, 1.0f, 0.3f,  "", 0.0f } } },

            { "limiter", "Limiter",
              { { "ceiling", "Ceiling", -12.0f, 0.0f, -1.0f, " dB", 0.0f },
                { "release", "Release", 1.0f, 500.0f, 100.0f, " ms", 100.0f } } },
        };
        return list;
    }

    inline const EffectDesc* find (const juce::String& type)
    {
        for (const auto& d : catalog())
            if (type == d.type)
                return &d;
        return nullptr;
    }
} // namespace fx

//==============================================================================
/** One effect instance in a channel's chain: type + parameter values. */
struct FxEffect
{
    juce::String type;
    std::vector<std::pair<juce::String, float>> params;

    float get (const juce::String& id) const
    {
        for (const auto& p : params)
            if (p.first == id)
                return p.second;
        if (const auto* d = fx::find (type))
            for (const auto& pd : d->params)
                if (id == pd.id)
                    return pd.defaultValue;
        return 0.0f;
    }

    void set (const juce::String& id, float value)
    {
        for (auto& p : params)
            if (p.first == id) { p.second = value; return; }
        params.emplace_back (id, value);
    }

    static FxEffect makeDefault (const juce::String& type)
    {
        FxEffect e;
        e.type = type;
        if (const auto* d = fx::find (type))
            for (const auto& pd : d->params)
                e.params.emplace_back (pd.id, pd.defaultValue);
        return e;
    }
};

/** A channel's whole FX chain (ordered). Plain values, JSON-friendly.

    The audio is ALWAYS stored/recorded raw - these settings are applied
    live at mix time (and again when playing/exporting a recording), so
    they can be changed non-destructively at any point. */
struct FxSettings
{
    std::vector<FxEffect> effects;   ///< processing order = list order

    bool isActive() const noexcept { return ! effects.empty(); }

    juce::var toVar() const
    {
        juce::Array<juce::var> arr;
        for (const auto& e : effects)
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("type", e.type);
            for (const auto& p : e.params)
                obj->setProperty (juce::Identifier (p.first), p.second);
            arr.add (juce::var (obj));
        }
        return juce::var (arr);
    }

    static FxSettings fromVar (const juce::var& v)
    {
        FxSettings s;

        if (auto* arr = v.getArray())
        {
            for (const auto& item : *arr)
            {
                const auto type = item.getProperty ("type", juce::String()).toString();
                const auto* desc = fx::find (type);
                if (desc == nullptr)
                    continue;

                FxEffect e;
                e.type = type;
                for (const auto& pd : desc->params)
                    e.params.emplace_back (pd.id,
                        (float) (double) item.getProperty (juce::Identifier (pd.id), pd.defaultValue));
                s.effects.push_back (std::move (e));
            }
            return s;
        }

        // Legacy format from the first FX version: { low, mid, high, reverb }.
        if (v.isObject())
        {
            const float low  = (float) (double) v.getProperty ("low", 0.0);
            const float mid  = (float) (double) v.getProperty ("mid", 0.0);
            const float high = (float) (double) v.getProperty ("high", 0.0);
            const float rev  = (float) (double) v.getProperty ("reverb", 0.0);

            if (std::abs (low) > 0.05f || std::abs (mid) > 0.05f || std::abs (high) > 0.05f)
            {
                auto e = FxEffect::makeDefault ("eq");
                e.set ("low", low); e.set ("mid", mid); e.set ("high", high);
                s.effects.push_back (std::move (e));
            }
            if (rev > 0.005f)
            {
                auto e = FxEffect::makeDefault ("reverb");
                e.set ("mix", rev);
                s.effects.push_back (std::move (e));
            }
        }
        return s;
    }
};

//==============================================================================
/** A channel's insert chain, built from FxSettings. Real-time behaviour:

    - The audio thread try-locks a spin lock around the chain; the message
      thread only holds that lock for a swap or a handful of parameter
      setters, so in the worst case the FX are bypassed for one block.
    - process*() never allocates. Chain rebuilds (add/remove/reorder)
      construct the new nodes on the message thread before swapping them in;
      parameter tweaks update the existing nodes in place, keeping their
      state (no clicks while dragging a slider).
    - prepare() must know the channel count: performer streams are mono,
      stems are stereo. */
class ChannelFx
{
public:
    void prepare (double sampleRate, int maxBlockSize, int numChannels = 2)
    {
        rate     = sampleRate > 0.0 ? sampleRate : 44100.0;
        maxBlock = juce::jmax (256, maxBlockSize);
        channels = juce::jlimit (1, 2, numChannels);

        auto fresh = buildChain (current);
        const juce::SpinLock::ScopedLockType sl (lock);
        chain.swap (fresh);
    }   // old nodes destroyed here, outside the lock

    /** Message thread (audio may be running). */
    void setSettings (const FxSettings& s)
    {
        bool sameStructure = s.effects.size() == current.effects.size();
        if (sameStructure)
            for (size_t i = 0; i < s.effects.size(); ++i)
                if (s.effects[i].type != current.effects[i].type)
                    { sameStructure = false; break; }

        if (sameStructure)
        {
            const juce::SpinLock::ScopedLockType sl (lock);
            for (size_t i = 0; i < chain.size() && i < s.effects.size(); ++i)
                chain[i]->update (s.effects[i]);
        }
        else
        {
            auto fresh = buildChain (s);
            {
                const juce::SpinLock::ScopedLockType sl (lock);
                chain.swap (fresh);
            }
        }

        current = s;
        active.store (! s.effects.empty());
    }

    FxSettings getSettings() const { return current; }

    /** Audio thread: skip the whole insert when the chain is empty. */
    bool isBypassed() const noexcept { return ! active.load(); }

    // -- audio thread ---------------------------------------------------------------
    void processMono (float* samples, int numSamples)
    {
        float* chans[1] = { samples };
        processChannels (chans, 1, numSamples);
    }

    void processStereo (float* left, float* right, int numSamples)
    {
        float* chans[2] = { left, right };
        processChannels (chans, 2, numSamples);
    }

private:
    //==============================================================================
    struct Node
    {
        virtual ~Node() = default;
        virtual void prepare (double rate, int maxBlock, int channels) = 0;
        virtual void update (const FxEffect& e) = 0;
        virtual void process (juce::dsp::AudioBlock<float>& block) = 0;
    };

    /** IIR-based: low cut, high cut, 3-band EQ (up to 3 bands x 2 channels). */
    struct FilterNode : Node
    {
        explicit FilterNode (const juce::String& typeToUse) : type (typeToUse) {}

        void prepare (double r, int, int) override
        {
            sampleRate = r;
            for (auto& chFilters : filters)
                for (auto& f : chFilters)
                    f.reset();
            update (lastEffect);
        }

        void update (const FxEffect& e) override
        {
            lastEffect = e;
            if (sampleRate <= 0.0)
                return;

            if (type == "lowcut")
            {
                const auto c = juce::IIRCoefficients::makeHighPass (sampleRate,
                                    juce::jlimit (10.0f, 2000.0f, e.get ("freq")));
                setBand (0, c);
                numBands = 1;
            }
            else if (type == "highcut")
            {
                const auto c = juce::IIRCoefficients::makeLowPass (sampleRate,
                                    juce::jlimit (200.0f, 20000.0f, e.get ("freq")));
                setBand (0, c);
                numBands = 1;
            }
            else // eq
            {
                setBand (0, juce::IIRCoefficients::makeLowShelf (sampleRate, 200.0, 0.707,
                             juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 24.0f, e.get ("low")))));
                setBand (1, juce::IIRCoefficients::makePeakFilter (sampleRate, 1000.0, 0.707,
                             juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 24.0f, e.get ("mid")))));
                setBand (2, juce::IIRCoefficients::makeHighShelf (sampleRate, 4000.0, 0.707,
                             juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 24.0f, e.get ("high")))));
                numBands = 3;
            }
        }

        void process (juce::dsp::AudioBlock<float>& block) override
        {
            for (size_t ch = 0; ch < juce::jmin ((size_t) 2, block.getNumChannels()); ++ch)
                for (int b = 0; b < numBands; ++b)
                    filters[ch][(size_t) b].processSamples (block.getChannelPointer (ch),
                                                            (int) block.getNumSamples());
        }

        void setBand (int band, const juce::IIRCoefficients& c)
        {
            for (auto& chFilters : filters)
                chFilters[(size_t) band].setCoefficients (c);
        }

        juce::String type;
        FxEffect lastEffect;
        double sampleRate { 0.0 };
        int numBands { 0 };
        std::array<std::array<juce::IIRFilter, 3>, 2> filters;
    };

    struct GateNode : Node
    {
        void prepare (double r, int block, int ch) override
        {
            gate.prepare ({ r, (juce::uint32) block, (juce::uint32) ch });
            update (lastEffect);
        }
        void update (const FxEffect& e) override
        {
            lastEffect = e;
            gate.setThreshold (e.get ("threshold"));
            gate.setRatio (juce::jmax (1.0f, e.get ("ratio")));
            gate.setAttack (e.get ("attack"));
            gate.setRelease (e.get ("release"));
        }
        void process (juce::dsp::AudioBlock<float>& block) override
        {
            juce::dsp::ProcessContextReplacing<float> ctx (block);
            gate.process (ctx);
        }
        FxEffect lastEffect { FxEffect::makeDefault ("gate") };
        juce::dsp::NoiseGate<float> gate;
    };

    struct CompNode : Node
    {
        void prepare (double r, int block, int ch) override
        {
            comp.prepare ({ r, (juce::uint32) block, (juce::uint32) ch });
            update (lastEffect);
        }
        void update (const FxEffect& e) override
        {
            lastEffect = e;
            comp.setThreshold (e.get ("threshold"));
            comp.setRatio (juce::jmax (1.0f, e.get ("ratio")));
            comp.setAttack (e.get ("attack"));
            comp.setRelease (e.get ("release"));
            makeup.store (juce::Decibels::decibelsToGain (e.get ("makeup")));
        }
        void process (juce::dsp::AudioBlock<float>& block) override
        {
            juce::dsp::ProcessContextReplacing<float> ctx (block);
            comp.process (ctx);
            block.multiplyBy (makeup.load());
        }
        FxEffect lastEffect { FxEffect::makeDefault ("comp") };
        juce::dsp::Compressor<float> comp;
        std::atomic<float> makeup { 1.0f };
    };

    struct DistNode : Node
    {
        void prepare (double, int, int) override { update (lastEffect); }
        void update (const FxEffect& e) override
        {
            lastEffect = e;
            drive.store (juce::Decibels::decibelsToGain (e.get ("drive")));
            level.store (juce::Decibels::decibelsToGain (e.get ("level")));
        }
        void process (juce::dsp::AudioBlock<float>& block) override
        {
            const float g = drive.load(), out = level.load();
            for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
            {
                auto* data = block.getChannelPointer (ch);
                for (size_t i = 0; i < block.getNumSamples(); ++i)
                    data[i] = std::tanh (data[i] * g) * out;
            }
        }
        FxEffect lastEffect { FxEffect::makeDefault ("dist") };
        std::atomic<float> drive { 1.0f }, level { 1.0f };
    };

    struct ChorusNode : Node
    {
        void prepare (double r, int block, int ch) override
        {
            chorus.prepare ({ r, (juce::uint32) block, (juce::uint32) ch });
            update (lastEffect);
        }
        void update (const FxEffect& e) override
        {
            lastEffect = e;
            chorus.setRate (e.get ("rate"));
            chorus.setDepth (e.get ("depth"));
            chorus.setCentreDelay (juce::jlimit (1.0f, 99.0f, e.get ("delay")));
            chorus.setFeedback (juce::jlimit (-0.95f, 0.95f, e.get ("feedback")));
            chorus.setMix (e.get ("mix"));
        }
        void process (juce::dsp::AudioBlock<float>& block) override
        {
            juce::dsp::ProcessContextReplacing<float> ctx (block);
            chorus.process (ctx);
        }
        FxEffect lastEffect { FxEffect::makeDefault ("chorus") };
        juce::dsp::Chorus<float> chorus;
    };

    struct PhaserNode : Node
    {
        void prepare (double r, int block, int ch) override
        {
            sampleRate = r;
            phaser.prepare ({ r, (juce::uint32) block, (juce::uint32) ch });
            update (lastEffect);
        }
        void update (const FxEffect& e) override
        {
            lastEffect = e;
            phaser.setRate (e.get ("rate"));
            phaser.setDepth (e.get ("depth"));
            phaser.setCentreFrequency (juce::jlimit (50.0f,
                juce::jmax (100.0f, (float) (sampleRate * 0.45)), e.get ("freq")));
            phaser.setFeedback (juce::jlimit (-0.95f, 0.95f, e.get ("feedback")));
            phaser.setMix (e.get ("mix"));
        }
        void process (juce::dsp::AudioBlock<float>& block) override
        {
            juce::dsp::ProcessContextReplacing<float> ctx (block);
            phaser.process (ctx);
        }
        FxEffect lastEffect { FxEffect::makeDefault ("phaser") };
        juce::dsp::Phaser<float> phaser;
        double sampleRate { 44100.0 };
    };

    struct DelayNode : Node
    {
        void prepare (double r, int block, int ch) override
        {
            sampleRate = r;
            delay.setMaximumDelayInSamples ((int) (r * 2.2) + 4);
            delay.prepare ({ r, (juce::uint32) block, (juce::uint32) ch });
            update (lastEffect);
        }
        void update (const FxEffect& e) override
        {
            lastEffect = e;
            if (sampleRate > 0.0)
                delay.setDelay ((float) juce::jlimit (1.0, sampleRate * 2.0,
                                                      e.get ("time") * 0.001 * sampleRate));
            feedback.store (juce::jlimit (0.0f, 0.95f, e.get ("feedback")));
            mix.store (juce::jlimit (0.0f, 1.0f, e.get ("mix")));
        }
        void process (juce::dsp::AudioBlock<float>& block) override
        {
            const float fb = feedback.load(), wet = mix.load();
            for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
            {
                auto* data = block.getChannelPointer (ch);
                for (size_t i = 0; i < block.getNumSamples(); ++i)
                {
                    const float in = data[i];
                    const float d  = delay.popSample ((int) ch);
                    delay.pushSample ((int) ch, in + d * fb);
                    data[i] = in + d * wet;
                }
            }
        }
        FxEffect lastEffect { FxEffect::makeDefault ("delay") };
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delay { 48000 * 3 };
        std::atomic<float> feedback { 0.35f }, mix { 0.3f };
        double sampleRate { 0.0 };
    };

    struct ReverbNode : Node
    {
        void prepare (double r, int, int) override
        {
            reverb.setSampleRate (r > 0.0 ? r : 44100.0);
            reverb.reset();
            update (lastEffect);
        }
        void update (const FxEffect& e) override
        {
            lastEffect = e;
            juce::Reverb::Parameters p;
            p.roomSize = juce::jlimit (0.0f, 1.0f, e.get ("room"));
            p.damping  = juce::jlimit (0.0f, 1.0f, e.get ("damping"));
            p.width    = juce::jlimit (0.0f, 1.0f, e.get ("width"));
            const float m = juce::jlimit (0.0f, 1.0f, e.get ("mix"));
            p.wetLevel = m;
            p.dryLevel = 1.0f - m * 0.4f;   // keep the dry signal mostly intact
            reverb.setParameters (p);
        }
        void process (juce::dsp::AudioBlock<float>& block) override
        {
            const int n = (int) block.getNumSamples();
            if (block.getNumChannels() >= 2)
                reverb.processStereo (block.getChannelPointer (0), block.getChannelPointer (1), n);
            else
                reverb.processMono (block.getChannelPointer (0), n);
        }
        FxEffect lastEffect { FxEffect::makeDefault ("reverb") };
        juce::Reverb reverb;
    };

    struct LimiterNode : Node
    {
        void prepare (double r, int block, int ch) override
        {
            limiter.prepare ({ r, (juce::uint32) block, (juce::uint32) ch });
            update (lastEffect);
        }
        void update (const FxEffect& e) override
        {
            lastEffect = e;
            limiter.setThreshold (e.get ("ceiling"));
            limiter.setRelease (e.get ("release"));
        }
        void process (juce::dsp::AudioBlock<float>& block) override
        {
            juce::dsp::ProcessContextReplacing<float> ctx (block);
            limiter.process (ctx);
        }
        FxEffect lastEffect { FxEffect::makeDefault ("limiter") };
        juce::dsp::Limiter<float> limiter;
    };

    //==============================================================================
    std::vector<std::unique_ptr<Node>> buildChain (const FxSettings& s) const
    {
        std::vector<std::unique_ptr<Node>> result;
        for (const auto& e : s.effects)
        {
            std::unique_ptr<Node> node;

            if      (e.type == "lowcut" || e.type == "highcut" || e.type == "eq")
                node = std::make_unique<FilterNode> (e.type);
            else if (e.type == "gate")    node = std::make_unique<GateNode>();
            else if (e.type == "comp")    node = std::make_unique<CompNode>();
            else if (e.type == "dist")    node = std::make_unique<DistNode>();
            else if (e.type == "chorus")  node = std::make_unique<ChorusNode>();
            else if (e.type == "phaser")  node = std::make_unique<PhaserNode>();
            else if (e.type == "delay")   node = std::make_unique<DelayNode>();
            else if (e.type == "reverb")  node = std::make_unique<ReverbNode>();
            else if (e.type == "limiter") node = std::make_unique<LimiterNode>();
            else
                continue;

            node->prepare (rate, maxBlock, channels);
            node->update (e);
            result.push_back (std::move (node));
        }
        return result;
    }

    void processChannels (float* const* chans, int numChans, int numSamples)
    {
        if (! active.load() || numSamples <= 0)
            return;

        const juce::SpinLock::ScopedTryLockType tl (lock);
        if (! tl.isLocked())
            return;   // chain is being edited - dry for one block

        juce::dsp::AudioBlock<float> block (chans, (size_t) numChans, (size_t) numSamples);
        for (auto& node : chain)
            node->process (block);
    }

    double rate { 44100.0 };
    int maxBlock { 512 };
    int channels { 2 };

    FxSettings current;                       ///< message-thread copy
    std::atomic<bool> active { false };

    juce::SpinLock lock;
    std::vector<std::unique_ptr<Node>> chain;
};

} // namespace bandjam
