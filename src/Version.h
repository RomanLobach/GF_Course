// Build identity + runtime facts about the running image, for the `version` command, the
// boot log line and GET /version.
//
// FW_VERSION / FW_GIT_HASH / FW_BUILD_DATE / FW_DIRTY come from scripts/version.py (git
// describe on hw6-v* tags) - never edit them by hand. The fallbacks below only exist so a
// build without the script (e.g. an IDE indexer) still compiles.
#pragma once

#include <Arduino.h>

#ifndef FW_VERSION
#define FW_VERSION "0.0.0-unknown"
#endif
#ifndef FW_GIT_HASH
#define FW_GIT_HASH "unknown"
#endif
#ifndef FW_BUILD_DATE
#define FW_BUILD_DATE "unknown"
#endif
#ifndef FW_DIRTY
#define FW_DIRTY 1
#endif

namespace Version {

constexpr const char *PROJECT = "ttgo-lora-bench";
constexpr const char *FIRMWARE = FW_VERSION;
constexpr const char *GIT_HASH = FW_GIT_HASH;
constexpr const char *BUILD_DATE = FW_BUILD_DATE;
constexpr bool DIRTY = FW_DIRTY != 0;

const char *roleName();          // "base" / "rover"
const char *runningPartition();  // "app0" / "app1"
const char *resetReasonName();   // why this boot happened
// Last 3 bytes of the factory MAC as 6 hex digits - stable per board, unique enough to tell
// devices apart in logs (and later in the SoftAP name).
const char *deviceId();

// Multi-line human-readable report (Serial `version`).
void print(Print &out);
// One JSON object (GET /version); returns the length written, truncated to fit.
size_t toJson(char *buf, size_t len);

} // namespace Version
