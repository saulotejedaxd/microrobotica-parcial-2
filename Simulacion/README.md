# Simulacion

Esta sección reúne el generador matemático de perfiles y la interfaz de PC.
La vista previa calcula la referencia; las gráficas en vivo muestran datos del
encoder. No es un modelo dinámico del motor ni una simulación de circuito.

Las dos primeras carpetas recibidas no incluyen un proyecto de simulación.

## Ejecutar la entrega disponible

Abre [Commit4_ControlMotor/instalar_y_abrir.bat](Commit4_ControlMotor/instalar_y_abrir.bat) en Windows.
El script prepara un entorno Python local e instala pyserial la primera vez.
El firmware correspondiente está en
[`../Firmware/Commit4_ControlMotor/motor_perfiles/motor_perfiles.ino`](../Firmware/Commit4_ControlMotor/motor_perfiles/motor_perfiles.ino).

Los archivos LEEME.md de cada entrega se conservan como documentos históricos:
sus rutas `arduino/...` corresponden al paquete original. Usa las rutas de este
README o del README principal para esta organización del repositorio.

Para ejecutar las pruebas de perfiles desde la raíz:

```console
python -m unittest discover -s Simulacion/Commit4_ControlMotor/tests -v
```
