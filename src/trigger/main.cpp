// Ініціатор спрацювання розумного вогнегасника (плата 1) — безпечна симуляція.
#include <Arduino.h>
#include <esp_system.h>
#include <soc/rtc_wdt.h>
#include <esp_task_wdt.h>

#include "buttons.h"
#include "config.h"
#include "dual_sensor.h"
#include "event_log.h"
#include "trigger_fsm.h"
#include "indicators.h"
#include "pwm_channel.h"
#include "selftest.h"
#include "vbus_monitor.h"
#include "web_ui.h"

namespace {
PwmChannel channelA(cfg::PIN_PWM_IN, pwmproto::DUTY_NORMAL_PCT, pwmproto::DUTY_FIRE_PCT, "A");
PwmChannel channelB(cfg::PIN_PWM_IN_B, pwmproto::DUTY_B_NORMAL_PCT, pwmproto::DUTY_B_FIRE_PCT, "B");
DualSensor sensor(&channelA, &channelB);
VbusMonitor vbus;
Buttons buttons;
TriggerFsm fsm;
StatusLed led;
StatusDisplay display;
WebUi web;
EventLog eventLog;
}  // namespace

void setup() {
  led.begin();  // перш за все: безпечний рівень на GPIO0
  Serial.begin(115200);
  Serial.printf("\n[boot] explosion trigger initiator, arm delay %lu ms\n", (unsigned long)cfg::ARM_DELAY);

  const SelfTestResult st = runSelfTest();
  if (st.displayOk) display.begin();

  const uint32_t now = millis();
  buttons.begin(now);
  vbus.begin(now);
  sensor.begin(now);

  eventLog.begin(st.bootNo);
  web.begin(&fsm, &eventLog);  // WiFi підключається у фоні, старт не блокується

  // Дати датчику та АЦП набрати перші кадри/вибірки перед стартом автомата
  const uint32_t t0 = millis();
  while (millis() - t0 < 400) {
    sensor.update(millis());
    vbus.update(millis());
    delay(1);
  }

  // Після аварійного перезавантаження (WDT/паніка/просадка) автомат стартує у FAULT до команди Stop
  const esp_reset_reason_t rr = esp_reset_reason();
  const bool abnormalReset = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
                             rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT;
  Serial.printf("[boot] reset reason %d%s\n", (int)rr, abnormalReset ? " (abnormal)" : "");
  fsm.setEventSink(&eventLog);
  fsm.begin(&sensor, &vbus, st.critical, abnormalReset, millis());
  char bootText[20];
  snprintf(bootText, sizeof(bootText), "boot rst=%d", (int)rr);
  eventLog.event(EventType::BOOT, fsm.state(), fsm.state(), bootText, millis());

  esp_task_wdt_init(cfg::WDT_TIMEOUT_S, true);  // зависання -> перезавантаження у SAFE
  esp_task_wdt_add(nullptr);

  // Другий, незалежний watchdog (RTC): страхує на випадок, якщо task WDT сам не спрацює.
  rtc_wdt_protect_off();
  rtc_wdt_disable();
  rtc_wdt_set_stage(RTC_WDT_STAGE0, RTC_WDT_STAGE_ACTION_RESET_SYSTEM);
  rtc_wdt_set_time(RTC_WDT_STAGE0, cfg::RTC_WDT_TIMEOUT_MS);
  rtc_wdt_enable();
  rtc_wdt_protect_on();
}

void loop() {
  const uint32_t now = millis();

  buttons.update(now);
  sensor.update(now);
  vbus.update(now);

  switch (buttons.take()) {
    case BtnEvent::YELLOW_START: fsm.command(Cmd::START, Source::BUTTON, now); break;
    case BtnEvent::GREEN_STOP: fsm.command(Cmd::STOP, Source::BUTTON, now); break;
    default: break;
  }

  fsm.update(now);

  static SensorStatus lastSensor = SensorStatus::UNKNOWN;  // діагностика: зміни статусу датчика в Serial
  static char lastDetail[32] = "";
  if (sensor.status() != lastSensor || strcmp(sensor.detail(), lastDetail) != 0) {
    lastSensor = sensor.status();
    strncpy(lastDetail, sensor.detail(), sizeof(lastDetail) - 1);
    Serial.printf("[sensor] %s (%s) duty %d%% vbus %lu mV\n", sensorStatusName(lastSensor), lastDetail, sensor.dutyPercent(),
                  (unsigned long)vbus.millivolts());
  }

  const Snapshot snap = fsm.snapshot(now);
  led.update(snap.state, fsm.explosionOutputActive(), snap.testMode, snap.fireIgnored, now);  // до повільних веб/дисплея
  web.handle();
  display.update(snap, web.ip().c_str(), now);
  eventLog.flush(now, fsm.state() != State::ARMING && fsm.state() != State::READY);

  if (fsm.takeWdtTestRequest()) {
    Serial.println("[wdt-test] hanging loop on purpose");
    for (;;) {}  // task WDT має перезавантажити плату; після скидання автомат стартує у FAULT
  }

  // Обидва watchdog-и годуємо в КІНЦІ ітерації: зависання будь-де в циклі веде до скидання
  esp_task_wdt_reset();
  rtc_wdt_protect_off();
  rtc_wdt_feed();
  rtc_wdt_protect_on();
  delay(1);
}
