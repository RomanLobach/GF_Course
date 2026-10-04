// Orchestration only - see Config.h for pins/timing, and each module's own header for its
// responsibility. Three tasks:
//  - loop() (core 1): radio/protocol, menu logic, logging, Wi-Fi, Serial console. Never blocks.
//  - inputTaskFunc (core 0, higher priority): encoder/button polling only.
//  - displayTaskFunc (core 0, lower priority): OLED rendering only.
// They talk only through g_uiEventQueue (input -> loop) and g_displaySnapshotQueue
// (loop -> display).
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "Config.h"
#include "UiTypes.h"
#include "RadioManager.h"
#include "FlashLog.h"
#include "SessionState.h"
#include "WifiOffload.h"
#include "Menu.h"
#include "Encoder.h"
#include "Display.h"
#include "SysLog.h"
#include "Version.h"
#include "Console.h"
#include "SystemCommands.h"
#include "ConfigStore.h"
#include "Post.h"
#include "Ota.h"

namespace {
QueueHandle_t g_uiEventQueue;
QueueHandle_t g_displaySnapshotQueue;

RadioManager g_radio;
FlashLog g_flashLog;
ConfigStore g_config;
SessionState g_session(g_radio, g_flashLog, g_config);
Console g_console;
WifiOffload g_wifiOffload(g_flashLog, g_session, g_console);
Ota g_ota(g_wifiOffload, g_radio, g_session, g_config);
MenuController g_menu(g_session, g_flashLog, g_wifiOffload, g_radio, g_ota);

// Runs alone: only touches Encoder + g_uiEventQueue. Strictly higher priority than
// displayTaskFunc so a render's I2C transfer never delays a poll() call - a missed poll
// mid-turn loses the whole detent. No Serial here, ever (CLAUDE.md rule 12): Serial.printf
// takes the UART lock and can block this task while the protocol core is printing.
void inputTaskFunc(void *) {
  Encoder encoder;
  encoder.begin();

  for (;;) {
    const int rotation = encoder.poll();
    if (rotation != 0) {
      UiEvent ev;
      ev.type = UiEvent::Type::Rotate;
      ev.rotateDelta = static_cast<int8_t>(rotation);
      xQueueSend(g_uiEventQueue, &ev, 0);
    }
    if (encoder.pollButton()) {
      UiEvent ev;
      ev.type = UiEvent::Type::ShortPress;
      xQueueSend(g_uiEventQueue, &ev, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(Config::UI_POLL_INTERVAL_MS));
  }
}

// The only task that touches Display. Re-renders on every new snapshot, and at least every
// 100 ms so the blinking pending-dot keeps blinking.
void displayTaskFunc(void *) {
  Display display;
  display.begin();

  DisplaySnapshot lastSnap;
  for (;;) {
    DisplaySnapshot snap;
    if (xQueueReceive(g_displaySnapshotQueue, &snap, pdMS_TO_TICKS(100)) == pdTRUE) {
      lastSnap = snap;
    }
    display.render(lastSnap);
  }
}

// One POST record per boot in the benchmark log, so the CSV shows which runs followed a
// degraded start.
void logSelfTest() {
  const Post::Result &post = Post::result();
  LogRecord rec{};
  rec.sessionId = g_session.sessionId();
  rec.type = static_cast<uint8_t>(LogRecordType::Post);
  rec.eventKind = post.mask;
  rec.size = post.batteryMv;
  rec.rssi = FlashLog::NO_RSSI;
  rec.snrTenths = FlashLog::NO_SNR;
  rec.deviceStatus = static_cast<uint8_t>(Protocol::DeviceState::Idle);
  g_flashLog.logEvent(rec);
}
} // namespace

void setup() {
  Serial.setTxBufferSize(Config::SERIAL_TX_BUFFER_SIZE); // before begin(), see Config.h
  Serial.begin(115200);

  // First, so every later init step lands in the persistent log.
  const bool sysLogOk = SysLog::begin();
  SLOG_I("boot", "fw %s %s", Version::FIRMWARE, Version::roleName());
  SLOG_I("boot", "git %s%s, reset %s, %s", Version::GIT_HASH, Version::DIRTY ? "+dirty" : "",
         Version::resetReasonName(), Version::runningPartition());
  if (!sysLogOk) SLOG_E("boot", "syslog file unavailable, RAM only");

  const bool nvsOk = g_config.begin();
#if !defined(APP_DEBUG_SERIAL) // a debug build keeps DEBUG regardless of the stored level
  SysLog::setLevel(static_cast<LogLevel>(g_config.get().logLevel));
#endif

  g_uiEventQueue = xQueueCreate(Config::UI_EVENT_QUEUE_LEN, sizeof(UiEvent));
  g_displaySnapshotQueue = xQueueCreate(1, sizeof(DisplaySnapshot));

  const bool radioOk = g_radio.begin();
  if (!radioOk) SLOG_E("boot", "radio init failed");
  const bool flashLogOk = g_flashLog.begin();
  if (!flashLogOk) SLOG_E("boot", "flash log init failed");
  g_session.begin();

  // Before the UI tasks: POST probes the OLED on I2C while nobody else owns the bus.
  Post::run({radioOk, g_radio.chipVersion(), sysLogOk && flashLogOk, nvsOk, g_config.healthy()});
  logSelfTest();
  // A freshly updated image is accepted here or rolled back (reboots, never returns).
  Ota::confirmBoot(Post::result().mask == 0);
  char selfTest[sizeof(DisplaySnapshot::text)];
  Post::describe(selfTest, sizeof(selfTest));
  g_menu.showSelfTest(selfTest, Post::result().mask != 0);

  SystemCommands::registerAll(g_console, g_config);
  g_ota.registerCommands(g_console);

  xTaskCreatePinnedToCore(inputTaskFunc, "input", Config::UI_TASK_STACK_WORDS, nullptr,
                          Config::UI_TASK_PRIORITY, nullptr, Config::UI_TASK_CORE);
  xTaskCreatePinnedToCore(displayTaskFunc, "display", Config::DISPLAY_TASK_STACK_WORDS, nullptr,
                          Config::DISPLAY_TASK_PRIORITY, nullptr, Config::UI_TASK_CORE);
}

void loop() {
  UiEvent ev;
  while (xQueueReceive(g_uiEventQueue, &ev, 0) == pdTRUE) {
    g_menu.handleEvent(ev);
  }

  g_session.update();
  g_menu.tick();
  g_ota.loopTask();
  g_console.loopTask();
  // No flash I/O inside a burst window.
  const bool allowFlashIo = !g_session.isTimeCritical();
  g_flashLog.loopTask(allowFlashIo);
  SysLog::loopTask(allowFlashIo);

  const DisplaySnapshot snap = g_menu.buildSnapshot();
  xQueueOverwrite(g_displaySnapshotQueue, &snap);
}
