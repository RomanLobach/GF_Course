// Юніт-тести fail-safe автомата на хості: pio test -e native
#include <unity.h>

#include "config.h"
#include "dual_sensor.h"
#include "extinguisher_fsm.h"

namespace {
struct FakeSensor : ISensor {
  SensorStatus st = SensorStatus::NORMAL;
  void begin(uint32_t) override {}
  void update(uint32_t) override {}
  SensorStatus status() const override { return st; }
  const char* detail() const override { return st == SensorStatus::FAULT ? "sensor fault" : "ok"; }
  int dutyPercent() const override { return 25; }
};
struct FakeVbus : IVoltageMonitor {
  bool good = true;
  uint32_t millivolts() const override { return 4380; }
  bool ok() const override { return good; }
};

FakeSensor sensor;
FakeVbus vbus;
ExtinguisherFsm fsm;
uint32_t t = 0;

void tick(uint32_t ms) {  // крок 10 мс, щоб автомат бачив усі проміжні моменти
  for (uint32_t i = 0; i < ms; i += 10) {
    t += 10;
    fsm.update(t);
  }
}
void boot(uint8_t critical = CF_NONE, bool abnormal = false) {
  sensor = FakeSensor();
  vbus = FakeVbus();
  fsm = ExtinguisherFsm();
  t = 1000;
  fsm.begin(&sensor, &vbus, critical, abnormal, t);
}
void toReady() {
  TEST_ASSERT_TRUE(fsm.command(Cmd::START, Source::BUTTON, t).accepted);
  tick(cfg::ARM_DELAY + 20);
  TEST_ASSERT_EQUAL(State::READY, fsm.state());
}
}  // namespace

void setUp() { boot(); }
void tearDown() {}

void test_boot_clean_is_safe() { TEST_ASSERT_EQUAL(State::SAFE, fsm.state()); }

void test_boot_critical_fault_blocks_start() {
  boot(CF_FILESYSTEM);
  TEST_ASSERT_EQUAL(State::FAULT, fsm.state());
  TEST_ASSERT_TRUE(fsm.command(Cmd::STOP, Source::BUTTON, t).accepted);
  TEST_ASSERT_EQUAL(State::SAFE, fsm.state());
  TEST_ASSERT_FALSE(fsm.command(Cmd::START, Source::BUTTON, t).accepted);
  TEST_ASSERT_EQUAL(State::SAFE, fsm.state());
}

void test_abnormal_reset_needs_ack() {
  boot(CF_NONE, true);
  TEST_ASSERT_EQUAL(State::FAULT, fsm.state());
  TEST_ASSERT_FALSE(fsm.command(Cmd::START, Source::BUTTON, t).accepted);
  fsm.command(Cmd::STOP, Source::BUTTON, t);
  TEST_ASSERT_TRUE(fsm.command(Cmd::START, Source::BUTTON, t).accepted);
}

void test_arming_timer_then_ready() {
  TEST_ASSERT_TRUE(fsm.command(Cmd::START, Source::BUTTON, t).accepted);
  tick(cfg::ARM_DELAY - 100);
  TEST_ASSERT_EQUAL(State::ARMING, fsm.state());
  tick(200);
  TEST_ASSERT_EQUAL(State::READY, fsm.state());
}

void test_start_refused_on_bad_inputs() {
  sensor.st = SensorStatus::FAULT;
  TEST_ASSERT_FALSE(fsm.command(Cmd::START, Source::BUTTON, t).accepted);
  sensor.st = SensorStatus::UNKNOWN;
  TEST_ASSERT_FALSE(fsm.command(Cmd::START, Source::BUTTON, t).accepted);
  sensor.st = SensorStatus::FIRE;
  TEST_ASSERT_FALSE(fsm.command(Cmd::START, Source::BUTTON, t).accepted);
  sensor.st = SensorStatus::NORMAL;
  vbus.good = false;
  TEST_ASSERT_FALSE(fsm.command(Cmd::START, Source::BUTTON, t).accepted);
  TEST_ASSERT_EQUAL(State::SAFE, fsm.state());
}

void test_fire_never_discharges_when_not_ready() {
  sensor.st = SensorStatus::FIRE;
  tick(10000);
  TEST_ASSERT_EQUAL(State::SAFE, fsm.state());
  sensor.st = SensorStatus::NORMAL;
  fsm.command(Cmd::START, Source::BUTTON, t);
  sensor.st = SensorStatus::FIRE;  // пожежа під час ARMING не спрацьовує, а в READY — лише після підтвердження
  tick(cfg::ARM_DELAY - 100);
  TEST_ASSERT_EQUAL(State::ARMING, fsm.state());
}

