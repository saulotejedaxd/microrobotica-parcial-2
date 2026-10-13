# Motor Lab: Arduino UNO + panel para PC

Incluye los nueve perfiles de las fotos, gráficas de velocidad en vivo, vista previa y exportación CSV. Conserva el potenciómetro, LCD, encoder de 880 cuentas/vuelta y botones del último programa completo del chat. No hace falta agregar componentes.

## 1. Cargar Arduino (una vez)

1. Abre `arduino/motor_perfiles/motor_perfiles.ino` en Arduino IDE.
2. Instala la biblioteca **hd44780 de Bill Perry** desde el gestor de bibliotecas, si no la tienes.
3. Selecciona **Arduino UNO** y su puerto; carga el programa. Este archivo sustituye al sketch anterior completo.
4. **Cierra el Monitor Serie y Serial Plotter** antes de conectar el panel.

El cableado sigue igual: encoder A D2 y B D3; D4 modo; D5 ENA del L298N sin jumper ENA; D7 marcha/paro/cero; IN1 D8; IN2 D9; D10 HOME; potenciómetro A0; LCD SDA A4 y SCL A5. Botones entre su pin y GND. El motor conserva su fuente externa y GND común. D5 no se conecta a 5V.

## 2. Abrir las gráficas

1. Extrae TODO el ZIP a una carpeta. No abras el programa dentro del ZIP.
2. Ejecuta **instalar_y_abrir.bat**. Usa Python 3 con Tk y crea un entorno `.venv` dentro de esta carpeta. La primera vez instala pyserial con internet; después funciona localmente sin internet.
3. Elige el puerto USB, pulsa **Conectar** y espera el mensaje **Arduino listo**.
4. Abrir el puerto puede reiniciar el UNO. Si HOME aparece sin definir, vuelve a guardar el cero. No se reanuda una prueba automáticamente al conectar.

Si falta Python, el lanzador muestra dónde instalarlo. No se requiere matplotlib, navegador ni una cuenta. El panel no sube el sketch: eso se hace en Arduino IDE.

## 3. Funciones físicas conservadas

| Control | Función |
|---|---|
| Potenciómetro, POS | Referencia de 0 a 360 grados |
| Potenciómetro, VEL | Velocidad y sentido; centro = cero |
| D4 | Detiene y alterna POS/VEL |
| D7 corto | Arranca/detiene en físico; durante un perfil PC lo detiene y devuelve el mando al control físico |
| D7 largo (1.5 s), empezando detenido | Guarda la posición actual como cero; soltar después no arranca |
| D10 | Regresa al cero guardado y se detiene |
| LCD | Referencia R, medida Y, estado y HOME |

El botón **Volver al control físico** detiene y deja seleccionado el último modo físico. Se arranca con D7. En físico, el panel observa y grafica; sus parámetros de aceleración no cambian el control del potenciómetro. Puedes usarlo sin la PC conectada.

Durante un perfil PC, el potenciómetro no modifica la referencia. D4 y D10 cancelan el perfil y ejecutan su acción física. D7 detiene. Una orden de referencia que quedó en tránsito no vuelve a arrancar el motor. Estando detenido con PC aún seleccionado, D7 indica iniciar desde el panel; selecciona control físico para arrancar con D7.

HOME usa el encoder incremental: debes establecerlo después de cada reinicio. No descubre por sí mismo una marca mecánica. No se guarda una posición absoluta durante apagados. Al terminar HOME corta potencia; no mantiene fuerza de sujeción. Se conservan el margen POS de 2 grados y HOME de 3 cuentas del último programa completo.

## 4. Probar un perfil

1. Selecciona **Constante** y comienza, por ejemplo, con **30 RPM**, límite **60 RPM**, aceleración/desaceleración **60 RPM/s** y duración **10 s**.
2. Pulsa **Previsualizar cambios** para ver la señal prevista sin mover nada.
3. Pulsa **INICIAR perfil desde PC**. El motor no necesita HOME para ensayos de velocidad, pero sí para regresar a cero.
4. Para cambiar de perfil o parámetros durante la prueba, edítalos y pulsa **Aplicar en vivo / reiniciar curva**. Esto reinicia el tiempo de la curva; conserva la velocidad de referencia actual y llega a la nueva mediante la rampa.
5. Al cumplirse la duración, la referencia desacelera hasta cero y la prueba termina. **PARAR**, Escape y D7 cortan la potencia de inmediato, sin rampa; el eje puede seguir moviéndose por inercia.

Las unidades son **RPM** y **RPM/s**, porque tu motor DC no usa pasos de un motor paso a paso. 280 RPM es el máximo de referencia permitido, no una velocidad física garantizada. El límite inicial del panel es 120 RPM; puedes subirlo hasta 280.

## 5. Los nueve perfiles

