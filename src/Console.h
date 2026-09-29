// Text command console: one command registry, two front-ends.
//
// - Serial: loopTask() reads whatever bytes are already buffered (never waits), assembles a
//   line, runs it. Commands print into a Print&; long listings (log dump) are paced through
//   the job hook so a single command never holds the protocol core for longer than the UART
//   buffer can absorb.
// - HTTP (WifiOffload's POST /cmd): execute() with any Print (the HTTP response), where
//   blocking is fine - Wi-Fi is only up in IDLE with the radio paused.
//
// Protocol core only. Handlers are plain function pointers + a context pointer - no heap.
#pragma once

#include <Arduino.h>

class Console {
public:
  static constexpr size_t MAX_COMMANDS = 16;
  static constexpr size_t MAX_ARGS = 8;
  static constexpr size_t LINE_LEN = 128;

  struct Context {
    Print &out;
    bool serial; // true: interactive Serial (paced output available), false: HTTP
  };
  using Handler = void (*)(void *self, int argc, char **argv, Context &ctx);

  // Background output job for Serial: called every loopTask() until it returns false.
  // May only write while out.availableForWrite() allows (see SysLog-style pacing).
  using Job = bool (*)(void *self, Print &out);

  Console();

  // `name` is the first word ("log"); the handler sees argv[0] == name. Returns false when
  // the registry is full.
  bool add(const char *name, const char *usage, Handler handler, void *self);

  void loopTask();
  void execute(char *line, Context &ctx); // tokenizes `line` in place

  // For Serial handlers: start a paced output job (replaces any running one).
  void startJob(Job job, void *self);

  // Deferred restart, so the reply gets out of the UART/HTTP before the chip resets.
  void scheduleRestart(uint32_t delayMs);

private:
  struct Command {
    const char *name;
    const char *usage;
    Handler handler;
    void *self;
  };

  Command commands_[MAX_COMMANDS]{};
  size_t count_ = 0;
  char line_[LINE_LEN]{};
  size_t lineLen_ = 0;
  bool lineOverflow_ = false;

  Job job_ = nullptr;
  void *jobSelf_ = nullptr;

  bool restartPending_ = false;
  uint32_t restartAtMs_ = 0;

  void printHelp(Print &out) const;
};
