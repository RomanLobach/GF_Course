// Rolling 1-hour duty-cycle accounting per sub-band.
//
// Fixed-size (60 one-minute buckets per sub-band, no heap). Fed by RadioManager with the TX
// duration it actually measured; read by SessionState to gate MEAS/TEST bursts.
#pragma once

#include <Arduino.h>
#include "Config.h"

class AirtimeBudget {
public:
  void record(const Config::SubBand band, const uint32_t ms) {
    Bucket &b = bucketFor(static_cast<uint8_t>(band), millis());
    b.usedMs += ms;
  }

  uint32_t usedMs(const Config::SubBand band) const {
    const uint32_t nowMinute = millis() / kBucketMs;
    uint32_t sum = 0;
    for (const Bucket &b : buckets_[static_cast<uint8_t>(band)]) {
      if (b.minute + kBuckets > nowMinute && b.minute <= nowMinute) sum += b.usedMs;
    }
    return sum;
  }

  static uint32_t limitMs(const Config::SubBand band) {
    return static_cast<uint32_t>(Config::AIRTIME_WINDOW_MS * Config::SUBBAND_DUTY_LIMIT[static_cast<uint8_t>(band)]);
  }

  // What MEAS/TEST may still spend in this band. G3 keeps SERVICE_BUDGET_RESERVE of its limit
  // for heartbeats and control traffic, which are never blocked.
  uint32_t availableForBenchMs(const Config::SubBand band) const {
    const uint32_t limit = limitMs(band);
    const uint32_t reserve = band == Config::SubBand::G3
                                 ? static_cast<uint32_t>(limit * Config::SERVICE_BUDGET_RESERVE)
                                 : 0;
    const uint32_t used = usedMs(band);
    return used + reserve >= limit ? 0 : limit - reserve - used;
  }

  // Rough wait until `neededMs` becomes available again (oldest buckets roll out first).
  uint32_t minutesUntilAvailable(const Config::SubBand band, const uint32_t neededMs) const {
    const uint32_t nowMinute = millis() / kBucketMs;
    for (uint32_t ahead = 1; ahead <= kBuckets; ahead++) {
      uint32_t sum = 0;
      for (const Bucket &b : buckets_[static_cast<uint8_t>(band)]) {
        if (b.minute + kBuckets > nowMinute + ahead && b.minute <= nowMinute) sum += b.usedMs;
      }
      const uint32_t limit = limitMs(band);
      const uint32_t reserve = band == Config::SubBand::G3
                                   ? static_cast<uint32_t>(limit * Config::SERVICE_BUDGET_RESERVE)
                                   : 0;
      if (sum + reserve + neededMs <= limit) return ahead;
    }
    return kBuckets;
  }

private:
  static constexpr uint32_t kBuckets = 60;
  static constexpr uint32_t kBucketMs = Config::AIRTIME_WINDOW_MS / kBuckets;

  struct Bucket {
    uint32_t minute = UINT32_MAX; // absolute minute index this bucket currently holds
    uint32_t usedMs = 0;
  };
  Bucket buckets_[Config::SUBBAND_COUNT][kBuckets];

  Bucket &bucketFor(const uint8_t band, const uint32_t nowMs) {
    const uint32_t minute = nowMs / kBucketMs;
    Bucket &b = buckets_[band][minute % kBuckets];
    if (b.minute != minute) {
      b.minute = minute;
      b.usedMs = 0;
    }
    return b;
  }
};
