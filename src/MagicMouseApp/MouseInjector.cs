using System;
using System.Runtime.InteropServices;

namespace MagicMouseApp
{
    public static class MouseInjector
    {
        [DllImport("user32.dll", SetLastError = true)]
        private static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);

        [DllImport("user32.dll")]
        private static extern IntPtr GetMessageExtraInfo();

        private const uint INPUT_MOUSE = 0;
        private const uint MOUSEEVENTF_WHEEL  = 0x0800;
        private const uint MOUSEEVENTF_HWHEEL = 0x1000;
        private const int  WHEEL_DELTA        = 120;

        [StructLayout(LayoutKind.Sequential)]
        private struct MOUSEINPUT
        {
            public int     dx;
            public int     dy;
            public uint    mouseData;
            public uint    dwFlags;
            public uint    time;
            public IntPtr  dwExtraInfo;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct INPUT
        {
            public uint type;
            public MOUSEINPUT mi;
        }

        public static void ScrollVertical(int ticks)
        {
            var input = new INPUT
            {
                type = INPUT_MOUSE,
                mi = new MOUSEINPUT
                {
                    dwFlags   = MOUSEEVENTF_WHEEL,
                    mouseData = (uint)(ticks * WHEEL_DELTA),
                    dwExtraInfo = GetMessageExtraInfo()
                }
            };
            SendInput(1, new[] { input }, Marshal.SizeOf(typeof(INPUT)));
        }

        public static void ScrollHorizontal(int ticks)
        {
            var input = new INPUT
            {
                type = INPUT_MOUSE,
                mi = new MOUSEINPUT
                {
                    dwFlags   = MOUSEEVENTF_HWHEEL,
                    mouseData = (uint)(ticks * WHEEL_DELTA),
                    dwExtraInfo = GetMessageExtraInfo()
                }
            };
            SendInput(1, new[] { input }, Marshal.SizeOf(typeof(INPUT)));
        }
    }
}
