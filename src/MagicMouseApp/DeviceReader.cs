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
        // ---------- Win32 imports ----------
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

        // ---------- HID API (only used as a fallback) ----------
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

        // ---------- SetupAPI ----------
        [DllImport("setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
        private static extern IntPtr SetupDiGetClassDevs(IntPtr ClassGuid, IntPtr Enumerator,
            IntPtr hwndParent, uint Flags);

        [DllImport("setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
        private static extern IntPtr SetupDiGetClassDevs(ref Guid ClassGuid, IntPtr Enumerator,
            IntPtr hwndParent, uint Flags);

        [DllImport("setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
        private static extern bool SetupDiEnumDeviceInfo(IntPtr DeviceInfoSet, uint MemberIndex,
            ref SP_DEVINFO_DATA DeviceInfoData);

        [DllImport("setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
        private static extern bool SetupDiGetDeviceRegistryProperty(IntPtr DeviceInfoSet,
            ref SP_DEVINFO_DATA DeviceInfoData, uint Property, out uint PropertyRegDataType,
            IntPtr PropertyBuffer, uint PropertyBufferSize, out uint RequiredSize);

        [DllImport("setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
        private static extern bool SetupDiEnumDeviceInterfaces(IntPtr DeviceInfoSet, IntPtr DeviceInfoData,
            ref Guid InterfaceClassGuid, uint MemberIndex, ref SP_DEVICE_INTERFACE_DATA DeviceInterfaceData);

        [DllImport("setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
        private static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr DeviceInfoSet,
            ref SP_DEVICE_INTERFACE_DATA DeviceInterfaceData, IntPtr DeviceInterfaceDetailData,
            uint DeviceInterfaceDetailDataSize, out uint RequiredSize, IntPtr DeviceInfoData);

        [DllImport("setupapi.dll")]
        private static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);

        [StructLayout(LayoutKind.Sequential)]
        private struct SP_DEVINFO_DATA
        {
            public int cbSize;
            public Guid ClassGuid;
            public uint DevInst;
            public IntPtr Reserved;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct SP_DEVICE_INTERFACE_DATA
        {
            public int cbSize;
            public Guid InterfaceClassGuid;
            public uint Flags;
            public IntPtr Reserved;
        }

        // ---------- Constants ----------
        private const uint GENERIC_READ          = 0x80000000;
        private const uint GENERIC_WRITE         = 0x40000000;
        private const uint FILE_SHARE_READ       = 0x00000001;
        private const uint FILE_SHARE_WRITE      = 0x00000002;
        private const uint OPEN_EXISTING         = 3;
        private const uint FILE_ATTRIBUTE_NORMAL = 0x80;

        private const uint DIGCF_PRESENT         = 0x00000002;
        private const uint DIGCF_ALLCLASSES      = 0x00000004;
        private const uint DIGCF_DEVICEINTERFACE = 0x00000010;

        // SetupDiGetDeviceRegistryProperty / SPDRP_*
        private const uint SPDRP_SERVICE                       = 0x00000004;
        private const uint SPDRP_HARDWAREID                    = 0x00000001;
        private const uint SPDRP_FRIENDLYNAME                  = 0x0000000C;
        private const uint SPDRP_PHYSICAL_DEVICE_OBJECT_NAME   = 0x0000000E;
        private const uint SPDRP_LOCATION_INFORMATION          = 0x0000000D;
        private const uint SPDRP_DEVICEDESC                    = 0x00000000;

        private const int  ERROR_INVALID_HANDLE         = 6;
        private const int  ERROR_DEVICE_NOT_CONNECTED   = 1167;
        private const int  ERROR_OPERATION_ABORTED      = 995;
        private const int  ERROR_NO_MORE_ITEMS          = 259;

        private const int  APPLE_VID = 0x05AC;

        // The service name used by Magic Utilities' kernel driver. The driver creates
        // a non-HID raw PDO (named e.g. "\Device\00000416") that the userland app reads
        // touch reports from. The PnP-assigned PDO number differs on every machine, so
        // we MUST look it up dynamically rather than hard-coding it.
        private static readonly string[] MagicMouseServiceNames = new[]
        {
            "MagicMouse",          // Magic Utilities mouse driver
            "MagicMouseUSB",       // possible USB-C variant
            "MagicMouseHID",       // possible alt name
            "AppleMagicMouse",     // long-shot
        };

        // Some custom drivers also publish a friendly DOS name. We try these as a final
        // fallback - they will simply not exist on most systems and that's fine.
        private static readonly string[] FallbackSymbolicLinks = new[]
        {
            @"\\.\MagicMouse",
            @"\\.\MagicMouseRawPDO",
            @"\\.\MagicMouseUSB",
            @"\\.\GLOBALROOT\Device\MagicMouseRawPDO",
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

        public string OpenedPath => _openedPath;

        // ---------- Public API ----------
        /// <summary>
        /// Run device discovery and return a human-readable report of what was found.
        /// Useful for diagnostics when scroll doesn't work.
        /// </summary>
        public string BuildDiagnostics()
        {
            var sb = new System.Text.StringBuilder();
            sb.AppendLine("=== Magic Mouse device discovery ===");
            sb.AppendLine();

            var pdo = EnumerateMagicMousePdoPaths();
            sb.AppendLine($"[1] PDO/interface candidates with service=MagicMouse*: {pdo.Count}");
            foreach (var c in pdo)
                sb.AppendLine($"    - {c.Description}");
            if (pdo.Count == 0)
                sb.AppendLine("    (none - is MagicMouse.sys installed and the mouse paired?)");
            sb.AppendLine();

            var hid = EnumerateAppleHidPaths();
            sb.AppendLine($"[2] Apple HID interfaces (VID_05AC): {hid.Count}");
            foreach (var p in hid)
                sb.AppendLine($"    - {Truncate(p, 100)}");
            if (hid.Count == 0)
                sb.AppendLine("    (none - is the mouse connected via Bluetooth?)");
            sb.AppendLine();

            sb.AppendLine($"[3] Currently opened path: {(_openedPath.Length == 0 ? "(none)" : _openedPath)}");
            sb.AppendLine($"    Report length: {_reportLen} bytes");
            sb.AppendLine();
            sb.AppendLine("Tips if scroll doesn't work:");
            sb.AppendLine("  - Run 'sc query MagicMouse' in PowerShell - it should be RUNNING.");
            sb.AppendLine("  - Pair the mouse via Bluetooth before launching this app.");
            sb.AppendLine("  - If [1] is empty but [2] has Apple devices, the driver isn't bound");
            sb.AppendLine("    to the device - reinstall it via 'pnputil /add-driver MagicMouse.inf /install'.");

            return sb.ToString();
        }

        public bool Open()
        {
            // Strategy 1: find the device whose driver service is "MagicMouse",
            // then open its raw PDO via "\\.\GLOBALROOT\Device\<pdo-name>".
            // This is the path the Magic Utilities driver actually exposes.
            foreach (var pdoCandidate in EnumerateMagicMousePdoPaths())
            {
                if (TryOpen(pdoCandidate.Path))
                {
                    StatusChanged?.Invoke(
                        $"Connected: {pdoCandidate.Description} (report={_reportLen}b)");
                    return true;
                }
            }

            // Strategy 2: HID enumeration filtered by Apple VID (handles BootCamp-style
            // setups where the device shows up as a regular HID mouse).
            foreach (var hid in EnumerateAppleHidPaths())
            {
                if (TryOpen(hid))
                {
                    StatusChanged?.Invoke($"Connected via HID: {Truncate(hid, 60)} (report={_reportLen}b)");
                    return true;
                }
            }

            // Strategy 3: well-known DOS names (older driver versions exposed these).
            foreach (var path in FallbackSymbolicLinks)
            {
                if (TryOpen(path))
                {
                    StatusChanged?.Invoke($"Connected via {path} (report={_reportLen}b)");
                    return true;
                }
            }

            StatusChanged?.Invoke(
                "Magic Mouse driver not found. Is the MagicMouse service running? " +
                "Run 'sc query MagicMouse' in PowerShell to verify.");
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
                    CancelIoEx(_handle, IntPtr.Zero);
            }
            catch { /* ignore */ }
            try { _handle?.Close(); } catch { }
            _handle = null;
        }

        public void Dispose() => Stop();

        // ---------- Internals: opening ----------
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
                _reportLen = 64; // default; updated below if HID

                // If this happens to be a HID interface, query its caps.
                if (HidD_GetPreparsedData(_handle, out var preparsed))
                {
                    if (HidP_GetCaps(preparsed, out var caps) >= 0 && caps.InputReportByteLength > 0)
                        _reportLen = caps.InputReportByteLength;
                    HidD_FreePreparsedData(preparsed);
                }

                return true;
            }
            catch
            {
                return false;
            }
        }

        // ---------- Internals: PDO enumeration by service name ----------
        private struct PdoCandidate
        {
            public string Path;          // e.g. \\.\GLOBALROOT\Device\00000416
            public string Description;   // human-readable for status text
        }

        private List<PdoCandidate> EnumerateMagicMousePdoPaths()
        {
            var result = new List<PdoCandidate>();

            // ALLCLASSES + PRESENT lists every device currently in the PnP tree
            // regardless of setup class. We then filter by driver service name.
            IntPtr devSet = SetupDiGetClassDevs(IntPtr.Zero, IntPtr.Zero, IntPtr.Zero,
                DIGCF_PRESENT | DIGCF_ALLCLASSES);

            if (devSet == IntPtr.Zero || devSet == new IntPtr(-1)) return result;

            try
            {
                var devInfo = new SP_DEVINFO_DATA();
                devInfo.cbSize = Marshal.SizeOf<SP_DEVINFO_DATA>();

                for (uint idx = 0; SetupDiEnumDeviceInfo(devSet, idx, ref devInfo); idx++)
                {
                    string service = ReadProp(devSet, ref devInfo, SPDRP_SERVICE) ?? "";
                    if (string.IsNullOrEmpty(service)) continue;

                    bool isMagic = false;
                    foreach (var s in MagicMouseServiceNames)
                    {
                        if (service.Equals(s, StringComparison.OrdinalIgnoreCase))
                        {
                            isMagic = true;
                            break;
                        }
                    }
                    if (!isMagic) continue;

                    string desc    = ReadProp(devSet, ref devInfo, SPDRP_FRIENDLYNAME)
                                  ?? ReadProp(devSet, ref devInfo, SPDRP_DEVICEDESC)
                                  ?? "MagicMouse";
                    string pdoName = ReadProp(devSet, ref devInfo, SPDRP_PHYSICAL_DEVICE_OBJECT_NAME) ?? "";

                    // pdoName looks like "\Device\00000416". Translate to a Win32 path:
                    if (!string.IsNullOrEmpty(pdoName))
                    {
                        result.Add(new PdoCandidate
                        {
                            Path = @"\\.\GLOBALROOT" + pdoName,
                            Description = $"{desc} svc={service} pdo={pdoName}"
                        });
                    }

                    // Bonus: also list every device-interface this device exposes,
                    // by trying the well-known interface GUIDs.
                    foreach (var ifaceGuid in WellKnownInterfaceGuids())
                    {
                        foreach (var ifacePath in EnumerateInterfacesForDevice(devSet, ref devInfo, ifaceGuid))
                        {
                            result.Add(new PdoCandidate
                            {
                                Path = ifacePath,
                                Description = $"{desc} iface={Truncate(ifacePath, 50)}"
                            });
                        }
                    }
                }
            }
            finally
            {
                SetupDiDestroyDeviceInfoList(devSet);
            }

            return result;
        }

        private static IEnumerable<Guid> WellKnownInterfaceGuids()
        {
            // GUID_DEVINTERFACE_HID
            yield return new Guid("4D1E55B2-F16F-11CF-88CB-001111000030");
            // GUID_DEVINTERFACE_MOUSE
            yield return new Guid("378DE44C-56EF-11D1-BC8C-00A0C91405DD");
        }

        private static IEnumerable<string> EnumerateInterfacesForDevice(IntPtr devSet,
            ref SP_DEVINFO_DATA devInfo, Guid ifaceGuid)
        {
            // We can't pass devInfo by ref into an iterator method, so collect first.
            var paths = new List<string>();
            var ifaceData = new SP_DEVICE_INTERFACE_DATA
            {
                cbSize = Marshal.SizeOf<SP_DEVICE_INTERFACE_DATA>()
            };

            // Pin devInfo on the stack of caller; SetupDiEnumDeviceInterfaces needs the
            // address of SP_DEVINFO_DATA, so we use a helper P/Invoke overload that
            // accepts ref devInfo. Here we copy into a managed pointer.
            IntPtr devInfoPtr = Marshal.AllocHGlobal(Marshal.SizeOf<SP_DEVINFO_DATA>());
            try
            {
                Marshal.StructureToPtr(devInfo, devInfoPtr, false);

                for (uint i = 0; SetupDiEnumDeviceInterfaces(devSet, devInfoPtr, ref ifaceGuid, i, ref ifaceData); i++)
                {
                    SetupDiGetDeviceInterfaceDetail(devSet, ref ifaceData, IntPtr.Zero, 0, out uint reqSize, IntPtr.Zero);
                    if (reqSize == 0) continue;

                    IntPtr detail = Marshal.AllocHGlobal((int)reqSize);
                    try
                    {
                        Marshal.WriteInt32(detail, IntPtr.Size == 8 ? 8 : 6);
                        if (SetupDiGetDeviceInterfaceDetail(devSet, ref ifaceData, detail, reqSize, out _, IntPtr.Zero))
                        {
                            string? p = Marshal.PtrToStringAuto(IntPtr.Add(detail, 4));
                            if (!string.IsNullOrEmpty(p)) paths.Add(p);
                        }
                    }
                    finally { Marshal.FreeHGlobal(detail); }
                }
            }
            finally
            {
                Marshal.FreeHGlobal(devInfoPtr);
            }

            return paths;
        }

        private static string? ReadProp(IntPtr devSet, ref SP_DEVINFO_DATA devInfo, uint property)
        {
            // First call to learn required size
            SetupDiGetDeviceRegistryProperty(devSet, ref devInfo, property, out _,
                IntPtr.Zero, 0, out uint reqSize);
            if (reqSize == 0) return null;

            IntPtr buf = Marshal.AllocHGlobal((int)reqSize);
            try
            {
                if (!SetupDiGetDeviceRegistryProperty(devSet, ref devInfo, property, out _,
                        buf, reqSize, out _))
                    return null;
                return Marshal.PtrToStringAuto(buf);
            }
            finally
            {
                Marshal.FreeHGlobal(buf);
            }
        }

        // ---------- Internals: HID fallback by VID ----------
        private List<string> EnumerateAppleHidPaths()
        {
            var paths = new List<string>();
            HidD_GetHidGuid(out var hidGuid);

            IntPtr devSet = SetupDiGetClassDevs(ref hidGuid, IntPtr.Zero, IntPtr.Zero,
                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (devSet == IntPtr.Zero || devSet == new IntPtr(-1)) return paths;

            try
            {
                var ifaceData = new SP_DEVICE_INTERFACE_DATA
                {
                    cbSize = Marshal.SizeOf<SP_DEVICE_INTERFACE_DATA>()
                };

                for (uint i = 0;
                    SetupDiEnumDeviceInterfaces(devSet, IntPtr.Zero, ref hidGuid, i, ref ifaceData);
                    i++)
                {
                    SetupDiGetDeviceInterfaceDetail(devSet, ref ifaceData, IntPtr.Zero, 0, out uint reqSize, IntPtr.Zero);
                    if (reqSize == 0) continue;

                    IntPtr detail = Marshal.AllocHGlobal((int)reqSize);
                    try
                    {
                        Marshal.WriteInt32(detail, IntPtr.Size == 8 ? 8 : 6);
                        if (!SetupDiGetDeviceInterfaceDetail(devSet, ref ifaceData, detail, reqSize, out _, IntPtr.Zero))
                            continue;

                        string? path = Marshal.PtrToStringAuto(IntPtr.Add(detail, 4));
                        if (string.IsNullOrEmpty(path)) continue;

                        if (LooksLikeMagicMouseHid(path))
                            paths.Add(path);
                    }
                    finally { Marshal.FreeHGlobal(detail); }
                }
            }
            finally
            {
                SetupDiDestroyDeviceInfoList(devSet);
            }

            return paths;
        }

        private static bool LooksLikeMagicMouseHid(string path)
        {
            string lower = path.ToLowerInvariant();
            if (lower.Contains("magicmouse")) return true;
            if (lower.Contains("vid_05ac")) return true; // Apple
            return false;
        }

        // ---------- Read loop ----------
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

        private static string Truncate(string s, int max)
            => string.IsNullOrEmpty(s) || s.Length <= max ? s : s.Substring(0, max) + "...";
    }
}
