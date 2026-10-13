/*
  Arduino UNO + L298N + encoder A/B + LCD I2C
  Encoder A=D2, B=D3. Potenciometro=A0.
  D4: modo POS/VEL. D7: marcha/paro; mantener 1.5 s estando
  parado guarda la posicion actual como HOME.
  D10: regresar al HOME guardado y detenerse.
  Los tres botones van de su pin a GND (INPUT_PULLUP).

  L298N: ENA=D5 (sin jumper ENA), IN1=D8, IN2=D9.
  Alimentacion de motor separada; GND comun con Arduino.
  No conectar D5 a 5V. Encoder incremental: HOME se pierde
  al reiniciar o desconectar Arduino.

  Libreria: hd44780, de Bill Perry (hd44780_I2Cexp).
  Panel PC: 115200 baudios. No abrir Monitor Serie simultaneamente.
*/

#include <Arduino.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <util/atomic.h>
#include <math.h>

const uint8_t ENC_A = 2, ENC_B = 3;
const uint8_t BTN_MODO = 4, ENA = 5, BTN_MARCHA = 7;
const uint8_t IN1 = 8, IN2 = 9, BTN_HOME = 10, POT = A0;

// Calibracion indicada por el usuario. Conteo en cuadratura x4.
const float CUENTAS_POR_VUELTA = 880.0f;
const int8_t SIGNO_ENCODER = 1;

// Margenes del ultimo programa completo; no se cambia la resolucion.
const float TOLERANCIA_GRADOS = 2.0f;
const int32_t TOLERANCIA_HOME_CUENTAS = 3;

// Limites de mando. 280 RPM es una referencia, no velocidad garantizada.
const float RPM_MAXIMAS = 280.0f;
const float RPM_POSICION = 120.0f;
const float RPM_HOME = 60.0f;
const int PWM_MAXIMO = 255;

// Control: parametros iniciales que requieren prueba en el motor real.
const float KP_POSICION = 1.2f;
const float KP_VELOCIDAD = 1.2f;
const float KI_VELOCIDAD = 3.0f;
const float PWM_APOYO = 30.0f;
const float ACELERACION = 240.0f; // RPM por segundo

const uint32_t PERIODO_CONTROL = 20;
const uint32_t TIEMPO_MAX_HOME = 20000;
const uint32_t TIEMPO_SIN_PULSOS = 1500;
// En PC, las colas de un perfil pueden pedir velocidades casi nulas.
// No interpretar falta de giro con poca potencia como averia del encoder.
// Vigilar igualmente una salida alta aunque la referencia sea pequena.
const float RPM_VIGILANCIA_PC = 5.0f;
const int PWM_VIGILANCIA_PC = 80;
const uint32_t TIEMPO_GUARDAR_HOME = 1500;

hd44780_I2Cexp lcd;
bool lcdOK = false;

enum Estado { PARADO, CONTROL, VOLVIENDO_HOME };
Estado estado = PARADO;
bool modoPosicion = true, homeValido = false, fallo = false;
// El modo fisico se conserva al seleccionar PC. SET nunca arranca el motor.
bool pcMode = false, remoteHome = false;
float pcTarget = 0, pcAccel = 240, pcDecel = 240;
uint32_t lastSet = 0, lastLink = 0;
bool haveSet = false;
char rxLine[80];
uint8_t rxLen = 0;
bool rxOverflow = false;
uint32_t lastByte = 0;

volatile uint32_t cuentasEncoder = 0;
volatile uint8_t estadoEncoder = 0;
const int8_t TABLA_ENCODER[16] = {
   0, -1,  1,  0,  1,  0,  0, -1,
  -1,  0,  0,  1,  0,  1, -1,  0
};

uint32_t cuentasHome = 0, cuentasAnteriores = 0;
uint32_t ultimoControl = 0, ultimoLCD = 0, ultimoSerial = 0;
uint32_t ultimoMovimiento = 0, inicioHome = 0;
uint32_t llegadaHomeDesde = 0, sinPulsosDesde = 0;
float posicion = 0, rpm = 0, referencia = 0;
float rpmRef = 0, integral = 0, potFiltrado = 0;
int pwmAplicado = 0, sentidoPIAnterior = 0;
char aviso1[17] = "", aviso2[17] = "";
uint32_t inicioAviso = 0;
bool avisoActivo = false;

