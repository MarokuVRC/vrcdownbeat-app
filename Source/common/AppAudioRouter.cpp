#include "AppAudioRouter.h"

#if JUCE_WINDOWS

#include <windows.h>
#include <initguid.h>   // defines the PKEY_/IID constants in this TU
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <winstring.h>
#include <roapi.h>
#include <vector>
#include <map>

#pragma comment (lib, "runtimeobject.lib")

namespace bandjam
{
namespace
{
    /** Minimal COM smart pointer (avoids depending on JUCE internals). */
    template <typename T>
    class ComPtr
    {
    public:
        ComPtr() = default;
        ComPtr (const ComPtr& other) : p (other.p) { if (p != nullptr) p->AddRef(); }
        ComPtr& operator= (const ComPtr& other)
        {
            if (other.p != nullptr) other.p->AddRef();
            release();
            p = other.p;
            return *this;
        }
        ~ComPtr() { release(); }

        T** put()             { release(); return &p; }
        void** putVoid()      { release(); return reinterpret_cast<void**> (&p); }
        T* operator->() const { return p; }
        T* get() const        { return p; }
        explicit operator bool() const { return p != nullptr; }

    private:
        void release() { if (p != nullptr) { p->Release(); p = nullptr; } }
        T* p { nullptr };
    };

    juce::String hstringToJuce (HSTRING h)
    {
        UINT32 length = 0;
        const wchar_t* raw = WindowsGetStringRawBuffer (h, &length);
        return juce::String (raw, (size_t) length);
    }

    /** Undocumented interface behind Windows' "App volume and device
        preferences" page. The 19 stubs keep the vtable aligned with the OS
        implementation (layout taken from EarTrumpet, which has used it in
        production since Windows 10 1803). */
    struct IAudioPolicyConfigFactory : public IInspectable
    {
        virtual HRESULT STDMETHODCALLTYPE stub_add_CtxVolumeChange() = 0;
        virtual HRESULT STDMETHODCALLTYPE stub_remove_CtxVolumeChanged() = 0;
        virtual HRESULT STDMETHODCALLTYPE stub_add_RingerVibrateStateChanged() = 0;
        virtual HRESULT STDMETHODCALLTYPE stub_remove_RingerVibrateStateChange() = 0;
        virtual HRESULT STDMETHODCALLTYPE stub_SetVolumeGroupGainForId() = 0;
        virtual HRESULT STDMETHODCALLTYPE stub_GetVolumeGroupGainForId() = 0;
        virtual HRESULT STDMETHODCALLTYPE stub_GetActiveVolumeGroupForEndpointId() = 0;
        virtual HRESULT STDMETHODCALLTYPE stub_GetVolumeGroupsForEndpoint() = 0;
        virtual HRESULT STDMETHODCALLTYPE stub_GetCurrentVolumeContext() = 0;
        virtual HRESULT STDMETHODCALLTYPE stub_SetVolumeGroupMuteForId() = 0;
        virtual HRESULT STDMETHODCALLTYPE stub_GetVolumeGroupMuteForId() = 0;
        virtual HRESULT STDMETHODCALLTYPE stub_SetRingerVibrateState() = 0;
        virtual HRESULT STDMETHODCALLTYPE stub_GetRingerVibrateState() = 0;
        virtual HRESULT STDMETHODCALLTYPE stub_SetPreferredChatApplication() = 0;
        virtual HRESULT STDMETHODCALLTYPE stub_ResetPreferredChatApplication() = 0;
        virtual HRESULT STDMETHODCALLTYPE stub_GetPreferredChatApplication() = 0;
        virtual HRESULT STDMETHODCALLTYPE stub_GetCurrentChatApplications() = 0;
        virtual HRESULT STDMETHODCALLTYPE stub_add_ChatContextChanged() = 0;
        virtual HRESULT STDMETHODCALLTYPE stub_remove_ChatContextChanged() = 0;
        virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint (UINT processId, EDataFlow flow,
                                                                            ERole role, HSTRING deviceId) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint (UINT processId, EDataFlow flow,
                                                                            ERole role, HSTRING* deviceId) = 0;
        virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
    };

    // The IID changed between Windows builds; try 21H2+ first, then the older one.
    constexpr GUID kPolicyConfigIid21H2      = { 0xab3d4648, 0xe242, 0x459f, { 0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24 } };
    constexpr GUID kPolicyConfigIidDownlevel = { 0x2a59116d, 0x6c4f, 0x45e0, { 0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58 } };

