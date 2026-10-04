// Firmware update over Wi-Fi from the release manifest (ConfigStore `ota_url`, by default the
// rolling `hw6-latest` release on GitHub), plus the boot-time rollback check.
//
// Flow: Wi-Fi (WifiOffload, radio paused) -> GET manifest.json -> check project, this role's
// image and the version -> GET the image, streamed into the inactive app partition by
// Update.write() while SHA-256 and size are computed -> Update.end() only if both match,
// otherwise Update.abort() and the running image stays. Then a reboot into the new image.
//
// The network part (TLS handshake, HTTP) blocks for seconds, so it runs in its own FreeRTOS
// worker task; the protocol core only starts it and polls its progress - loop() never waits.
// Only from IDLE. Shared state between the two is a few volatile fields written by the
// worker and a cancel flag written by the protocol core.
//
// Rollback: verifyRollbackLater() keeps a freshly installed image "pending verify". confirmBoot()
// (setup(), after POST) marks it valid only when POST passed and the role matches the one stored
// in NVS; otherwise it is marked invalid and the bootloader returns to the previous image.
// An image flashed by cable is never pending, so a failing cable build doesn't loop.
#pragma once

#include <Arduino.h>
#include "Console.h"
#include "ConfigStore.h"
#include "RadioManager.h"
#include "SessionState.h"
#include "WifiOffload.h"

class Ota {
public:
  enum class Phase : uint8_t {
    Idle,
    WaitWifi,    // Wi-Fi connecting
    Manifest,    // worker: fetching / checking manifest.json
    Download,    // worker: writing the image
    Rebooting,   // installed, restart scheduled
    Done,        // finished without installing (check only / up to date / failed)
  };

  Ota(WifiOffload &wifi, RadioManager &radio, SessionState &session, ConfigStore &config);

  // setup(), right after POST. May not return (rollback reboot).
  static void confirmBoot(bool postOk);

  // install=false: only report what the manifest offers. force: install even if not newer.
  // Returns nullptr when started, otherwise the reason (English, for the console).
  const char *start(bool install, bool force);
  void cancel();
  void loopTask(); // every main-loop iteration

  bool active() const { return phase_ != Phase::Idle && phase_ != Phase::Done; }
  Phase phase() const { return phase_; }
  int8_t percent() const; // -1 when unknown
  // UI text (Ukrainian) for the OTA screen / result popup.
  void screenText(char *buf, size_t len) const;
  // Result of the last finished run; consumed by the menu to show a popup once.
  bool popResult(char *buf, size_t len);

  void registerCommands(Console &console);

private:
  WifiOffload &wifi_;
  RadioManager &radio_;
  SessionState &session_;
  ConfigStore &config_;

  Phase phase_ = Phase::Idle;
  bool install_ = false;
  bool force_ = false;
  bool startedWifi_ = false;
  bool pausedRadio_ = false;
  uint32_t phaseStartMs_ = 0;
  uint32_t rebootAtMs_ = 0;
  bool resultPending_ = false;

  void startWorker();
  void finish();
  static void cmdOta(void *self, int argc, char **argv, Console::Context &ctx);
  void printStatus(Print &out) const;
};
