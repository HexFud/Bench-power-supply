#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

static const uint8_t ENC_V_CLK = 10, ENC_V_DT = 8, ENC_V_SW = 6;
static const uint8_t ENC_I_CLK = 9, ENC_I_DT = 7, ENC_I_SW = 5;
static const uint8_t DPS_RX_PIN = 44;
static const uint8_t DPS_TX_PIN = 43;
static const uint8_t I2C_SDA_PIN = 11;
static const uint8_t I2C_SCL_PIN = 12;

static const bool ENC_V_INVERT = false;
static const bool ENC_I_INVERT = false;

static const uint8_t  DPS_ADDR = 1;
static const uint32_t DPS_BAUD = 9600;

static const uint16_t REG_USET   = 0x0000;
static const uint16_t REG_ISET   = 0x0001;
static const uint16_t REG_UOUT   = 0x0002;
static const uint16_t REG_IOUT   = 0x0003;
static const uint16_t REG_UIN    = 0x0005;
static const uint16_t REG_PROT   = 0x0007;
static const uint16_t REG_CVCC   = 0x0008;
static const uint16_t REG_ONOFF  = 0x0009;
static const uint16_t REG_COUNT  = 10;
static const float    V_SCALE = 100.0f;
static const float    I_SCALE = 1000.0f;

static const float V_MAX_CFG    = 23.0f;
static const float I_MAX_CFG    = 5.0f;
static const float VIN_HEADROOM = 1.5f;

static const float V_STEPS[] = {0.01f, 0.1f, 1.0f};
static const float I_STEPS[] = {0.001f, 0.01f, 0.1f};

#define USE_DISPLAY2 1
static const uint8_t OLED1_ADDR = 0x3C;
static const uint8_t OLED2_ADDR = 0x3D;

static const uint32_t POLL_MS       = 250;
static const uint32_t DRAW_MS       = 100;
static const uint32_t WRITE_GAP_MS  = 40;
static const uint32_t SYNC_HOLD_MS  = 600;
static const uint32_t LONG_PRESS_MS = 800;

#define dpsSerial Serial1
static Adafruit_SSD1306 oled1(128, 64, &Wire, -1);
#if USE_DISPLAY2
static Adafruit_SSD1306 oled2(128, 64, &Wire, -1);
#endif
static bool oled1Ok = false, oled2Ok = false;

struct DpsState {
  float uset = 0, iset = 0, uout = 0, iout = 0, uin = 0;
  bool cc = false, on = false, linkOk = false;
  uint8_t prot = 0;
};
static DpsState dps;

struct Encoder {
  uint8_t pinA, pinB;
  bool invert;
  volatile uint8_t state;
  volatile int32_t acc;
};

enum BtnEvent { BTN_NONE, BTN_SHORT, BTN_LONG };

struct Button {
  uint8_t pin;
  bool down;
  bool longFired;
  uint32_t tChange, tPress;
};

static float targetV = 0.0f, targetI = 0.5f;
static bool dirtyV = false, dirtyI = false;
static uint32_t lastEditMs = 0, lastWriteMs = 0, lastPollMs = 0, lastDrawMs = 0;
static uint8_t stepIdxV = 1, stepIdxI = 1;

static uint16_t crc16(const uint8_t* d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
  }
  return crc;
}

static bool txrx(const uint8_t* req, size_t reqLen, uint8_t* resp, size_t respLen) {
  while (dpsSerial.available()) dpsSerial.read();
  dpsSerial.write(req, reqLen);
  dpsSerial.flush();
  size_t n = 0;
  uint32_t t0 = millis();
  while (n < respLen && millis() - t0 < 100) {
    if (dpsSerial.available()) resp[n++] = (uint8_t)dpsSerial.read();
    else delay(1);
  }
  return n == respLen;
}

static bool readRegs(uint16_t start, uint16_t count, uint16_t* out) {
  if (count == 0 || count > 16) return false;
  uint8_t req[8] = {DPS_ADDR, 0x03, (uint8_t)(start >> 8), (uint8_t)(start & 0xFF),
                    (uint8_t)(count >> 8), (uint8_t)(count & 0xFF), 0, 0};
  uint16_t c = crc16(req, 6);
  req[6] = c & 0xFF;
  req[7] = c >> 8;

  uint8_t resp[5 + 2 * 16];
  size_t rl = 5 + 2 * (size_t)count;
  if (!txrx(req, 8, resp, rl)) return false;
  if (resp[0] != DPS_ADDR || resp[1] != 0x03 || resp[2] != 2 * count) return false;
  uint16_t rc = crc16(resp, rl - 2);
  if (resp[rl - 2] != (rc & 0xFF) || resp[rl - 1] != (rc >> 8)) return false;
  for (uint16_t i = 0; i < count; i++) out[i] = ((uint16_t)resp[3 + 2 * i] << 8) | resp[4 + 2 * i];
  return true;
}