    /** Owned HSTRING wrapper. */
    class HString
    {
    public:
        explicit HString (const juce::String& text)
        {
            const auto wide = text.toWideCharPointer();
            WindowsCreateString (wide, (UINT32) wcslen (wide), &handle);
        }
        ~HString() { if (handle != nullptr) WindowsDeleteString (handle); }
        HSTRING get() const { return handle; }

    private:
        HSTRING handle { nullptr };
        JUCE_DECLARE_NON_COPYABLE (HString)
    };

    ComPtr<IAudioPolicyConfigFactory> createPolicyConfig()
    {
        // JUCE initialises classic COM (STA) but not the Windows Runtime, and
        // RoGetActivationFactory fails without it. S_FALSE / RPC_E_CHANGED_MODE
        // mean "already initialised" and are fine.
        RoInitialize (RO_INIT_SINGLETHREADED);

        ComPtr<IAudioPolicyConfigFactory> factory;
        HString className ("Windows.Media.Internal.AudioPolicyConfig");

        for (const auto& iid : { kPolicyConfigIid21H2, kPolicyConfigIidDownlevel })
            if (SUCCEEDED (RoGetActivationFactory (className.get(), iid, factory.putVoid())) && factory)
                break;

        return factory;
    }

    // Device-id packing used by the policy-config API.
    const juce::String kMmDevApiToken   = "\\\\?\\SWD#MMDEVAPI#";
    const juce::String kRenderSuffix    = "#{e6327cad-dcec-4949-ae8a-991e976a79d2}";

    juce::String packRenderDeviceId (const juce::String& rawId)
    {
        return kMmDevApiToken + rawId + kRenderSuffix;
    }

    juce::String unpackDeviceId (juce::String packed)
    {
        if (packed.startsWith (kMmDevApiToken)) packed = packed.substring (kMmDevApiToken.length());
        if (packed.endsWith (kRenderSuffix))    packed = packed.dropLastCharacters (kRenderSuffix.length());
        return packed;
    }

    juce::String getProcessExeName (DWORD pid)
    {
        juce::String result;
        if (HANDLE process = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
        {
            wchar_t path[MAX_PATH] = {};
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW (process, 0, path, &size))
                result = juce::File (juce::String (path)).getFileNameWithoutExtension();
            CloseHandle (process);
        }
        return result;
    }

    /** Property key behind the mic-properties "Listen" tab (undocumented but
        stable for many Windows versions; mmsys.cpl writes the same values).
        pid 1 = "Listen to this device" checkbox (VT_BOOL),
        pid 0 = "Playback through this device" (VT_LPWSTR id, VT_EMPTY = default). */
    constexpr GUID kListenGuid = { 0x24DBB0FC, 0x9311, 0x4B3D, { 0x9C, 0xF0, 0x18, 0xFF, 0x15, 0x56, 0x39, 0xD4 } };
    constexpr PROPERTYKEY kListenEnabledKey  = { kListenGuid, 1 };
    constexpr PROPERTYKEY kListenPlaybackKey = { kListenGuid, 0 };

