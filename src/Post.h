// Power-on self-test. Runs once in setup(), after the radio / file system / config are up
// and before the UI tasks start (it probes the OLED on I2C itself, while nothing else owns
// the bus yet).
//
// Result = bit mask, one bit per failed check, 0 = all good:
//   bit 0 power    battery present but below Config::POST_BATT_MIN_MV (USB only is fine)
//   bit 1 nvs      NVS could not be opened
//   bit 2 radio    SX1276 init failed or its version register isn't 0x12
//   bit 3 oled     no I2C ACK from Config::OLED_I2C_ADDR
//   bit 4 storage  LittleFS / log files unavailable
//   bit 5 config   stored config damaged beyond recovery (defaults in use)
//   bit 6 reset    previous reset was a panic / interrupt or task watchdog / brownout
//   bit 7 forced   built with -D FW_FORCE_POST_FAIL (rollback demo image)
// The plain RTC watchdog reset (`wdt`) is not a failure: esptool leaves the chip that way
// after every flash. It is logged at INFO only.
//
// Protocol core only; the result is reported to SysLog, FlashLog (CSV type POST), /info,
// the start-up popup and the `post` command.
#pragma once

#include <Arduino.h>

namespace Post {

enum Bit : uint8_t {
  Power = 1u << 0,
  Nvs = 1u << 1,
  Radio = 1u << 2,
  Oled = 1u << 3,
  Storage = 1u << 4,
  ConfigBad = 1u << 5,
  ResetReason = 1u << 6,
  Forced = 1u << 7,
};
constexpr uint8_t BIT_COUNT = 8;

struct Inputs {
  bool radioInit;
  int16_t radioChipVersion;
  bool storageOk;
  bool nvsOk;
  bool configHealthy;
};

struct Result {
  uint8_t mask = 0;
  uint16_t batteryMv = 0; // 0 = no battery on the divider
  int16_t radioChipVersion = 0;
  bool ran = false;
};

void run(const Inputs &in);
const Result &result();

const char *bitName(uint8_t bit);   // "power", "nvs", ... (logs, console)
const char *bitLabel(uint8_t bit);  // Ukrainian, for the OLED popup
void describe(char *buf, size_t len); // popup text: "Самотест OK" / "Самотест: збій радіо, OLED"
void print(Print &out);               // `post` command

} // namespace Post
