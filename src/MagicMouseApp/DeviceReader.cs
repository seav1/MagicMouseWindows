using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.Win32.SafeHandles;

namespace MagicMouseApp
{
    public class TouchReport
    {
        public byte ReportId;
        public sbyte DeltaX;
        public sbyte DeltaY;
        public byte[] Raw = Array.Empty<byte>();
    }

    public class DeviceReader : IDisposable
    {
        // ---------- Win32 / HID / SetupAPI imports ----------
        [DllImport("kernel32.dll", CharSet = CharSet.Auto, SetLastError = true)]
        private static extern SafeFileHandle CreateFile(
            string lpFileName, uint dwDesiredAccess, uint dwShareMode,
            IntPtr lpSecurityAttributes, uint dwCreationDisposition,
            uint dwFlagsAndAttributes, IntPtr hTemplateFile);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool ReadFile(
            SafeFileHandle hFile, byte[] lpBuffer, uint nNumberOfBytesToRead,
            out uint lpNumberOfBytesRead, IntPtr lpOverlapped);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CancelIoEx(SafeFileHandle hFile, IntPtr lpOverlapped);

        [DllImport("hid.dll")]
        private static extern void HidD_GetHidGuid(out Guid HidGuid);

        [DllImport("hid.dll")]
        private static extern bool HidD_GetPreparsedData(SafeFileHandle hDevice, out IntPtr PreparsedData);

        [DllImport("hid.dll")]
        private static extern bool HidD_FreePreparsedData(IntPtr PreparsedData);

        [DllImport("hid.dll")]
        private static extern int HidP_GetCaps(IntPtr PreparsedData, out HIDP_CAPS Capabilities);

        [DllImport("hid.dll")]
        private static extern bool HidD_GetAttributes(SafeFileHandle hDevice, ref HIDD_ATTRIBUTES Attributes);

        [StructLayout(LayoutKind.Sequential)]
        private struct HIDD_ATTRIBUTES
        {
            public int Size;
            public ushort VendorID;
            public ushort ProductID;
            public ushort VersionNumber;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct HIDP_CAPS
        {
            public ushort Usage, UsagePage, InputReportByteLength, OutputReportByteLength, FeatureReportByteLength;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 17)] public ushort[] Reserved;
            public ushort NumberLinkCollectionNodes, NumberInputButtonCaps, NumberInputValueCaps,
                NumberInputDataIndices, NumberOutputButtonCaps, NumberOutputValueCaps,
                NumberOutputDataIndices, NumberFeatureButtonCaps, NumberFeatureValueCaps,
                NumberFeatureDataIndices;
        }