| Perfil | Parámetros y comportamiento |
|---|---|
| Constante | Velocidad fija p0 |
| Rampa lineal | Inicio, final y pendiente; llega al final y lo mantiene |
| Trapezoidal | Base, meseta, pendientes de ida/vuelta y tiempo en meseta; repite el ciclo |
| Senoidal | Centro, amplitud, frecuencia y fase en grados |
| Escalonada/cuadrada | Dos velocidades y periodo; alterna cada medio periodo |
| Exponencial (foto) | p0·(1+alfa·t)^k; se implementa literalmente tu fórmula, que matemáticamente es una potencia |
| Gaussiano | A·exp(-((t-t0)/sigma)^2); amplitud, centro y ancho |
| Diente de sierra | Amplitud y periodo; reinicia a cero cada periodo |
| Ruido blanco uniforme | Centro, amplitud, intervalo de muestreo y semilla reproducible |

Todos se recortan al límite de RPM elegido. Valores negativos invierten el giro. El ruido está muestreado: no es ruido blanco continuo ideal. Senoidal, cuadrada, sierra y ruido se ejecutan durante la duración elegida; el pulso gaussiano usa el tiempo desde el inicio. La rampa y la potencia dejan de crecer al alcanzar el límite de velocidad.

El límite general de aceleración también afecta a perfiles discontinuos. Una cuadrada o sierra no produce cambios instantáneos en el eje. Puedes distinguir el perfil pedido de la referencia que efectivamente se manda al PI.

## 6. Leer las gráficas

- **Azul:** velocidad solicitada, después del límite de RPM del panel. En POS físico es la demanda del regulador exterior antes de limitar velocidad.
- **Naranja:** referencia de velocidad limitada por aceleración y límites del modo.
- **Verde:** velocidad medida por el encoder, con el mismo filtrado del controlador.
- El valor de posición aparece arriba; también se exporta a CSV junto con PWM, modo, HOME y fallos.

La vista previa es un cálculo, no una medición ni una simulación del motor. La gráfica en vivo se llena solamente con telemetría recibida. Muestra los últimos 20 segundos; el CSV conserva hasta la última hora a 10 muestras/segundo. Los perfiles se envían aproximadamente a 20 Hz y el control del UNO se actualiza a 50 Hz.

## 7. Comunicación y fallos

- El control PC se detiene y marca fallo si no recibe referencias por 1 segundo. HOME iniciado desde el panel también exige enlace; HOME iniciado con D10 sigue siendo autónomo.
- En PC, la vigilancia de pulsos se activa con referencia de al menos 5 RPM en magnitud **o** salida PWM de al menos 80, siempre que haya salida de al menos 20. Si esa condición continúa 1.5 s sin pulsos, corta potencia y marca **Sin pulsos**. Así una cola gaussiana casi nula, con poca potencia y sin movimiento por rozamiento, no dispara la alarma anterior. La detección de control físico y HOME no cambia.
- **Borrar fallo** no arranca. Un fallo invalida HOME: vuelve a guardar cero cuando corresponda.
- Una interfaz congelada, un cierre o la pérdida del USB no provocan reanudación automática. El paro por software corta salida; no sustituye un desconectador físico si tu montaje lo requiere.
- El panel nunca abre un puerto ni mueve el motor por sí solo. Debes elegir Conectar e Iniciar.

Los ajustes PI, la compensación de rozamiento `PWM_APOYO = 30` y la calibración mecánica necesitan verificación con el motor real. Se conserva `CUENTAS_POR_VUELTA = 880` indicado por ti; no es una especificación verificada del fabricante.

### Corrección del perfil gaussiano

La versión corregida no modifica la curva ni la resolución del encoder. Corrige la condición que antes vigilaba ausencia de pulsos desde 0.1 RPM con muy poca potencia. **Sin pulsos** no demuestra por sí solo que el encoder esté averiado: también puede significar motor detenido, rozamiento, atasco o alimentación insuficiente. Si ocurre cerca del máximo del pulso y el motor sí está girando, hay que comprobar la señal del encoder; este ajuste de baja velocidad no resuelve ese caso.

El motor puede permanecer parado en las colas muy pequeñas: no se garantiza seguimiento físico de velocidades arbitrariamente bajas. Para actualizar, desconecta el panel del puerto, vuelve a cargar `motor_perfiles.ino` y reconecta. No necesitas cambiar el panel. Al reiniciar Arduino se debe volver a definir HOME.

## Archivos y comprobación

- `arduino/motor_perfiles/motor_perfiles.ino`: firmware completo.
- `panel_motor.py`: interfaz Tk.
- `perfiles.py`: fórmulas y validación de telemetría.
- `tests/test_perfiles.py`: pruebas de curvas y validación (`python -m unittest discover -s tests -v`).

Conexión serial basada en [pySerial](https://pyserial.readthedocs.io/en/latest/pyserial_api.html), con lectura no bloqueante. LCD mediante [hd44780](https://github.com/duinoWitchery/hd44780).

El firmware fue compilado para ATmega328P/UNO y se probaron las fórmulas, interfaz y transiciones con E/S simuladas. No se ha cargado al Arduino ni probado el movimiento del motor físico desde aquí.