    juce::String getEndpointFriendlyName (IMMDevice* device)
    {
        ComPtr<IPropertyStore> store;
        if (FAILED (device->OpenPropertyStore (STGM_READ, store.put())))
            return {};

        PROPVARIANT value;
        PropVariantInit (&value);
        juce::String name;
        if (SUCCEEDED (store->GetValue (PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
            name = juce::String (value.pwszVal);
        PropVariantClear (&value);
        return name;
    }
}

//==============================================================================
struct AppAudioRouter::Impl
{
    struct CachedSession
    {
        ComPtr<ISimpleAudioVolume>     volume;
        ComPtr<IAudioMeterInformation> meter;
    };

    struct CachedApp
    {
        juce::uint32 pid { 0 };
        std::vector<CachedSession> sessions;
    };

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IAudioPolicyConfigFactory> policy;
    bool policyChecked { false };
    std::map<juce::uint32, CachedApp> byPid;   ///< representative pid -> live sessions

    Impl()
    {
        CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof (IMMDeviceEnumerator), enumerator.putVoid());
    }

    IAudioPolicyConfigFactory* getPolicy()
    {
        if (! policyChecked)
        {
            policyChecked = true;
            policy = createPolicyConfig();
        }
        return policy.get();
    }
};

//==============================================================================
AppAudioRouter::AppAudioRouter()  : impl (std::make_unique<Impl>()) {}
AppAudioRouter::~AppAudioRouter() = default;

bool AppAudioRouter::isRoutingSupported() const
{
    return impl->getPolicy() != nullptr;
}

static juce::Array<AudioAppEndpoint> listEndpoints (IMMDeviceEnumerator* enumerator, EDataFlow flow)
{
    juce::Array<AudioAppEndpoint> result;
    if (enumerator == nullptr)
        return result;

    ComPtr<IMMDeviceCollection> devices;
    if (FAILED (enumerator->EnumAudioEndpoints (flow, DEVICE_STATE_ACTIVE, devices.put())))
        return result;

    UINT count = 0;
    devices->GetCount (&count);
    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> device;
        if (FAILED (devices->Item (i, device.put())))
            continue;

        LPWSTR id = nullptr;
        if (FAILED (device->GetId (&id)))
            continue;

        AudioAppEndpoint endpoint;
        endpoint.id   = juce::String (id);
        endpoint.name = getEndpointFriendlyName (device.get());
        CoTaskMemFree (id);

        if (endpoint.name.isNotEmpty())
            result.add (std::move (endpoint));
    }
    return result;
}

juce::Array<AudioAppEndpoint> AppAudioRouter::getRenderEndpoints() const
{
    return listEndpoints (impl->enumerator.get(), eRender);
}

juce::Array<AudioAppEndpoint> AppAudioRouter::getCaptureEndpoints() const
{
    return listEndpoints (impl->enumerator.get(), eCapture);
}

AudioAppEndpoint AppAudioRouter::getDefaultRenderEndpoint() const
{
    AudioAppEndpoint result;
    if (! impl->enumerator)
        return result;

    ComPtr<IMMDevice> device;
    if (FAILED (impl->enumerator->GetDefaultAudioEndpoint (eRender, eMultimedia, device.put())))
        return result;

    LPWSTR id = nullptr;
    if (SUCCEEDED (device->GetId (&id)))
    {
        result.id = juce::String (id);
        CoTaskMemFree (id);
    }
    result.name = getEndpointFriendlyName (device.get());
    return result;
}

bool AppAudioRouter::getListen (const juce::String& captureDeviceId, ListenState& state) const
{
    if (! impl->enumerator)
        return false;

    ComPtr<IMMDevice> device;
    if (FAILED (impl->enumerator->GetDevice (captureDeviceId.toWideCharPointer(), device.put())))
        return false;

    ComPtr<IPropertyStore> store;
    if (FAILED (device->OpenPropertyStore (STGM_READ, store.put())))
        return false;

    state = {};

    PROPVARIANT value;
    PropVariantInit (&value);
    if (SUCCEEDED (store->GetValue (kListenEnabledKey, &value)) && value.vt == VT_BOOL)
        state.enabled = value.boolVal != VARIANT_FALSE;
    PropVariantClear (&value);

    PropVariantInit (&value);
    if (SUCCEEDED (store->GetValue (kListenPlaybackKey, &value)) && value.vt == VT_LPWSTR)
        state.playbackDeviceId = juce::String (value.pwszVal);
    PropVariantClear (&value);

    return true;
}

bool AppAudioRouter::setListen (const juce::String& captureDeviceId, const ListenState& state,
                                juce::String& error)
{
    if (! impl->enumerator)
    {
        error = "Audio device enumerator unavailable.";
        return false;
    }

    ComPtr<IMMDevice> device;
    if (FAILED (impl->enumerator->GetDevice (captureDeviceId.toWideCharPointer(), device.put())))
    {
        error = "Input device not found.";
        return false;
    }

    ComPtr<IPropertyStore> store;
    HRESULT hr = device->OpenPropertyStore (STGM_READWRITE, store.put());
    if (FAILED (hr))
    {
        error = hr == E_ACCESSDENIED
                    ? "Windows denied access to the device settings."
                    : "Could not open the device settings (0x" + juce::String::toHexString ((int) hr) + ").";
        return false;
    }

    // Playback target first, then the checkbox - the audio service starts the
    // loopback as soon as the checkbox flips, so the target must already be set.
    PROPVARIANT value;
    PropVariantInit (&value);
    if (state.playbackDeviceId.isNotEmpty())
    {
        value.vt = VT_LPWSTR;
        value.pwszVal = const_cast<LPWSTR> (state.playbackDeviceId.toWideCharPointer());
    }
    hr = store->SetValue (kListenPlaybackKey, value);

    if (SUCCEEDED (hr))
    {
        PROPVARIANT enabled;
        PropVariantInit (&enabled);
        enabled.vt = VT_BOOL;
        enabled.boolVal = state.enabled ? VARIANT_TRUE : VARIANT_FALSE;
        hr = store->SetValue (kListenEnabledKey, enabled);
    }

    if (SUCCEEDED (hr))
        store->Commit();

    if (FAILED (hr))
    {
        error = "Windows refused the change (0x" + juce::String::toHexString ((int) hr) + ").";
        return false;
    }
    return true;
}

void AppAudioRouter::refreshSessions()
{
    apps.clear();
    impl->byPid.clear();

    if (! impl->enumerator)
        return;

    const auto ownPid = (juce::uint32) GetCurrentProcessId();

    // exe name -> row index in 'apps' (Chrome & co. spawn many audio processes;
    // group them like the Windows mixer does).
    std::map<juce::String, int> rowByName;

    ComPtr<IMMDeviceCollection> devices;
    if (FAILED (impl->enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, devices.put())))
        return;

