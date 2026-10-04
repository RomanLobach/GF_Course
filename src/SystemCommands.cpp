#include "SystemCommands.h"

#include "Config.h"
#include "Post.h"
#include "SysLog.h"
#include "Version.h"
#include "WifiStore.h"

namespace SystemCommands {
namespace {

constexpr uint32_t DEFAULT_LOG_LINES = 30;
constexpr size_t LINE_LEN = 96;

// Paced Serial dump state (one dump at a time; a new `log` replaces a running one).
uint32_t g_dumpNext = 0;
uint32_t g_dumpEnd = 0;

bool printRecord(const uint32_t seq, Print &out) {
  SysLog::Record rec{};
  if (!SysLog::read(seq, rec)) return false;
  char line[LINE_LEN];
  SysLog::format(rec, line, sizeof(line));
  out.println(line);
  return true;
}

bool dumpJob(void *, Print &out) {
  while (g_dumpNext < g_dumpEnd) {
    if (out.availableForWrite() < static_cast<int>(LINE_LEN + 2)) return true; // resume next loop
    printRecord(g_dumpNext++, out);
  }
  out.println(F("-- end of log --"));
  return false;
}

void cmdVersion(void *, int, char **, Console::Context &ctx) {
  Version::print(ctx.out);
}

void cmdLog(void *self, const int argc, char **argv, Console::Context &ctx) {
  auto *console = static_cast<Console *>(self);
  Print &out = ctx.out;

  if (argc >= 2 && strcasecmp(argv[1], "level") == 0) {
    if (argc >= 3) {
      LogLevel lvl;
      if (!SysLog::parseLevel(argv[2], lvl)) {
        out.println(F("error: level must be error|warn|info|debug"));
        return;
      }
      SysLog::setLevel(lvl);
      SLOG_I("log", "level set to %s", SysLog::levelName(lvl));
    }
    out.printf("log level: %s\n", SysLog::levelName(SysLog::level()));
    return;
  }
  if (argc >= 2 && strcasecmp(argv[1], "clear") == 0) {
    out.println(SysLog::clear() ? F("log cleared") : F("error: log clear failed"));
    return;
  }
  if (argc >= 2 && strcasecmp(argv[1], "stats") == 0) {
    out.printf("boot %u, records %lu..%lu, level %s, dropped echo %lu, dropped flash %lu\n", SysLog::bootId(),
               static_cast<unsigned long>(SysLog::firstSeq()), static_cast<unsigned long>(SysLog::nextSeq() - 1),
               SysLog::levelName(SysLog::level()), static_cast<unsigned long>(SysLog::droppedEcho()),
               static_cast<unsigned long>(SysLog::droppedFlash()));
    return;
  }

  uint32_t lines = DEFAULT_LOG_LINES;
  if (argc >= 2) {
    if (strcasecmp(argv[1], "all") == 0) {
      lines = UINT32_MAX;
    } else {
      const long n = strtol(argv[1], nullptr, 10);
      if (n <= 0) {
        out.println(F("usage: log [n|all] | log level [error|warn|info|debug] | log clear | log stats"));
        return;
      }
      lines = static_cast<uint32_t>(n);
    }
  }
  const uint32_t first = SysLog::firstSeq();
  const uint32_t end = SysLog::nextSeq();
  const uint32_t start = end - first > lines ? end - lines : first;

  if (ctx.serial) {
    g_dumpNext = start;
    g_dumpEnd = end;
    console->startJob(dumpJob, nullptr);
  } else {
    for (uint32_t seq = start; seq < end; seq++) printRecord(seq, out);
  }
}

void cmdReboot(void *self, int, char **, Console::Context &ctx) {
  SLOG_W("sys", "reboot requested from console");
  SysLog::loopTask(true); // get the line onto flash before the reset
  ctx.out.println(F("rebooting..."));
  static_cast<Console *>(self)->scheduleRestart(300);
}

constexpr auto CONFIG_USAGE =
    "config [show] | config get <key> | config set <key> <value> | config reset | config stress [n] | config tear-test";

// `config stress`: back-to-back saves for the power-cut experiment. One save per job call,
// at most one every STRESS_PERIOD_MS; any byte typed on Serial stops it.
constexpr uint32_t STRESS_DEFAULT = 500;
constexpr uint32_t STRESS_MAX = 5000;
constexpr uint32_t STRESS_PERIOD_MS = 20;
Console *g_console = nullptr; // for starting the stress job from cmdConfig
ConfigStore *g_stressConfig = nullptr;
uint32_t g_stressNext = 0;
uint32_t g_stressEnd = 0;
uint32_t g_stressLastMs = 0;

bool stressJob(void *, Print &out) {
  if (Serial.available() > 0) {
    out.printf("stress stopped at %lu\n", static_cast<unsigned long>(g_stressNext));
    return false;
  }
  if (g_stressNext >= g_stressEnd) {
    out.println(F("stress done (config reset restores ota_url)"));
    return false;
  }
  if (millis() - g_stressLastMs < STRESS_PERIOD_MS || out.availableForWrite() < 48) return true;
  g_stressLastMs = millis();
  const uint32_t n = ++g_stressNext;
  // "begin" goes out before the write and "ok" after it: a cut between them is a cut mid-save.
  out.printf("write %lu -> ", static_cast<unsigned long>(n));
  if (!g_stressConfig->stressWrite(n)) {
    out.println(F("FAILED"));
    return false;
  }
  out.printf("ok slot %c gen %lu\n", g_stressConfig->activeSlotName(),
             static_cast<unsigned long>(g_stressConfig->generation()));
  return true;
}

void cmdConfig(void *self, const int argc, char **argv, Console::Context &ctx) {
  auto *config = static_cast<ConfigStore *>(self);
  Print &out = ctx.out;
  const char *sub = argc >= 2 ? argv[1] : "show";

  if (strcasecmp(sub, "show") == 0) {
    config->printStatus(out);
    config->print(out, nullptr);
    return;
  }
  if (strcasecmp(sub, "get") == 0 && argc >= 3) {
    if (!config->print(out, argv[2])) out.printf("error: unknown key '%s'\n", argv[2]);
    return;
  }
  if (strcasecmp(sub, "set") == 0 && argc >= 4) {
    switch (config->set(argv[2], argv[3])) {
      case ConfigStore::SetResult::Ok:
        SysLog::setLevel(static_cast<LogLevel>(config->get().logLevel));
        config->print(out, argv[2]);
        return;
      case ConfigStore::SetResult::UnknownKey: out.printf("error: unknown key '%s'\n", argv[2]); return;
      case ConfigStore::SetResult::ReadOnly: out.printf("error: '%s' is read-only\n", argv[2]); return;
      case ConfigStore::SetResult::Invalid:
        out.println(F("error: value out of range:"));
        config->print(out, argv[2]);
        return;
      case ConfigStore::SetResult::SaveFailed: out.println(F("error: saving to NVS failed, value unchanged")); return;
    }
    return;
  }
  if (strcasecmp(sub, "reset") == 0) {
    const bool ok = config->resetToDefaults();
    SysLog::setLevel(static_cast<LogLevel>(config->get().logLevel));
    out.println(ok ? F("config reset to defaults (session id kept)") : F("error: saving to NVS failed"));
    return;
  }
  if (argc >= 2 && strcmp(argv[1], "tear-test") == 0) {
    // Simulated power cut mid-save: a torn blob in the inactive slot, then a restart. The
    // board must come back with the old values and `load: recovered`.
    if (!ctx.serial) {
      out.println(F("error: config tear-test only from the Serial console"));
      return;
    }
    if (!config->tearTest()) {
      out.println(F("error: NVS write failed"));
      return;
    }
    out.printf("torn write of ota_url = http://torn.write/ (half the blob); kept: %s\nrebooting...\n",
               config->get().otaUrl);
    SysLog::loopTask(true);
    g_console->scheduleRestart(300);
    return;
  }
  if (argc >= 2 && strcmp(argv[1], "stress") == 0) {
    if (!ctx.serial) {
      out.println(F("error: config stress only from the Serial console"));
      return;
    }
    long n = argc >= 3 ? strtol(argv[2], nullptr, 10) : STRESS_DEFAULT;
    if (n < 1 || n > static_cast<long>(STRESS_MAX)) {
      out.printf("error: n must be 1..%lu\n", static_cast<unsigned long>(STRESS_MAX));
      return;
    }
    SLOG_W("cfg", "stress: %ld writes, gen %lu", n, static_cast<unsigned long>(config->generation()));
    g_stressConfig = config;
    g_stressNext = 0;
    g_stressEnd = static_cast<uint32_t>(n);
    g_stressLastMs = millis() - STRESS_PERIOD_MS;
    g_console->startJob(stressJob, nullptr);
    return;
  }
  out.printf("usage: %s\n", CONFIG_USAGE);
}

constexpr auto WIFI_USAGE = "wifi list | wifi add <ssid> [password] | wifi del <n>  (\"quotes\" for spaces)";

void cmdWifi(void *, const int argc, char **argv, Console::Context &ctx) {
  Print &out = ctx.out;
  const char *sub = argc >= 2 ? argv[1] : "list";

  if (strcasecmp(sub, "list") == 0) {
    WifiStore::Network net{};
    for (size_t i = 0; i < Config::WIFI_STORED_MAX; i++) {
      if (WifiStore::read(i, net)) {
        out.printf("  %u: %s%s\n", static_cast<unsigned>(i + 1), net.ssid, net.password[0] ? "" : "  (open)");
      } else {
        out.printf("  %u: -\n", static_cast<unsigned>(i + 1));
      }
    }
    size_t builtIn = 0;
    for (const auto &cred : Config::WIFI_CREDENTIALS) builtIn += cred.ssid[0] != '\0';
    out.printf("built-in (secrets.h): %u, tried after the saved ones\n", static_cast<unsigned>(builtIn));
    return;
  }

  const bool add = strcasecmp(sub, "add") == 0 && argc >= 3;
  const bool del = strcasecmp(sub, "del") == 0 && argc >= 3;
  if ((add || del) && !ctx.serial) {
    // Passwords never travel over plain HTTP.
    out.println(F("error: wifi add/del only from the Serial console"));
    return;
  }
  if (add) {
    size_t slot = 0;
    switch (WifiStore::add(argv[2], argc >= 4 ? argv[3] : "", slot)) {
      case WifiStore::AddResult::Added:
      case WifiStore::AddResult::Updated:
        SLOG_I("wifi", "network #%u saved", static_cast<unsigned>(slot + 1));
        out.printf("saved as %u\n", static_cast<unsigned>(slot + 1));
        return;
      case WifiStore::AddResult::Full:
        out.printf("error: all %u slots used (wifi del <n>)\n", static_cast<unsigned>(Config::WIFI_STORED_MAX));
        return;
      case WifiStore::AddResult::Invalid:
        out.printf("error: ssid 1..%u chars, password empty or 8..%u chars\n",
                   static_cast<unsigned>(WifiStore::SSID_MAX), static_cast<unsigned>(WifiStore::PASSWORD_MAX));
        return;
      case WifiStore::AddResult::NvsError: out.println(F("error: saving to NVS failed")); return;
    }
    return;
  }
  if (del) {
    const long n = strtol(argv[2], nullptr, 10);
    if (n < 1 || n > static_cast<long>(Config::WIFI_STORED_MAX) || !WifiStore::remove(static_cast<size_t>(n - 1))) {
      out.println(F("error: no such saved network"));
      return;
    }
    SLOG_I("wifi", "network #%ld removed", n);
    out.println(F("removed"));
    return;
  }
  out.printf("usage: %s\n", WIFI_USAGE);
}

void cmdPost(void *, int, char **, Console::Context &ctx) {
  Post::print(ctx.out);
}

} // namespace

void registerAll(Console &console, ConfigStore &config) {
  g_console = &console;
  console.add("version", "version", cmdVersion, nullptr);
  console.add("log", "log [n|all] | log level [error|warn|info|debug] | log clear | log stats", cmdLog, &console);
  console.add("reboot", "reboot", cmdReboot, &console);
  console.add("config", CONFIG_USAGE, cmdConfig, &config);
  console.add("wifi", WIFI_USAGE, cmdWifi, nullptr);
  console.add("post", "post  (self-test result of this boot)", cmdPost, nullptr);
}

} // namespace SystemCommands
