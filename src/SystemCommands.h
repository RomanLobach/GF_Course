// Console commands about the firmware itself: version, log, reboot, config, wifi, post.
#pragma once

#include "Console.h"
#include "ConfigStore.h"

namespace SystemCommands {
void registerAll(Console &console, ConfigStore &config);
} // namespace SystemCommands
