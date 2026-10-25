#pragma once
#include <Arduino.h>

constexpr uint8_t MAV_FLAG_ARMED = 0x80;  // MAV_MODE_FLAG_SAFETY_ARMED

// Останній відомий стан апарата. Кожна група має мітку часу (мс від старту ESP),
// 0 означає "ще не отримували".
struct Telemetry {
    // HEARTBEAT
    uint32_t hb_ms = 0;
    uint32_t custom_mode = 0;
    uint8_t  mav_type = 0;
    uint8_t  autopilot = 0;
    uint8_t  base_mode = 0;
    uint8_t  system_status = 0;

    // SYS_STATUS
    uint32_t sys_ms = 0;
    uint16_t batt_mv = 0xFFFF;   // 0xFFFF = невідомо
    int16_t  batt_ca = -1;       // сА, -1 = невідомо
    int8_t   batt_pct = -1;      // %, -1 = невідомо

    // GPS_RAW_INT
    uint32_t gps_ms = 0;
    uint8_t  fix_type = 0;       // 0/1 No fix, 2 2D, 3 3D ...
    uint8_t  sats = 255;         // 255 = невідомо
    uint16_t eph = 0xFFFF;       // HDOP*100, 0xFFFF = невідомо

    // GLOBAL_POSITION_INT
    uint32_t pos_ms = 0;
    int32_t  lat = 0, lon = 0;   // degE7
    int32_t  alt_msl_mm = 0;
    int32_t  alt_rel_mm = 0;

    // VFR_HUD
    uint32_t hud_ms = 0;
    float    groundspeed = 0;    // м/с
    float    climb = 0;          // м/с
    uint16_t throttle = 0;       // %

    // ATTITUDE
    uint32_t att_ms = 0;
    float    roll = 0, pitch = 0, yaw = 0;  // рад

    bool armed() const { return (base_mode & MAV_FLAG_ARMED) != 0; }
};

enum class LinkState : uint8_t { WaitingFc, Connected, Lost };

const char* linkStateName(LinkState s);
const char* flightModeName(const Telemetry& t);   // назва режиму ArduCopter

// CSV: заголовок і рядок (порожня клітинка = значення невідоме або застаріле)
const char* csvHeader();
size_t telemetryToCsv(const Telemetry& t, uint32_t now_ms, char* buf, size_t len);

// JSON для веб-інтерфейсу
String telemetryToJson(const Telemetry& t, LinkState link, uint32_t now_ms);
