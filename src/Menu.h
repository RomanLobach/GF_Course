// Menu pages, availability rules, popups and the delete-confirm dialog.
//
// Owned by and only ever called from the radio/protocol core (main loop()) - it talks to
// SessionState/FlashLog/WifiOffload/RadioManager directly and produces DisplaySnapshot
// values for the display task. Never touches Display/Encoder.
#pragma once

#include "Config.h"
#include "UiTypes.h"
#include "SessionState.h"
#include "FlashLog.h"
#include "WifiOffload.h"
#include "RadioManager.h"

class MenuController {
public:
  MenuController(SessionState &session, FlashLog &log, WifiOffload &wifi, RadioManager &radio);

  void handleEvent(const UiEvent &event);
  // Every main-loop iteration: popup timers, Wi-Fi progress, SessionState notices -> popups.
  void tick();

  DisplaySnapshot buildSnapshot() const;

  // Start-up self-test result: a failure stays on screen until a press, OK closes by itself.
  void showSelfTest(const char *text, bool failed) { showPopup(text, failed); }

private:
  SessionState &session_;
  FlashLog &log_;
  WifiOffload &wifi_;
  RadioManager &radio_;

  bool menuOpen_ = false;
  MenuItems::Page page_ = MenuItems::Page::Main;
  uint8_t menuIndex_ = 0;

  bool confirmDelete_ = false;
  uint8_t confirmIndex_ = 0;

  bool popupActive_ = false;
  char popupText_[96] = {};
  uint32_t popupUntilMs_ = 0;
  bool popupSticky_ = false; // closes only on a press

  bool wifiScreenActive_ = false;

  uint8_t itemCount() const;
  MenuItems::Suffix suffixFor(MenuItems::Page page, uint8_t index) const;
  const char *unavailableMessage(MenuItems::Page page, uint8_t index) const;
  void selectItem(uint8_t index);
  void selectMain(uint8_t index);
  void selectLogs(uint8_t index);
  void showPopup(const char *text, bool sticky = false);
  void closeMenu();
  static const char *noticeText(SessionState::Notice n);
};
