using System;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.Win32.SafeHandles;

namespace MagicMouseApp
{
    public class TouchReport
    {
        public byte  ReportId;
        public sbyte DeltaX;
        public sbyte DeltaY;
        public byte[] Raw;
    }

    public class DeviceReader : IDisposable
    {
        // Win32 imports
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
        private static extern bool DeviceIoControl(
            SafeFileHandle hDevice, uint dwIoControlCode,
            byte[] lpInBuffer, uint nInBufferSize,
            byte[] lpOutBuffer, uint nOutBufferSize,
            out uint lpBytesReturned, IntPtr lpOverlapped);

        [DllImport("hid.dll")]
        private static extern bool HidD_GetPreparsedData(SafeFileHandle hDevice, out IntPtr PreparsedData);

        [DllImport("hid.dll")]
        private static extern bool HidD_FreePreparsedData(IntPtr PreparsedData);

        [DllImport("hid.dll")]
        private static extern int HidP_GetCaps(IntPtr PreparsedData, out HIDP_CAPS Capabilities);

        [DllImport("hid.dll")]
        private static extern bool HidD_SetFeature(SafeFileHandle hDevice, byte[] ReportBuffer, uint ReportBufferLength);

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

        private const uint GENERIC_READ       = 0x80000000;
        private const uint GENERIC_WRITE      = 0x40000000;
        private const uint FILE_SHARE_READ    = 0x00000001;
        private const uint FILE_SHARE_WRITE   = 0x00000002;
        private const uint OPEN_EXISTING      = 3;
        private const uint FILE_ATTRIBUTE_NORMAL = 0x80;

        private SafeFileHandle _handle;
        private Thread         _readThread;
        private bool           _running;
        private int            _reportLen = 8; // default, updated after open

        public event Action<TouchReport> ReportReceived;
        public event Action<string>      StatusChanged;
        public event Action              DeviceDisconnected;

        // Known device paths for Magic Mouse driver PDO
        private static readonly string[] KnownPaths = new[]
        {
            // Magic Utilities exposes this interface GUID
            @"\\.\MagicMouse",
            @"\\.\MagicMouseRawPDO",
        };

        public bool Open()
        {
            // Try known named paths first
            foreach (var path in KnownPaths)
            {
                if (TryOpen(path)) return true;
            }

            // Try finding via SetupDi
            var found = FindDevicePath();
            if (found != null && TryOpen(found)) return true;

            StatusChanged?.Invoke("Device not found - is Magic Mouse connected?");
            return false;
        }

        private bool TryOpen(string path)
        {
            try
            {
                var h = CreateFile(path,
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    IntPtr.Zero, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, IntPtr.Zero);

                if (h == null || h.IsInvalid)
                {
                    h = CreateFile(path,
                        GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        IntPtr.Zero, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, IntPtr.Zero);
                }

                if (h == null || h.IsInvalid) return false;

                _handle = h;

                // Get report length
                IntPtr preparsed;
                if (HidD_GetPreparsedData(_handle, out preparsed))
                {
                    HIDP_CAPS caps;
                    HidP_GetCaps(preparsed, out caps);
                    if (caps.InputReportByteLength > 0)
                        _reportLen = caps.InputReportByteLength;
                    HidD_FreePreparsedData(preparsed);
                }

                StatusChanged?.Invoke($"Connected via {path} (report={_reportLen}b)");
                return true;
            }
            catch
            {
                return false;
            }
        }

        private string FindDevicePath()
        {
            // Try the PDO name we found: \Device\00000416
            // Access via \\.\GlobalRoot\Device\00000416
            var pdoPath = @"\\.\GlobalRoot\Device\00000416";
            return pdoPath;
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

        private void ReadLoop()
        {
            var buf = new byte[Math.Max(_reportLen, 64)];

            while (_running)
            {
                try
                {
                    uint bytesRead = 0;
                    bool ok = ReadFile(_handle, buf, (uint)buf.Length, out bytesRead, IntPtr.Zero);

                    if (!ok || bytesRead == 0)
                    {
                        int err = Marshal.GetLastWin32Error();
                        if (err == 6 || err == 1167) // ERROR_INVALID_HANDLE or device disconnected
                        {
                            DeviceDisconnected?.Invoke();
                            break;
                        }
                        Thread.Sleep(10);
                        continue;
                    }

                    var report = ParseReport(buf, (int)bytesRead);
                    if (report != null)
                    {
                        ReportReceived?.Invoke(report);
                    }
                }
                catch (Exception ex)
                {
                    StatusChanged?.Invoke($"Read error: {ex.Message}");
                    Thread.Sleep(100);
                }
            }
        }

        private TouchReport ParseReport(byte[] buf, int len)
        {
            if (len < 3) return null;

            var raw = new byte[len];
            Array.Copy(buf, raw, len);

            return new TouchReport
            {
                ReportId = buf[0],
                DeltaY   = (sbyte)buf[1],
                DeltaX   = (sbyte)buf[2],
                Raw      = raw
            };
        }

        public void Stop()
        {
            _running = false;
            _handle?.Close();
        }

        public void Dispose()
        {
            Stop();
        }
    }
}
