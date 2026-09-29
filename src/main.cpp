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

namespace {
QueueHandle_t g_uiEventQueue;
QueueHandle_t g_displaySnapshotQueue;

RadioManager g_radio;
FlashLog g_flashLog;
SessionState g_session(g_radio, g_flashLog);
Console g_console;
WifiOffload g_wifiOffload(g_flashLog, g_session, g_console);
MenuController g_menu(g_session, g_flashLog, g_wifiOffload, g_radio);

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

  g_uiEventQueue = xQueueCreate(Config::UI_EVENT_QUEUE_LEN, sizeof(UiEvent));
  g_displaySnapshotQueue = xQueueCreate(1, sizeof(DisplaySnapshot));

  if (!g_radio.begin()) {
    SLOG_E("boot", "radio init failed");
  }
  if (!g_flashLog.begin()) {
    SLOG_E("boot", "flash log init failed");
  }
  g_session.begin();

  SystemCommands::registerAll(g_console);

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
  g_console.loopTask();
  // No flash I/O inside a burst window.
  const bool allowFlashIo = !g_session.isTimeCritical();
  g_flashLog.loopTask(allowFlashIo);
  SysLog::loopTask(allowFlashIo);

  const DisplaySnapshot snap = g_menu.buildSnapshot();
  xQueueOverwrite(g_displaySnapshotQueue, &snap);
}
