using System;
using System.Windows.Forms;

namespace MagicMouseApp
{
    static class Program
    {
        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            
            // Run as system tray app
            using (var trayApp = new TrayApplication())
            {
                Application.Run();
            }
        }
    }
}
