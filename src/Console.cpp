#include "Console.h"

Console::Console() = default;

bool Console::add(const char *name, const char *usage, const Handler handler, void *self) {
  if (count_ >= MAX_COMMANDS) return false;
  commands_[count_++] = {name, usage, handler, self};
  return true;
}

void Console::loopTask() {
  if (restartPending_ && static_cast<int32_t>(millis() - restartAtMs_) >= 0) {
    ESP.restart();
  }

  if (job_ && !job_(jobSelf_, Serial)) {
    job_ = nullptr;
  }

  // Only what is already buffered - Serial.available() never waits.
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (lineOverflow_) {
        Serial.println(F("error: line too long"));
      } else if (lineLen_ > 0) {
        line_[lineLen_] = '\0';
        Context ctx{Serial, true};
        execute(line_, ctx);
      }
      lineLen_ = 0;
      lineOverflow_ = false;
    } else if (c == '\b' || c == 0x7F) {
      if (lineLen_ > 0) lineLen_--;
    } else if (c >= 0x20) {
      if (lineLen_ < LINE_LEN - 1) {
        line_[lineLen_++] = static_cast<char>(c);
      } else {
        lineOverflow_ = true;
      }
    }
  }
}

void Console::execute(char *line, Context &ctx) {
  char *argv[MAX_ARGS];
  int argc = 0;
  char *save = nullptr;
  for (char *tok = strtok_r(line, " \t", &save); tok && argc < static_cast<int>(MAX_ARGS);
       tok = strtok_r(nullptr, " \t", &save)) {
    argv[argc++] = tok;
  }
  if (argc == 0) return;

  if (strcasecmp(argv[0], "help") == 0 || strcmp(argv[0], "?") == 0) {
    printHelp(ctx.out);
    return;
  }
  for (size_t i = 0; i < count_; i++) {
    if (strcasecmp(argv[0], commands_[i].name) == 0) {
      commands_[i].handler(commands_[i].self, argc, argv, ctx);
      return;
    }
  }
  ctx.out.printf("error: unknown command '%s' (try 'help')\n", argv[0]);
}

void Console::startJob(const Job job, void *self) {
  job_ = job;
  jobSelf_ = self;
}

void Console::scheduleRestart(const uint32_t delayMs) {
  restartPending_ = true;
  restartAtMs_ = millis() + delayMs;
}

void Console::printHelp(Print &out) const {
  out.println(F("commands:"));
  out.println(F("  help"));
  for (size_t i = 0; i < count_; i++) {
    out.printf("  %s\n", commands_[i].usage);
  }
}
