#include "buttons.h"

#include <Arduino.h>

#include "config.h"

void Buttons::begin(uint32_t now) {
  green_ = {cfg::PIN_BTN_GREEN, false, false, now};
  yellow_ = {cfg::PIN_BTN_YELLOW, false, false, now};
  pinMode(green_.pin, INPUT);        // GPIO34 без внутрішньої підтяжки, є зовнішня до 3V3
  pinMode(yellow_.pin, INPUT_PULLUP);
  green_.raw = green_.stable = digitalRead(green_.pin) == LOW;
  yellow_.raw = yellow_.stable = digitalRead(yellow_.pin) == LOW;
}

bool Buttons::step(Btn& b, uint32_t now) {
  const bool raw = digitalRead(b.pin) == LOW;
  if (raw != b.raw) {
    b.raw = raw;
    b.changedMs = now;
  }
  if (now - b.changedMs >= cfg::BTN_DEBOUNCE_MS && b.stable != b.raw) {
    b.stable = b.raw;
    return b.stable;
  }
  return false;
}

void Buttons::update(uint32_t now) {
  const bool g = step(green_, now);
  const bool y = step(yellow_, now);

  if (blocked_) {
    if (!green_.stable && !yellow_.stable) blocked_ = false;
    return;
  }
  if (green_.stable && yellow_.stable) {  // комбінація — некоректний ввід
    blocked_ = true;
    pending_ = BtnEvent::NONE;
    return;
  }
  if (g) pending_ = BtnEvent::GREEN_STOP;
  else if (y) pending_ = BtnEvent::YELLOW_START;
}

BtnEvent Buttons::take() {
  const BtnEvent e = pending_;
  pending_ = BtnEvent::NONE;
  return e;
}
