"""Perfiles de velocidad en RPM; tiempo en segundos. Sin dependencias externas."""
import math
import random
from dataclasses import dataclass

# nombre, parámetros (clave, etiqueta, valor, mínimo, máximo), explicación
PERFILES = {
    'Constante': ([('p0', 'Velocidad (RPM)', 60, -280, 280)],
                  'Velocidad fija: p(t) = p0.'),
    'Rampa lineal': ([('p0', 'Inicio (RPM)', 0, -280, 280),
                      ('fin', 'Final (RPM)', 100, -280, 280),
                      ('a', 'Pendiente, magnitud (RPM/s)', 20, 0.1, 1000)],
                     'Sube o baja hasta la velocidad final y la mantiene.'),
    'Trapezoidal': ([('p0', 'Base (RPM)', 0, -280, 280),
                     ('pico', 'Meseta (RPM)', 100, -280, 280),
                     ('subida', 'Pendiente de ida (RPM/s)', 40, 0.1, 1000),
                     ('bajada', 'Pendiente de vuelta (RPM/s)', 40, 0.1, 1000),
                     ('meseta', 'Tiempo en meseta (s)', 3, 0, 3600)],
                    'Va de base a meseta, mantiene y vuelve a base. Repite el ciclo.'),
    'Senoidal': ([('p0', 'Centro (RPM)', 60, -280, 280),
                  ('A', 'Amplitud (RPM)', 40, 0, 280),
                  ('f', 'Frecuencia (Hz)', 0.1, 0.001, 2),
                  ('fase', 'Fase (grados)', 0, -360, 360)],
                 'p0 + A·sen(2πft + fase). Puede invertir el giro si cruza cero.'),
    'Escalonada / cuadrada': ([('p1', 'Nivel 1 (RPM)', 30, -280, 280),
                              ('p2', 'Nivel 2 (RPM)', 100, -280, 280),
                              ('T', 'Periodo (s)', 6, 0.5, 3600)],
                             'Alterna los niveles cada medio periodo.'),
    'Exponencial (foto)': ([('p0', 'Inicio (RPM)', 10, -280, 280),
                            ('alpha', 'Alfa (1/s)', 0.1, 0, 10),
                            ('k', 'Exponente k', 2, 0.05, 8)],
                           'Fórmula de tu foto: p0·(1 + alfa·t)^k. Matemáticamente es una potencia.'),
    'Gaussiano': ([('A', 'Amplitud (RPM)', 100, -280, 280),
                   ('t0', 'Centro del pulso (s)', 5, 0, 3600),
                   ('sigma', 'Ancho sigma (s)', 1.5, 0.1, 3600)],
                  'A·exp(-((t-t0)/sigma)²), igual a la fórmula de la foto.'),
    'Diente de sierra': ([('A', 'Amplitud (RPM)', 100, -280, 280),
                          ('T', 'Periodo (s)', 5, 0.5, 3600)],
                         'Sube de cero a A y reinicia cada periodo.'),
    'Ruido blanco uniforme': ([('p0', 'Centro (RPM)', 60, -280, 280),
                               ('A', 'Amplitud (RPM)', 20, 0, 280),
                               ('intervalo', 'Tiempo entre muestras (s)', 0.2, 0.05, 60),
                               ('semilla', 'Semilla entera', 1, 0, 1000000)],
                              'p0 + U(-A,A). Ruido muestreado; la semilla permite repetir la prueba.'),
}


def defaults(name):
    return {key: value for key, _, value, _, _ in PERFILES[name][0]}


def validate(name, params):
    if name not in PERFILES:
        raise ValueError('Selecciona un perfil válido.')
    result = {}
    for key, label, _, low, high in PERFILES[name][0]:
        try:
            v = float(str(params[key]).replace(',', '.'))
        except (KeyError, TypeError, ValueError):
            raise ValueError(f'{label}: escribe un número válido.') from None
        if not math.isfinite(v) or not low <= v <= high:
            raise ValueError(f'{label}: debe estar entre {low} y {high}.')
        if key == 'semilla' and v != int(v):
            raise ValueError('La semilla debe ser un número entero.')
        result[key] = v
    return result


def value(name, p, t):
    t = max(0.0, t)
    if name == 'Constante':
        return p['p0']
    if name == 'Rampa lineal':
        d = p['fin'] - p['p0']
        return p['p0'] + math.copysign(min(abs(d), p['a'] * t), d)
    if name == 'Trapezoidal':
        d = p['pico'] - p['p0']
        up, down = abs(d) / p['subida'], abs(d) / p['bajada']
        cycle = up + p['meseta'] + down
        if cycle == 0:
            return p['p0']
        x = t % cycle
        if x < up:
            return p['p0'] + math.copysign(p['subida'] * x, d)
        if x < up + p['meseta']:
            return p['pico']
        return p['pico'] - math.copysign(p['bajada'] * (x-up-p['meseta']), d)
    if name == 'Senoidal':
        return p['p0'] + p['A'] * math.sin(2*math.pi*p['f']*t + math.radians(p['fase']))
    if name == 'Escalonada / cuadrada':
        return p['p1'] if t % p['T'] < p['T']/2 else p['p2']
    if name == 'Exponencial (foto)':
        return p['p0'] * (1 + p['alpha']*t)**p['k']
    if name == 'Gaussiano':
        return p['A'] * math.exp(-((t-p['t0'])/p['sigma'])**2)
    if name == 'Diente de sierra':
        return p['A'] * ((t/p['T']) % 1)
    if name == 'Ruido blanco uniforme':
        i = math.floor(t / p['intervalo'])
        return p['p0'] + random.Random(int(p['semilla']) + i).uniform(-p['A'], p['A'])
    raise ValueError('Perfil desconocido')


def bounded_value(name, params, t, limit):
    return max(-limit, min(limit, value(name, params, t)))


@dataclass(frozen=True)
class Telemetry:
    ms: int
    mode: int
    state: int
    home: bool
    fault: bool
    requested: float
    limited: float
    rpm: float
    angle: float
    pwm: int

    @classmethod
    def parse(cls, line):
        p = line.strip().split(',')
        if len(p) != 12 or p[:2] != ['T', '1']:
            raise ValueError('Trama desconocida')
        ms, mode, state, home, fault = map(int, p[2:7])
        values = list(map(float, p[7:11]))
        pwm = int(p[11])
        if (not all(map(math.isfinite, values)) or mode not in range(4)
                or state not in range(3) or home not in (0, 1)
                or fault not in (0, 1) or abs(pwm) > 255 or ms < 0):
            raise ValueError('Telemetría inválida')
        return cls(ms, mode, state, bool(home), bool(fault), *values, pwm)
