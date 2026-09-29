#include "Version.h"

#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include "Config.h"

namespace Version {

const char *roleName() {
  return Config::DEVICE_ROLE == Config::Role::Base ? "base" : "rover";
}

const char *runningPartition() {
  const esp_partition_t *p = esp_ota_get_running_partition();
  return p ? p->label : "?";
}

const char *resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int-wdt";
    case ESP_RST_TASK_WDT:  return "task-wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
  }
}

const char *deviceId() {
  static char id[7] = {};
  if (id[0] == '\0') {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(id, sizeof(id), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  }
  return id;
}

void print(Print &out) {
  out.printf("%s %s\n", PROJECT, FIRMWARE);
  out.printf("  git:       %s%s\n", GIT_HASH, DIRTY ? " (dirty)" : "");
  out.printf("  built:     %s (commit date)\n", BUILD_DATE);
  out.printf("  role:      %s\n", roleName());
  out.printf("  device:    %s\n", deviceId());
  out.printf("  partition: %s\n", runningPartition());
  out.printf("  reset:     %s\n", resetReasonName());
  out.printf("  uptime:    %lu s\n", static_cast<unsigned long>(millis() / 1000));
  out.printf("  chip:      ESP32 rev %u, flash %lu KB, free heap %lu B\n", ESP.getChipRevision(),
             static_cast<unsigned long>(ESP.getFlashChipSize() / 1024),
             static_cast<unsigned long>(ESP.getFreeHeap()));
}

size_t toJson(char *buf, const size_t len) {
  const int n = snprintf(buf, len,
                         "{\"project\":\"%s\",\"version\":\"%s\",\"git\":\"%s\",\"dirty\":%s,"
                         "\"built\":\"%s\",\"role\":\"%s\",\"device\":\"%s\",\"partition\":\"%s\","
                         "\"reset\":\"%s\",\"uptimeS\":%lu,\"freeHeap\":%lu}",
                         PROJECT, FIRMWARE, GIT_HASH, DIRTY ? "true" : "false", BUILD_DATE, roleName(),
                         deviceId(), runningPartition(), resetReasonName(),
                         static_cast<unsigned long>(millis() / 1000),
                         static_cast<unsigned long>(ESP.getFreeHeap()));
  if (n < 0) return 0;
  return static_cast<size_t>(n) < len ? static_cast<size_t>(n) : len - 1;
}

} // namespace Version