static bool writeReg(uint16_t reg, uint16_t val) {
  uint8_t req[8] = {DPS_ADDR, 0x06, (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF),
                    (uint8_t)(val >> 8), (uint8_t)(val & 0xFF), 0, 0};
  uint16_t c = crc16(req, 6);
  req[6] = c & 0xFF;
  req[7] = c >> 8;
  uint8_t resp[8];
  if (!txrx(req, 8, resp, 8)) return false;
  return memcmp(req, resp, 8) == 0;
}

static bool pollDps() {
  uint16_t r[REG_COUNT];
  if (!readRegs(REG_USET, REG_COUNT, r)) {
    dps.linkOk = false;
    return false;
  }
  dps.uset = r[REG_USET] / V_SCALE;
  dps.iset = r[REG_ISET] / I_SCALE;
  dps.uout = r[REG_UOUT] / V_SCALE;
  dps.iout = r[REG_IOUT] / I_SCALE;
  dps.uin  = r[REG_UIN]  / V_SCALE;
  dps.prot = (uint8_t)r[REG_PROT];
  dps.cc   = (r[REG_CVCC] == 1);
  dps.on   = (r[REG_ONOFF] == 1);
  dps.linkOk = true;
  return true;
}

DRAM_ATTR static const int8_t ENC_TABLE[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

static Encoder encV = {ENC_V_CLK, ENC_V_DT, ENC_V_INVERT, 0, 0};
static Encoder encI = {ENC_I_CLK, ENC_I_DT, ENC_I_INVERT, 0, 0};
static portMUX_TYPE encMux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR encIsr(void* arg) {
  Encoder* e = (Encoder*)arg;
  uint8_t s = (uint8_t)((digitalRead(e->pinA) << 1) | digitalRead(e->pinB));
  e->state = ((e->state << 2) | s) & 0x0F;
  e->acc = e->acc + ENC_TABLE[e->state];
}

static int32_t takeDetents(Encoder& e) {
  portENTER_CRITICAL(&encMux);
  int32_t a = e.acc;
  int32_t d = a / 4;
  e.acc = a - d * 4;
  portEXIT_CRITICAL(&encMux);
  return e.invert ? -d : d;
}

static void encBegin(Encoder& e) {
  pinMode(e.pinA, INPUT_PULLUP);
  pinMode(e.pinB, INPUT_PULLUP);
  e.state = (uint8_t)((digitalRead(e.pinA) << 1) | digitalRead(e.pinB));
  attachInterruptArg(digitalPinToInterrupt(e.pinA), encIsr, &e, CHANGE);
  attachInterruptArg(digitalPinToInterrupt(e.pinB), encIsr, &e, CHANGE);
}

static Button btnV = {ENC_V_SW, false, false, 0, 0};
static Button btnI = {ENC_I_SW, false, false, 0, 0};

static BtnEvent pollButton(Button& b) {
  uint32_t now = millis();
  bool pressed = (digitalRead(b.pin) == LOW);
  if (pressed != b.down && now - b.tChange > 25) {
    b.tChange = now;
    b.down = pressed;
    if (pressed) {
      b.tPress = now;
      b.longFired = false;
    } else if (!b.longFired) {
      return BTN_SHORT;
    }
  }
  if (b.down && !b.longFired && now - b.tPress > LONG_PRESS_MS) {
    b.longFired = true;
    return BTN_LONG;
  }
  return BTN_NONE;
}

static float effectiveVmax() {
  float vmax = V_MAX_CFG;
  if (dps.linkOk && dps.uin > 1.0f) {
    float lim = dps.uin - VIN_HEADROOM;
    if (lim < vmax) vmax = lim;
  }
  return vmax < 0 ? 0 : vmax;
}

static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static void setOutput(bool on) {
  if (writeReg(REG_ONOFF, on ? 1 : 0)) dps.on = on;
}

static void handleInputs() {
  int32_t dv = takeDetents(encV);
  if (dv != 0) {
    targetV = clampf(targetV + dv * V_STEPS[stepIdxV], 0.0f, effectiveVmax());
    dirtyV = true;
    lastEditMs = millis();
  }
  int32_t di = takeDetents(encI);
  if (di != 0) {
    targetI = clampf(targetI + di * I_STEPS[stepIdxI], 0.0f, I_MAX_CFG);
    dirtyI = true;
    lastEditMs = millis();
  }

  BtnEvent ev = pollButton(btnV);
  if (ev == BTN_SHORT) stepIdxV = (stepIdxV + 1) % (sizeof(V_STEPS) / sizeof(V_STEPS[0]));
  if (ev == BTN_LONG) setOutput(!dps.on);

  ev = pollButton(btnI);
  if (ev == BTN_SHORT) stepIdxI = (stepIdxI + 1) % (sizeof(I_STEPS) / sizeof(I_STEPS[0]));
  if (ev == BTN_LONG) setOutput(false);
}

static void sendPending() {
  if (millis() - lastWriteMs < WRITE_GAP_MS) return;
  if (dirtyV) {
    if (writeReg(REG_USET, (uint16_t)lroundf(targetV * V_SCALE))) dirtyV = false;
    lastWriteMs = millis();
    return;
  }
  if (dirtyI) {
    if (writeReg(REG_ISET, (uint16_t)lroundf(targetI * I_SCALE))) dirtyI = false;
    lastWriteMs = millis();
  }
}

static void syncFromDps() {

  if (!dps.linkOk || dirtyV || dirtyI) return;
  if (millis() - lastEditMs < SYNC_HOLD_MS) return;
  targetV = dps.uset;
  targetI = dps.iset;
}

static void drawSetpoint() {
  if (!oled1Ok) return;
  char buf[28];
  oled1.clearDisplay();
  oled1.setTextColor(SSD1306_WHITE);
  oled1.setTextSize(1);
  oled1.setCursor(0, 0);
  oled1.print("SETPOINT");
  oled1.setCursor(78, 0);
  oled1.print(dps.on ? "OUT ON" : "OUT OFF");
  oled1.setTextSize(2);
  snprintf(buf, sizeof(buf), "%6.2f V", targetV);
  oled1.setCursor(0, 14);
  oled1.print(buf);
  snprintf(buf, sizeof(buf), "%6.3f A", targetI);
  oled1.setCursor(0, 34);
  oled1.print(buf);
  oled1.setTextSize(1);
  snprintf(buf, sizeof(buf), "step V %.2f  I %.3f", V_STEPS[stepIdxV], I_STEPS[stepIdxI]);
  oled1.setCursor(0, 56);
  oled1.print(buf);
  oled1.display();
}

#if USE_DISPLAY2
static void drawMeasured() {
  if (!oled2Ok) return;
  char buf[28];
  oled2.clearDisplay();
  oled2.setTextColor(SSD1306_WHITE);
  oled2.setTextSize(1);
  oled2.setCursor(0, 0);
  oled2.print("MEASURED");
  oled2.setCursor(78, 0);
  oled2.print(!dps.linkOk ? "NO LINK" : (dps.cc ? "CC" : "CV"));
  oled2.setTextSize(2);
  snprintf(buf, sizeof(buf), "%6.2f V", dps.uout);
  oled2.setCursor(0, 14);
  oled2.print(buf);
  snprintf(buf, sizeof(buf), "%6.3f A", dps.iout);
  oled2.setCursor(0, 34);
  oled2.print(buf);
  oled2.setTextSize(1);
  oled2.setCursor(0, 56);
  if (dps.linkOk && dps.prot != 0) {
    const char* names[] = {"", "OVP", "OCP", "OPP"};
    snprintf(buf, sizeof(buf), "PROTEZIONE: %s", dps.prot <= 3 ? names[dps.prot] : "?");
  } else {
    snprintf(buf, sizeof(buf), "P %.1fW  Vin %.1fV", dps.uout * dps.iout, dps.uin);
  }
  oled2.print(buf);
  oled2.display();
}
#endif

void setup() {
  Serial.begin(115200);

  pinMode(ENC_V_SW, INPUT_PULLUP);
  pinMode(ENC_I_SW, INPUT_PULLUP);
  encBegin(encV);
  encBegin(encI);

  dpsSerial.begin(DPS_BAUD, SERIAL_8N1, DPS_RX_PIN, DPS_TX_PIN);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);
  oled1Ok = oled1.begin(SSD1306_SWITCHCAPVCC, OLED1_ADDR);
#if USE_DISPLAY2
  oled2Ok = oled2.begin(SSD1306_SWITCHCAPVCC, OLED2_ADDR);
#endif
  Serial.printf("OLED1=%d OLED2=%d\n", oled1Ok, oled2Ok);

  if (pollDps()) {
    targetV = dps.uset;
    targetI = dps.iset;
  }
  Serial.printf("DPS link: %s\n", dps.linkOk ? "OK" : "NO");
}

void loop() {
  uint32_t now = millis();

  handleInputs();
  sendPending();

  if (now - lastPollMs >= POLL_MS) {
    lastPollMs = now;
    pollDps();
    syncFromDps();
  }

  if (now - lastDrawMs >= DRAW_MS) {
    lastDrawMs = now;
    drawSetpoint();
#if USE_DISPLAY2
    drawMeasured();
#endif
  }
}
