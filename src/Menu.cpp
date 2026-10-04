#include "Menu.h"
#include "SysLog.h"
#include <cstring>

using MenuItems::Page;
using MenuItems::Suffix;
using Protocol::DeviceState;

namespace {
bool isRun(const DeviceState s) { return s == DeviceState::Measuring || s == DeviceState::Testing; }
} // namespace

MenuController::MenuController(SessionState &session, FlashLog &log, WifiOffload &wifi, RadioManager &radio, Ota &ota)
  : session_(session), log_(log), wifi_(wifi), radio_(radio), ota_(ota) {}

uint8_t MenuController::itemCount() const {
  return page_ == Page::Main ? MenuItems::MAIN_COUNT : MenuItems::SERVICE_COUNT;
}

Suffix MenuController::suffixFor(const Page page, const uint8_t index) const {
  const DeviceState st = session_.state();
  if (page == Page::Main) {
    switch (index) {
      case MenuItems::Sync:
        return st == DeviceState::Idle ? Suffix::Start : Suffix::Finish;
      case MenuItems::Measure:
        if (st == DeviceState::Measuring) return Suffix::Stop;
        return st == DeviceState::Synced && !session_.isStartPending() ? Suffix::Start : Suffix::Unavailable;
      case MenuItems::Test:
        if (st == DeviceState::Testing) return Suffix::Finish;
        return st == DeviceState::Synced && !session_.isStartPending() ? Suffix::Start : Suffix::Unavailable;
      case MenuItems::Service:
        return Suffix::Submenu;
      default:
        return Suffix::None;
    }
  }
  switch (index) {
    case MenuItems::ServiceWifi:
    case MenuItems::ServiceUpdate:
      return st == DeviceState::Idle ? Suffix::None : Suffix::Unavailable;
    case MenuItems::ServiceErase:
      return isRun(st) ? Suffix::Unavailable : Suffix::None;
    default:
      return Suffix::None;
  }
}

const char *MenuController::unavailableMessage(const Page page, const uint8_t index) const {
  const DeviceState st = session_.state();
  if (page == Page::Main) {
    if (session_.isStartPending()) return "Зачекайте, виконується запит";
    if (index == MenuItems::Measure && st == DeviceState::Testing) return "Спершу завершіть тест частот";
    if (index == MenuItems::Test && st == DeviceState::Measuring) return "Спершу зупиніть вимірювання";
    return "Не можу почати: немає синхронізації";
  }
  if (index == MenuItems::ServiceWifi || index == MenuItems::ServiceUpdate) return "Спершу завершіть синхронізацію";
  return "Недоступно під час вимірювання";
}

void MenuController::showPopup(const char *text, const bool sticky) {
  popupActive_ = true;
  popupSticky_ = sticky;
  std::strncpy(popupText_, text, sizeof(popupText_) - 1);
  popupText_[sizeof(popupText_) - 1] = 0;
  popupUntilMs_ = millis() + Config::UI_POPUP_DURATION_MS;
}

void MenuController::closeMenu() {
  menuOpen_ = false;
  page_ = Page::Main;
  menuIndex_ = 0;
}

void MenuController::selectItem(const uint8_t index) {
  if (suffixFor(page_, index) == Suffix::Unavailable) {
    showPopup(unavailableMessage(page_, index));
    closeMenu();
    return;
  }
  if (page_ == Page::Main) {
    selectMain(index);
  } else {
    selectService(index);
  }
}

