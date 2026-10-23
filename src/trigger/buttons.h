#pragma once
#include <stdint.h>

enum class BtnEvent : uint8_t { NONE, GREEN_STOP, YELLOW_START };

// Дві кнопки з дебаунсом, активний LOW. Одночасне натискання ігнорується.
class Buttons {
 public:
  void begin(uint32_t now);
  void update(uint32_t now);
  BtnEvent take();                 // подія натискання (одноразова)
  bool anyPressed() const { return green_.stable || yellow_.stable; }

 private:
  struct Btn {
    int pin;
    bool raw;
    bool stable;
    uint32_t changedMs;
  };
  bool step(Btn& b, uint32_t now);  // true, якщо щойно підтверджено натискання

  Btn green_{};
  Btn yellow_{};
  BtnEvent pending_ = BtnEvent::NONE;
  bool blocked_ = false;  // після комбінації чекаємо відпускання обох
};