    UINT deviceCount = 0;
    devices->GetCount (&deviceCount);

    for (UINT d = 0; d < deviceCount; ++d)
    {
        ComPtr<IMMDevice> device;
        if (FAILED (devices->Item (d, device.put())))
            continue;

        const auto deviceName = getEndpointFriendlyName (device.get());

        ComPtr<IAudioSessionManager2> manager;
        if (FAILED (device->Activate (__uuidof (IAudioSessionManager2), CLSCTX_ALL,
                                      nullptr, manager.putVoid())))
            continue;

        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED (manager->GetSessionEnumerator (sessions.put())))
            continue;

        int sessionCount = 0;
        sessions->GetCount (&sessionCount);

        for (int s = 0; s < sessionCount; ++s)
        {
            ComPtr<IAudioSessionControl> control;
            if (FAILED (sessions->GetSession (s, control.put())))
                continue;

            ComPtr<IAudioSessionControl2> control2;
            if (FAILED (control->QueryInterface (__uuidof (IAudioSessionControl2), control2.putVoid())))
                continue;

            DWORD pid = 0;
            control2->GetProcessId (&pid);
            if (pid == 0 || pid == ownPid || control2->IsSystemSoundsSession() == S_OK)
                continue;

            AudioSessionState state = AudioSessionStateInactive;
            control->GetState (&state);
            if (state == AudioSessionStateExpired)
                continue;

            const auto exeName = getProcessExeName (pid);
            if (exeName.isEmpty())
                continue;

            Impl::CachedSession cached;
            control->QueryInterface (__uuidof (ISimpleAudioVolume), cached.volume.putVoid());
            control->QueryInterface (__uuidof (IAudioMeterInformation), cached.meter.putVoid());

            auto it = rowByName.find (exeName);
            if (it == rowByName.end())
            {
                AudioAppInfo info;
                info.pid  = pid;
                info.name = exeName;
                info.sessionDevices = deviceName;
                if (state == AudioSessionStateActive)
                    info.activeDevices = deviceName;

                if (cached.volume)
                {
                    float volume = 1.0f; BOOL mute = FALSE;
                    cached.volume->GetMasterVolume (&volume);
                    cached.volume->GetMute (&mute);
                    info.volume = volume;
                    info.mute   = mute != FALSE;
                }

                if (auto* policy = impl->getPolicy())
                {
                    HSTRING routed = nullptr;
                    if (SUCCEEDED (policy->GetPersistedDefaultAudioEndpoint (pid, eRender, eMultimedia, &routed))
                        && routed != nullptr)
                    {
                        info.routedDeviceId = unpackDeviceId (hstringToJuce (routed));
                        WindowsDeleteString (routed);
                    }
                }

                rowByName[exeName] = apps.size();
                apps.add (std::move (info));
                impl->byPid[pid].pid = pid;
                impl->byPid[pid].sessions.push_back (std::move (cached));
            }
            else
            {
                auto& row = apps.getReference (it->second);
                if (! row.sessionDevices.contains (deviceName))
                    row.sessionDevices << ", " << deviceName;
                if (state == AudioSessionStateActive && ! row.activeDevices.contains (deviceName))
                    row.activeDevices << (row.activeDevices.isEmpty() ? "" : ", ") << deviceName;
                impl->byPid[row.pid].sessions.push_back (std::move (cached));
            }
        }
    }
}