struct Boton {
  uint8_t pin;
  bool estable = HIGH, anterior = HIGH;
  bool pulsado = false, soltado = false;
  uint32_t cambio = 0;

  explicit Boton(uint8_t p) : pin(p) {}

  void iniciar() {
    pinMode(pin, INPUT_PULLUP);
    estable = anterior = digitalRead(pin);
    cambio = millis();
  }

  void actualizar(uint32_t ahora) {
    pulsado = soltado = false;
    bool lectura = digitalRead(pin);
    if (lectura != anterior) {
      anterior = lectura;
      cambio = ahora;
    }
    if (lectura != estable && ahora - cambio >= 30) {
      estable = lectura;
      pulsado = (estable == LOW);
      soltado = (estable == HIGH);
    }
  }

  bool abajo() const { return estable == LOW; }
};

Boton botonModo(BTN_MODO), botonMarcha(BTN_MARCHA), botonHome(BTN_HOME);
uint32_t inicioPulsacion = 0;
bool permitirAccionMarcha = false, largaAtendida = false;

float limitar(float valor, float minimo, float maximo) {
  return valor < minimo ? minimo : (valor > maximo ? maximo : valor);
}

void encoderISR() {
  uint8_t nuevo = (PIND >> 2) & 0x03;
  int8_t paso = TABLA_ENCODER[(estadoEncoder << 2) | nuevo];
  cuentasEncoder += (int8_t)(paso * SIGNO_ENCODER);
  estadoEncoder = nuevo;
}

uint32_t leerCuentas() {
  uint32_t resultado;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { resultado = cuentasEncoder; }
  return resultado;
}

void aviso(const char *linea1, const char *linea2) {
  snprintf(aviso1, sizeof(aviso1), "%s", linea1);
  snprintf(aviso2, sizeof(aviso2), "%s", linea2);
  inicioAviso = millis();
  avisoActivo = true;
}

void aplicarMotor(int pwm) {
  pwm = constrain(pwm, -PWM_MAXIMO, PWM_MAXIMO);
  int sentidoNuevo = (pwm > 0) - (pwm < 0);
  int sentidoViejo = (pwmAplicado > 0) - (pwmAplicado < 0);
  if (sentidoNuevo != sentidoViejo) {
    analogWrite(ENA, 0);
    digitalWrite(IN1, LOW);
    digitalWrite(IN2, LOW);
    delayMicroseconds(10);
  }

  if (pwm == 0) {
    analogWrite(ENA, 0);
    digitalWrite(IN1, LOW);
    digitalWrite(IN2, LOW);
  } else {
    digitalWrite(IN1, pwm > 0 ? HIGH : LOW);
    digitalWrite(IN2, pwm > 0 ? LOW : HIGH);
    analogWrite(ENA, abs(pwm));
  }
  pwmAplicado = pwm;
}

void reiniciarControl() {
  integral = 0;
  rpmRef = 0;
  sentidoPIAnterior = 0;
  sinPulsosDesde = 0;
}

void detenerMotor() {
  estado = PARADO;
  remoteHome = false;
  pcTarget = 0;
  haveSet = false;
  reiniciarControl();
  llegadaHomeDesde = 0;
  aplicarMotor(0);
}

void declararFallo(const char *motivo) {
  detenerMotor();
  fallo = true;
  homeValido = false;
  aviso("FALLO: detenido", motivo);
  Serial.print(F("FALLO: "));
  Serial.println(motivo);
}

void guardarHome(uint32_t ahora) {
  if (estado != PARADO) return;
  if (ahora - ultimoMovimiento < 300 ||
      leerCuentas() != cuentasAnteriores) {
    aviso("Aun se mueve", "Intenta de nuevo");
    return;
  }
  cuentasHome = leerCuentas();
  cuentasAnteriores = cuentasHome;
  posicion = 0;
  rpm = 0;
  reiniciarControl();
  homeValido = true;
  aviso("HOME guardado", "Este punto es 0");
}

