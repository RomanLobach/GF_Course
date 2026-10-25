#include "telemetry.h"
#include "config.h"
#include <math.h>

namespace {

constexpr float RAD2DEG = 57.2957795f;

bool fresh(uint32_t stamp, uint32_t now) {
    return stamp != 0 && (now - stamp) < DATA_STALE_MS;
}

bool isCopter(const Telemetry& t) {
    if (t.autopilot != 3) return false;  // MAV_AUTOPILOT_ARDUPILOTMEGA
    switch (t.mav_type) {
        case 2: case 3: case 4: case 13: case 14: case 15: case 29: return true;
        default: return false;
    }
}

}  // namespace

const char* linkStateName(LinkState s) {
    switch (s) {
        case LinkState::WaitingFc: return "WAITING";
        case LinkState::Connected: return "CONNECTED";
        case LinkState::Lost:      return "LOST";
    }
    return "?";
}

const char* flightModeName(const Telemetry& t) {
    if (t.hb_ms == 0) return "";
    if (!isCopter(t)) return "UNKNOWN";
    switch (t.custom_mode) {
        case 0:  return "STABILIZE";
        case 1:  return "ACRO";
        case 2:  return "ALT_HOLD";
        case 3:  return "AUTO";
        case 4:  return "GUIDED";
        case 5:  return "LOITER";
        case 6:  return "RTL";
        case 7:  return "CIRCLE";
        case 9:  return "LAND";
        case 11: return "DRIFT";
        case 13: return "SPORT";
        case 14: return "FLIP";
        case 15: return "AUTOTUNE";
        case 16: return "POSHOLD";
        case 17: return "BRAKE";
        case 18: return "THROW";
        case 19: return "AVOID_ADSB";
        case 20: return "GUIDED_NOGPS";
        case 21: return "SMART_RTL";
        case 22: return "FLOWHOLD";
        case 23: return "FOLLOW";
        case 24: return "ZIGZAG";
        case 25: return "SYSTEMID";
        case 26: return "AUTOROTATE";
        case 27: return "AUTO_RTL";
        default: return "UNKNOWN";
    }
}

const char* csvHeader() {
    return "ms,mode,armed,batt_v,batt_a,batt_pct,alt_rel_m,alt_msl_m,"
           "lat,lon,fix,sats,hdop,gs_ms,climb_ms,roll_deg,pitch_deg,yaw_deg,throttle_pct\n";
}

size_t telemetryToCsv(const Telemetry& t, uint32_t now, char* buf, size_t len) {
    size_t n = 0;
    auto add = [&](const char* fmt, auto... args) {
        if (n >= len) return;
        int w = snprintf(buf + n, len - n, fmt, args...);
        if (w > 0) n += (size_t)w;
    };

    add("%lu,", (unsigned long)now);

    // HEARTBEAT: режим і armed
    if (fresh(t.hb_ms, now)) add("%s,%d,", flightModeName(t), t.armed() ? 1 : 0);
    else add(",,");

    // SYS_STATUS: батарея
    if (fresh(t.sys_ms, now)) {
        if (t.batt_mv != 0xFFFF) add("%.2f,", t.batt_mv / 1000.0f); else add(",");
        if (t.batt_ca >= 0)      add("%.2f,", t.batt_ca / 100.0f);  else add(",");
        if (t.batt_pct >= 0)     add("%d,", t.batt_pct);            else add(",");
    } else add(",,,");

    // GLOBAL_POSITION_INT: висота, координати
    if (fresh(t.pos_ms, now)) {
        add("%.2f,%.2f,%.7f,%.7f,", t.alt_rel_mm / 1000.0f, t.alt_msl_mm / 1000.0f,
            t.lat / 1e7, t.lon / 1e7);
    } else add(",,,,");

    // GPS_RAW_INT
    if (fresh(t.gps_ms, now)) {
        add("%u,", t.fix_type);
        if (t.sats != 255)     add("%u,", t.sats);        else add(",");
        if (t.eph != 0xFFFF)   add("%.2f,", t.eph / 100.0f); else add(",");
    } else add(",,,");

    // VFR_HUD
    if (fresh(t.hud_ms, now)) add("%.2f,%.2f,", t.groundspeed, t.climb);
    else add(",,");

    // ATTITUDE
    if (fresh(t.att_ms, now)) add("%.1f,%.1f,%.1f,", t.roll * RAD2DEG, t.pitch * RAD2DEG, t.yaw * RAD2DEG);
    else add(",,,");

    // VFR_HUD: газ (останній стовпець без коми)
    if (fresh(t.hud_ms, now)) add("%u", t.throttle);
    add("\n");

    if (n >= len) n = len - 1;
    return n;
}

String telemetryToJson(const Telemetry& t, LinkState link, uint32_t now) {
    String j;
    j.reserve(400);
    j += "{\"link\":\"";
    j += linkStateName(link);
    j += "\"";

    char b[64];
    if (fresh(t.hb_ms, now)) {
        j += ",\"mode\":\""; j += flightModeName(t); j += "\"";
        j += ",\"armed\":"; j += t.armed() ? "true" : "false";
    }
    if (fresh(t.sys_ms, now)) {
        if (t.batt_mv != 0xFFFF) { snprintf(b, sizeof b, ",\"batt_v\":%.2f", t.batt_mv / 1000.0f); j += b; }
        if (t.batt_ca >= 0)      { snprintf(b, sizeof b, ",\"batt_a\":%.2f", t.batt_ca / 100.0f);  j += b; }
        if (t.batt_pct >= 0)     { snprintf(b, sizeof b, ",\"batt_pct\":%d", t.batt_pct);          j += b; }
    }
    if (fresh(t.pos_ms, now)) {
        snprintf(b, sizeof b, ",\"alt_rel\":%.2f", t.alt_rel_mm / 1000.0f); j += b;
        snprintf(b, sizeof b, ",\"alt_msl\":%.2f", t.alt_msl_mm / 1000.0f); j += b;
        snprintf(b, sizeof b, ",\"lat\":%.7f", t.lat / 1e7);                j += b;
        snprintf(b, sizeof b, ",\"lon\":%.7f", t.lon / 1e7);                j += b;
    }
    if (fresh(t.gps_ms, now)) {
        snprintf(b, sizeof b, ",\"fix\":%u", t.fix_type); j += b;
        if (t.sats != 255)   { snprintf(b, sizeof b, ",\"sats\":%u", t.sats);               j += b; }
        if (t.eph != 0xFFFF) { snprintf(b, sizeof b, ",\"hdop\":%.2f", t.eph / 100.0f);     j += b; }
    }
    if (fresh(t.hud_ms, now)) {
        snprintf(b, sizeof b, ",\"gs\":%.2f", t.groundspeed);    j += b;
        snprintf(b, sizeof b, ",\"climb\":%.2f", t.climb);       j += b;
        snprintf(b, sizeof b, ",\"throttle\":%u", t.throttle);   j += b;
    }
    if (fresh(t.att_ms, now)) {
        snprintf(b, sizeof b, ",\"roll\":%.1f", t.roll * RAD2DEG);   j += b;
        snprintf(b, sizeof b, ",\"pitch\":%.1f", t.pitch * RAD2DEG); j += b;
        snprintf(b, sizeof b, ",\"yaw\":%.1f", t.yaw * RAD2DEG);     j += b;
    }
    j += "}";
    return j;
}
