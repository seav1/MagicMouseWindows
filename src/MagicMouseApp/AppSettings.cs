using System;
using System.IO;
using System.Text.Json;

namespace MagicMouseApp
{
    public class AppSettings
    {
        public float ScrollSpeed    { get; set; } = 3.0f;
        public bool  NaturalScroll  { get; set; } = false;
        public bool  HorizontalScroll { get; set; } = true;
        public bool  StartWithWindows { get; set; } = true;
        public float Acceleration   { get; set; } = 1.0f;

        private static string SettingsPath =>
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "MagicMouseApp", "settings.json");

        public static AppSettings Load()
        {
            try
            {
                if (File.Exists(SettingsPath))
                {
                    var json = File.ReadAllText(SettingsPath);
                    return JsonSerializer.Deserialize<AppSettings>(json) ?? new AppSettings();
                }
            }
            catch { }
            return new AppSettings();
        }

        public void Save()
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(SettingsPath));
                var json = JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true });
                File.WriteAllText(SettingsPath, json);
            }
            catch { }
        }
    }
}
