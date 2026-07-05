#include "AppCapture.h"

#if JUCE_WINDOWS

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <atomic>

#pragma comment (lib, "mmdevapi.lib")

namespace bandjam
{
namespace
{
    /** Minimal completion handler for ActivateAudioInterfaceAsync (must be
        agile, i.e. callable from the MTA worker that Windows uses). */
    class ActivationHandler : public IActivateAudioInterfaceCompletionHandler,
                              public IAgileObject
    {
    public:
        HANDLE doneEvent { CreateEventW (nullptr, TRUE, FALSE, nullptr) };

        ~ActivationHandler() { CloseHandle (doneEvent); }

        HRESULT STDMETHODCALLTYPE ActivateCompleted (IActivateAudioInterfaceAsyncOperation*) override
        {
            SetEvent (doneEvent);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void** object) override
        {
            if (riid == __uuidof (IUnknown)
                || riid == __uuidof (IActivateAudioInterfaceCompletionHandler))
            {
                *object = static_cast<IActivateAudioInterfaceCompletionHandler*> (this);
                AddRef();
                return S_OK;
            }
            if (riid == __uuidof (IAgileObject))
            {
                *object = static_cast<IAgileObject*> (this);
                AddRef();
                return S_OK;
            }
            *object = nullptr;
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override  { return (ULONG) ++refCount; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const auto count = (ULONG) --refCount;
            if (count == 0)
                delete this;
            return count;
        }

    private:
        std::atomic<int> refCount { 1 };
    };
}

//==============================================================================
struct AppCapture::Impl : public juce::Thread
{
    Impl (juce::uint32 pidToUse, StreamOutput& streamOutput)
        : juce::Thread ("AppCapture"), pid (pidToUse), streamOut (streamOutput)
    {
        aux = streamOut.createAuxSource();
        aux->setRate (kRate);
        startThread (juce::Thread::Priority::high);
    }

    ~Impl() override
    {
        signalThreadShouldExit();
        if (wakeEvent != nullptr)
            SetEvent (wakeEvent);
        stopThread (4000);
        streamOut.removeAuxSource (aux);
    }

    void run() override
    {
        CoInitializeEx (nullptr, COINIT_MULTITHREADED);
        captureLoop();
        CoUninitialize();
        active.store (false);
    }

    void captureLoop()
    {
        // -- activate a process-loopback IAudioClient -------------------------------
        AUDIOCLIENT_ACTIVATION_PARAMS params = {};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        params.ProcessLoopbackParams.TargetProcessId = (DWORD) pid;

        PROPVARIANT activateParams = {};
        activateParams.vt = VT_BLOB;
        activateParams.blob.cbSize = sizeof (params);
        activateParams.blob.pBlobData = reinterpret_cast<BYTE*> (&params);

        auto* handler = new ActivationHandler();
        IActivateAudioInterfaceAsyncOperation* asyncOp = nullptr;

        HRESULT hr = ActivateAudioInterfaceAsync (VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                                  __uuidof (IAudioClient), &activateParams,
                                                  handler, &asyncOp);
        IAudioClient* client = nullptr;
        if (SUCCEEDED (hr))
        {
            WaitForSingleObject (handler->doneEvent, 5000);

            HRESULT activateResult = E_FAIL;
            IUnknown* unknown = nullptr;
            if (SUCCEEDED (asyncOp->GetActivateResult (&activateResult, &unknown))
                && SUCCEEDED (activateResult) && unknown != nullptr)
            {
                unknown->QueryInterface (__uuidof (IAudioClient), reinterpret_cast<void**> (&client));
            }
            if (unknown != nullptr) unknown->Release();
        }
        if (asyncOp != nullptr) asyncOp->Release();
        handler->Release();

        if (client == nullptr)
            return;

        // -- initialise: the loopback engine converts to the format we ask for -------
        WAVEFORMATEX format = {};
        format.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels       = 2;
        format.nSamplesPerSec  = (DWORD) kRate;
        format.wBitsPerSample  = 32;
        format.nBlockAlign     = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        wakeEvent = CreateEventW (nullptr, FALSE, FALSE, nullptr);

        IAudioCaptureClient* capture = nullptr;
        hr = client->Initialize (AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 500000 /* 50 ms */, 0, &format, nullptr);
        if (SUCCEEDED (hr)) hr = client->SetEventHandle (wakeEvent);
        if (SUCCEEDED (hr)) hr = client->GetService (__uuidof (IAudioCaptureClient),
                                                     reinterpret_cast<void**> (&capture));
        if (SUCCEEDED (hr)) hr = client->Start();

        if (SUCCEEDED (hr))
        {
            active.store (true);

            while (! threadShouldExit())
            {
                WaitForSingleObject (wakeEvent, 200);
                if (threadShouldExit())
                    break;

                UINT32 packetFrames = 0;
                while (SUCCEEDED (capture->GetNextPacketSize (&packetFrames)) && packetFrames > 0)
                {
                    BYTE* buffer = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;
                    if (FAILED (capture->GetBuffer (&buffer, &frames, &flags, nullptr, nullptr)))
                        break;

                    if (frames > 0)
                    {
                        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0)
                        {
                            // Keep the aux clock ticking with zeros.
                            silence.calloc (frames * 2);
                            aux->pushAudio (silence.getData(), (int) frames);
                        }
                        else
                        {
                            aux->pushAudio (reinterpret_cast<const float*> (buffer), (int) frames);
                        }
                    }
                    capture->ReleaseBuffer (frames);
                }
            }
            client->Stop();
        }

        if (capture != nullptr) capture->Release();
        client->Release();
        if (wakeEvent != nullptr) { CloseHandle (wakeEvent); wakeEvent = nullptr; }
    }

    static constexpr double kRate = 48000.0;

    const juce::uint32 pid;
    StreamOutput& streamOut;
    StreamOutput::AuxSource* aux { nullptr };
    HANDLE wakeEvent { nullptr };
    juce::HeapBlock<float> silence;
    std::atomic<bool> active { false };
};

//==============================================================================
AppCapture::AppCapture (juce::uint32 pidToUse, StreamOutput& streamOutput)
    : pid (pidToUse), impl (std::make_unique<Impl> (pidToUse, streamOutput))
{
}

AppCapture::~AppCapture() = default;

bool AppCapture::isActive() const noexcept
{
    return impl->active.load();
}

bool AppCapture::isSupported()
{
    // Per-process loopback needs Windows 10 2004 (build 19041) or newer.
    using RtlGetVersionFn = LONG (WINAPI*) (PRTL_OSVERSIONINFOW);
    if (auto* ntdll = GetModuleHandleW (L"ntdll.dll"))
        if (auto fn = (RtlGetVersionFn) (void*) GetProcAddress (ntdll, "RtlGetVersion"))
        {
            RTL_OSVERSIONINFOW info = {};
            info.dwOSVersionInfoSize = sizeof (info);
            if (fn (&info) == 0)
                return info.dwMajorVersion > 10
                       || (info.dwMajorVersion == 10 && info.dwBuildNumber >= 19041);
        }
    return false;
}

} // namespace bandjam

#else // !JUCE_WINDOWS

namespace bandjam
{
struct AppCapture::Impl {};
AppCapture::AppCapture (juce::uint32 pidToUse, StreamOutput&) : pid (pidToUse) {}
AppCapture::~AppCapture() = default;
bool AppCapture::isActive() const noexcept { return false; }
bool AppCapture::isSupported() { return false; }
} // namespace bandjam

#endif