void test_sensor_fire_discharges_instantly_after_settle() {
  toReady();
  tick(cfg::READY_SETTLE_MS);
  sensor.st = SensorStatus::FIRE;
  tick(10);  // один тік автомата
  TEST_ASSERT_EQUAL(State::DISCHARGE, fsm.state());
}

void test_fire_present_through_settle_discharges_when_settled() {
  toReady();
  sensor.st = SensorStatus::FIRE;
  tick(cfg::READY_SETTLE_MS - 100);
  TEST_ASSERT_EQUAL(State::READY, fsm.state());
  tick(200);
  TEST_ASSERT_EQUAL(State::DISCHARGE, fsm.state());
}

void test_fire_present_through_arming_is_ignored_until_it_clears() {
  fsm.command(Cmd::START, Source::BUTTON, t);
  sensor.st = SensorStatus::FIRE;  // сигнал почався ДО READY
  tick(cfg::ARM_DELAY + 20);
  TEST_ASSERT_EQUAL(State::READY, fsm.state());
  TEST_ASSERT_TRUE(fsm.snapshot(t).fireIgnored);
  tick(10000);  // скільки б він не тримався, спрацювання немає
  TEST_ASSERT_EQUAL(State::READY, fsm.state());
  sensor.st = SensorStatus::NORMAL;  // сигнал зник...
  tick(100);
  TEST_ASSERT_FALSE(fsm.snapshot(t).fireIgnored);
  sensor.st = SensorStatus::FIRE;    // ...і з'явився знову вже в READY -> новий сигнал
  tick(20);
  TEST_ASSERT_EQUAL(State::DISCHARGE, fsm.state());
}

void test_fire_started_in_last_moment_of_arming_is_ignored() {
  fsm.command(Cmd::START, Source::BUTTON, t);
  tick(cfg::ARM_DELAY - 30);
  sensor.st = SensorStatus::FIRE;
  tick(60);  // перехід у READY відбувається тут
  tick(cfg::READY_SETTLE_MS + 500);
  TEST_ASSERT_EQUAL(State::READY, fsm.state());
}

void test_fire_during_settle_ignored() {
  toReady();
  sensor.st = SensorStatus::FIRE;
  tick(cfg::READY_SETTLE_MS - 100);  // toReady() повертається на 10-20 мс після входу в READY
  TEST_ASSERT_EQUAL(State::READY, fsm.state());
  sensor.st = SensorStatus::NORMAL;
  tick(2000);
  TEST_ASSERT_EQUAL(State::READY, fsm.state());
}

void test_sensor_fault_in_ready_goes_fault_not_discharge() {
  toReady();
  tick(cfg::READY_SETTLE_MS);
  sensor.st = SensorStatus::FAULT;
  tick(20);
  TEST_ASSERT_EQUAL(State::FAULT, fsm.state());
  sensor.st = SensorStatus::FIRE;
  tick(10000);
  TEST_ASSERT_EQUAL(State::FAULT, fsm.state());
}

void test_power_fault_in_arming_goes_fault() {
  fsm.command(Cmd::START, Source::BUTTON, t);
  vbus.good = false;
  tick(20);
  TEST_ASSERT_EQUAL(State::FAULT, fsm.state());
}

void test_manual_discharge_is_two_step_web_only() {
  toReady();
  tick(cfg::READY_SETTLE_MS + 20);
  TEST_ASSERT_FALSE(fsm.command(Cmd::DISCHARGE, Source::BUTTON, t).accepted);
  CmdResult r1 = fsm.command(Cmd::DISCHARGE, Source::WEB, t);
  TEST_ASSERT_TRUE(r1.accepted);
  TEST_ASSERT_TRUE(r1.needsConfirm);
  TEST_ASSERT_EQUAL(State::READY, fsm.state());
  CmdResult r2 = fsm.command(Cmd::DISCHARGE, Source::WEB, t + 100);
  TEST_ASSERT_TRUE(r2.accepted);
  TEST_ASSERT_EQUAL(State::DISCHARGE, fsm.state());
}

