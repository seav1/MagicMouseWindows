using System;

namespace MagicMouseApp
{
    public class ScrollEngine
    {
        private AppSettings _settings;
        private float _accumX;
        private float _accumY;

        public ScrollEngine(AppSettings settings)
        {
            _settings = settings;
        }

        public void UpdateSettings(AppSettings settings)
        {
            _settings = settings;
        }

        public void ProcessReport(TouchReport report)
        {
            float rawY = report.DeltaY;
            float rawX = report.DeltaX;

            if (rawY == 0 && rawX == 0) return;

            // Apply natural scroll
            if (_settings.NaturalScroll)
            {
                rawY = -rawY;
                rawX = -rawX;
            }

            // Apply speed and acceleration
            float speed = _settings.ScrollSpeed;
            rawY *= speed * _settings.Acceleration;
            rawX *= speed * _settings.Acceleration;

            // Accumulate
            _accumY += rawY;
            _accumX += rawX;

            // Convert to wheel ticks (120 = one standard tick)
            int tickY = (int)(_accumY / 120f);
            int tickX = (int)(_accumX / 120f);

            _accumY -= tickY * 120f;
            _accumX -= tickX * 120f;

            if (tickY != 0)
            {
                MouseInjector.ScrollVertical(tickY);
            }

            if (tickX != 0 && _settings.HorizontalScroll)
            {
                MouseInjector.ScrollHorizontal(tickX);
            }
        }
    }
}
