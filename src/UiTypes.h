// Cross-core message types passed between the radio/protocol core (loop()) and the two UI
// tasks (input, display). Plain fixed-size structs only - they are copied through FreeRTOS
// queues, never shared by reference (CLAUDE.md rules 2 and 9).
#pragma once

#include <Arduino.h>
#include "Protocol.h"

// Menu layout. Labels live on the display side (Display.cpp); the
// protocol core only sends item indices and suffix codes.
namespace MenuItems {
constexpr uint8_t MAX_ITEMS = 6;

enum class Page : uint8_t { Main, Service };

constexpr uint8_t MAIN_COUNT = 5;
constexpr uint8_t Sync = 0;
constexpr uint8_t Measure = 1;
constexpr uint8_t Test = 2;
constexpr uint8_t Service = 3;
constexpr uint8_t Exit = 4;

constexpr uint8_t SERVICE_COUNT = 6;
constexpr uint8_t ServiceWifi = 0;
constexpr uint8_t ServicePortal = 1;
constexpr uint8_t ServiceUpdate = 2;
constexpr uint8_t ServiceVersion = 3;
constexpr uint8_t ServiceErase = 4;
constexpr uint8_t ServiceBack = 5;

// What follows the label: "[почати]", "[X]", "›"...
enum class Suffix : uint8_t { None, Start, Finish, Stop, Unavailable, Submenu };
} // namespace MenuItems

enum class AppScreen : uint8_t {
  Main,
  Menu,
  ConfirmDelete,
  Popup,
  WifiScreen,
  OtaScreen,
  PortalScreen,
};

struct UiEvent {
  enum class Type : uint8_t { Rotate, ShortPress } type = Type::Rotate;
  int8_t rotateDelta = 0; // +1 / -1, only meaningful when type == Rotate
};

struct DisplaySnapshot {
  AppScreen screen = AppScreen::Main;

  // Status bar
  Protocol::DeviceState state = Protocol::DeviceState::Idle;
  bool wifi = false;    // shows "WIFI" instead of the protocol state
  bool pending = false; // blinking dot: some action is still in progress
  uint16_t sessionId = 0;
  uint32_t sessionElapsedMs = 0;

  // Centre block
  float rssi = 0;
  float snr = 0;
  bool isBase = false;
  uint8_t runProfile = 0;        // MEAS/TEST: current profile 1..6
  uint8_t runDesiredProfile = 0; // TEST: chosen but not yet applied (0 = none)
  uint8_t burstSize = 0;
  uint8_t burstTx = 0;
  uint8_t burstRx = 0;
  uint8_t lastRecv = 0xFF;       // 0xFF = no finished burst yet
  uint8_t lastSent = 0;          // packets sent in that burst (a TEST burst can be cut short)
  bool testPaused = false;
  bool logFull = false;
  bool runHasSignal = false;
  float runRssi = 0;
  float runSnr = 0;

  // Menu
  MenuItems::Page menuPage = MenuItems::Page::Main;
  uint8_t menuIndex = 0;
  uint8_t menuCount = 0;
  MenuItems::Suffix menuSuffix[MenuItems::MAX_ITEMS] = {};

  // Confirm-delete dialog
  uint8_t confirmIndex = 0; // 0 = "Ні", 1 = "Так"

  // Firmware update screen: download progress, -1 = no bar
  int8_t otaPercent = -1;

  // Popup / Wi-Fi / update / portal screen free text (UTF-8, lines split by '\n')
  char text[96] = {};
};