void test_manual_discharge_confirm_expires() {
  toReady();
  tick(cfg::READY_SETTLE_MS + 20);
  fsm.command(Cmd::DISCHARGE, Source::WEB, t);
  tick(cfg::DISCHARGE_CONFIRM_WINDOW_MS + 100);
  CmdResult r = fsm.command(Cmd::DISCHARGE, Source::WEB, t);
  TEST_ASSERT_TRUE(r.needsConfirm);  // знову перший крок, спрацювання немає
  TEST_ASSERT_EQUAL(State::READY, fsm.state());
}

void test_manual_discharge_rejected_outside_ready() {
  TEST_ASSERT_FALSE(fsm.command(Cmd::DISCHARGE, Source::WEB, t).accepted);  // SAFE
  fsm.command(Cmd::START, Source::BUTTON, t);
  TEST_ASSERT_FALSE(fsm.command(Cmd::DISCHARGE, Source::WEB, t).accepted);  // ARMING
  fsm.command(Cmd::STOP, Source::BUTTON, t);
  sensor.st = SensorStatus::FAULT;
  boot(CF_SETTINGS);
  TEST_ASSERT_FALSE(fsm.command(Cmd::DISCHARGE, Source::WEB, t).accepted);  // FAULT
}

void test_manual_discharge_blocked_by_fault_between_steps() {
  toReady();
  tick(cfg::READY_SETTLE_MS + 20);
  fsm.command(Cmd::DISCHARGE, Source::WEB, t);
  sensor.st = SensorStatus::FAULT;
  CmdResult r = fsm.command(Cmd::DISCHARGE, Source::WEB, t + 10);
  TEST_ASSERT_FALSE(r.accepted);
  TEST_ASSERT_NOT_EQUAL(State::DISCHARGE, fsm.state());
}

void test_manual_discharge_blocked_during_settle() {
  toReady();
  fsm.command(Cmd::DISCHARGE, Source::WEB, t);
  CmdResult r = fsm.command(Cmd::DISCHARGE, Source::WEB, t + 10);
  TEST_ASSERT_FALSE(r.accepted);
  TEST_ASSERT_EQUAL(State::READY, fsm.state());
}

void test_discharge_latched_until_stop() {
  toReady();
  sensor.st = SensorStatus::FIRE;
  tick(cfg::READY_SETTLE_MS + 100);
  TEST_ASSERT_EQUAL(State::DISCHARGE, fsm.state());
  sensor.st = SensorStatus::NORMAL;
  tick(5000);
  TEST_ASSERT_EQUAL(State::DISCHARGE, fsm.state());
  TEST_ASSERT_FALSE(fsm.command(Cmd::START, Source::BUTTON, t).accepted);
  fsm.command(Cmd::STOP, Source::BUTTON, t);
  TEST_ASSERT_EQUAL(State::SAFE, fsm.state());
}

void test_stop_from_every_state() {
  fsm.command(Cmd::START, Source::BUTTON, t);
  fsm.command(Cmd::STOP, Source::BUTTON, t);
  TEST_ASSERT_EQUAL(State::SAFE, fsm.state());
  toReady();
  fsm.command(Cmd::STOP, Source::BUTTON, t);
  TEST_ASSERT_EQUAL(State::SAFE, fsm.state());
  sensor.st = SensorStatus::FAULT;
  fsm.command(Cmd::START, Source::BUTTON, t);  // відхилено
  boot(CF_CHIP_ID);
  fsm.command(Cmd::STOP, Source::BUTTON, t);
  TEST_ASSERT_EQUAL(State::SAFE, fsm.state());
}

void test_illegal_transitions_rejected() {
  TEST_ASSERT_FALSE(fsm.transitionForTest(State::READY, t));      // SAFE -> READY
  TEST_ASSERT_EQUAL(State::FAULT, fsm.state());                   // помилка коду -> FAULT
  boot();
  TEST_ASSERT_FALSE(fsm.transitionForTest(State::DISCHARGE, t));  // SAFE -> DISCHARGE
  TEST_ASSERT_NOT_EQUAL(State::DISCHARGE, fsm.state());
  boot(CF_SETTINGS);
  TEST_ASSERT_FALSE(fsm.transitionForTest(State::DISCHARGE, t));  // FAULT -> DISCHARGE
  TEST_ASSERT_EQUAL(State::FAULT, fsm.state());
}

