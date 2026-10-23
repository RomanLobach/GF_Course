// Імітатор датчика пожежі (плата 2) з ін'єкцією відмов.
//  * Коротке натискання red: NORMAL <-> FIRE (з режиму відмови повертає в NORMAL).
//  * Утримання red >= LONG_PRESS_MS: наступний режим відмови (по колу), щоб перевіряти fail-safe ініціатора.
// Стан передається двома PWM-лініями (див. pwm_protocol.h): A пряма, B інверсна.
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <pwm_protocol.h>

namespace {
constexpr int PIN_BTN = 34;        // red, підтяжка до 3V3 ззовні, активний LOW
constexpr int PIN_PWM_A = 13;
constexpr int PIN_PWM_B = 15;
constexpr int PIN_LED = 14;        // активний HIGH: GPIO14 -> LED -> R -> GND
constexpr int PIN_OLED_SDA = 21;
constexpr int PIN_OLED_SCL = 22;

#ifndef SENSOR_LED_ACTIVE_LOW
#define SENSOR_LED_ACTIVE_LOW 0
#endif

// LEDC-канали 0 і 2 лежать на різних таймерах, тож частоту A і B можна міняти незалежно.
constexpr uint8_t CH_A = 0;
constexpr uint8_t CH_B = 2;
constexpr uint8_t PWM_RES_BITS = 10;
constexpr uint32_t BTN_DEBOUNCE_MS = 30;
constexpr uint32_t LONG_PRESS_MS = 700;
constexpr uint32_t DISPLAY_PERIOD_MS = 200;
constexpr uint32_t GLITCH_PERIOD_MS = 1000;
constexpr uint32_t GLITCH_LEN_MS = 20;  // коротше за фільтр ініціатора -> спрацювання не очікується

enum class Mode : uint8_t {
  NORMAL,
  FIRE,
  F_NO_SIGNAL,      // обидві лінії LOW (обрив/мертвий датчик)
  F_STUCK_HIGH,     // обидві лінії HIGH (КЗ на живлення)
  F_BAD_FREQ,       // 500 Гц замість 1 кГц
  F_BAD_DUTY,       // шпаруватість 50% (поза вікнами)
  F_ONLY_A,         // лінія B мертва
  F_ONLY_B,         // лінія A мертва
  F_SAME_POLARITY,  // B не інверсна (замкнуті лінії / помилка кабелю): A=FIRE, B=NORMAL-рівень
  F_FIRE_GLITCH,    // короткі хибні імпульси FIRE на фоні NORMAL
  COUNT
};

const char* modeName(Mode m) {
  switch (m) {
    case Mode::NORMAL: return "NORMAL";
    case Mode::FIRE: return "FIRE!";
    case Mode::F_NO_SIGNAL: return "no signal";
    case Mode::F_STUCK_HIGH: return "stuck high";
    case Mode::F_BAD_FREQ: return "bad freq 500Hz";
    case Mode::F_BAD_DUTY: return "bad duty 50%";
    case Mode::F_ONLY_A: return "line B dead";
    case Mode::F_ONLY_B: return "line A dead";
    case Mode::F_SAME_POLARITY: return "B not inverted";
    case Mode::F_FIRE_GLITCH: return "fire glitches";
    default: return "?";
  }
}

bool isFault(Mode m) { return m != Mode::NORMAL && m != Mode::FIRE; }

// Екран на платі змонтований догори дригом -> поворот на 180 градусів (R2).
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R2, U8X8_PIN_NONE, PIN_OLED_SCL, PIN_OLED_SDA);

Mode mode = Mode::NORMAL;
bool glitching = false;
uint32_t glitchSinceMs = 0;
uint32_t lastDisplayMs = 0;

bool btnRaw = false;
bool btnStable = false;
bool longHandled = false;
uint32_t btnChangedMs = 0;
uint32_t btnPressedMs = 0;

// Лінія як PWM із заданою частотою/шпаруватістю або як константа (0/1).
struct Drive {
  int pct;     // 0..100, -1 = константа LOW, -2 = константа HIGH
  uint32_t freq;
};

void driveLine(int pin, uint8_t ch, Drive d) {
  if (d.pct == -1 || d.pct == -2) {
    ledcDetachPin(pin);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, d.pct == -2 ? HIGH : LOW);
    return;
  }
  ledcSetup(ch, d.freq, PWM_RES_BITS);
  ledcAttachPin(pin, ch);
  ledcWrite(ch, (1UL << PWM_RES_BITS) * d.pct / 100);
}

