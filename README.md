# Microrobótica — Parcial 2

Control de posición y velocidad de un motorreductor GA25-370 con encoder de
cuadratura, Arduino UNO, driver L298N, potenciómetro y pantalla LCD I2C.
Repositorio organizado en Firmware, Simulacion, README.md y .gitignore.

## Organización

```text
microrobotica-parcial-2/
├── Firmware/
│   └── control_motor/
├── Simulacion/
├── README.md
└── .gitignore
```

`Firmware` contiene las versiones Arduino, identificadas con los nombres de
las cuatro carpetas entregadas. `Simulacion` contiene las herramientas de PC
cuando están incluidas en la versión: perfiles matemáticos, vista previa,
gráficas y pruebas. No se recibió un proyecto de simulación mecánica o de circuito.

## Historial de entregas

| Commit / etiqueta | Carpeta de origen | Contenido |
|---|---|---|
| 1 | `control_motor` | Control inicial con potenciómetro, LCD y encoder; conserva la configuración original de esta versión. |

Los commits se incorporaron en ese orden, con etiquetas `v1` a `v1`.
Son instantáneas importadas de las carpetas proporcionadas: las fechas de Git
corresponden a la importación, no a fechas de desarrollo reconstruidas.
Las versiones anteriores permanecen disponibles en sus carpetas y en el historial.
El código fuente se conserva sin modificaciones.

## Componentes

- Arduino UNO R3.
- Motorreductor de 12 V con encoder Hall de cuadratura.
- Driver L298N y fuente externa para el motor, con GND común.
- Potenciómetro lineal de 5 kΩ y LCD 16×2 I2C.
- Pulsadores físicos; las versiones con HOME usan un tercer botón.

## Conexiones de las versiones con HOME

| Arduino | Conexión |
|---|---|
| D2 / D3 | Encoder A / B |
| D4 | Botón de modo a GND |
| D5 | ENA del L298N, sin jumper ENA |
| D7 | Botón marcha/paro y guardar cero a GND |
| D8 / D9 | IN1 / IN2 del L298N |
| D10 | Botón regresar a HOME a GND |
| A0 | Cursor del potenciómetro; extremos a 5V y GND |
| A4 / A5 | SDA / SCL del LCD I2C |

D5 es una señal PWM: no se conecta al pin de alimentación 5V. El motor usa su
fuente externa; los GPIO no alimentan el motor. Revisa el encabezado de cada
sketch porque la primera entrega no tiene el mismo conjunto de funciones.

## Firmware Arduino

Instala la biblioteca **hd44780 de Bill Perry** desde el gestor de bibliotecas.
Abre únicamente el sketch de la versión elegida; selecciona Arduino UNO y
el puerto correcto en Arduino IDE. No mezcles varios sketches de entregas
distintas en una sola carpeta de compilación.