void test_state_corruption_goes_fault() {
  toReady();
  fsm.corruptForTest((uint8_t)State::DISCHARGE);  // зіпсували змінну без оновлення інверсної копії
  TEST_ASSERT_EQUAL(State::FAULT, fsm.state());   // навіть геттер не віддає DISCHARGE
  fsm.update(t + 10);
  TEST_ASSERT_EQUAL(State::FAULT, fsm.state());
  fsm.corruptForTest(200);                        // значення поза enum
  fsm.update(t + 20);
  TEST_ASSERT_EQUAL(State::FAULT, fsm.state());
  fsm.command(Cmd::STOP, Source::BUTTON, t + 30);
  TEST_ASSERT_EQUAL(State::SAFE, fsm.state());
}

void test_double_button_start_ignored_when_not_safe() {
  fsm.command(Cmd::START, Source::BUTTON, t);
  TEST_ASSERT_FALSE(fsm.command(Cmd::START, Source::BUTTON, t + 10).accepted);
  TEST_ASSERT_EQUAL(State::ARMING, fsm.state());
}


// ---------- DualSensor ----------
namespace {
FakeSensor chA, chB;
DualSensor dual(&chA, &chB);
uint32_t dt = 0;
void dtick(uint32_t ms) {
  for (uint32_t i = 0; i < ms; i += 10) {
    dt += 10;
    dual.update(dt);
  }
}
void dualReset(SensorStatus a = SensorStatus::NORMAL, SensorStatus b = SensorStatus::NORMAL) {
  chA = FakeSensor();
  chB = FakeSensor();
  chA.st = a;
  chB.st = b;
  dual = DualSensor(&chA, &chB);
  dt = 1000;
  dtick(20);
}
}  // namespace

void test_dual_agreeing_channels() {
  dualReset();
  TEST_ASSERT_EQUAL(SensorStatus::NORMAL, dual.status());
  chA.st = chB.st = SensorStatus::FIRE;
  dtick(20);
  TEST_ASSERT_EQUAL(SensorStatus::FIRE, dual.status());
}

void test_dual_waits_for_both_channels() {
  dualReset(SensorStatus::NORMAL, SensorStatus::UNKNOWN);
  TEST_ASSERT_EQUAL(SensorStatus::UNKNOWN, dual.status());
}

void test_dual_channel_fault_is_fault() {
  dualReset();
  chB.st = SensorStatus::FAULT;
  dtick(20);
  TEST_ASSERT_EQUAL(SensorStatus::FAULT, dual.status());
  chB.st = SensorStatus::NORMAL;
  dtick(20);
  TEST_ASSERT_EQUAL(SensorStatus::NORMAL, dual.status());
}

void test_dual_short_skew_does_not_trigger_fire() {
  dualReset();
  chA.st = SensorStatus::FIRE;  // лише один канал
  dtick(cfg::CHANNEL_MISMATCH_MS - 30);
  TEST_ASSERT_EQUAL(SensorStatus::NORMAL, dual.status());  // не FIRE і не FAULT
  chB.st = SensorStatus::FIRE;
  dtick(20);
  TEST_ASSERT_EQUAL(SensorStatus::FIRE, dual.status());
}

void test_dual_single_channel_fire_never_reports_fire() {
  dualReset();
  chA.st = SensorStatus::FIRE;  // B застряг у NORMAL
  for (int i = 0; i < 100; i++) {
    dtick(10);
    TEST_ASSERT_NOT_EQUAL(SensorStatus::FIRE, dual.status());
  }
  TEST_ASSERT_EQUAL(SensorStatus::FAULT, dual.status());
  TEST_ASSERT_EQUAL_STRING("channels disagree", dual.detail());
}

void test_fsm_does_not_discharge_on_single_channel_fire() {
  FakeVbus v;
  ExtinguisherFsm f;
  dualReset();
  uint32_t ft = dt;
  f.begin(&dual, &v, CF_NONE, false, ft);
  TEST_ASSERT_TRUE(f.command(Cmd::START, Source::BUTTON, ft).accepted);
  for (uint32_t i = 0; i < cfg::ARM_DELAY + 100; i += 10) {
    ft += 10; dt = ft; dual.update(ft); f.update(ft);
  }
  TEST_ASSERT_EQUAL(State::READY, f.state());
  chA.st = SensorStatus::FIRE;  // пошкоджена друга лінія: FIRE бачить лише канал A
  for (uint32_t i = 0; i < 3000; i += 10) {
    ft += 10; dt = ft; dual.update(ft); f.update(ft);
    TEST_ASSERT_NOT_EQUAL(State::DISCHARGE, f.state());
  }
  TEST_ASSERT_EQUAL(State::FAULT, f.state());
}