void iniciarHome(uint32_t ahora) {
  if (fallo) {
    aviso("Hay un fallo", "D7 para limpiar");
    return;
  }
  if (!homeValido) {
    aviso("Falta guardar 0", "Mantener D7");
    return;
  }
  detenerMotor();
  modoPosicion = true;
  referencia = 0;
  inicioHome = ahora;
  estado = VOLVIENDO_HOME;
  aviso("Regresando a 0", "D7 para parar");
}

void atenderBotones(uint32_t ahora) {
  botonModo.actualizar(ahora);
  botonMarcha.actualizar(ahora);
  botonHome.actualizar(ahora);

  if (botonModo.pulsado) {
    pcMode = false;
    detenerMotor();
    modoPosicion = !modoPosicion;
    permitirAccionMarcha = false;
    aviso(modoPosicion ? "Modo POSICION" : "Modo VELOCIDAD",
          "D7 para iniciar");
  }

  if (botonMarcha.pulsado) {
    inicioPulsacion = ahora;
    largaAtendida = false;
    permitirAccionMarcha =
        estado == PARADO && !botonModo.abajo() && !botonHome.abajo();
    if (estado != PARADO) {
      pcMode = false;
      detenerMotor();
      aviso("Motor detenido", "D7 para iniciar");
    }
  }

  if (botonMarcha.abajo() && permitirAccionMarcha &&
      !largaAtendida && ahora - inicioPulsacion >= TIEMPO_GUARDAR_HOME) {
    largaAtendida = true;
    if (fallo) aviso("Hay un fallo", "Pulsa D7 corto");
    else guardarHome(ahora);
  }

  if (botonMarcha.soltado) {
    if (permitirAccionMarcha && !largaAtendida) {
      if (fallo) {
        fallo = false;
        aviso("Fallo borrado", "Volver a fijar 0");
      } else if (pcMode) {
        aviso("Modo PC", "Iniciar en panel");
      } else if (modoPosicion && !homeValido) {
        aviso("Falta guardar 0", "Mantener D7");
      } else {
        reiniciarControl();
        estado = CONTROL;
        aviso("Motor activo", "D7 para parar");
      }
    }
    permitirAccionMarcha = false;
  }

  if (botonHome.pulsado && !botonMarcha.abajo() &&
      !botonMarcha.soltado && !botonModo.abajo()) {
    // Incluso si falta HOME, D10 cancela la orden PC antes de salir.
    if (pcMode) detenerMotor();
    pcMode = false;
    iniciarHome(ahora);
  }
}

// Integral expresada en unidades de PWM, con compensacion de rozamiento.
int calcularPWM(float objetivoRPM, float medidaRPM, float dt) {
  int sentido = (objetivoRPM > 0.01f) - (objetivoRPM < -0.01f);
  if (sentido != sentidoPIAnterior) {
    integral = 0;
    sentidoPIAnterior = sentido;
  }
  if (sentido == 0) {
    integral = 0;
    return 0;
  }

  float errorRPM = objetivoRPM - medidaRPM;
  float apoyo = sentido * PWM_APOYO;
  float integralNueva = integral + KI_VELOCIDAD * errorRPM * dt;
  float propuesta = apoyo + KP_VELOCIDAD * errorRPM + integralNueva;

  bool noSaturada = propuesta >= -PWM_MAXIMO && propuesta <= PWM_MAXIMO;
  bool recupera = (propuesta > PWM_MAXIMO && errorRPM < 0) ||
                  (propuesta < -PWM_MAXIMO && errorRPM > 0);
  if (noSaturada || recupera)
    integral = limitar(integralNueva, -PWM_MAXIMO, PWM_MAXIMO);

  float salida = apoyo + KP_VELOCIDAD * errorRPM + integral;
  return (int)roundf(limitar(salida, -PWM_MAXIMO, PWM_MAXIMO));
}

