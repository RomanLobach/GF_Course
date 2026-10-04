// Static configuration for the LoRa/FSK benchmark practicum firmware.
// Pins, radio profiles, timing and UI parameters - the single source of truth for constants.
#pragma once

#include <Arduino.h>

#if defined(DEVICE_ROLE_BASE) && defined(DEVICE_ROLE_ROVER)
#error "Define exactly one of DEVICE_ROLE_BASE / DEVICE_ROLE_ROVER (see platformio.ini envs)"
#endif
#if !defined(DEVICE_ROLE_BASE) && !defined(DEVICE_ROLE_ROVER)
#error "No device role defined - build with the 'base' or 'rover' PlatformIO environment"
#endif

namespace Config {

enum class Role : uint8_t { Base, Rover };
#if defined(DEVICE_ROLE_BASE)
constexpr auto DEVICE_ROLE = Role::Base;
#else
constexpr auto DEVICE_ROLE = Role::Rover;
#endif

// ---- LoRa module (SX1276) SPI pins ----
constexpr int PIN_LORA_SCK  = 5;
constexpr int PIN_LORA_MISO = 19;
constexpr int PIN_LORA_MOSI = 27;
constexpr int PIN_LORA_CS   = 18;
constexpr int PIN_LORA_RST  = 23;
constexpr int PIN_LORA_DIO0 = 26;

// ---- OLED (I2C SSD1306) ----
constexpr int PIN_OLED_SDA = 21;
constexpr int PIN_OLED_SCL = 22;
constexpr int OLED_WIDTH   = 128;
constexpr int OLED_HEIGHT  = 64;
constexpr uint8_t OLED_I2C_ADDR = 0x3C;
// Fast Mode (SSD1306-supported). Wire's default of 100kHz makes the full
// 1024-byte framebuffer transfer (U8g2 "_F_" full-buffer ctor) take ~90ms;
// at 400kHz it's ~25ms - still enough to warrant DISPLAY_TASK_PRIORITY below,
// but a meaningful reduction in how long each render blocks the I2C bus.
constexpr uint32_t OLED_I2C_CLOCK_HZ = 400000;

// Logs are stored on the ESP32's own internal flash (LittleFS, see FlashLog.h) -
// no external SPI flash chip is wired up, so there are no pins to declare here.
// GPIO2/4/15/25 (formerly the W25Q128JVSM SPI bus) are unused/free.

// ---- Encoder + button (input-only pins, external pull-ups on the board) ----
constexpr int PIN_ENCODER_A      = 36; // S_VP
constexpr int PIN_ENCODER_B      = 34;
constexpr int PIN_ENCODER_BUTTON = 39; // S_VN
// Button debounce + rejection of rotation crosstalk: a press is ignored
// if any A/B edge happened from LOCKOUT ms before it started until it was confirmed.
constexpr uint32_t BUTTON_DEBOUNCE_MS = 25;
constexpr uint32_t BUTTON_ROTATION_LOCKOUT_MS = 60;

// GPIO0 and GPIO12 are never touched anywhere in this firmware (critical strapping pins).

// ---- Sub-bands & duty cycle ----
// ETSI EN 300 220: the duty-cycle limit applies per sub-band, over a 1-hour observation
// window. Two sub-bands are used:
//   G3 = 869.40-869.65 MHz, 10%: service channel + the narrow profiles (1, 3, 5);
//   G1 = 868.00-868.60 MHz,  1%: the wide profiles (2, 4, 6) that don't fit in G3's 250 kHz.
enum class SubBand : uint8_t { G3 = 0, G1 = 1 };
constexpr uint8_t SUBBAND_COUNT = 2;
constexpr float SUBBAND_DUTY_LIMIT[SUBBAND_COUNT] = {0.10f, 0.01f};
constexpr uint32_t AIRTIME_WINDOW_MS = 3600000UL; // rolling 1-hour observation window
// Share of the G3 budget MEAS/TEST may never eat into - keeps room for heartbeats/control.
constexpr float SERVICE_BUDGET_RESERVE = 0.25f;

constexpr float RADIO_TX_POWER_DBM = 14.0f; // same power everywhere - 25 mW

// ---- Service channel ----
constexpr float SERVICE_FREQ_MHZ = 869.525f; // centre of G3
constexpr float SERVICE_BW_KHZ   = 125.0f;
constexpr uint8_t SERVICE_SF     = 10;
constexpr uint8_t SERVICE_CR     = 5; // 4/5

// ---- Heartbeat / search / loss ----
constexpr uint32_t HEARTBEAT_INTERVAL_MS     = 10000; // T_HB: ~0.207 s airtime -> ~21% of G3 budget
constexpr uint8_t HEARTBEAT_MISS_LIMIT       = 3;
constexpr uint32_t HEARTBEAT_MISS_TOLERANCE_MS = 1500;
constexpr uint32_t HEARTBEAT_BASE_ENTRY_DELAY_MS = 100; // Base's first HB after (re)entering SYNC
constexpr uint32_t SEARCH_FAST_PERIOD_MS     = 2000;
constexpr uint32_t SEARCH_JITTER_MS          = 700;   // random 0..N added to every search TX
constexpr uint32_t SEARCH_FAST_PHASE_MS      = 30000;
constexpr uint32_t SEARCH_TIMEOUT_MS         = 180000;
constexpr uint32_t ABORT_SUPPRESS_MS         = 2 * HEARTBEAT_INTERVAL_MS; // T_SUPPRESS

// ---- Transactions ----
constexpr uint32_t CONTROL_ACK_TIMEOUT_MS = 1500;
constexpr uint8_t CONTROL_MAX_RETRIES     = 3;
constexpr uint32_t START_DELAY_MS         = 5000; // D_START: commit window before a scheduled start
// Base keeps re-answering stray SLOT_REPORTs with its final decision this long - must outlast
// the longest burst window (MEAS profile 3: ~23 s) plus a slot.
constexpr uint32_t LINGER_MS              = 30000;
constexpr uint32_t PENDING_REPLY_EXPIRY_MS = 400; // a reactive reply that can't go out by then is stale

// ---- Burst + slot cycle (MEAS/TEST) ----
constexpr uint8_t BENCH_CONFIG_COUNT       = 6;
constexpr uint8_t MEAS_PACKETS_PER_BURST   = 8;
constexpr uint8_t TEST_PACKETS_PER_BURST   = 4;
constexpr uint8_t MAX_PACKETS_PER_BURST    = 8;
constexpr size_t BENCH_PACKET_SIZE         = 64;
constexpr uint32_t BURST_LEAD_MS           = 50;  // Base waits this long after the switch before TX #0
constexpr uint32_t BURST_GUARD_MS          = 20;  // gap between consecutive bench packets
constexpr uint32_t BURST_MARGIN_MS         = 200; // tail margin of every burst window
constexpr uint32_t SLOT_DURATION_MS        = 1500;
constexpr uint32_t SLOT_GUARD_MS           = 50;  // Rover waits this long before its SLOT_REPORT
constexpr uint32_t SLOT_REPORT_RETRY_MS    = 700; // Rover resends SLOT_REPORT once at this offset
constexpr uint8_t SLOT_MISS_LIMIT          = 2;
// TEST burst interruption: after every TEST packet the Base listens on the
// service channel for TEST_GAP_MS, so the Rover can cut the burst short with an early
// SLOT_REPORT; the Base cuts it with an end-of-burst marker instead of its next packet.
constexpr uint32_t TEST_GAP_MS              = 300;
constexpr uint32_t TEST_GAP_TX_OFFSET_MS    = 30;   // Rover sends this long after the predicted TX end
constexpr uint32_t TEST_GAP_REPLY_WAIT_MS   = 600;  // Rover waits this long for the reply, then resumes RX
constexpr uint32_t TEST_GAP_SWITCH_MARGIN_MS = 20;  // Base switches back to the profile this early
constexpr uint32_t TEST_FAST_NEXT_MS        = 1000; // next TEST burst starts this soon after a slot report

enum class Modulation : uint8_t { LoRaMod, FSK };

struct BenchConfig {
  Modulation modulation;
  SubBand band;
  float freqMhz;
  // LoRa: sf/bwKhz used. FSK: bitrate/fdevHz/rxBwKhz used.
  uint8_t sf;
  float bwKhz;
  uint32_t bitrate;
  uint32_t fdevHz;
  float rxBwKhz; // FSK receiver filter bandwidth - nearest SX127x-supported discrete value
};

// Index 0..5 == profile 1..6 in the on-air config_index field.
constexpr BenchConfig BENCH_CONFIGS[BENCH_CONFIG_COUNT] = {
  { .modulation = Modulation::LoRaMod, .band = SubBand::G3, .freqMhz = 869.525f, .sf = 6,  .bwKhz = 125.0f, .bitrate = 0,      .fdevHz = 0,      .rxBwKhz = 0.0f   }, // 1: SF6 / BW125
  { .modulation = Modulation::LoRaMod, .band = SubBand::G1, .freqMhz = 868.300f, .sf = 6,  .bwKhz = 500.0f, .bitrate = 0,      .fdevHz = 0,      .rxBwKhz = 0.0f   }, // 2: SF6 / BW500
  { .modulation = Modulation::LoRaMod, .band = SubBand::G3, .freqMhz = 869.525f, .sf = 12, .bwKhz = 125.0f, .bitrate = 0,      .fdevHz = 0,      .rxBwKhz = 0.0f   }, // 3: SF12 / BW125
  { .modulation = Modulation::LoRaMod, .band = SubBand::G1, .freqMhz = 868.300f, .sf = 12, .bwKhz = 500.0f, .bitrate = 0,      .fdevHz = 0,      .rxBwKhz = 0.0f   }, // 4: SF12 / BW500
  { .modulation = Modulation::FSK,     .band = SubBand::G3, .freqMhz = 869.525f, .sf = 0,  .bwKhz = 0.0f,   .bitrate = 15200,  .fdevHz = 15200,  .rxBwKhz = 50.0f  }, // 5: FSK 15200/15200
  { .modulation = Modulation::FSK,     .band = SubBand::G1, .freqMhz = 868.300f, .sf = 0,  .bwKhz = 0.0f,   .bitrate = 100000, .fdevHz = 100000, .rxBwKhz = 250.0f }, // 6: FSK 100000/100000
};

// ---- Logging ----
constexpr uint8_t LOG_TEST_FILL_LIMIT_PERCENT = 80;
constexpr uint32_t LOG_MEAS_RESERVE_RECORDS   = 120;

// ---- Wi-Fi offload ----
struct WifiCredential {
  const char *ssid;
  const char *password;
};
// Networks come from include/secrets.h (git-ignored, see secrets.example.h), so the real
// SSIDs/passwords never reach the repository. Tried in order.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#warning "include/secrets.h missing - copy include/secrets.example.h; Wi-Fi offload will not connect"
#define WIFI_SECRETS { { .ssid = "", .password = "" } }
#endif
constexpr WifiCredential WIFI_CREDENTIALS[] = WIFI_SECRETS;
constexpr size_t WIFI_CREDENTIAL_COUNT = sizeof(WIFI_CREDENTIALS) / sizeof(WIFI_CREDENTIALS[0]);
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 8000;
// Networks added from the Serial console (`wifi add`) live in NVS and are tried before the
// built-in ones - release images carry no secrets.h, so this is how they get Wi-Fi at all.
constexpr auto WIFI_NVS_NAMESPACE = "ttgo_wifi";
constexpr size_t WIFI_STORED_MAX = 4;
// Setup portal (see WifiPortal.h): WPA2 access point "<prefix><deviceId>" with a fresh random
// password per opening, shown on the OLED; closes by itself after this long without a request.
constexpr auto PORTAL_AP_PREFIX = "ttgo-lora-bench-";
constexpr size_t PORTAL_PASSWORD_LEN = 8;
constexpr uint32_t PORTAL_IDLE_TIMEOUT_MS = 10UL * 60 * 1000;
constexpr size_t PORTAL_SCAN_MAX = 12; // networks listed on the page, strongest first

// ---- Persistent config (NVS, see ConfigStore.h) ----
// Versioned blob in two slots (A/B) + CRC32; written only when a value changes.
constexpr auto CFG_NVS_NAMESPACE = "ttgo_cfg";
// Pre-config firmware kept only the session id here; read once to migrate it, then removed.
constexpr auto LEGACY_NVS_NAMESPACE = "lora_session";
constexpr auto LEGACY_NVS_KEY_SESSION_ID = "last_id";
constexpr size_t OTA_URL_MAX = 128;
constexpr auto OTA_DEFAULT_MANIFEST_URL =
  "https://github.com/RomanLobach/GF_Course/releases/download/hw6-latest/manifest.json";

// ---- Firmware update (see Ota.h) ----
// The download runs in its own task (TLS + HTTP block for seconds), on the protocol core at
// loop()'s priority so the two share the CPU by time slicing; the radio is paused meanwhile.
constexpr uint32_t OTA_TASK_STACK_BYTES = 12288;
constexpr UBaseType_t OTA_TASK_PRIORITY = 1;
constexpr BaseType_t OTA_TASK_CORE = 1;
constexpr size_t OTA_CHUNK_BYTES = 4096;        // one Update.write() per chunk
constexpr uint32_t OTA_HTTP_TIMEOUT_MS = 15000; // connect / no data
constexpr uint32_t OTA_REBOOT_DELAY_MS = 2000;  // lets the reply and the log get out first
// Role pinned at the first boot (an image of the other role is rolled back) + the last
// rollback already reported.
constexpr auto DEV_NVS_NAMESPACE = "ttgo_dev";

// ---- Power-on self-test (see Post.h) ----
constexpr int PIN_BATTERY_ADC = 35;          // battery through a 1:2 divider
constexpr uint32_t BATTERY_DIVIDER = 2;
// Below ABSENT there is no battery on the divider (USB only) - that's fine, not a failure.
constexpr uint32_t POST_BATT_ABSENT_MV = 1500;
constexpr uint32_t POST_BATT_MIN_MV = 3300;  // LiPo nearly empty - radio TX may brown out
constexpr uint32_t POST_OLED_PROBE_TRIES = 5;   // I2C ACK attempts before "oled" fails
constexpr uint32_t POST_OLED_PROBE_GAP_MS = 20;

// ---- UI tasks (core split) ----
// radio/protocol stays on the default loop() core (1); encoder/button polling
// and display rendering both live on core 0, but as two SEPARATE tasks:
// oled_.sendBuffer() blocks for ~25-90ms per frame (see OLED_I2C_CLOCK_HZ), and
// that must never stall encoder/button polling - a poll() call missed mid-turn
// loses the whole detent (the quadrature state machine needs every edge).
// Giving the input task the higher priority guarantees the scheduler preempts
// a blocked-on-I2C display task in favor of it on every tick.
constexpr BaseType_t UI_TASK_CORE = 0;
constexpr uint32_t UI_TASK_STACK_WORDS = 4096;
constexpr UBaseType_t UI_TASK_PRIORITY = 2;        // encoder/button polling task
constexpr uint32_t UI_POLL_INTERVAL_MS = 1;        // encoder polling period
constexpr uint32_t DISPLAY_TASK_STACK_WORDS = 4096;
constexpr UBaseType_t DISPLAY_TASK_PRIORITY = 1;   // strictly below UI_TASK_PRIORITY
constexpr size_t UI_EVENT_QUEUE_LEN = 16;
constexpr uint32_t UI_POPUP_DURATION_MS = 1500;

// ---- System log / console (protocol core only; never print from the input task) ----
// SysLog level at boot: 0 error, 1 warn, 2 info, 3 debug. -D APP_DEBUG_SERIAL starts at debug
// (state-machine transitions and slot decisions); `log level` changes it at runtime.
#if defined(APP_DEBUG_SERIAL)
constexpr uint8_t SYSLOG_DEFAULT_LEVEL = 3;
#else
constexpr uint8_t SYSLOG_DEFAULT_LEVEL = 2;
#endif
// UART TX ring buffer (set before Serial.begin()). Log echo and console replies only write
// what fits in it, so Serial output never blocks the protocol core.
constexpr size_t SERIAL_TX_BUFFER_SIZE = 4096;

} // namespace Config