void MenuController::selectMain(const uint8_t index) {
  switch (index) {
    case MenuItems::Sync:
      if (session_.state() == DeviceState::Idle) {
        if (session_.requestStartSearching()) showPopup("Пошук партнера…");
      } else {
        switch (session_.requestAbort()) {
          case SessionState::AbortResult::Stopped:
            showPopup("Пошук зупинено");
            break;
          case SessionState::AbortResult::Pending:
            showPopup(isRun(session_.state()) ? "Завершення після поточної пачки…" : "Завершення синхронізації…");
            break;
          case SessionState::AbortResult::NotApplicable:
            break;
        }
      }
      break;

    case MenuItems::Measure:
    case MenuItems::Test: {
      if (isRun(session_.state())) {
        session_.requestStopRun();
        showPopup(index == MenuItems::Measure ? "Зупинка після поточного профілю…" : "Завершення після поточної пачки…");
        break;
      }
      const SessionState::StartResult r = index == MenuItems::Measure ? session_.requestStartMeasurement()
                                                                      : session_.requestStartTest();
      switch (r) {
        case SessionState::StartResult::Sent:
          showPopup("Запит надіслано…");
          break;
        case SessionState::StartResult::Budget: {
          char buf[64];
          snprintf(buf, sizeof(buf), "Ліміт ефіру: зачекайте %u хв",
                   static_cast<unsigned>(session_.lastBudgetWaitMinutes()));
          showPopup(buf);
          break;
        }
        case SessionState::StartResult::LogFull:
          showPopup("Пам'ять логів заповнена");
          break;
        case SessionState::StartResult::Busy:
          showPopup("Зачекайте, виконується запит");
          break;
        case SessionState::StartResult::NotSynced:
          showPopup("Не можу почати: немає синхронізації");
          break;
      }
      break;
    }

    case MenuItems::Service:
      page_ = Page::Service;
      menuIndex_ = 0;
      return; // stay in the menu

    case MenuItems::Exit:
    default:
      break;
  }
  closeMenu();
}

void MenuController::selectService(const uint8_t index) {
  switch (index) {
    case MenuItems::ServiceWifi:
      radio_.pause();
      wifi_.start();
      wifiScreenActive_ = true;
      break;
    case MenuItems::ServiceUpdate:
      if (const char *err = ota_.start(true, false)) {
        SLOG_W("ota", "%s", err);
        showPopup("Оновлення зараз недоступне");
      }
      break;
    case MenuItems::ServiceErase:
      confirmDelete_ = true;
      confirmIndex_ = 0; // "Ні" pre-selected
      return;
    case MenuItems::ServiceBack:
    default:
      page_ = Page::Main;
      menuIndex_ = MenuItems::Service;
      return;
  }
  closeMenu();
}

void MenuController::handleEvent(const UiEvent &event) {
  const bool press = event.type == UiEvent::Type::ShortPress;

  // The update screen covers everything; a press cancels it (until the image is in).
  if (ota_.active()) {
    if (press) ota_.cancel();
    return;
  }

  if (wifiScreenActive_) {
    if (press) {
      wifi_.stop();
      radio_.resume();
      wifiScreenActive_ = false;
    }
    return;
  }

  // A press closes the popup early and does nothing else; rotation is
  // ignored while it's up. Events never reach whatever is hidden underneath.
  if (popupActive_) {
    if (press) popupActive_ = false;
    return;
  }

  if (confirmDelete_) {
    if (!press) {
      confirmIndex_ = static_cast<uint8_t>((confirmIndex_ + event.rotateDelta + 2) % 2);
    } else {
      confirmDelete_ = false;
      closeMenu();
      if (confirmIndex_ == 1) {
        log_.eraseAll();
        showPopup("Логи стерто");
      } else {
        showPopup("Скасовано");
      }
    }
    return;
  }

  if (!menuOpen_) {
    if (press) {
      menuOpen_ = true;
      page_ = Page::Main;
      menuIndex_ = 0;
    } else if (session_.state() == DeviceState::Testing) {
      // On the main screen the encoder picks the TEST profile.
      session_.requestTestProfileDelta(event.rotateDelta);
    }
    return;
  }

  if (!press) {
    const int n = itemCount();
    menuIndex_ = static_cast<uint8_t>(((menuIndex_ + event.rotateDelta) % n + n) % n);
  } else {
    selectItem(menuIndex_);
  }
}