void actualizarControl(uint32_t ahora, float dt) {
  uint32_t cuentas = leerCuentas();
  int32_t incremento = (int32_t)(cuentas - cuentasAnteriores);
  cuentasAnteriores = cuentas;
  int32_t desdeHome = (int32_t)(cuentas - cuentasHome);

  if (incremento != 0) ultimoMovimiento = ahora;

  posicion = (float)desdeHome * 360.0f / CUENTAS_POR_VUELTA;
  float rpmInstantaneas =
      (float)incremento * 60.0f / (CUENTAS_POR_VUELTA * dt);
  rpm += dt / (0.08f + dt) * (rpmInstantaneas - rpm);

  // Se conserva el filtrado y el recorrido del potenciometro.
  potFiltrado += dt / (0.06f + dt) * (analogRead(POT) - potFiltrado);
  float fraccion = limitar((potFiltrado - 5.0f) / 1013.0f, 0, 1);

  if (pcMode) {
    referencia = pcTarget;
  } else if (estado == VOLVIENDO_HOME) {
    referencia = 0;
  } else if (modoPosicion) {
    referencia = fraccion * 360.0f;
  } else {
    float mando = fraccion * 2.0f - 1.0f;
    if (fabsf(mando) < 0.03f) mando = 0;
    else mando = (mando > 0 ? 1.0f : -1.0f) *
                 (fabsf(mando) - 0.03f) / 0.97f;
    referencia = mando * RPM_MAXIMAS;
  }

  if (estado == PARADO) {
    aplicarMotor(0);
    return;
  }

  if (estado == VOLVIENDO_HOME && ahora - inicioHome >= TIEMPO_MAX_HOME) {
    declararFallo("Tiempo HOME");
    return;
  }

  if ((pcMode && (!haveSet || ahora - lastSet > 1000)) ||
      (remoteHome && ahora - lastLink > 1000)) {
    declararFallo("Sin enlace PC");
    return;
  }

  float objetivoRPM;
  if (!pcMode && modoPosicion) {
    float errorPosicion = referencia - posicion;
    bool dentro = estado == VOLVIENDO_HOME
        ? (desdeHome >= -TOLERANCIA_HOME_CUENTAS &&
           desdeHome <=  TOLERANCIA_HOME_CUENTAS)
        : fabsf(errorPosicion) <= TOLERANCIA_GRADOS;

    // Cortar al llegar, sin esperar a que el filtro RPM marque casi cero.
    // Si el eje sale del margen, la siguiente iteracion vuelve a corregir.
    if (dentro) {
      aplicarMotor(0);
      reiniciarControl();
      if (estado == VOLVIENDO_HOME) {
        if (llegadaHomeDesde == 0) llegadaHomeDesde = ahora;
        if (ahora - llegadaHomeDesde >= 300 &&
            ahora - ultimoMovimiento >= 150) {
          detenerMotor();
          aviso("HOME alcanzado", "D7: usar pot");
        }
      }
      return;
    }
    llegadaHomeDesde = 0;

    float limiteRPM = estado == VOLVIENDO_HOME ? RPM_HOME : RPM_POSICION;
    objetivoRPM = limitar(KP_POSICION * errorPosicion, -limiteRPM, limiteRPM);

    // Reducir la velocidad de referencia con distancia suficiente
    // para la rampa de desaceleracion (6 grados/s por cada RPM).
    // Es una ayuda de control, no una garantia de frenado mecanico.
    float limiteFrenado = sqrtf(2.0f * ACELERACION *
                               fabsf(errorPosicion) / 6.0f);
    objetivoRPM = limitar(objetivoRPM, -limiteFrenado, limiteFrenado);
  } else {
    objetivoRPM = referencia;
  }

  // Al invertir sentido, primero desacelerar a cero.
  float tasa = ACELERACION;
  if (pcMode) {
    if (rpmRef * objetivoRPM < 0) {
      objetivoRPM = 0;
      tasa = pcDecel;
    } else {
      tasa = fabsf(objetivoRPM) < fabsf(rpmRef) ? pcDecel : pcAccel;
    }
  }
  float paso = tasa * dt;
  rpmRef += limitar(objetivoRPM - rpmRef, -paso, paso);
  aplicarMotor(calcularPWM(rpmRef, rpm, dt));

  // Mantener la deteccion original en control fisico y HOME.
  // En PC exigir una demanda significativa O potencia elevada, para no
  // fallar en las colas casi nulas del gaussiano por rozamiento del motor.
  bool vigilarMovimiento = fabsf(rpmRef) >= 0.1f;
  if (pcMode) {
    vigilarMovimiento = fabsf(rpmRef) >= RPM_VIGILANCIA_PC ||
                         abs(pwmAplicado) >= PWM_VIGILANCIA_PC;
  }
  if (incremento != 0 || abs(pwmAplicado) < 20 || !vigilarMovimiento) {
    sinPulsosDesde = 0;
  } else {
    if (sinPulsosDesde == 0) sinPulsosDesde = ahora;
    if (ahora - sinPulsosDesde >= TIEMPO_SIN_PULSOS)
      declararFallo("Sin pulsos");
  }
}