        // SetupAPI
        [DllImport("setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
        private static extern IntPtr SetupDiGetClassDevs(ref Guid ClassGuid, IntPtr Enumerator,
            IntPtr hwndParent, uint Flags);

        [DllImport("setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
        private static extern bool SetupDiEnumDeviceInterfaces(IntPtr DeviceInfoSet, IntPtr DeviceInfoData,
            ref Guid InterfaceClassGuid, uint MemberIndex, ref SP_DEVICE_INTERFACE_DATA DeviceInterfaceData);

        [DllImport("setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
        private static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr DeviceInfoSet,
            ref SP_DEVICE_INTERFACE_DATA DeviceInterfaceData, IntPtr DeviceInterfaceDetailData,
            uint DeviceInterfaceDetailDataSize, out uint RequiredSize, IntPtr DeviceInfoData);

        [DllImport("setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
        private static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr DeviceInfoSet,
            ref SP_DEVICE_INTERFACE_DATA DeviceInterfaceData, IntPtr DeviceInterfaceDetailData,
            uint DeviceInterfaceDetailDataSize, IntPtr RequiredSize, IntPtr DeviceInfoData);

        [DllImport("setupapi.dll")]
        private static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);

        [StructLayout(LayoutKind.Sequential)]
        private struct SP_DEVICE_INTERFACE_DATA
        {
            public int cbSize;
            public Guid InterfaceClassGuid;
            public uint Flags;
            public IntPtr Reserved;
        }

        private const uint GENERIC_READ          = 0x80000000;
        private const uint GENERIC_WRITE         = 0x40000000;
        private const uint FILE_SHARE_READ       = 0x00000001;
        private const uint FILE_SHARE_WRITE      = 0x00000002;
        private const uint OPEN_EXISTING         = 3;
        private const uint FILE_ATTRIBUTE_NORMAL = 0x80;

        private const uint DIGCF_PRESENT         = 0x00000002;
        private const uint DIGCF_DEVICEINTERFACE = 0x00000010;

        private const int  ERROR_INVALID_HANDLE  = 6;
        private const int  ERROR_DEVICE_NOT_CONNECTED = 1167;
        private const int  ERROR_OPERATION_ABORTED = 995;

        private const int  APPLE_VID            = 0x05AC;
        // Apple Magic Mouse PIDs (covers original, Magic Mouse 2, USB-C 2024 model)
        private static readonly ushort[] MagicMousePids = new ushort[]
        {
            0x030D, // Magic Mouse (Bluetooth)
            0x0269, // Magic Mouse 2
            0x0323, // Magic Mouse (USB-C variant, A3204 - placeholder if reported)
        };

        // ---------- Instance state ----------
        private SafeFileHandle? _handle;
        private Thread? _readThread;
        private volatile bool _running;
        private int _reportLen = 64;
        private string _openedPath = "";

        public event Action<TouchReport>? ReportReceived;
        public event Action<string>? StatusChanged;
        public event Action? DeviceDisconnected;

        // Common symbolic-link fallbacks (some custom drivers expose these)
        private static readonly string[] FallbackSymbolicLinks = new[]
        {
            @"\\.\MagicMouse",
            @"\\.\MagicMouseRawPDO",
            @"\\.\GLOBALROOT\Device\MagicMouseRawPDO",
        };

        // ---------- Public API ----------
        public bool Open()
        {
            // 1) Robust HID enumeration (works on Win10 and Win11)
            var candidates = EnumerateHidCandidates();

            // Prefer Apple devices first
            candidates.Sort((a, b) =>
            {
                int sa = a.IsAppleDevice ? 0 : 1;
                int sb = b.IsAppleDevice ? 0 : 1;
                if (sa != sb) return sa - sb;
                // longer InputReport length usually means richer data (raw touch interface)
                return b.InputReportByteLength.CompareTo(a.InputReportByteLength);
            });

            foreach (var c in candidates)
            {
                if (TryOpen(c.Path))
                {
                    StatusChanged?.Invoke($"Connected: VID={c.Vid:X4} PID={c.Pid:X4} report={_reportLen}b");
                    return true;
                }
            }

            // 2) Fallback to well-known symbolic links (older Magic Utilities builds)
            foreach (var path in FallbackSymbolicLinks)
            {
                if (TryOpen(path))
                {
                    StatusChanged?.Invoke($"Connected via {path} (report={_reportLen}b)");
                    return true;
                }
            }

            StatusChanged?.Invoke("Magic Mouse not found - check Bluetooth + driver");
            return false;
        }

        public void StartReading()
        {
            _running = true;
            _readThread = new Thread(ReadLoop)
            {
                IsBackground = true,
                Name = "MagicMouseReader"
            };
            _readThread.Start();
        }

        public void Stop()
        {
            _running = false;
            try
            {
                if (_handle is { IsInvalid: false, IsClosed: false })
                {
                    CancelIoEx(_handle, IntPtr.Zero);
                }
            }
            catch { /* ignore */ }
            try { _handle?.Close(); } catch { }
            _handle = null;
        }

        public void Dispose() => Stop();

        // ---------- Internals ----------
        private bool TryOpen(string path)
        {
            try
            {
                var h = CreateFile(path,
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    IntPtr.Zero, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, IntPtr.Zero);

                if (h is null || h.IsInvalid)
                {
                    h = CreateFile(path,
                        GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        IntPtr.Zero, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, IntPtr.Zero);
                }

                if (h is null || h.IsInvalid) return false;

                _handle = h;
                _openedPath = path;

                // Probe HID caps to figure out report length (raw devices may not be HID)
                if (HidD_GetPreparsedData(_handle, out var preparsed))
                {
                    if (HidP_GetCaps(preparsed, out var caps) >= 0 && caps.InputReportByteLength > 0)
                    {
                        _reportLen = caps.InputReportByteLength;
                    }
                    HidD_FreePreparsedData(preparsed);
                }
                else
                {
                    // Custom raw device - use a generous buffer
                    _reportLen = 64;
                }

                return true;
            }
            catch
            {
                return false;
            }
        }

        private struct HidCandidate
        {
            public string Path;
            public ushort Vid;
            public ushort Pid;
            public int InputReportByteLength;
            public bool IsAppleDevice;
        }

        private List<HidCandidate> EnumerateHidCandidates()
        {
            var list = new List<HidCandidate>();
            HidD_GetHidGuid(out var hidGuid);

            IntPtr devSet = SetupDiGetClassDevs(ref hidGuid, IntPtr.Zero, IntPtr.Zero,
                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

            if (devSet == IntPtr.Zero || devSet == new IntPtr(-1))
                return list;

            try
            {
                uint index = 0;
                while (true)
                {
                    var ifaceData = new SP_DEVICE_INTERFACE_DATA();
                    ifaceData.cbSize = Marshal.SizeOf<SP_DEVICE_INTERFACE_DATA>();

                    if (!SetupDiEnumDeviceInterfaces(devSet, IntPtr.Zero, ref hidGuid, index, ref ifaceData))
                        break;
                    index++;

                    SetupDiGetDeviceInterfaceDetail(devSet, ref ifaceData, IntPtr.Zero, 0, out uint reqSize, IntPtr.Zero);
                    if (reqSize == 0) continue;

                    IntPtr detail = Marshal.AllocHGlobal((int)reqSize);
                    try
                    {
                        // SP_DEVICE_INTERFACE_DETAIL_DATA cbSize is 8 on x64 (4-byte int + 4-byte alignment for the
                        // first wide char), and 6 on x86. Using IntPtr.Size + 4 covers both safely.
                        Marshal.WriteInt32(detail, IntPtr.Size == 8 ? 8 : 6);

                        if (!SetupDiGetDeviceInterfaceDetail(devSet, ref ifaceData, detail, reqSize, out _, IntPtr.Zero))
                            continue;

                        // DevicePath starts after the cbSize field
                        IntPtr pathPtr = IntPtr.Add(detail, 4);
                        string? path = Marshal.PtrToStringAuto(pathPtr);
                        if (string.IsNullOrEmpty(path)) continue;

                        var cand = ProbeCandidate(path);
                        if (cand.HasValue) list.Add(cand.Value);
                    }
                    finally
                    {
                        Marshal.FreeHGlobal(detail);
                    }
                }
            }
            finally
            {
                SetupDiDestroyDeviceInfoList(devSet);
            }

            return list;
        }

        private HidCandidate? ProbeCandidate(string path)
        {
            // Lightweight probe: open, query attributes & caps, then close.
            var h = CreateFile(path,
                0, // no access - just identify
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                IntPtr.Zero, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, IntPtr.Zero);

            if (h is null || h.IsInvalid)
            {
                // Some interfaces refuse zero-access opens; fall back to read share
                h = CreateFile(path,
                    GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    IntPtr.Zero, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, IntPtr.Zero);
            }

            if (h is null || h.IsInvalid)
            {
                // Even if probe fails, the path may still be openable later via the main path.
                // Use VID heuristic from the path string itself.
                return PathHeuristic(path);
            }

            ushort vid = 0, pid = 0;
            int inputLen = 0;

            try
            {
                var attrs = new HIDD_ATTRIBUTES { Size = Marshal.SizeOf<HIDD_ATTRIBUTES>() };
                if (HidD_GetAttributes(h, ref attrs))
                {
                    vid = attrs.VendorID;
                    pid = attrs.ProductID;
                }

                if (HidD_GetPreparsedData(h, out var pp))
                {
                    if (HidP_GetCaps(pp, out var caps) >= 0)
                        inputLen = caps.InputReportByteLength;
                    HidD_FreePreparsedData(pp);
                }
            }
            finally
            {
                h.Close();
            }

            bool isApple = vid == APPLE_VID;
            bool isMagicMouse = isApple && Array.IndexOf(MagicMousePids, pid) >= 0;

            // Heuristic by string when HID metadata is unavailable
            if (vid == 0)
            {
                var heur = PathHeuristic(path);
                if (heur.HasValue) return heur;
                return null;
            }

            // Skip non-Apple HID devices to keep enumeration tight (but keep magic-mouse named devices)
            if (!isApple && !path.Contains("magicmouse", StringComparison.OrdinalIgnoreCase))
                return null;

            return new HidCandidate
            {
                Path = path,
                Vid = vid,
                Pid = pid,
                InputReportByteLength = inputLen,
                IsAppleDevice = isMagicMouse || isApple
            };
        }

        private static HidCandidate? PathHeuristic(string path)
        {
            string lower = path.ToLowerInvariant();
            bool looksApple = lower.Contains("vid_05ac") || lower.Contains("magicmouse");
            if (!looksApple) return null;

            ushort vid = 0, pid = 0;
            int idx = lower.IndexOf("vid_", StringComparison.Ordinal);
            if (idx >= 0 && lower.Length >= idx + 8)
                ushort.TryParse(lower.Substring(idx + 4, 4), System.Globalization.NumberStyles.HexNumber,
                    System.Globalization.CultureInfo.InvariantCulture, out vid);
            int pidx = lower.IndexOf("pid_", StringComparison.Ordinal);
            if (pidx >= 0 && lower.Length >= pidx + 8)
                ushort.TryParse(lower.Substring(pidx + 4, 4), System.Globalization.NumberStyles.HexNumber,
                    System.Globalization.CultureInfo.InvariantCulture, out pid);

            return new HidCandidate
            {
                Path = path,
                Vid = vid,
                Pid = pid,
                InputReportByteLength = 0,
                IsAppleDevice = true
            };
        }

        private void ReadLoop()
        {
            int bufSize = Math.Max(_reportLen, 64);
            var buf = new byte[bufSize];

            while (_running && _handle is { IsInvalid: false, IsClosed: false })
            {
                try
                {
                    bool ok = ReadFile(_handle, buf, (uint)buf.Length, out uint bytesRead, IntPtr.Zero);

                    if (!ok || bytesRead == 0)
                    {
                        int err = Marshal.GetLastWin32Error();
                        if (err == ERROR_INVALID_HANDLE ||
                            err == ERROR_DEVICE_NOT_CONNECTED ||
                            err == ERROR_OPERATION_ABORTED)
                        {
                            DeviceDisconnected?.Invoke();
                            break;
                        }
                        Thread.Sleep(10);
                        continue;
                    }

                    var report = ParseReport(buf, (int)bytesRead);
                    if (report != null) ReportReceived?.Invoke(report);
                }
                catch (Exception ex)
                {
                    StatusChanged?.Invoke($"Read error: {ex.Message}");
                    Thread.Sleep(100);
                }
            }
        }

        private TouchReport? ParseReport(byte[] buf, int len)
        {
            if (len < 3) return null;

            var raw = new byte[len];
            Array.Copy(buf, raw, len);

            return new TouchReport
            {
                ReportId = buf[0],
                DeltaY = (sbyte)buf[1],
                DeltaX = (sbyte)buf[2],
                Raw = raw
            };
        }
    }
}