// ---------- тест-режим, watchdog-тест, журнал ----------
namespace {
struct Rec : IEventSink {
  int transitions = 0, rejects = 0, testEvents = 0, ignored = 0;
  State lastTo = State::INIT;
  void event(EventType t, State, State to, const char*, uint32_t) override {
    if (t == EventType::TRANSITION) { transitions++; lastTo = to; }
    if (t == EventType::CMD_REJECT) rejects++;
    if (t == EventType::TEST_MODE) testEvents++;
    if (t == EventType::FIRE_IGNORED) ignored++;
  }
};
}  // namespace

void test_test_mode_suppresses_discharge_output() {
  TEST_ASSERT_FALSE(fsm.command(Cmd::TEST, Source::BUTTON, t).accepted);  // лише веб
  TEST_ASSERT_TRUE(fsm.command(Cmd::TEST, Source::WEB, t).accepted);
  TEST_ASSERT_TRUE(fsm.testMode());
  toReady();
  sensor.st = SensorStatus::FIRE;
  tick(cfg::READY_SETTLE_MS + 100);
  TEST_ASSERT_EQUAL(State::DISCHARGE, fsm.state());  // ланцюг відпрацював повністю
  TEST_ASSERT_FALSE(fsm.dischargeOutputActive());    // але вихід не вмикається
  fsm.command(Cmd::STOP, Source::BUTTON, t);
  TEST_ASSERT_FALSE(fsm.testMode());                 // повернення в SAFE скидає тест-режим
}

void test_real_discharge_activates_output() {
  toReady();
  sensor.st = SensorStatus::FIRE;
  tick(cfg::READY_SETTLE_MS + 100);
  TEST_ASSERT_EQUAL(State::DISCHARGE, fsm.state());
  TEST_ASSERT_TRUE(fsm.dischargeOutputActive());
}

void test_output_never_active_outside_discharge() {
  TEST_ASSERT_FALSE(fsm.dischargeOutputActive());               // SAFE
  fsm.command(Cmd::START, Source::BUTTON, t);
  TEST_ASSERT_FALSE(fsm.dischargeOutputActive());               // ARMING
  tick(cfg::ARM_DELAY + 20);
  TEST_ASSERT_FALSE(fsm.dischargeOutputActive());               // READY
  sensor.st = SensorStatus::FAULT;
  tick(20);
  TEST_ASSERT_FALSE(fsm.dischargeOutputActive());               // FAULT
}

void test_test_mode_only_from_safe_and_stop_exits() {
  fsm.command(Cmd::START, Source::BUTTON, t);
  TEST_ASSERT_FALSE(fsm.command(Cmd::TEST, Source::WEB, t).accepted);  // ARMING
  fsm.command(Cmd::STOP, Source::BUTTON, t);
  fsm.command(Cmd::TEST, Source::WEB, t);
  TEST_ASSERT_TRUE(fsm.command(Cmd::STOP, Source::BUTTON, t).accepted);  // Stop у SAFE виходить із тест-режиму
  TEST_ASSERT_FALSE(fsm.testMode());
}

void test_wdt_test_requires_test_mode_in_safe_via_web() {
  TEST_ASSERT_FALSE(fsm.command(Cmd::WDT_TEST, Source::WEB, t).accepted);   // без тест-режиму
  fsm.command(Cmd::TEST, Source::WEB, t);
  TEST_ASSERT_FALSE(fsm.command(Cmd::WDT_TEST, Source::BUTTON, t).accepted);
  TEST_ASSERT_FALSE(fsm.takeWdtTestRequest());
  TEST_ASSERT_TRUE(fsm.command(Cmd::WDT_TEST, Source::WEB, t).accepted);
  TEST_ASSERT_TRUE(fsm.takeWdtTestRequest());
  TEST_ASSERT_FALSE(fsm.takeWdtTestRequest());  // одноразовий запит
}

