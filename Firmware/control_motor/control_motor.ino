/*
  Arduino UNO R3 + JGA25-370 + L298N + LCD 16x2 I2C.
  Posicion: P exterior -> referencia RPM -> PI de velocidad.
  Velocidad: potenciometro -> referencia RPM -> PI de velocidad.
  Libreria: "hd44780" de Bill Perry, instalada desde Library Manager.
  Leer GUIA.md antes de conectar la alimentacion del motor.
  Los valores del motor y las ganancias son EJEMPLOS: requieren ajuste.
*/
#include <Arduino.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <util/atomic.h>
#include <math.h>

const uint8_t ENC_A = 2, ENC_B = 3;
const uint8_t BTN_MODE = 4, BTN_RUN = 7;
const uint8_t ENA = 5, IN1 = 8, IN2 = 9, POT = A0;

// CAMBIAR SEGUN TU MOTOR. Cuenta x4 en ambos canales, eje DE SALIDA.
// EJEMPLO solamente: 11 ciclos/canal/vuelta motor * 4 * reduccion 10 = 440.
const float COUNTS_PER_OUTPUT_REV = 440.0f;
const int8_t ENCODER_SIGN = 1; // Cambiar a -1 si prueba 't' da cuentas negativas.
const float MAX_RPM = 60.0f;   // Limite de referencia, segun motor y carga.
const float POSITION_RANGE_DEG = 180.0f; // Pot: -180 ... +180 grados.
const int MAX_PWM = 180;      // Limite inicial (0...255), NO limita corriente.
const int TEST_PWM = 80;      // Prueba breve en lazo abierto.

// Ganancias iniciales; NO son universales para todos los JGA25-370.
const float KP_SPEED = 1.2f;  // PWM / RPM
const float KI_SPEED = 3.0f;  // PWM / (RPM*s)
const float KP_POSITION = 0.8f; // RPM / grado
const float RPM_SLEW = 80.0f; // Cambio maximo referencia: RPM/s.
const float POSITION_TOL_DEG = 2.0f;
const uint32_t SAMPLE_US = 20000UL; // 50 Hz

hd44780_I2Cexp lcd; // Autodetecta direccion y cableado del backpack.
bool lcdOK = false, positionMode = true, running = false;
bool fault = false, testing = false;
uint32_t testStarted = 0;
volatile uint32_t encoderCount = 0; // Aritmetica modular evita overflow con signo.
volatile uint8_t previousAB = 0;
uint32_t zeroCount = 0, previousCount = 0;
uint32_t lastSample = 0, lastDisplay = 0, lastMotion = 0;
float angleDeg = 0, rpm = 0, goal = 0, rpmRef = 0, integral = 0;
float potFiltered = 512;
int appliedPWM = 0;

struct Button {
  uint8_t pin;
  bool raw = HIGH, stable = HIGH;
  bool pressed = false, released = false;
  uint32_t changedAt = 0;
  explicit Button(uint8_t p) : pin(p) {}
  void begin() { pinMode(pin, INPUT_PULLUP); }
  void update(uint32_t now) {
    pressed = released = false;
    bool value = digitalRead(pin);
    if (value != raw) { raw = value; changedAt = now; }
    if (now - changedAt >= 30 && stable != raw) {
      stable = raw;
      pressed = (stable == LOW);
      released = (stable == HIGH);
    }
  }
};
Button modeButton(BTN_MODE), runButton(BTN_RUN);
bool ignoreRunRelease = false, zeroDone = false;
uint32_t runPressedAt = 0;

