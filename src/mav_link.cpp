#include "mav_link.h"
#include "config.h"
#include <common/mavlink.h>

namespace {

Telemetry g_tlm;
LinkState g_state = LinkState::WaitingFc;
SemaphoreHandle_t g_lock = nullptr;

// доступ лише з mav-задачі
uint8_t  g_fcSysId = 0;
uint32_t g_lastHbMs = 0;
uint32_t g_ownHbMs = 0;
uint32_t g_lastReqMs = 0;
uint8_t  g_reqCount = 0;

struct Lock {
    Lock()  { xSemaphoreTake(g_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(g_lock); }
};

void sendMessage(const mavlink_message_t& m) {
    uint8_t buf[MAVLINK_MAX_PACKET_LEN];
    uint16_t n = mavlink_msg_to_send_buffer(buf, &m);
    Serial1.write(buf, n);
}

void sendOwnHeartbeat() {
    mavlink_message_t m;
    mavlink_msg_heartbeat_pack(OWN_SYSID, MAV_COMP_ID_ONBOARD_COMPUTER, &m,
                               MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID,
                               0, 0, MAV_STATE_ACTIVE);
    sendMessage(m);
}

void requestMessage(uint32_t msgId, uint32_t periodMs) {
    mavlink_message_t m;
    mavlink_msg_command_long_pack(OWN_SYSID, MAV_COMP_ID_ONBOARD_COMPUTER, &m,
                                  g_fcSysId, MAV_COMP_ID_AUTOPILOT1,
                                  MAV_CMD_SET_MESSAGE_INTERVAL, 0,
                                  (float)msgId, (float)(periodMs * 1000UL), 0, 0, 0, 0, 0);
    sendMessage(m);
}

void requestStreams() {
    requestMessage(MAVLINK_MSG_ID_SYS_STATUS, 1000);
    requestMessage(MAVLINK_MSG_ID_GPS_RAW_INT, 500);
    requestMessage(MAVLINK_MSG_ID_GLOBAL_POSITION_INT, 500);
    requestMessage(MAVLINK_MSG_ID_VFR_HUD, 500);
    requestMessage(MAVLINK_MSG_ID_ATTITUDE, 200);
}

void handle(const mavlink_message_t& m) {
    const uint32_t now = millis();

    if (m.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
        // лише від автопілота: heartbeat від GCS чи інших компонентів ігноруємо
        if (m.compid != MAV_COMP_ID_AUTOPILOT1) return;
        mavlink_heartbeat_t hb;
        mavlink_msg_heartbeat_decode(&m, &hb);
        if (hb.autopilot == MAV_AUTOPILOT_INVALID || hb.type == MAV_TYPE_GCS) return;

        g_fcSysId = m.sysid;
        g_lastHbMs = now;
        Lock l;
        g_tlm.hb_ms = now;
        g_tlm.custom_mode = hb.custom_mode;
        g_tlm.mav_type = hb.type;
        g_tlm.autopilot = hb.autopilot;
        g_tlm.base_mode = hb.base_mode;
        g_tlm.system_status = hb.system_status;
        return;
    }

    if (g_fcSysId == 0 || m.sysid != g_fcSysId) return;

    switch (m.msgid) {
        case MAVLINK_MSG_ID_SYS_STATUS: {
            mavlink_sys_status_t s;
            mavlink_msg_sys_status_decode(&m, &s);
            Lock l;
            g_tlm.sys_ms = now;
            g_tlm.batt_mv = s.voltage_battery;
            g_tlm.batt_ca = s.current_battery;
            g_tlm.batt_pct = s.battery_remaining;
            break;
        }
        case MAVLINK_MSG_ID_GPS_RAW_INT: {
            mavlink_gps_raw_int_t g;
            mavlink_msg_gps_raw_int_decode(&m, &g);
            Lock l;
            g_tlm.gps_ms = now;
            g_tlm.fix_type = g.fix_type;
            g_tlm.sats = g.satellites_visible;
            g_tlm.eph = g.eph;
            break;
        }
        case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
            mavlink_global_position_int_t p;
            mavlink_msg_global_position_int_decode(&m, &p);
            Lock l;
            g_tlm.pos_ms = now;
            g_tlm.lat = p.lat;
            g_tlm.lon = p.lon;
            g_tlm.alt_msl_mm = p.alt;
            g_tlm.alt_rel_mm = p.relative_alt;
            break;
        }
        case MAVLINK_MSG_ID_VFR_HUD: {
            mavlink_vfr_hud_t h;
            mavlink_msg_vfr_hud_decode(&m, &h);
            Lock l;
            g_tlm.hud_ms = now;
            g_tlm.groundspeed = h.groundspeed;
            g_tlm.climb = h.climb;
            g_tlm.throttle = h.throttle;
            break;
        }
        case MAVLINK_MSG_ID_ATTITUDE: {
            mavlink_attitude_t a;
            mavlink_msg_attitude_decode(&m, &a);
            Lock l;
            g_tlm.att_ms = now;
            g_tlm.roll = a.roll;
            g_tlm.pitch = a.pitch;
            g_tlm.yaw = a.yaw;
            break;
        }
        default:
            break;
    }
}

void setState(LinkState s) {
    Lock l;
    g_state = s;
}

void tick() {
    const uint32_t now = millis();

    // власний heartbeat: без нього ArduPilot може не слати потоки на цей порт
    if (now - g_ownHbMs >= OWN_HEARTBEAT_MS) {
        g_ownHbMs = now;
        sendOwnHeartbeat();
    }

    const bool alive = g_lastHbMs != 0 && (now - g_lastHbMs) < HEARTBEAT_TIMEOUT_MS;
    const LinkState s = mav_link::state();

    if (alive && s != LinkState::Connected) {
        setState(LinkState::Connected);
        g_reqCount = 0;  // після (пере)підключення знову просимо потоки
        Serial.printf("[mav] FC sysid=%u: CONNECTED\n", g_fcSysId);
    } else if (!alive && s == LinkState::Connected) {
        setState(LinkState::Lost);
        Serial.println("[mav] heartbeat lost: LOST");
    }

    if (alive) {
        const uint32_t period = g_reqCount < REQUEST_FAST_COUNT ? REQUEST_FAST_MS : REQUEST_SLOW_MS;
        if (g_reqCount == 0 || now - g_lastReqMs >= period) {
            g_lastReqMs = now;
            requestStreams();
            if (g_reqCount < 255) g_reqCount++;
        }
    }
}

void mavTask(void*) {
    mavlink_message_t msg;
    mavlink_status_t status;
    for (;;) {
        int avail = Serial1.available();
        while (avail-- > 0) {
            uint8_t c = (uint8_t)Serial1.read();
            if (mavlink_parse_char(MAVLINK_COMM_0, c, &msg, &status)) handle(msg);
        }
        tick();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

}  // namespace

namespace mav_link {

void begin() {
    g_lock = xSemaphoreCreateMutex();
    Serial1.setRxBufferSize(2048);
    Serial1.begin(MAV_BAUD, SERIAL_8N1, MAV_RX_PIN, MAV_TX_PIN);
    xTaskCreate(mavTask, "mav", 6144, nullptr, 3, nullptr);
}

Telemetry snapshot() {
    Lock l;
    return g_tlm;
}

LinkState state() {
    Lock l;
    return g_state;
}

}  // namespace mav_link
