#include "SystemCommands.h"

#include "SysLog.h"
#include "Version.h"

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

} // namespace

void registerAll(Console &console) {
  console.add("version", "version", cmdVersion, nullptr);
  console.add("log", "log [n|all] | log level [error|warn|info|debug] | log clear | log stats", cmdLog, &console);
  console.add("reboot", "reboot", cmdReboot, &console);
}

} // namespace SystemCommands
