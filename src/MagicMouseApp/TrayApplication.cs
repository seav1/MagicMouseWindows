using System;
using System.Drawing;
using System.Threading;
using System.Windows.Forms;

namespace MagicMouseApp
{
    public class TrayApplication : IDisposable
    {
        private NotifyIcon   _trayIcon;
        private DeviceReader _reader;
        private ScrollEngine _scrollEngine;
        private AppSettings  _settings;
        private SettingsForm _settingsForm;
        private string       _status = "Starting...";
        private System.Windows.Forms.Timer _reconnectTimer;

        public TrayApplication()
        {
            _settings     = AppSettings.Load();
            _scrollEngine = new ScrollEngine(_settings);

            InitTrayIcon();
            InitDevice();
        }

        private void InitTrayIcon()
        {
            var menu = new ContextMenuStrip();
            menu.BackColor = Color.FromArgb(44, 44, 46);
            menu.ForeColor = Color.White;

            var titleItem = new ToolStripMenuItem("🖱  Magic Mouse") { Enabled = false };
            titleItem.Font = new Font("Segoe UI", 10f, FontStyle.Bold);

            var statusItem = new ToolStripMenuItem("● Connecting...") { Enabled = false };
            statusItem.Font = new Font("Segoe UI", 9f);

            var settingsItem = new ToolStripMenuItem("Settings...");
            settingsItem.Click += (s, e) => ShowSettings();

            var diagnosticsItem = new ToolStripMenuItem("Diagnostics...");
            diagnosticsItem.Click += (s, e) => ShowDiagnostics();

            var reconnectItem = new ToolStripMenuItem("Reconnect");
            reconnectItem.Click += (s, e) => ForceReconnect();

            var separator = new ToolStripSeparator();

            var exitItem = new ToolStripMenuItem("Quit");
            exitItem.Click += (s, e) => ExitApp();

            menu.Items.Add(titleItem);
            menu.Items.Add(statusItem);
            menu.Items.Add(new ToolStripSeparator());
            menu.Items.Add(settingsItem);
            menu.Items.Add(diagnosticsItem);
            menu.Items.Add(reconnectItem);
            menu.Items.Add(separator);
            menu.Items.Add(exitItem);

            // Create icon programmatically (circle)
            var bmp = new Bitmap(16, 16);
            using (var g = Graphics.FromImage(bmp))
            {
                g.Clear(Color.Transparent);
                g.FillEllipse(Brushes.White, 2, 2, 12, 12);
                g.FillEllipse(new SolidBrush(Color.FromArgb(10, 132, 255)), 4, 4, 8, 8);
            }

            _trayIcon = new NotifyIcon
            {
                Icon             = Icon.FromHandle(bmp.GetHicon()),
                ContextMenuStrip = menu,
                Visible          = true,
                Text             = "Magic Mouse"
            };

            _trayIcon.DoubleClick += (s, e) => ShowSettings();

            // Update status in menu
            var updateTimer = new System.Windows.Forms.Timer { Interval = 1000 };
            updateTimer.Tick += (s, e) =>
            {
                statusItem.Text = $"● {_status}";
                statusItem.ForeColor = _status.StartsWith("Connected")
                    ? Color.FromArgb(48, 209, 88)
                    : Color.FromArgb(255, 159, 10);
            };
            updateTimer.Start();
        }

        private void InitDevice()
        {
            _reader = new DeviceReader();

            _reader.StatusChanged += status =>
            {
                _status = status;
                _settingsForm?.UpdateStatus(status);
            };

            _reader.ReportReceived += report =>
            {
                _scrollEngine.ProcessReport(report);
            };

            _reader.DeviceDisconnected += () =>
            {
                _status = "Disconnected - reconnecting...";
                ScheduleReconnect();
            };

            Connect();
        }