void test_ignored_fire_is_reported_but_never_discharges() {
  Rec rec;
  fsm.setEventSink(&rec);
  fsm.command(Cmd::START, Source::BUTTON, t);
  sensor.st = SensorStatus::FIRE;
  tick(cfg::ARM_DELAY + 20);
  TEST_ASSERT_EQUAL(1, rec.ignored);                 // оператору повідомлено рівно раз
  TEST_ASSERT_TRUE(fsm.snapshot(t).fireIgnored);     // індикація (LED/OLED/веб)
  tick(20000);
  TEST_ASSERT_EQUAL(State::READY, fsm.state());      // але спрацювання немає
  TEST_ASSERT_FALSE(fsm.dischargeOutputActive());
  TEST_ASSERT_EQUAL(1, rec.ignored);                 // без спаму в журнал
}

void test_no_warning_when_armed_with_clear_sensor() {
  Rec rec;
  fsm.setEventSink(&rec);
  fsm.command(Cmd::START, Source::BUTTON, t);
  tick(cfg::ARM_DELAY + 20);
  TEST_ASSERT_EQUAL(0, rec.ignored);
  TEST_ASSERT_FALSE(fsm.snapshot(t).fireIgnored);
}

void test_events_are_reported() {
  Rec rec;
  fsm.setEventSink(&rec);
  fsm.command(Cmd::START, Source::BUTTON, t);           // SAFE -> ARMING
  fsm.command(Cmd::START, Source::BUTTON, t);           // відхилено
  sensor.st = SensorStatus::FAULT;
  tick(20);                                              // ARMING -> FAULT
  TEST_ASSERT_EQUAL(2, rec.transitions);
  TEST_ASSERT_EQUAL(State::FAULT, rec.lastTo);
  TEST_ASSERT_TRUE(rec.rejects >= 1);
  fsm.command(Cmd::STOP, Source::BUTTON, t);
  fsm.command(Cmd::TEST, Source::WEB, t);
  TEST_ASSERT_EQUAL(1, rec.testEvents);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_boot_clean_is_safe);
  RUN_TEST(test_boot_critical_fault_blocks_start);
  RUN_TEST(test_abnormal_reset_needs_ack);
  RUN_TEST(test_arming_timer_then_ready);
  RUN_TEST(test_start_refused_on_bad_inputs);
  RUN_TEST(test_fire_never_discharges_when_not_ready);
  RUN_TEST(test_sensor_fire_discharges_instantly_after_settle);
  RUN_TEST(test_fire_present_through_settle_discharges_when_settled);
  RUN_TEST(test_fire_present_through_arming_is_ignored_until_it_clears);
  RUN_TEST(test_fire_started_in_last_moment_of_arming_is_ignored);
  RUN_TEST(test_fire_during_settle_ignored);
  RUN_TEST(test_sensor_fault_in_ready_goes_fault_not_discharge);
  RUN_TEST(test_power_fault_in_arming_goes_fault);
  RUN_TEST(test_manual_discharge_is_two_step_web_only);
  RUN_TEST(test_manual_discharge_confirm_expires);
  RUN_TEST(test_manual_discharge_rejected_outside_ready);
  RUN_TEST(test_manual_discharge_blocked_by_fault_between_steps);
  RUN_TEST(test_manual_discharge_blocked_during_settle);
  RUN_TEST(test_discharge_latched_until_stop);
  RUN_TEST(test_stop_from_every_state);
  RUN_TEST(test_illegal_transitions_rejected);
  RUN_TEST(test_state_corruption_goes_fault);
  RUN_TEST(test_double_button_start_ignored_when_not_safe);
  RUN_TEST(test_dual_agreeing_channels);
  RUN_TEST(test_dual_waits_for_both_channels);
  RUN_TEST(test_dual_channel_fault_is_fault);
  RUN_TEST(test_dual_short_skew_does_not_trigger_fire);
  RUN_TEST(test_dual_single_channel_fire_never_reports_fire);
  RUN_TEST(test_fsm_does_not_discharge_on_single_channel_fire);
  RUN_TEST(test_test_mode_suppresses_discharge_output);
  RUN_TEST(test_real_discharge_activates_output);
  RUN_TEST(test_output_never_active_outside_discharge);
  RUN_TEST(test_test_mode_only_from_safe_and_stop_exits);
  RUN_TEST(test_wdt_test_requires_test_mode_in_safe_via_web);
  RUN_TEST(test_ignored_fire_is_reported_but_never_discharges);
  RUN_TEST(test_no_warning_when_armed_with_clear_sensor);
  RUN_TEST(test_events_are_reported);
  return UNITY_END();
}
