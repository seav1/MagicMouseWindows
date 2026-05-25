using System;
using System.Threading;
using System.Windows.Forms;

namespace MagicMouseApp
{
    static class Program
    {
        // Single-instance guard so users don't accidentally run two trays at once.
        private static Mutex? _singleInstanceMutex;

        [STAThread]
        static void Main()
        {
            _singleInstanceMutex = new Mutex(true, "Global\\MagicMouseApp_SingleInstance", out bool createdNew);
            if (!createdNew)
            {
                // Already running - just exit silently.
                return;
            }

            // Order matters: SetHighDpiMode must be called before any UI is created.
            // PerMonitorV2 works on Windows 10 (1703+) and Windows 11.
            try { Application.SetHighDpiMode(HighDpiMode.PerMonitorV2); } catch { /* older runtime */ }

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);

            using var trayApp = new TrayApplication();
            Application.Run();

            GC.KeepAlive(_singleInstanceMutex);
        }
    }
}
