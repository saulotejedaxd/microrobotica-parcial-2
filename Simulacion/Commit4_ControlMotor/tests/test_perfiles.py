import math
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from perfiles import PERFILES, defaults, value, bounded_value, validate, Telemetry


class ProfilesTest(unittest.TestCase):
    def test_nine_profiles(self):
        self.assertEqual(len(PERFILES), 9)
        for name in PERFILES:
            p = validate(name, defaults(name))
            for i in range(500):
                self.assertTrue(math.isfinite(value(name, p, i * .05)), name)
                self.assertLessEqual(abs(bounded_value(name, p, i*.05, 120)), 120)

    def test_linear_both_directions_and_hold(self):
        p = dict(p0=10, fin=50, a=20)
        self.assertEqual(value('Rampa lineal', p, 1), 30)
        self.assertEqual(value('Rampa lineal', p, 100), 50)
        p['fin'] = -30
        self.assertEqual(value('Rampa lineal', p, 1), -10)
        self.assertEqual(value('Rampa lineal', p, 100), -30)

    def test_trapezoid_continuity_and_repeat(self):
        p = dict(p0=0, pico=100, subida=50, bajada=25, meseta=3)
        for t, expected in [(0, 0), (1, 50), (2, 100), (5, 100), (7, 50), (9, 0), (10, 50)]:
            self.assertAlmostEqual(value('Trapezoidal', p, t), expected)
        p.update(pico=-100)
        self.assertEqual(value('Trapezoidal', p, 7), -50)
        p.update(pico=0, meseta=0)
        self.assertEqual(value('Trapezoidal', p, 50), 0)

    def test_waveforms(self):
        p = dict(p0=20, A=10, f=1, fase=0)
        self.assertAlmostEqual(value('Senoidal', p, .25), 30)
        self.assertAlmostEqual(value('Senoidal', p, .75), 10)
        self.assertEqual(value('Escalonada / cuadrada', dict(p1=10, p2=20, T=2), 1), 20)
        self.assertEqual(value('Diente de sierra', dict(A=100, T=5), 5), 0)
        self.assertEqual(value('Diente de sierra', dict(A=100, T=5), 2.5), 50)

    def test_gaussian_power_noise(self):
        self.assertEqual(value('Gaussiano', dict(A=100, t0=5, sigma=1), 5), 100)
        self.assertEqual(value('Exponencial (foto)', dict(p0=10, alpha=.1, k=2), 10), 40)
        p = defaults('Ruido blanco uniforme')
        self.assertEqual(value('Ruido blanco uniforme', p, 0), value('Ruido blanco uniforme', p, .1))
        for i in range(100):
            v = value('Ruido blanco uniforme', p, i)
            self.assertTrue(40 <= v <= 80)

    def test_bad_parameters(self):
        for bad in ['nan', 'inf', '-inf', 'abc', 281]:
            with self.assertRaises(ValueError):
                validate('Constante', dict(p0=bad))
        with self.assertRaises(ValueError):
            validate('Diente de sierra', dict(A=1, T=0))
        p = defaults('Ruido blanco uniforme')
        p['semilla'] = 1.5
        with self.assertRaises(ValueError):
            validate('Ruido blanco uniforme', p)

    def test_telemetry(self):
        t = Telemetry.parse('T,1,500,2,1,1,0,60,50,49.5,180,-23')
        self.assertEqual(t.rpm, 49.5)
        self.assertEqual(t.angle, 180)
        self.assertEqual(t.pwm, -23)
        for bad in ['T,1', 'T,2,500,2,1,1,0,60,50,49.5,180,-23',
                    'T,1,500,2,1,1,0,nan,50,49,180,23',
                    'T,1,500,9,1,1,0,60,50,49,180,23',
                    'T,1,500,2,1,1,0,60,50,49,180,300']:
            with self.assertRaises(ValueError):
                Telemetry.parse(bad)


if __name__ == '__main__':
    unittest.main()
