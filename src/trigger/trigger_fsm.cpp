#include "trigger_fsm.h"

#include "config.h"

#ifdef ARDUINO
#include <Arduino.h>
#define FSM_LOG(...) Serial.printf(__VA_ARGS__)
#else
#include <cstdio>
#define FSM_LOG(...) printf(__VA_ARGS__)
#endif

void TriggerFsm::begin(ISensor* sensor, IVoltageMonitor* vbus, uint8_t criticalMask, bool abnormalReset, uint32_t now) {
  sensor_ = sensor;
  vbus_ = vbus;
  critical_ = criticalMask;
  setState(State::INIT, now);
  if (critical_ != CF_NONE) transition(State::FAULT, now, "selftest failed");
  else if (abnormalReset) transition(State::FAULT, now, "abnormal reset, press Stop");
  else transition(State::SAFE, now, "boot");
}

State TriggerFsm::state() const {
  if (!integrityOk()) return State::FAULT;
  return (State)raw_;
}

bool TriggerFsm::integrityOk() const {
  return raw_ <= (uint8_t)State::FAULT && (uint8_t)(raw_ ^ inv_) == 0xFF;
}

void TriggerFsm::notify(EventType type, State from, State to, const char* text, uint32_t now) {
  if (sink_) sink_->event(type, from, to, text, now);
}

void TriggerFsm::setState(State s, uint32_t now) {
  if (s == State::SAFE) testMode_ = false;  // тест-режим живе лише до першого повернення в SAFE
  raw_ = (uint8_t)s;
  inv_ = (uint8_t)~raw_;
  stateSince_ = now;
  fireSince_ = 0;
  confirmPending_ = false;
  fireClearSeen_ = false;
}

void TriggerFsm::forceFault(uint32_t now, const char* reason) {
  const State from = state();
  FSM_LOG("[fsm] !!! %s -> FAULT (%s)\n", stateName(from), reason);
  setState(State::FAULT, now);
  notify(EventType::TRANSITION, from, State::FAULT, reason, now);
  fault_ = reason;
}

// Єдина таблиця дозволених переходів. EXPLOSION і FAULT виходять лише в SAFE.
bool TriggerFsm::allowed(State from, State to) {
  switch (from) {
    case State::INIT: return to == State::SAFE || to == State::FAULT;
    case State::SAFE: return to == State::ARMING || to == State::FAULT;
    case State::ARMING: return to == State::READY || to == State::SAFE || to == State::FAULT;
    case State::READY: return to == State::EXPLOSION || to == State::SAFE || to == State::FAULT;
    case State::EXPLOSION: return to == State::SAFE;
    case State::FAULT: return to == State::SAFE;
  }
  return false;
}

const char* TriggerFsm::runtimeFault() const {
  if (critical_ != CF_NONE) return "selftest failed";
  const SensorStatus st = sensor_->status();
  if (st != SensorStatus::NORMAL && st != SensorStatus::FIRE) return sensor_->detail();
  if (!vbus_->ok()) return "supply voltage out of range";
  return nullptr;
}

const char* TriggerFsm::startBlocker() const {
  if (const char* f = runtimeFault()) return f;
  if (sensor_->status() == SensorStatus::FIRE) return "sensor reports fire";
  return nullptr;
}

// Умови, які мають виконуватись В МОМЕНТ переходу, незалежно від того, хто його запросив.
const char* TriggerFsm::guard(State to, uint32_t now) const {
  switch (to) {
    case State::ARMING: return startBlocker();
    case State::READY:
      if (now - stateSince_ < cfg::ARM_DELAY) return "arm delay not elapsed";
      return runtimeFault();
    case State::EXPLOSION:
      if (now - stateSince_ < cfg::READY_SETTLE_MS) return "ready settling";
      return runtimeFault();
    default: return nullptr;
  }
}

bool TriggerFsm::transition(State to, uint32_t now, const char* reason) {
  const State from = state();
  if (!allowed(from, to)) {
    FSM_LOG("[fsm] ILLEGAL %s -> %s (%s)\n", stateName(from), stateName(to), reason);
    if (allowed(from, State::FAULT)) forceFault(now, "illegal transition");
    lastReject_ = "illegal transition";
    return false;
  }
  if (const char* g = guard(to, now)) {
    FSM_LOG("[fsm] blocked %s -> %s: %s\n", stateName(from), stateName(to), g);
    lastReject_ = g;
    return false;
  }
  FSM_LOG("[fsm] %s -> %s (%s)\n", stateName(from), stateName(to), reason);
  setState(to, now);
  fault_ = (to == State::FAULT) ? reason : "";
  if (to == State::READY) {
    // Приймаємо лише сигнал, що почався після постановки: FIRE у момент входу в READY — не новий
    fireClearSeen_ = sensor_->status() == SensorStatus::NORMAL;
    if (!fireClearSeen_) {  // страховка: оператор має знати, що сигнал до постановки НЕ спрацьовує
      FSM_LOG("[fsm] fire present at arming: ignored until it clears\n");
      notify(EventType::FIRE_IGNORED, from, to, "fire before arm", now);
    }
  }
  notify(EventType::TRANSITION, from, to, reason, now);
  return true;
}

