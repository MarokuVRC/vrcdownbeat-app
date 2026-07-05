param([int]$TargetPid = 0)

# Tests the undocumented per-app routing API (IAudioPolicyConfigFactory)
# outside of BandJam, to isolate activation problems.

$src = @"
using System;
using System.Runtime.InteropServices;

namespace RouteTest
{
    public enum EDataFlow { Render, Capture, All }
    public enum ERole { Console, Multimedia, Communications }

    [Guid("ab3d4648-e242-459f-b02f-541c70306324")]
    [InterfaceType(ComInterfaceType.InterfaceIsIInspectable)]
    public interface IAudioPolicyConfigFactory21H2
    {
        int s1(); int s2(); int s3(); int s4(); int s5(); int s6(); int s7(); int s8(); int s9(); int s10();
        int s11(); int s12(); int s13(); int s14(); int s15(); int s16(); int s17(); int s18(); int s19();
        [PreserveSig]
        int SetPersistedDefaultAudioEndpoint(uint processId, EDataFlow flow, ERole role, IntPtr deviceId);
        [PreserveSig]
        int GetPersistedDefaultAudioEndpoint(uint processId, EDataFlow flow, ERole role,
            [Out, MarshalAs(UnmanagedType.HString)] out string deviceId);
        [PreserveSig]
        int ClearAllPersistedApplicationDefaultEndpoints();
    }

    [Guid("2a59116d-6c4f-45e0-a74f-707e3fef9258")]
    [InterfaceType(ComInterfaceType.InterfaceIsIInspectable)]
    public interface IAudioPolicyConfigFactoryDown
    {
        int s1(); int s2(); int s3(); int s4(); int s5(); int s6(); int s7(); int s8(); int s9(); int s10();
        int s11(); int s12(); int s13(); int s14(); int s15(); int s16(); int s17(); int s18(); int s19();
        [PreserveSig]
        int SetPersistedDefaultAudioEndpoint(uint processId, EDataFlow flow, ERole role, IntPtr deviceId);
        [PreserveSig]
        int GetPersistedDefaultAudioEndpoint(uint processId, EDataFlow flow, ERole role,
            [Out, MarshalAs(UnmanagedType.HString)] out string deviceId);
        [PreserveSig]
        int ClearAllPersistedApplicationDefaultEndpoints();
    }

    public static class Native
    {
        [DllImport("combase.dll", PreserveSig = true)]
        public static extern int RoGetActivationFactory(
            [MarshalAs(UnmanagedType.HString)] string activatableClassId,
            [In] ref Guid iid,
            [Out, MarshalAs(UnmanagedType.IInspectable)] out object factory);

        public static void Run(uint pid)
        {
            var iid21 = typeof(IAudioPolicyConfigFactory21H2).GUID;
            object o;
            int hr = RoGetActivationFactory("Windows.Media.Internal.AudioPolicyConfig", ref iid21, out o);
            Console.WriteLine("activate 21H2 iid: hr=0x" + hr.ToString("X8"));

            if (hr != 0)
            {
                var iidDown = typeof(IAudioPolicyConfigFactoryDown).GUID;
                hr = RoGetActivationFactory("Windows.Media.Internal.AudioPolicyConfig", ref iidDown, out o);
                Console.WriteLine("activate downlevel iid: hr=0x" + hr.ToString("X8"));
                if (hr != 0) return;
                var fd = (IAudioPolicyConfigFactoryDown)o;
                string devD;
                hr = fd.GetPersistedDefaultAudioEndpoint(pid, EDataFlow.Render, ERole.Multimedia, out devD);
                Console.WriteLine("get (down): hr=0x" + hr.ToString("X8") + " device='" + devD + "'");
                return;
            }

            var f = (IAudioPolicyConfigFactory21H2)o;
            string dev;
            hr = f.GetPersistedDefaultAudioEndpoint(pid, EDataFlow.Render, ERole.Multimedia, out dev);
            Console.WriteLine("get (21H2): hr=0x" + hr.ToString("X8") + " device='" + dev + "'");
        }
    }
}
"@

Add-Type -TypeDefinition $src -Language CSharp

if ($TargetPid -eq 0)
{
    $spotify = Get-Process Spotify -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($spotify) { $TargetPid = $spotify.Id }
}
Write-Output "target pid: $TargetPid"
[RouteTest.Native]::Run([uint32]$TargetPid)
