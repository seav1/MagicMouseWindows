using System;
using System.Drawing;
using System.Windows.Forms;

namespace MagicMouseApp
{
    public class SettingsForm : Form
    {
        private AppSettings _settings;
        public event Action<AppSettings> SettingsChanged;

        private TrackBar  _speedSlider;
        private Label     _speedLabel;
        private CheckBox  _naturalScrollCheck;
        private CheckBox  _horizontalScrollCheck;
        private CheckBox  _startWithWindowsCheck;
        private TrackBar  _accelSlider;
        private Label     _accelLabel;
        private Label     _statusLabel;
        private Button    _saveBtn;
        private Panel     _headerPanel;

        public SettingsForm(AppSettings settings, string status)
        {
            _settings = settings;
            InitUI();
            UpdateStatus(status);
        }

        private void InitUI()
        {
            Text            = "Magic Mouse Settings";
            Size            = new Size(420, 520);
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox     = false;
            StartPosition   = FormStartPosition.CenterScreen;
            BackColor       = Color.FromArgb(28, 28, 30);
            ForeColor       = Color.White;
            Font            = new Font("Segoe UI", 10f);

            // Header
            _headerPanel = new Panel
            {
                Dock      = DockStyle.Top,
                Height    = 70,
                BackColor = Color.FromArgb(44, 44, 46)
            };

            var titleLabel = new Label
            {
                Text      = "🖱  Magic Mouse",
                Font      = new Font("Segoe UI", 16f, FontStyle.Bold),
                ForeColor = Color.White,
                Location  = new Point(20, 10),
                AutoSize  = true
            };

            var subtitleLabel = new Label
            {
                Text      = "USB-C Scroll Controller",
                Font      = new Font("Segoe UI", 9f),
                ForeColor = Color.FromArgb(142, 142, 147),
                Location  = new Point(22, 42),
                AutoSize  = true
            };

            _headerPanel.Controls.Add(titleLabel);
            _headerPanel.Controls.Add(subtitleLabel);

            // Status bar
            _statusLabel = new Label
            {
                Text      = "Connecting...",
                ForeColor = Color.FromArgb(48, 209, 88),
                Location  = new Point(20, 85),
                AutoSize  = true,
                Font      = new Font("Segoe UI", 9f)
            };

            // Scroll Speed
            var speedTitle = MakeLabel("Scroll Speed", 120);
            _speedLabel = MakeLabel($"{_settings.ScrollSpeed:F1}x", 120);
            _speedLabel.Left = 340;

            _speedSlider = new TrackBar
            {
                Location = new Point(20, 140),
                Width    = 370,
                Minimum  = 1,
                Maximum  = 100,
                Value    = (int)(_settings.ScrollSpeed * 10),
                TickFrequency = 10,
                BackColor = Color.FromArgb(28, 28, 30)
            };
            _speedSlider.ValueChanged += (s, e) =>
            {
                _settings.ScrollSpeed = _speedSlider.Value / 10f;
                _speedLabel.Text = $"{_settings.ScrollSpeed:F1}x";
            };

            // Acceleration
            var accelTitle = MakeLabel("Acceleration", 185);
            _accelLabel = MakeLabel($"{_settings.Acceleration:F1}x", 185);
            _accelLabel.Left = 340;

            _accelSlider = new TrackBar
            {
                Location = new Point(20, 205),
                Width    = 370,
                Minimum  = 1,
                Maximum  = 30,
                Value    = (int)(_settings.Acceleration * 10),
                TickFrequency = 5,
                BackColor = Color.FromArgb(28, 28, 30)
            };
            _accelSlider.ValueChanged += (s, e) =>
            {
                _settings.Acceleration = _accelSlider.Value / 10f;
                _accelLabel.Text = $"{_settings.Acceleration:F1}x";
            };

            // Checkboxes
            _naturalScrollCheck = MakeCheckbox("Natural Scroll (macOS style)", 270,
                _settings.NaturalScroll);
            _horizontalScrollCheck = MakeCheckbox("Horizontal Scroll", 310,
                _settings.HorizontalScroll);
            _startWithWindowsCheck = MakeCheckbox("Start with Windows", 350,
                _settings.StartWithWindows);

            // Save button
            _saveBtn = new Button
            {
                Text      = "Save Settings",
                Location  = new Point(20, 410),
                Width     = 370,
                Height    = 45,
                BackColor = Color.FromArgb(10, 132, 255),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
                Font      = new Font("Segoe UI", 11f, FontStyle.Bold),
                Cursor    = Cursors.Hand
            };
            _saveBtn.FlatAppearance.BorderSize = 0;
            _saveBtn.Click += SaveBtn_Click;

            Controls.Add(_headerPanel);
            Controls.Add(_statusLabel);
            Controls.Add(speedTitle);
            Controls.Add(_speedLabel);
            Controls.Add(_speedSlider);
            Controls.Add(accelTitle);
            Controls.Add(_accelLabel);
            Controls.Add(_accelSlider);
            Controls.Add(_naturalScrollCheck);
            Controls.Add(_horizontalScrollCheck);
            Controls.Add(_startWithWindowsCheck);
            Controls.Add(_saveBtn);
        }

        private Label MakeLabel(string text, int top)
        {
            return new Label
            {
                Text      = text,
                ForeColor = Color.FromArgb(209, 209, 214),
                Location  = new Point(20, top),
                AutoSize  = true,
                Font      = new Font("Segoe UI", 10f)
            };
        }

        private CheckBox MakeCheckbox(string text, int top, bool isChecked)
        {
            var cb = new CheckBox
            {
                Text      = text,
                Checked   = isChecked,
                ForeColor = Color.FromArgb(209, 209, 214),
                Location  = new Point(20, top),
                AutoSize  = true,
                Font      = new Font("Segoe UI", 10f)
            };
            return cb;
        }

        private void SaveBtn_Click(object sender, EventArgs e)
        {
            _settings.NaturalScroll     = _naturalScrollCheck.Checked;
            _settings.HorizontalScroll  = _horizontalScrollCheck.Checked;
            _settings.StartWithWindows  = _startWithWindowsCheck.Checked;
            _settings.Save();

            // Apply startup setting
            SetStartup(_settings.StartWithWindows);

            SettingsChanged?.Invoke(_settings);

            _saveBtn.Text = "✓ Saved";
            var t = new System.Windows.Forms.Timer { Interval = 1500 };
            t.Tick += (s, ev) => { _saveBtn.Text = "Save Settings"; t.Stop(); };
            t.Start();
        }

        private void SetStartup(bool enable)
        {
            try
            {
                var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(
                    @"SOFTWARE\Microsoft\Windows\CurrentVersion\Run", true);
                if (enable)
                    key?.SetValue("MagicMouseApp",
                        $"\"{System.Reflection.Assembly.GetExecutingAssembly().Location}\"");
                else
                    key?.DeleteValue("MagicMouseApp", false);
            }
            catch { }
        }

        public void UpdateStatus(string status)
        {
            if (InvokeRequired)
            {
                Invoke(new Action(() => UpdateStatus(status)));
                return;
            }
            _statusLabel.Text = $"● {status}";
            _statusLabel.ForeColor = status.Contains("Connected")
                ? Color.FromArgb(48, 209, 88)
                : Color.FromArgb(255, 159, 10);
        }
    }
}