void TriggerFsm::update(uint32_t now) {
  if (!integrityOk()) {
    forceFault(now, "state corrupted");
    return;
  }
  const State s = state();
  if (s != State::ARMING && s != State::READY) return;  // SAFE/FAULT/DISCHARGE тримаються до Stop

  if (const char* f = runtimeFault()) {  // некоректні дані -> безпечний стан без спрацювання
    transition(State::FAULT, now, f);
    return;
  }

  if (s == State::ARMING) {
    if (now - stateSince_ >= cfg::ARM_DELAY) transition(State::READY, now, "arm delay elapsed");
    return;
  }

  // READY: сигнал має ПОЧАТИСЬ після постановки, тому спершу потрібно побачити відсутність FIRE
  if (sensor_->status() == SensorStatus::NORMAL) fireClearSeen_ = true;

  // у перші READY_SETTLE_MS сигнал ігнорується; далі потрібен безперервний FIRE
  if (now - stateSince_ < cfg::READY_SETTLE_MS || sensor_->status() != SensorStatus::FIRE || !fireClearSeen_) {
    fireSince_ = 0;
    return;
  }
  if (fireSince_ == 0) fireSince_ = now;
  if ((int32_t)(now - fireSince_) >= (int32_t)cfg::FIRE_CONFIRM_MS) transition(State::EXPLOSION, now, "confirmed sensor fire");
}

CmdResult TriggerFsm::command(Cmd cmd, Source src, uint32_t now) {
  const CmdResult r = commandImpl(cmd, src, now);
  if (!r.accepted) notify(EventType::CMD_REJECT, state(), state(), r.message, now);
  return r;
}

CmdResult TriggerFsm::commandImpl(Cmd cmd, Source src, uint32_t now) {
  if (!integrityOk()) forceFault(now, "state corrupted");  // далі Stop ще зможе повернути в SAFE
  const State s = state();
  FSM_LOG("[fsm] cmd %d from %s in %s\n", (int)cmd, src == Source::WEB ? "web" : "button", stateName(s));

  switch (cmd) {
    case Cmd::START:
      if (s != State::SAFE) return {false, false, "Start allowed only in SAFE"};
      if (!transition(State::ARMING, now, "start command")) return {false, false, lastReject_};
      return {true, false, "arming started"};

    case Cmd::STOP:
      if (s == State::SAFE) {
        if (!testMode_) return {true, false, "already SAFE"};
        testMode_ = false;
        notify(EventType::TEST_MODE, s, s, "test mode off", now);
        return {true, false, "test mode off"};
      }
      if (!transition(State::SAFE, now, "stop command")) return {false, false, lastReject_};
      return {true, false, "SAFE"};

    case Cmd::EXPLOSION:
      if (src != Source::WEB) return {false, false, "explosion command only via web"};
      if (s != State::READY) return {false, false, "explosion allowed only in READY"};
      if (const char* f = runtimeFault()) return {false, false, f};
      if (!confirmPending_ || now - confirmStartedMs_ > cfg::EXPLOSION_CONFIRM_WINDOW_MS) {
        confirmPending_ = true;  // перший крок: чекаємо підтвердження
        confirmStartedMs_ = now;
        return {true, true, "confirm explosion within 5 s"};
      }
      if (!transition(State::EXPLOSION, now, "manual explosion command")) return {false, false, lastReject_};
      return {true, false, "explosion simulated"};

    case Cmd::TEST:
      if (src != Source::WEB) return {false, false, "test mode only via web"};
      if (s != State::SAFE) return {false, false, "test mode allowed only in SAFE"};
      if (testMode_) return {true, false, "already in test mode"};
      testMode_ = true;
      notify(EventType::TEST_MODE, s, s, "test mode on", now);
      return {true, false, "test mode on: outputs disabled"};

    case Cmd::WDT_TEST:  // навмисне зависання циклу для перевірки watchdog (лише тест-режим у SAFE)
      if (src != Source::WEB) return {false, false, "watchdog test only via web"};
      if (s != State::SAFE || !testMode_) return {false, false, "watchdog test needs test mode in SAFE"};
      wdtTestRequested_ = true;
      return {true, false, "watchdog test: loop will hang, board resets"};
  }
  return {false, false, "unknown command"};
}

Snapshot TriggerFsm::snapshot(uint32_t now) const {
  Snapshot s{};
  s.state = state();
  s.testMode = testMode_;
  s.fireIgnored = s.state == State::READY && !fireClearSeen_ && sensor_->status() == SensorStatus::FIRE;
  if (s.state == State::ARMING) {
    const uint32_t el = now - stateSince_;
    s.armRemainingMs = el >= cfg::ARM_DELAY ? 0 : cfg::ARM_DELAY - el;
  }
  s.fireConfirmedMs = (s.state == State::READY && fireSince_) ? now - fireSince_ : 0;
  s.sensor = sensor_->status();
  s.duty = sensor_->dutyPercent();
  s.vbusMv = vbus_->millivolts();
  s.vbusOk = vbus_->ok();
  s.fault = (s.state == State::FAULT) ? fault_ : "";
  return s;
}