void escribirLCD(uint8_t fila, const char *texto) {
  char linea[17];
  snprintf(linea, sizeof(linea), "%-16.16s", texto);
  lcd.setCursor(0, fila);
  lcd.print(linea);
}

void actualizarLCD(uint32_t ahora) {
  if (!lcdOK) return;
  if (avisoActivo && ahora - inicioAviso < 1800) {
    escribirLCD(0, aviso1);
    escribirLCD(1, aviso2);
    return;
  }
  avisoActivo = false;
  char linea[17], numero[24];
  char estadoLetra = fallo ? 'F' :
      (estado == PARADO ? 'S' : (estado == VOLVIENDO_HOME ? 'H' : 'R'));
  const char *modo = pcMode ? "PC " : (estado == VOLVIENDO_HOME ? "HOM" :
                     (modoPosicion ? "POS" : "VEL"));

  dtostrf(referencia, 6, 1, numero);
  snprintf(linea, sizeof(linea), "%s %c R:%s", modo, estadoLetra, numero);
  escribirLCD(0, linea);
  dtostrf(!pcMode && modoPosicion ? posicion : rpm, 6, 1, numero);
  snprintf(linea, sizeof(linea), "Y:%s H:%s", numero, homeValido ? "OK" : "--");
  escribirLCD(1, linea);
}


void hello() {
  Serial.println(F("HELLO,MOTOR9,1,880"));
}

bool numberToken(char *token, float &out) {
  if (!token || !*token) return false;
  char *end;
  out = strtod(token, &end);
  return *end == '\0' && isfinite(out);
}

void commandError(const __FlashStringHelper *why) {
  Serial.print(F("ERR,"));
  Serial.println(why);
}

void serialCommand(uint32_t now) {
  char *cmd = strtok(rxLine, ",");
  if (!cmd) return;
  if (!strcmp(cmd, "SET")) {
    float target, acc, dec;
    bool valid = numberToken(strtok(NULL, ","), target);
    valid = numberToken(strtok(NULL, ","), acc) && valid;
    valid = numberToken(strtok(NULL, ","), dec) && valid;
    if (!valid || strtok(NULL, ",") || fabsf(target) > RPM_MAXIMAS ||
        acc < 1 || acc > 1000 || dec < 1 || dec > 1000) {
      commandError(F("PARAM"));
      return;
    }
    if (!pcMode || fallo) { commandError(F("MODE")); return; }
    pcTarget = target; pcAccel = acc; pcDecel = dec;
    lastSet = lastLink = now;
    haveSet = true;
    return; // Aceptar referencia nunca inicia ni reanuda movimiento.
  }
  if (!strcmp(cmd, "MODE")) {
    char *mode = strtok(NULL, ",");
    if (!mode || strtok(NULL, ",") ||
        (strcmp(mode, "PC") && strcmp(mode, "LOCAL"))) {
      commandError(F("MODE")); return;
    }
    detenerMotor();
    pcMode = !strcmp(mode, "PC");
    Serial.println(F("OK,MODE"));
    return;
  }
  if (strtok(NULL, ",")) { commandError(F("PARAM")); return; }
  if (!strcmp(cmd, "HELLO?")) { hello(); return; }
  if (!strcmp(cmd, "PING")) { lastLink = now; return; }
  if (!strcmp(cmd, "STOP")) {
    detenerMotor();
    Serial.println(F("OK,STOP"));
  } else if (!strcmp(cmd, "RUN")) {
    if (fallo || !pcMode || !haveSet || now - lastSet > 500) {
      commandError(F("START")); return;
    }
    reiniciarControl();
    estado = CONTROL;
    Serial.println(F("OK,RUN"));
  } else if (!strcmp(cmd, "ZERO")) {
    if (fallo || estado != PARADO || now - ultimoMovimiento < 300 ||
        leerCuentas() != cuentasAnteriores) {
      commandError(F("STOP_FIRST")); return;
    }
    guardarHome(now);
    Serial.println(F("OK,ZERO"));
  } else if (!strcmp(cmd, "HOME")) {
    if (fallo || !homeValido) { commandError(F("NO_HOME")); return; }
    pcMode = false;
    iniciarHome(now);
    remoteHome = true; lastLink = now;
    Serial.println(F("OK,HOME"));
  } else if (!strcmp(cmd, "CLEAR")) {
    detenerMotor();
    fallo = false;
    Serial.println(F("OK,CLEAR"));
  } else commandError(F("COMMAND"));
}