        private void Connect()
        {
            ThreadPool.QueueUserWorkItem(_ =>
            {
                if (_reader.Open())
                {
                    _status = "Connected - Magic Mouse active";
                    _reader.StartReading();
                }
                else
                {
                    _status = "Not connected";
                    ScheduleReconnect();
                }
            });
        }

        private void ScheduleReconnect()
        {
            if (_reconnectTimer != null) return;

            _reconnectTimer = new System.Windows.Forms.Timer { Interval = 3000 };
            _reconnectTimer.Tick += (s, e) =>
            {
                _reconnectTimer.Stop();
                _reconnectTimer.Dispose();
                _reconnectTimer = null;

                _reader = new DeviceReader();
                _reader.StatusChanged      += status => { _status = status; _settingsForm?.UpdateStatus(status); };
                _reader.ReportReceived     += report => _scrollEngine.ProcessReport(report);
                _reader.DeviceDisconnected += () => { _status = "Disconnected"; ScheduleReconnect(); };
                Connect();
            };
            _reconnectTimer.Start();
        }

        private void ShowSettings()
        {
            if (_settingsForm != null && !_settingsForm.IsDisposed)
            {
                _settingsForm.BringToFront();
                return;
            }

            _settingsForm = new SettingsForm(_settings, _status);
            _settingsForm.SettingsChanged += newSettings =>
            {
                _settings = newSettings;
                _scrollEngine.UpdateSettings(_settings);
            };
            _settingsForm.Show();
        }

        private void ShowDiagnostics()
        {
            string report;
            try
            {
                report = _reader != null
                    ? _reader.BuildDiagnostics()
                    : "Reader not initialized.";
            }
            catch (Exception ex)
            {
                report = $"Failed to gather diagnostics: {ex}";
            }

            using var form = new Form
            {
                Text            = "Magic Mouse - Diagnostics",
                Size            = new Size(720, 520),
                StartPosition   = FormStartPosition.CenterScreen,
                BackColor       = Color.FromArgb(28, 28, 30),
                ForeColor       = Color.White
            };

            var textBox = new TextBox
            {
                Multiline    = true,
                ReadOnly     = true,
                ScrollBars   = ScrollBars.Both,
                WordWrap     = false,
                Dock         = DockStyle.Fill,
                Font         = new Font("Consolas", 9.5f),
                BackColor    = Color.FromArgb(28, 28, 30),
                ForeColor    = Color.FromArgb(220, 220, 220),
                BorderStyle  = BorderStyle.None,
                Text         = report
            };

            var copyBtn = new Button
            {
                Text      = "Copy to clipboard",
                Dock      = DockStyle.Bottom,
                Height    = 36,
                FlatStyle = FlatStyle.Flat,
                BackColor = Color.FromArgb(10, 132, 255),
                ForeColor = Color.White
            };
            copyBtn.FlatAppearance.BorderSize = 0;
            copyBtn.Click += (s, e) =>
            {
                try { Clipboard.SetText(report); copyBtn.Text = "Copied"; } catch { }
            };

            form.Controls.Add(textBox);
            form.Controls.Add(copyBtn);
            form.ShowDialog();
        }

        private void ForceReconnect()
        {
            try { _reader?.Stop(); } catch { }
            _status = "Reconnecting...";

            if (_reconnectTimer != null)
            {
                _reconnectTimer.Stop();
                _reconnectTimer.Dispose();
                _reconnectTimer = null;
            }

            _reader = new DeviceReader();
            _reader.StatusChanged      += status => { _status = status; _settingsForm?.UpdateStatus(status); };
            _reader.ReportReceived     += report => _scrollEngine.ProcessReport(report);
            _reader.DeviceDisconnected += () => { _status = "Disconnected"; ScheduleReconnect(); };
            Connect();
        }

        private void ExitApp()
        {
            _reader?.Stop();
            _trayIcon.Visible = false;
            Application.Exit();
        }

        public void Dispose()
        {
            _reader?.Stop();
            _trayIcon?.Dispose();
        }
    }
}
