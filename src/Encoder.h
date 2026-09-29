// Quadrature encoder + button, pure polling - no interrupts (see Config.h /
// project notes: this lives on its own dedicated UI-task core, so a plain
// poll every ~1-2ms is simple, reliable, and needs no ISR at all).
#pragma once

#include <Arduino.h>

class Encoder {
public:
  void begin();

  // Call every UI task tick. Returns rotation delta accumulated since the
  // last call: +1 per clockwise detent, -1 per counter-clockwise detent.
  int poll();

  // Call every UI task tick, after poll(). Returns true exactly once per confirmed short
  // press; a "press" that coincides with A/B activity (see Config::BUTTON_ROTATION_LOCKOUT_MS)
  // is treated as crosstalk from the rotation and ignored.
  bool pollButton();

private:
  uint8_t quadratureState_ = 0;
  uint8_t lastPinState_ = 0;
  uint32_t lastQuadEdgeMs_ = 0;

  bool lastRawButton_ = true;   // idle = HIGH (external pull-up)
  bool stableButton_ = true;
  uint32_t lastButtonChangeMs_ = 0;
};
