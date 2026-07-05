#include "Mp3Encoder.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <vector>

#pragma comment (lib, "mfplat.lib")
#pragma comment (lib, "mfreadwrite.lib")
#pragma comment (lib, "mfuuid.lib")

namespace bandjam
{
namespace
{
    template <typename T>
    struct Com
    {
        T* p = nullptr;
        ~Com() { reset(); }
        void reset() { if (p) { p->Release(); p = nullptr; } }
        T** put() { reset(); return &p; }
        T* operator->() const { return p; }
        explicit operator bool() const { return p != nullptr; }
    };
}

struct Mp3Writer::Impl
{
    Com<IMFSinkWriter> writer;
    DWORD streamIndex = 0;
    double rate = 44100.0;
    int channels = 2;
    juce::int64 framesWritten = 0;
    bool mfStarted = false;
    bool comInitialised = false;
    std::vector<juce::int16> conv;

    ~Impl()
    {
        writer.reset();
        if (mfStarted)
            MFShutdown();
        if (comInitialised)
            CoUninitialize();
    }
};

Mp3Writer::Mp3Writer()  = default;
Mp3Writer::~Mp3Writer() = default;

bool Mp3Writer::open (const juce::File& file, double sampleRate, int numChannels,
                      int bitrateKbps, juce::String& error)
{
    if (! isSampleRateSupported (sampleRate))
    {
        error = "MP3 encoding needs 32/44.1/48 kHz audio";
        return false;
    }

    impl = std::make_unique<Impl>();
    impl->rate = sampleRate;
    impl->channels = juce::jlimit (1, 2, numChannels);

    const HRESULT co = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
    impl->comInitialised = SUCCEEDED (co);   // RPC_E_CHANGED_MODE: already initialised, fine

    HRESULT hr = MFStartup (MF_VERSION, MFSTARTUP_LITE);
    if (FAILED (hr))
    {
        error = "Media Foundation unavailable (0x" + juce::String::toHexString ((int) hr) + ")";
        impl.reset();
        return false;
    }
    impl->mfStarted = true;

    file.deleteFile();

    auto fail = [&] (const juce::String& where, HRESULT code)
    {
        error = where + " failed (0x" + juce::String::toHexString ((int) code) + ")";
        impl.reset();
        return false;
    };

    hr = MFCreateSinkWriterFromURL (file.getFullPathName().toWideCharPointer(),
                                    nullptr, nullptr, impl->writer.put());
    if (FAILED (hr))
        return fail ("Creating the MP3 file", hr);

    // Output: MP3 at the requested bitrate.
    {
        Com<IMFMediaType> out;
        if (FAILED (hr = MFCreateMediaType (out.put())))                                       return fail ("MFCreateMediaType", hr);
        out->SetGUID   (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out->SetGUID   (MF_MT_SUBTYPE, MFAudioFormat_MP3);
        out->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32) sampleRate);
        out->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, (UINT32) impl->channels);
        out->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32) (bitrateKbps * 1000 / 8));

        if (FAILED (hr = impl->writer->AddStream (out.p, &impl->streamIndex)))
            return fail ("MP3 encoder setup (bitrate " + juce::String (bitrateKbps) + " kbps)", hr);
    }

    // Input: 16-bit PCM.
    {
        Com<IMFMediaType> in;
        if (FAILED (hr = MFCreateMediaType (in.put())))                                        return fail ("MFCreateMediaType", hr);
        const UINT32 blockAlign = (UINT32) impl->channels * 2u;
        in->SetGUID   (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in->SetGUID   (MF_MT_SUBTYPE, MFAudioFormat_PCM);
        in->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32) sampleRate);
        in->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, (UINT32) impl->channels);
        in->SetUINT32 (MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign);
        in->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32) sampleRate * blockAlign);
        in->SetUINT32 (MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

        if (FAILED (hr = impl->writer->SetInputMediaType (impl->streamIndex, in.p, nullptr)))
            return fail ("PCM input setup", hr);
    }

    if (FAILED (hr = impl->writer->BeginWriting()))
        return fail ("BeginWriting", hr);

    return true;
}

bool Mp3Writer::writeInterleaved (const float* interleaved, int numFrames)
{
    if (impl == nullptr || ! impl->writer || numFrames <= 0)
        return false;

    const int numValues = numFrames * impl->channels;
    impl->conv.resize ((size_t) numValues);
    for (int i = 0; i < numValues; ++i)
        impl->conv[(size_t) i] = (juce::int16) juce::jlimit (-32768, 32767,
                                    (int) std::lround (juce::jlimit (-1.0f, 1.0f, interleaved[i]) * 32767.0f));

    const DWORD numBytes = (DWORD) numValues * sizeof (juce::int16);

    Com<IMFMediaBuffer> buffer;
    if (FAILED (MFCreateMemoryBuffer (numBytes, buffer.put())))
        return false;

    BYTE* dest = nullptr;
    if (FAILED (buffer->Lock (&dest, nullptr, nullptr)))
        return false;
    std::memcpy (dest, impl->conv.data(), numBytes);
    buffer->Unlock();
    buffer->SetCurrentLength (numBytes);

    Com<IMFSample> sample;
    if (FAILED (MFCreateSample (sample.put())))
        return false;
    sample->AddBuffer (buffer.p);

    // Timestamps in 100 ns units.
    const auto toTicks = [rate = impl->rate] (juce::int64 frames)
    {
        return (LONGLONG) ((double) frames * 10'000'000.0 / rate + 0.5);
    };
    sample->SetSampleTime (toTicks (impl->framesWritten));
    sample->SetSampleDuration (toTicks (impl->framesWritten + numFrames) - toTicks (impl->framesWritten));

    if (FAILED (impl->writer->WriteSample (impl->streamIndex, sample.p)))
        return false;

    impl->framesWritten += numFrames;
    return true;
}

bool Mp3Writer::finish()
{
    if (impl == nullptr || ! impl->writer)
        return false;

    const bool ok = SUCCEEDED (impl->writer->Finalize());
    impl.reset();
    return ok;
}

} // namespace bandjam