float clampFloat(float x, float lo, float hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

// Lectura simultanea de D2/D3 mediante PIND: especifico de UNO R3/ATmega328P.
// Dos interrupciones CHANGE = cuadratura x4. Descarta transiciones invalidas.
void encoderISR() {
  const int8_t table[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
  uint8_t ab = (PIND >> 2) & 0x03;
  int8_t step = table[(previousAB << 2) | ab] * ENCODER_SIGN;
  if (step > 0) ++encoderCount;
  else if (step < 0) --encoderCount;
  previousAB = ab;
}

uint32_t readEncoder() {
  uint32_t n;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { n = encoderCount; }
  return n;
}

void driveMotor(int pwm) {
  pwm = constrain(pwm, -MAX_PWM, MAX_PWM);
  // Deshabilitar antes de modificar sentido.
  if ((pwm > 0 && appliedPWM <= 0) || (pwm < 0 && appliedPWM >= 0)) {
    analogWrite(ENA, 0);
    digitalWrite(IN1, pwm > 0 ? HIGH : LOW);
    digitalWrite(IN2, pwm > 0 ? LOW : HIGH);
    delayMicroseconds(10);
  }
  if (pwm == 0) {
    analogWrite(ENA, 0); // Paro libre: no sostiene carga ni frena activamente.
    digitalWrite(IN1, LOW);
    digitalWrite(IN2, LOW);
  } else {
    analogWrite(ENA, abs(pwm));
  }
  appliedPWM = pwm;
}

void stopMotor() {
  running = testing = false;
  integral = rpmRef = 0;
  driveMotor(0);
}

void setZero() {
  if (running || testing) return;
  zeroCount = readEncoder();
  angleDeg = 0;
  Serial.println(F("Cero establecido. Centra el potenciometro antes de arrancar."));
}

void serviceButtons(uint32_t now) {
  modeButton.update(now);
  runButton.update(now);
  if (modeButton.pressed) {
    stopMotor();
    positionMode = !positionMode;
    // Cambiar de modo siempre exige una nueva pulsacion para arrancar.
    ignoreRunRelease = true;
  }
  if (runButton.pressed) {
    runPressedAt = now;
    zeroDone = false;
    ignoreRunRelease = running || testing || modeButton.stable == LOW;
    if (running || testing) stopMotor(); // Detener al presionar.
  }
  if (runButton.stable == LOW && !ignoreRunRelease && !zeroDone &&
      now - runPressedAt >= 1500) {
    setZero();
    zeroDone = true;
  }
  if (runButton.released && !ignoreRunRelease && !zeroDone) {
    if (fault) {
      fault = false; // Primer toque reconoce fallo, no arranca.
    } else {
      running = true; // Arrancar al soltar una pulsacion corta.
      integral = rpmRef = 0;
      lastMotion = now;
    }
  }
}

void serviceSerial(uint32_t now) {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c == 's' || c == 'S') stopMotor();
  if (c == 'z' || c == 'Z') setZero();
  if ((c == 't' || c == 'T') && !running && !testing && !fault) {
    testing = true;
    testStarted = now;
    driveMotor(TEST_PWM);
    Serial.println(F("TEST +PWM 400ms: las cuentas deben AUMENTAR."));
  }
}

void controlStep(float dt, uint32_t now) {
  uint32_t n = readEncoder();
  int32_t delta = (int32_t)(n - previousCount);
  previousCount = n;
  angleDeg = (int32_t)(n - zeroCount) * (360.0f / COUNTS_PER_OUTPUT_REV);
  float instantRPM = delta * (60.0f / COUNTS_PER_OUTPUT_REV) / dt;
  rpm += (dt / (0.08f + dt)) * (instantRPM - rpm);
  if (delta != 0) lastMotion = now;

  potFiltered += (dt / (0.06f + dt)) * (analogRead(POT) - potFiltered);
  float knob = clampFloat((potFiltered - 511.5f) / 511.5f, -1, 1);
  // Zona central muerta 3%, reescalada para mantener extremos.
  if (fabsf(knob) < 0.03f) knob = 0;
  else knob = (knob > 0 ? 1 : -1) * (fabsf(knob) - 0.03f) / 0.97f;
  goal = knob * (positionMode ? POSITION_RANGE_DEG : MAX_RPM);

  if (testing) {
    if (now - testStarted >= 400) stopMotor();
    return;
  }
  if (!running) { driveMotor(0); return; }

  float errorPosition = goal - angleDeg;
  float requestedRPM = positionMode
    ? clampFloat(KP_POSITION * errorPosition, -MAX_RPM, MAX_RPM) : goal;
  bool atRest = positionMode
    ? (fabsf(errorPosition) <= POSITION_TOL_DEG && fabsf(rpm) < 2.0f)
    : (goal == 0 && fabsf(rpm) < 2.0f);
  if (atRest) {
    integral = rpmRef = 0;
    driveMotor(0);
    lastMotion = now;
    return;
  }

  rpmRef += clampFloat(requestedRPM - rpmRef, -RPM_SLEW * dt, RPM_SLEW * dt);
  float errorRPM = rpmRef - rpm;
  float candidateI = clampFloat(integral + KI_SPEED * errorRPM * dt, -MAX_PWM, MAX_PWM);
  float candidateU = KP_SPEED * errorRPM + candidateI;
  // Anti-windup: integrar si no satura o si el error ayuda a salir de saturacion.
  if ((candidateU <= MAX_PWM && candidateU >= -MAX_PWM) ||
      (candidateU > MAX_PWM && errorRPM < 0) ||
      (candidateU < -MAX_PWM && errorRPM > 0)) integral = candidateI;
  float u = clampFloat(KP_SPEED * errorRPM + integral, -MAX_PWM, MAX_PWM);
  driveMotor((int)roundf(u));

  // Deteccion basica de falta de pulsos; NO es proteccion contra sobrecorriente.
  if (abs(appliedPWM) >= 80 && fabsf(rpmRef) > 5 && now - lastMotion > 1200) {
    stopMotor();
    fault = true;
    Serial.println(F("FALLO: sin pulsos. Revisar encoder, atasco o PWM insuficiente."));
  }
}

void updateDisplay() {
  if (lcdOK) {
    char row[17];
    // Limita solo la representacion LCD, no la variable de control.
    int shownGoal = (int)clampFloat(goal, -999, 999);
    float actual = positionMode ? angleDeg : rpm;
    int shownActual = (int)clampFloat(actual, -999, 999);
    char state = fault ? 'F' : (testing ? 'T' : (running ? 'R' : 'S'));
    snprintf(row, sizeof(row), "%s %c R:%+4d    ", positionMode ? "POS" : "VEL", state, shownGoal);
    lcd.setCursor(0, 0); lcd.print(row);
    snprintf(row, sizeof(row), "Y:%+4d %-3s     ", shownActual, positionMode ? "deg" : "rpm");
    lcd.setCursor(0, 1); lcd.print(row);
  }
  Serial.print(F("modo=")); Serial.print(positionMode ? F("POS") : F("VEL"));
  Serial.print(F(" activo=")); Serial.print(running);
  Serial.print(F(" fallo=")); Serial.print(fault);
  Serial.print(F(" cuentas=")); Serial.print((int32_t)(readEncoder() - zeroCount));
  Serial.print(F(" ref=")); Serial.print(goal, 1);
  Serial.print(F(" grados=")); Serial.print(angleDeg, 1);
  Serial.print(F(" rpm=")); Serial.print(rpm, 1);
  Serial.print(F(" pwm=")); Serial.println(appliedPWM);
}

void setup() {
  pinMode(ENA, OUTPUT); pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  driveMotor(0);
  pinMode(ENC_A, INPUT_PULLUP); pinMode(ENC_B, INPUT_PULLUP);
  modeButton.begin(); runButton.begin();
  Serial.begin(115200);
  previousAB = (PIND >> 2) & 0x03;
  attachInterrupt(digitalPinToInterrupt(ENC_A), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), encoderISR, CHANGE);
  Wire.begin();
#if defined(WIRE_HAS_TIMEOUT)
  Wire.setWireTimeout(3000, true);
#endif
  lcdOK = (lcd.begin(16, 2) == 0);
  if (!lcdOK) Serial.println(F("LCD no detectada: revisar I2C. Disponible monitor serie."));
  potFiltered = analogRead(POT);
  zeroCount = previousCount = readEncoder();
  lastSample = micros(); lastMotion = millis();
  Serial.println(F("Inicia detenido. s=parar; z=cero detenido; t=prueba +PWM 400ms."));
}

void loop() {
  uint32_t now = millis();
  serviceButtons(now);
  serviceSerial(now);
  uint32_t us = micros();
  uint32_t elapsed = us - lastSample;
  if (elapsed >= SAMPLE_US) {
    lastSample = us;
    controlStep(elapsed * 0.000001f, now);
  }
  if (now - lastDisplay >= 250) {
    lastDisplay = now;
    updateDisplay();
  }
}