void applyMode() {
  const uint32_t f = pwmproto::FREQ_HZ;
  Drive a{pwmproto::DUTY_NORMAL_PCT, f};
  Drive b{pwmproto::DUTY_B_NORMAL_PCT, f};
  switch (mode) {
    case Mode::FIRE: a.pct = pwmproto::DUTY_FIRE_PCT; b.pct = pwmproto::DUTY_B_FIRE_PCT; break;
    case Mode::F_NO_SIGNAL: a.pct = b.pct = -1; break;
    case Mode::F_STUCK_HIGH: a.pct = b.pct = -2; break;
    case Mode::F_BAD_FREQ: a.freq = b.freq = f / 2; break;
    case Mode::F_BAD_DUTY: a.pct = b.pct = 50; break;
    case Mode::F_ONLY_A: b.pct = -1; break;
    case Mode::F_ONLY_B: a.pct = -1; break;
    case Mode::F_SAME_POLARITY: a.pct = pwmproto::DUTY_FIRE_PCT; b.pct = pwmproto::DUTY_B_NORMAL_PCT; break;
    default: break;  // NORMAL і F_FIRE_GLITCH (глітч додається в loop)
  }
  driveLine(PIN_PWM_A, CH_A, a);
  driveLine(PIN_PWM_B, CH_B, b);
}

void setGlitch(bool on) {
  glitching = on;
  ledcWrite(CH_A, (1UL << PWM_RES_BITS) * (on ? pwmproto::DUTY_FIRE_PCT : pwmproto::DUTY_NORMAL_PCT) / 100);
  ledcWrite(CH_B, (1UL << PWM_RES_BITS) * (on ? pwmproto::DUTY_B_FIRE_PCT : pwmproto::DUTY_B_NORMAL_PCT) / 100);
}

void setMode(Mode m) {
  mode = m;
  glitching = false;
  applyMode();
  Serial.printf("[sensor] mode: %s\n", modeName(mode));
}

void drawDisplay() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 10, "SENSOR SIMULATOR");
  oled.setFont(u8g2_font_logisoso24_tr);
  oled.drawStr(0, 44, mode == Mode::NORMAL ? "NORMAL" : mode == Mode::FIRE ? "FIRE!" : "FAULT");
  oled.setFont(u8g2_font_6x10_tf);
  char line[28];
  if (isFault(mode)) snprintf(line, sizeof(line), "%s", modeName(mode));
  else if (mode == Mode::FIRE) snprintf(line, sizeof(line), "A%u%% B%u%% %luHz", pwmproto::DUTY_FIRE_PCT, pwmproto::DUTY_B_FIRE_PCT, (unsigned long)pwmproto::FREQ_HZ);
  else snprintf(line, sizeof(line), "A%u%% B%u%% %luHz", pwmproto::DUTY_NORMAL_PCT, pwmproto::DUTY_B_NORMAL_PCT, (unsigned long)pwmproto::FREQ_HZ);
  oled.drawStr(0, 60, line);
  oled.sendBuffer();
}

void updateLed(uint32_t now) {
  bool on;
  if (isFault(mode)) on = (now / 125) % 2 == 0;  // відмова: швидке миготіння
  else on = mode == Mode::FIRE;
  digitalWrite(PIN_LED, (on != (SENSOR_LED_ACTIVE_LOW != 0)) ? HIGH : LOW);
}

void updateButton(uint32_t now) {
  const bool raw = digitalRead(PIN_BTN) == LOW;
  if (raw != btnRaw) {
    btnRaw = raw;
    btnChangedMs = now;
  }
  if (now - btnChangedMs >= BTN_DEBOUNCE_MS && btnStable != btnRaw) {
    btnStable = btnRaw;
    if (btnStable) {
      btnPressedMs = now;
      longHandled = false;
    } else if (!longHandled) {  // коротке натискання спрацьовує при відпусканні
      setMode(mode == Mode::NORMAL ? Mode::FIRE : Mode::NORMAL);
      drawDisplay();
    }
  }
  if (btnStable && !longHandled && now - btnPressedMs >= LONG_PRESS_MS) {  // довге: наступна відмова
    longHandled = true;
    const uint8_t next = isFault(mode) ? (uint8_t)mode + 1 : (uint8_t)Mode::F_NO_SIGNAL;
    setMode(next >= (uint8_t)Mode::COUNT ? Mode::NORMAL : (Mode)next);
    drawDisplay();
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN, INPUT);
  pinMode(PIN_LED, OUTPUT);
  oled.begin();
  btnRaw = btnStable = digitalRead(PIN_BTN) == LOW;
  setMode(Mode::NORMAL);  // завжди стартуємо у NORMAL
  drawDisplay();
  Serial.println("[sensor] ready: short press = NORMAL/FIRE, long press = next fault mode");
}

void loop() {
  const uint32_t now = millis();
  updateButton(now);
  updateLed(now);

  if (mode == Mode::F_FIRE_GLITCH) {
    if (!glitching && now - glitchSinceMs >= GLITCH_PERIOD_MS) {
      glitchSinceMs = now;
      setGlitch(true);
    } else if (glitching && now - glitchSinceMs >= GLITCH_LEN_MS) {
      setGlitch(false);
    }
  }

  if (now - lastDisplayMs >= DISPLAY_PERIOD_MS) {
    lastDisplayMs = now;
    drawDisplay();
  }
}
