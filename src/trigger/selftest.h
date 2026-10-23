#pragma once
#include <stdint.h>

struct SelfTestResult {
  uint8_t critical;     // маска CriticalFault
  bool displayOk;       // некритично
  uint32_t bootNo;      // лічильник завантажень з NVS (0, якщо NVS недоступна)
};

// Перевіряє ID чіпа, файлову систему (LittleFS + index.html), NVS-налаштування,
// відсутність залипання кнопок і наявність OLED. Результат друкується в Serial.
SelfTestResult runSelfTest();
