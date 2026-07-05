# Tries to activate + initialize (shared mode) every ACTIVE render endpoint
# whose name matches the filter, reporting the HRESULT of each step.
param([string]$Filter = "CABLE")

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public class WasapiProbe
{
    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
    class MMDeviceEnumeratorComObject { }

    [Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceEnumerator
    {
        int EnumAudioEndpoints(int dataFlow, int stateMask, out IMMDeviceCollection devices);
    }

    [Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceCollection
    {
        int GetCount(out int count);
        int Item(int index, out IMMDevice device);
    }

    [Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDevice
    {
        int Activate(ref Guid iid, int clsCtx, IntPtr activationParams, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
        int OpenPropertyStore(int access, out IPropertyStore properties);
        int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
        int GetState(out int state);
    }

    [Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPropertyStore
    {
        int GetCount(out int count);
        int GetAt(int index, out PropertyKey key);
        int GetValue(ref PropertyKey key, out PropVariant value);
    }

    [Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioClient
    {
        int Initialize(int shareMode, int streamFlags, long bufferDuration, long periodicity, IntPtr format, IntPtr audioSessionGuid);
        int GetBufferSize(out uint bufferSize);
        int GetStreamLatency(out long latency);
        int GetCurrentPadding(out uint padding);
        int IsFormatSupported(int shareMode, IntPtr format, out IntPtr closestMatch);
        int GetMixFormat(out IntPtr format);
    }

    [StructLayout(LayoutKind.Sequential)]
    struct PropertyKey { public Guid fmtid; public int pid; }

    [StructLayout(LayoutKind.Explicit)]
    struct PropVariant
    {
        [FieldOffset(0)] public short vt;
        [FieldOffset(8)] public IntPtr pointerValue;
    }

    public static void Probe(string filter)
    {
        var enumerator = (IMMDeviceEnumerator)(object)new MMDeviceEnumeratorComObject();
        IMMDeviceCollection coll;
        enumerator.EnumAudioEndpoints(0 /* render */, 1 /* active */, out coll);
        int count; coll.GetCount(out count);
        var nameKey = new PropertyKey { fmtid = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"), pid = 2 };
        var iidAudioClient = new Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2");

        for (int i = 0; i < count; i++)
        {
            IMMDevice dev; coll.Item(i, out dev);
            IPropertyStore store; dev.OpenPropertyStore(0, out store);
            PropVariant v; store.GetValue(ref nameKey, out v);
            string name = v.vt == 31 ? Marshal.PtrToStringUni(v.pointerValue) : "(?)";
            if (filter.Length > 0 && name.IndexOf(filter, StringComparison.OrdinalIgnoreCase) < 0)
                continue;

            Console.WriteLine("=== " + name + " ===");

            object obj;
            int hr = dev.Activate(ref iidAudioClient, 1 /* CLSCTX_INPROC_SERVER */, IntPtr.Zero, out obj);
            Console.WriteLine("  Activate(IAudioClient): 0x" + hr.ToString("X8"));
            if (hr != 0) continue;

            var client = (IAudioClient)obj;
            IntPtr mixFormat;
            hr = client.GetMixFormat(out mixFormat);
            Console.WriteLine("  GetMixFormat: 0x" + hr.ToString("X8"));
            if (hr != 0) continue;

            short chans = Marshal.ReadInt16(mixFormat, 2);
            int rate = Marshal.ReadInt32(mixFormat, 4);
            short bits = Marshal.ReadInt16(mixFormat, 14);
            Console.WriteLine("  mix format: " + chans + " ch, " + rate + " Hz, " + bits + " bit");

            // Shared mode, event callback flag omitted (plain init), 100 ms buffer.
            hr = client.Initialize(0 /* shared */, 0, 1000000, 0, mixFormat, IntPtr.Zero);
            Console.WriteLine("  Initialize(shared, mix format): 0x" + hr.ToString("X8") + (hr == 0 ? "  <- OK" : "  <- FAILED"));
        }
    }
}
"@

[WasapiProbe]::Probe($Filter)
