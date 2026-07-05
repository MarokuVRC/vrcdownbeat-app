# Reads the live peak meter of an audio endpoint (render or capture) whose
# name contains the filter string. Samples for a few seconds.
param([string]$Filter = "CABLE Input", [int]$Flow = 0, [int]$Seconds = 5)

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public class MeterProbe
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

    [Guid("C02216F6-8C67-4B5B-9D00-D008E73E0064"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioMeterInformation
    {
        int GetPeakValue(out float peak);
    }

    [StructLayout(LayoutKind.Sequential)]
    struct PropertyKey { public Guid fmtid; public int pid; }

    [StructLayout(LayoutKind.Explicit)]
    struct PropVariant
    {
        [FieldOffset(0)] public short vt;
        [FieldOffset(8)] public IntPtr pointerValue;
    }

    public static void Watch(string filter, int flow, int seconds)
    {
        var enumerator = (IMMDeviceEnumerator)(object)new MMDeviceEnumeratorComObject();
        IMMDeviceCollection coll;
        enumerator.EnumAudioEndpoints(flow, 1, out coll);
        int count; coll.GetCount(out count);
        var nameKey = new PropertyKey { fmtid = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"), pid = 2 };
        var iidMeter = new Guid("C02216F6-8C67-4B5B-9D00-D008E73E0064");

        for (int i = 0; i < count; i++)
        {
            IMMDevice dev; coll.Item(i, out dev);
            IPropertyStore store; dev.OpenPropertyStore(0, out store);
            PropVariant v; store.GetValue(ref nameKey, out v);
            string name = v.vt == 31 ? Marshal.PtrToStringUni(v.pointerValue) : "(?)";
            if (name.IndexOf(filter, StringComparison.OrdinalIgnoreCase) < 0)
                continue;

            object obj;
            int hr = dev.Activate(ref iidMeter, 1, IntPtr.Zero, out obj);
            if (hr != 0) { Console.WriteLine(name + ": meter activate failed 0x" + hr.ToString("X8")); continue; }
            var meter = (IAudioMeterInformation)obj;

            float max = 0f;
            for (int t = 0; t < seconds * 10; t++)
            {
                float peak; meter.GetPeakValue(out peak);
                if (peak > max) max = peak;
                System.Threading.Thread.Sleep(100);
            }
            Console.WriteLine(name + ": max peak over " + seconds + " s = " + max.ToString("0.0000"));
        }
    }
}
"@

[MeterProbe]::Watch($Filter, $Flow, $Seconds)
