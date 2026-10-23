#pragma once
#include <stdint.h>

#include "power_iface.h"
#include "sensor_iface.h"

enum class State : uint8_t { INIT, SAFE, ARMING, READY, EXPLOSION, FAULT };

enum class Cmd : uint8_t { START, STOP, EXPLOSION, TEST, WDT_TEST };

enum class Source : uint8_t { BUTTON, WEB };

// Критичні помилки самотесту: у цьому разі Start заборонений до перезавантаження.
enum CriticalFault : uint8_t {
    CF_NONE = 0,
    CF_CHIP_ID = 1 << 0,
    CF_FILESYSTEM = 1 << 1,
    CF_SETTINGS = 1 << 2,
    CF_BUTTON_STUCK = 1 << 3,
};

struct CmdResult {
    bool accepted;
    bool needsConfirm; // ручний підрив: потрібен другий виклик у межах вікна підтвердження
    const char *message;
};

struct Snapshot {
    State state;
    bool testMode;
    bool fireIgnored; // READY: FIRE був уже до постановки, ігнорується, доки не зникне
    uint32_t armRemainingMs;
    uint32_t fireConfirmedMs;
    SensorStatus sensor;
    int duty;
    uint32_t vbusMv;
    bool vbusOk;
    const char *fault;
};

inline const char *stateName(State s) {
    switch (s) {
        case State::INIT: return "INIT";
        case State::SAFE: return "SAFE";
        case State::ARMING: return "ARMING";
        case State::READY: return "READY";
        case State::EXPLOSION: return "EXPLOSION";
        default: return "FAULT";
    }
}

// AUTH_FAIL не використовується (авторизацію прибрано), значення лишено, щоб не зсунути номери в старих журналах
enum class EventType : uint8_t { BOOT, TRANSITION, CMD_REJECT, AUTH_FAIL, TEST_MODE, FIRE_IGNORED };

// Приймач подій (журнал). Автомат лише повідомляє, не залежить від реалізації.
class IEventSink {
public:
    virtual ~IEventSink() = default;

    virtual void event(EventType type, State from, State to, const char *text, uint32_t now) = 0;
};

// Fail-safe автомат. Принципи:
//  * стан змінюється ТІЛЬКИ через transition(): таблиця дозволених переходів + arm на кожен перехід;
//  * EXPLOSION досяжний лише з READY, після READY_SETTLE_MS, без помилок датчика/живлення;
//  * змінна стану зберігається з інвертованою копією: розбіжність -> FAULT;
//  * спрацювання лише по сигналу, що ПОЧАВСЯ після READY (фронт): FIRE, який був до постановки, ігнорується;
//  * ручне гасіння (лише веб) потребує двох викликів у вікні EXPLOSION_CONFIRM_WINDOW_MS;
//  * будь-яка помилка -> FAULT; вихід із FAULT/EXPLOSION лише командою Stop;
//  * тест-режим (лише веб, лише з SAFE): весь ланцюг працює, але вихід спрацювання НЕ вмикається.
class TriggerFsm {
public:
    void begin(ISensor *sensor, IVoltageMonitor *vbus, uint8_t criticalMask, bool abnormalReset, uint32_t now);

    void update(uint32_t now);

    CmdResult command(Cmd cmd, Source src, uint32_t now);

    State state() const; // при пошкодженій змінній повертає FAULT
    void setEventSink(IEventSink *sink) { sink_ = sink; }
    bool testMode() const { return testMode_; }
    // Єдине джерело правди для виходу спрацювання (червоний LED): лише EXPLOSION і не в тест-режимі.
    bool explosionOutputActive() const { return state() == State::EXPLOSION && !testMode_; }

    bool takeWdtTestRequest() {
        const bool r = wdtTestRequested_;
        wdtTestRequested_ = false;
        return r;
    }

    Snapshot snapshot(uint32_t now) const;

#ifdef UNIT_TEST
    void corruptForTest(uint8_t raw) { raw_ = raw; }
    bool transitionForTest(State to, uint32_t now) { return transition(to, now, "test"); }
#endif

private:
    CmdResult commandImpl(Cmd cmd, Source src, uint32_t now);

    void notify(EventType type, State from, State to, const char *text, uint32_t now);

    static bool allowed(State from, State to);

    const char *guard(State to, uint32_t now) const; // nullptr, якщо перехід дозволений
    bool transition(State to, uint32_t now, const char *reason);

    void forceFault(uint32_t now, const char *reason); // в обхід таблиці: лише для пошкодження стану
    void setState(State s, uint32_t now);

    bool integrityOk() const;

    const char *startBlocker() const; // nullptr, якщо Start дозволений
    const char *runtimeFault() const; // nullptr, якщо все гаразд

    ISensor *sensor_ = nullptr;
    IVoltageMonitor *vbus_ = nullptr;
    uint8_t critical_ = CF_NONE;
    uint8_t raw_ = (uint8_t) State::INIT;
    uint8_t inv_ = (uint8_t) ~(uint8_t) State::INIT;
    uint32_t stateSince_ = 0;
    uint32_t fireSince_ = 0;
    bool fireClearSeen_ = false; // у READY бачили не-FIRE (NORMAL) -> наступний FIRE є новим сигналом
    IEventSink *sink_ = nullptr;
    bool testMode_ = false;
    bool wdtTestRequested_ = false;
    bool confirmPending_ = false;
    uint32_t confirmStartedMs_ = 0;
    const char *fault_ = "";
    const char *lastReject_ = "";
};