float AppAudioRouter::getPeak (juce::uint32 pid) const
{
    float peak = 0.0f;
    const auto it = impl->byPid.find (pid);
    if (it == impl->byPid.end())
        return peak;

    for (const auto& session : it->second.sessions)
        if (session.meter)
        {
            float value = 0.0f;
            if (SUCCEEDED (session.meter->GetPeakValue (&value)))
                peak = juce::jmax (peak, value);
        }
    return peak;
}

void AppAudioRouter::setVolume (juce::uint32 pid, float volume0to1)
{
    const auto it = impl->byPid.find (pid);
    if (it == impl->byPid.end())
        return;

    for (const auto& session : it->second.sessions)
        if (session.volume)
            session.volume->SetMasterVolume (juce::jlimit (0.0f, 1.0f, volume0to1), nullptr);
}

void AppAudioRouter::setMute (juce::uint32 pid, bool mute)
{
    const auto it = impl->byPid.find (pid);
    if (it == impl->byPid.end())
        return;

    for (const auto& session : it->second.sessions)
        if (session.volume)
            session.volume->SetMute (mute ? TRUE : FALSE, nullptr);
}

bool AppAudioRouter::routeAppToDevice (juce::uint32 pid, const juce::String& deviceIdOrEmpty,
                                       juce::String& error)
{
    auto* policy = impl->getPolicy();
    if (policy == nullptr)
    {
        error = "Per-app routing is not available on this Windows version.";
        return false;
    }

    HRESULT hr = S_OK;
    if (deviceIdOrEmpty.isEmpty())
    {
        // Back to "Default": clear both roles.
        hr = policy->SetPersistedDefaultAudioEndpoint (pid, eRender, eMultimedia, nullptr);
        if (SUCCEEDED (hr))
            hr = policy->SetPersistedDefaultAudioEndpoint (pid, eRender, eConsole, nullptr);
    }
    else
    {
        HString packed (packRenderDeviceId (deviceIdOrEmpty));
        hr = policy->SetPersistedDefaultAudioEndpoint (pid, eRender, eMultimedia, packed.get());
        if (SUCCEEDED (hr))
            hr = policy->SetPersistedDefaultAudioEndpoint (pid, eRender, eConsole, packed.get());
    }

    if (FAILED (hr))
    {
        error = "Windows refused the routing change (0x" + juce::String::toHexString ((int) hr) + ").";
        return false;
    }
    return true;
}

} // namespace bandjam

#else // !JUCE_WINDOWS

namespace bandjam
{
struct AppAudioRouter::Impl {};
AppAudioRouter::AppAudioRouter()  : impl (std::make_unique<Impl>()) {}
AppAudioRouter::~AppAudioRouter() = default;
juce::Array<AudioAppEndpoint> AppAudioRouter::getRenderEndpoints() const { return {}; }
juce::Array<AudioAppEndpoint> AppAudioRouter::getCaptureEndpoints() const { return {}; }
AudioAppEndpoint AppAudioRouter::getDefaultRenderEndpoint() const { return {}; }
bool AppAudioRouter::getListen (const juce::String&, ListenState&) const { return false; }
bool AppAudioRouter::setListen (const juce::String&, const ListenState&, juce::String& error)
{
    error = "Windows only";
    return false;
}
void AppAudioRouter::refreshSessions() {}
float AppAudioRouter::getPeak (juce::uint32) const { return 0.0f; }
void AppAudioRouter::setVolume (juce::uint32, float) {}
void AppAudioRouter::setMute (juce::uint32, bool) {}
bool AppAudioRouter::routeAppToDevice (juce::uint32, const juce::String&, juce::String& error)
{
    error = "Windows only";
    return false;
}
bool AppAudioRouter::isRoutingSupported() const { return false; }
} // namespace bandjam

#endif