const char *MenuController::noticeText(const SessionState::Notice n) {
  using N = SessionState::Notice;
  switch (n) {
    case N::Synced: return "Синхронізовано";
    case N::SearchTimeout: return "Партнера не знайдено";
    case N::SyncLost: return "Зв'язок втрачено";
    case N::AbortDone: return "Синхронізацію завершено";
    case N::AbortNoAck: return "Синхронізацію завершено (партнер не відповів)";
    case N::AbortByPeer: return "Партнер завершив синхронізацію";
    case N::NoResponse: return "Партнер не відповідає";
    case N::PeerBusy: return "Партнер зайнятий";
    case N::PeerBudget: return "Партнер: ліміт ефіру";
    case N::PeerLogFull: return "Партнер: пам'ять логів заповнена";
    case N::MeasStarted: return "Вимірювання розпочато";
    case N::MeasDone: return "Вимірювання завершено";
    case N::MeasStopped: return "Вимірювання зупинено";
    case N::MeasLinkLost: return "Вимірювання перервано: немає зв'язку";
    case N::TestStarted: return "Тест частот розпочато";
    case N::TestStopped: return "Тест частот завершено";
    case N::TestLinkLost: return "Тест перервано: немає зв'язку";
  }
  return "";
}

void MenuController::tick() {
  wifi_.loopTask();

  if (popupActive_ && !popupSticky_ && static_cast<int32_t>(millis() - popupUntilMs_) >= 0) {
    popupActive_ = false;
  }

  if (wifiScreenActive_ && wifi_.phase() == WifiOffload::Phase::Failed) {
    wifi_.stop();
    radio_.resume();
    wifiScreenActive_ = false;
    showPopup("Wi-Fi недоступний");
  }

  char otaResult[sizeof(popupText_)];
  if (ota_.popResult(otaResult, sizeof(otaResult))) showPopup(otaResult, true);

  SessionState::Notice n;
  while (session_.popNotice(n)) {
    showPopup(noticeText(n)); // the newest result wins the popup
  }
}

DisplaySnapshot MenuController::buildSnapshot() const {
  DisplaySnapshot snap;
  snap.state = session_.state();
  snap.wifi = wifiScreenActive_;
  snap.pending = session_.isBusy();
  snap.sessionId = session_.sessionId();
  snap.sessionElapsedMs = session_.sessionElapsedMs();
  snap.rssi = session_.rssi();
  snap.snr = session_.snr();
  snap.isBase = Config::DEVICE_ROLE == Config::Role::Base;

  const SessionState::RunView rv = session_.runView();
  snap.runProfile = rv.profile;
  snap.runDesiredProfile = rv.desiredProfile;
  snap.burstSize = rv.burstSize;
  snap.burstTx = rv.burstTx;
  snap.burstRx = rv.burstRx;
  snap.lastRecv = rv.lastRecv;
  snap.lastSent = rv.lastSent;
  snap.testPaused = rv.paused;
  snap.runHasSignal = rv.hasSignal;
  snap.runRssi = rv.rssi;
  snap.runSnr = rv.snr;
  snap.logFull = snap.state == DeviceState::Testing && !log_.canLogTest();

  if (ota_.active()) {
    snap.screen = AppScreen::OtaScreen;
    snap.wifi = true;
    snap.otaPercent = ota_.percent();
    ota_.screenText(snap.text, sizeof(snap.text));
    return snap;
  }
  if (wifiScreenActive_) {
    snap.screen = AppScreen::WifiScreen;
    wifi_.statusText().toCharArray(snap.text, sizeof(snap.text));
    return snap;
  }
  if (popupActive_) {
    snap.screen = AppScreen::Popup;
    std::strncpy(snap.text, popupText_, sizeof(snap.text) - 1);
    return snap;
  }
  if (confirmDelete_) {
    snap.screen = AppScreen::ConfirmDelete;
    snap.confirmIndex = confirmIndex_;
    return snap;
  }
  if (menuOpen_) {
    snap.screen = AppScreen::Menu;
    snap.menuPage = page_;
    snap.menuIndex = menuIndex_;
    snap.menuCount = itemCount();
    for (uint8_t i = 0; i < snap.menuCount; i++) snap.menuSuffix[i] = suffixFor(page_, i);
    return snap;
  }
  snap.screen = AppScreen::Main;
  return snap;
}
