#include "Encoder.h"
#include "Config.h"

namespace {
// Buxton full-step quadrature state table - a small, well-tested polling
// state machine (no interrupts needed): feed it the 2-bit A/B pin reading
// each tick, it returns a direction exactly once per detent.
constexpr uint8_t R_START = 0x0;
constexpr uint8_t R_CW_FINAL = 0x1;
constexpr uint8_t R_CW_BEGIN = 0x2;
constexpr uint8_t R_CW_NEXT = 0x3;
constexpr uint8_t R_CCW_BEGIN = 0x4;
constexpr uint8_t R_CCW_FINAL = 0x5;
constexpr uint8_t R_CCW_NEXT = 0x6;
constexpr uint8_t DIR_CW = 0x10;
constexpr uint8_t DIR_CCW = 0x20;

const uint8_t kTransitionTable[7][4] = {
  {R_START, R_CW_BEGIN, R_CCW_BEGIN, R_START},
  {R_CW_NEXT, R_START, R_CW_FINAL, R_START | DIR_CW},
  {R_CW_NEXT, R_CW_BEGIN, R_START, R_START},
  {R_CW_NEXT, R_CW_BEGIN, R_CW_FINAL, R_START},
  {R_CCW_NEXT, R_START, R_CCW_BEGIN, R_START},
  {R_CCW_NEXT, R_CCW_FINAL, R_START, R_START | DIR_CCW},
  {R_CCW_NEXT, R_CCW_FINAL, R_CCW_BEGIN, R_START},
};

} // namespace

void Encoder::begin() {
  pinMode(Config::PIN_ENCODER_A, INPUT);
  pinMode(Config::PIN_ENCODER_B, INPUT);
  pinMode(Config::PIN_ENCODER_BUTTON, INPUT);
  quadratureState_ = R_START;
  lastRawButton_ = stableButton_ = digitalRead(Config::PIN_ENCODER_BUTTON);
  lastButtonChangeMs_ = millis();
  lastPinState_ = (digitalRead(Config::PIN_ENCODER_B) << 1) | digitalRead(Config::PIN_ENCODER_A);
  lastQuadEdgeMs_ = 0;
}

int Encoder::poll() {
  const uint8_t pinState = (digitalRead(Config::PIN_ENCODER_B) << 1) | digitalRead(Config::PIN_ENCODER_A);
  if (pinState != lastPinState_) {
    lastPinState_ = pinState;
    lastQuadEdgeMs_ = millis(); // any A/B activity - used to reject coupled "presses" below
  }
  quadratureState_ = kTransitionTable[quadratureState_ & 0x0F][pinState];
  const uint8_t direction = quadratureState_ & 0x30;
  if (direction == DIR_CW) return 1;
  if (direction == DIR_CCW) return -1;
  return 0;
}

bool Encoder::pollButton() {
  const bool raw = digitalRead(Config::PIN_ENCODER_BUTTON);
  const uint32_t now = millis();
  if (raw != lastRawButton_) {
    lastRawButton_ = raw;
    lastButtonChangeMs_ = now;
  }
  if (now - lastButtonChangeMs_ >= Config::BUTTON_DEBOUNCE_MS && stableButton_ != lastRawButton_) {
    const bool previouslyStable = stableButton_;
    stableButton_ = lastRawButton_;
    // Active-low button (external pull-up): a press is a HIGH->LOW transition.
    if (previouslyStable == true && stableButton_ == false) {
      // A weak/missing pull-up on the button line lets the A/B edges couple into it, so a
      // turn looks like a press. Reject any "press" that began within the lockout window of
      // quadrature activity (the low level started at lastButtonChangeMs_).
      const uint32_t pressStart = lastButtonChangeMs_;
      const bool nearRotation = lastQuadEdgeMs_ != 0 &&
                                static_cast<int32_t>(lastQuadEdgeMs_ - (pressStart - Config::BUTTON_ROTATION_LOCKOUT_MS)) >= 0;
      return !nearRotation;
    }
  }
  return false;
}