void readSerial(uint32_t now) {
  if ((rxLen || rxOverflow) && now - lastByte > 250) {
    rxLen = 0; rxOverflow = false;
  }
  uint8_t budget = 64;
  while (Serial.available() && budget--) {
    char c = Serial.read();
    lastByte = now;
    if (c == '\r') continue;
    if (c == '\n') {
      if (rxOverflow) commandError(F("LINE"));
      else { rxLine[rxLen] = '\0'; serialCommand(now); }
      rxLen = 0; rxOverflow = false;
    } else if (rxLen < sizeof(rxLine) - 1 && !rxOverflow) {
      rxLine[rxLen++] = c;
    } else rxOverflow = true;
  }
}

void telemetry(uint32_t now) {
  // T,version,ms,mode(0=POS/1=VEL/2=PC/3=HOME),state,
  // home,fault,requested_rpm,limited_rpm,measured_rpm,angle,pwm
  Serial.print(F("T,1,")); Serial.print(now); Serial.print(',');
  Serial.print(pcMode ? 2 : (estado == VOLVIENDO_HOME ? 3 : (modoPosicion ? 0 : 1)));
  Serial.print(','); Serial.print((int)estado);
  Serial.print(','); Serial.print(homeValido);
  Serial.print(','); Serial.print(fallo);
  float request = pcMode ? pcTarget :
      (modoPosicion ? KP_POSICION * (referencia - posicion) : referencia);
  if (estado == PARADO) request = 0;
  Serial.print(','); Serial.print(request, 2);
  Serial.print(','); Serial.print(rpmRef, 2);
  Serial.print(','); Serial.print(rpm, 2);
  Serial.print(','); Serial.print(posicion, 2);
  Serial.print(','); Serial.println(pwmAplicado);
}


void setup() {
  pinMode(ENA, OUTPUT);
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  aplicarMotor(0);
  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  botonModo.iniciar();
  botonMarcha.iniciar();
  botonHome.iniciar();

  estadoEncoder = (PIND >> 2) & 0x03;
  attachInterrupt(digitalPinToInterrupt(ENC_A), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), encoderISR, CHANGE);

  Serial.begin(115200);
  Wire.begin();
#if defined(WIRE_HAS_TIMEOUT)
  Wire.setWireTimeout(3000, true);
#endif
  lcdOK = lcd.begin(16, 2) == 0;
  if (lcdOK) lcd.backlight();

  cuentasAnteriores = cuentasHome = leerCuentas();
  potFiltrado = analogRead(POT);
  ultimoControl = ultimoMovimiento = millis();
  aviso("Primero fijar 0", "Mantener D7");
  hello();
}

void loop() {
  uint32_t ahora = millis();
  readSerial(ahora);
  atenderBotones(ahora);

  uint32_t transcurrido = ahora - ultimoControl;
  if (transcurrido >= PERIODO_CONTROL) {
    ultimoControl = ahora;
    // Un bloqueo largo invalida la continuidad del control.
    if (transcurrido > 250 && estado != PARADO)
      declararFallo("Pausa de control");
    actualizarControl(ahora, transcurrido * 0.001f);
  }

  if (ahora - ultimoLCD >= 250) {
    ultimoLCD = ahora;
    actualizarLCD(ahora);
  }

  if (ahora - ultimoSerial >= 100) {
    ultimoSerial = ahora;
    telemetry(ahora);
  }
}
