#include "SessionState.h"
#include <esp_system.h>
#include "Airtime.h"
#include "SysLog.h"

using Protocol::DeviceState;
using Protocol::HbState;
using Protocol::MsgType;
using Protocol::ServicePacket;
using Protocol::SlotDecision;
using Protocol::SlotReply;
using Protocol::SlotReport;
using Protocol::SlotRequest;

namespace {
constexpr bool kIsBase = Config::DEVICE_ROLE == Config::Role::Base;

uint32_t jitterMs() { return esp_random() % (Config::SEARCH_JITTER_MS + 1); }


ServicePacket makePacket(const MsgType type, const uint8_t b1, const uint16_t value) {
  ServicePacket p{};
  p.type = static_cast<uint8_t>(type);
  p.b1 = b1;
  p.value = value;
  return p;
}

uint8_t wrapProfile(const int p) {
  constexpr int n = Config::BENCH_CONFIG_COUNT;
  return static_cast<uint8_t>(((p - 1) % n + n) % n + 1);
}
} // namespace

SessionState::SessionState(RadioManager &radio, FlashLog &log, ConfigStore &config)
  : radio_(radio), log_(log), config_(config) {}

// ============================================================================
// Lifecycle
// ============================================================================

void SessionState::begin() {
  loadSessionId();
  radio_.useServiceChannel();
  radio_.startReceive();
  state_ = DeviceState::Idle;
}

void SessionState::update() {
  const uint32_t now = millis();

  if (radio_.isTransmitDone()) {
    if (benchTxInFlight_) {
      benchTxInFlight_ = false;
      if (endMarkerInFlight_) {
        // TEST: the Base cut its burst short - go straight to the slot.
        endMarkerInFlight_ = false;
        enterSlot(now);
      } else {
        // Base bench packet finished - its measured on-air time is the record's txTimeUs.
        pendingTxRecord_.txTimeUs = radio_.lastTxDurationUs();
        if (runMode_ == RunMode::Meas) log_.logEvent(pendingTxRecord_);
        txTimeSumUs_ += radio_.lastTxDurationUs();
        txTimeCount_++;
        nextTxAtMs_ = now + Config::BURST_GUARD_MS;
        if (runMode_ == RunMode::Test) {
          // Listen gap: the Rover may cut the burst short with an early SLOT_REPORT.
          radio_.useServiceChannel();
          radio_.startReceive();
          baseInGap_ = true;
        }
      }
    } else {
      radio_.startReceive(); // back to listening on the service channel
    }
  }

  const bool inBurst = runMode_ != RunMode::None && runPhase_ == RunPhase::Burst;
  if (inBurst) {
    if (kIsBase) {
      if (baseInGap_) pollService(now);
    } else if (roverIrqInFlight_) {
      pollService(now);
    } else {
      pollBurstRover(now);
    }
  } else {
    pollService(now);
  }

  if (lingerUntilMs_ != 0 && reached(now, lingerUntilMs_)) {
    lingerUntilMs_ = 0;
    lingerSlotReply_ = false;
  }

  switch (state_) {
    case DeviceState::Idle:
      break;
    case DeviceState::Searching:
    case DeviceState::SeenYou:
      tickSearch(now);
      break;
    case DeviceState::Synced:
      tickSynced(now);
      break;
    case DeviceState::Measuring:
    case DeviceState::Testing:
      tickRun(now);
      break;
  }
  tickTxn(now);

  if (runMode_ == RunMode::None || runPhase_ == RunPhase::Slot) serviceTx(millis());
}

uint32_t SessionState::sessionElapsedMs() const {
  if (state_ == DeviceState::Idle || state_ == DeviceState::Searching || state_ == DeviceState::SeenYou) return 0;
  return millis() - syncedAtMs_;
}

bool SessionState::isBusy() const {
  if (state_ == DeviceState::Searching || state_ == DeviceState::SeenYou) return true;
  if (txn_ != Txn::None || scheduledMode_ != RunMode::None) return true;
  if (runMode_ != RunMode::None) {
    return localRequest_ != SlotRequest::None || desiredProfile_ != 0;
  }
  return false;
}

bool SessionState::isTimeCritical() const {
  return runMode_ != RunMode::None && runPhase_ == RunPhase::Burst;
}

SessionState::RunView SessionState::runView() const {
  RunView v;
  if (runMode_ == RunMode::None) return v;
  v.profile = profile_;
  v.desiredProfile = desiredProfile_;
  v.burstSize = burstSize_;
  v.burstTx = txSent_;
  v.burstRx = rxCount_;
  v.lastRecv = lastBurstRecv_;
  v.lastSent = lastBurstSent_;
  v.paused = testPaused_;
  v.hasSignal = lastBurstHasSignal_;
  v.rssi = lastBurstRssi_;
  v.snr = lastBurstSnr_;
  return v;
}

// ============================================================================
// Operator actions
// ============================================================================

bool SessionState::requestStartSearching() {
  if (state_ != DeviceState::Idle || radio_.isPaused()) return false;
  suppressActive_ = false; // explicit restart re-enables the auto-responder right away
  enterSearching(millis());
  return true;
}

SessionState::AbortResult SessionState::requestAbort() {
  const uint32_t now = millis();
  switch (state_) {
    case DeviceState::Idle:
      return AbortResult::NotApplicable;
    case DeviceState::Searching:
      // Peer never answered us yet - nothing to negotiate.
      dropToIdle();
      suppressActive_ = true;
      suppressUntilMs_ = now + Config::ABORT_SUPPRESS_MS;
      return AbortResult::Stopped;
    case DeviceState::SeenYou:
    case DeviceState::Synced:
      if (txn_ == Txn::Abort) return AbortResult::Pending;
      cancelPendingWork();
      txn_ = Txn::Abort;
      txnNeedsSend_ = true;
      txnAttempts_ = 0;
      return AbortResult::Pending;
    case DeviceState::Measuring:
    case DeviceState::Testing:
      localRequest_ = SlotRequest::Abort; // delivered in the next slot
      return AbortResult::Pending;
  }
  return AbortResult::NotApplicable;
}

bool SessionState::baseHasBudgetForMeas(uint32_t &waitMinutes) const {
  uint32_t need[Config::SUBBAND_COUNT] = {};
  for (uint8_t p = 1; p <= Config::BENCH_CONFIG_COUNT; p++) {
    need[static_cast<uint8_t>(Config::BENCH_CONFIGS[p - 1].band)] += Airtime::burstCostMs(p, Config::MEAS_PACKETS_PER_BURST);
  }
  waitMinutes = 0;
  bool ok = true;
  for (uint8_t b = 0; b < Config::SUBBAND_COUNT; b++) {
    const auto band = static_cast<Config::SubBand>(b);
    if (radio_.budget().availableForBenchMs(band) < need[b]) {
      ok = false;
      const uint32_t w = radio_.budget().minutesUntilAvailable(band, need[b]);
      if (w > waitMinutes) waitMinutes = w;
    }
  }
  return ok;
}

bool SessionState::budgetAllowsBurst(const uint8_t profile) const {
  const Config::SubBand band = Config::BENCH_CONFIGS[profile - 1].band;
  return radio_.budget().availableForBenchMs(band) >= Airtime::burstCostMs(profile, Config::TEST_PACKETS_PER_BURST);
}

SessionState::StartResult SessionState::requestStartMeasurement() {
  if (state_ != DeviceState::Synced) return StartResult::NotSynced;
  if (txn_ != Txn::None || scheduledMode_ != RunMode::None) return StartResult::Busy;
  if (!log_.hasRoomForMeasRun()) return StartResult::LogFull;
  if (kIsBase) {
    uint32_t wait = 0;
    if (!baseHasBudgetForMeas(wait)) {
      lastBudgetWaitMin_ = wait;
      return StartResult::Budget;
    }
  }
  txn_ = Txn::MeasStart;
  txnNeedsSend_ = true;
  txnAttempts_ = 0;
  return StartResult::Sent;
}

SessionState::StartResult SessionState::requestStartTest() {
  if (state_ != DeviceState::Synced) return StartResult::NotSynced;
  if (txn_ != Txn::None || scheduledMode_ != RunMode::None) return StartResult::Busy;
  txn_ = Txn::TestStart;
  txnNeedsSend_ = true;
  txnAttempts_ = 0;
  return StartResult::Sent;
}

bool SessionState::requestStopRun() {
  if (runMode_ == RunMode::None) return false;
  if (localRequest_ != SlotRequest::Abort) localRequest_ = SlotRequest::Stop;
  return true;
}

void SessionState::requestTestProfileDelta(const int8_t delta) {
  if (runMode_ != RunMode::Test || delta == 0) return;
  const uint8_t base = desiredProfile_ != 0 ? desiredProfile_ : profile_;
  const uint8_t next = wrapProfile(base + delta);
  desiredProfile_ = next == profile_ ? 0 : next;
}

void SessionState::resetSessionId() {
  if (state_ != DeviceState::Idle) return;
  sessionId_ = 0;
  saveSessionId();
}

bool SessionState::popNotice(Notice &out) {
  if (noticeTail_ == noticeHead_) return false;
  out = notices_[noticeTail_];
  noticeTail_ = (noticeTail_ + 1) % NOTICE_CAPACITY;
  return true;
}

// ============================================================================
// Helpers
// ============================================================================

void SessionState::notify(const Notice n) {
  const uint8_t next = (noticeHead_ + 1) % NOTICE_CAPACITY;
  if (next == noticeTail_) noticeTail_ = (noticeTail_ + 1) % NOTICE_CAPACITY; // drop oldest
  notices_[noticeHead_] = n;
  noticeHead_ = next;
}

void SessionState::loadSessionId() {
  sessionId_ = config_.get().sessionId;
}

void SessionState::saveSessionId() {
  if (!config_.setSessionId(sessionId_)) SLOG_E("sess", "session id %u not saved", sessionId_);
}

uint16_t SessionState::nextId(const uint16_t id) {
  // 0000 is the "no session yet" sentinel; 9999 wraps to 0001.
  return id >= 9999 ? 1 : static_cast<uint16_t>(id + 1);
}

void SessionState::logSessionEvent(const LogEventKind kind) {
  LogRecord rec{};
  rec.sessionId = sessionId_;
  rec.type = static_cast<uint8_t>(LogRecordType::SessionEvent);
  rec.eventKind = static_cast<uint8_t>(kind);
  rec.sessionTimeMs = sessionElapsedMs();
  rec.rssi = static_cast<int16_t>(lroundf(lastRssi_));
  rec.snrTenths = static_cast<int16_t>(lroundf(lastSnr_ * 10.0f));
  rec.deviceStatus = static_cast<uint8_t>(state_);
  log_.logEvent(rec);
}

void SessionState::logBench(const uint8_t direction, const bool ok, const float rssi, const float snr,
                            const uint32_t txTimeUs) {
  LogRecord rec{};
  rec.sessionId = sessionId_;
  rec.direction = direction;
  rec.type = static_cast<uint8_t>(LogRecordType::Bench);
  rec.configIndex = profile_;
  rec.sessionTimeMs = sessionElapsedMs();
  rec.size = Protocol::BENCH_PACKET_SIZE;
  rec.rssi = direction == 1 && ok ? static_cast<int16_t>(lroundf(rssi)) : FlashLog::NO_RSSI;
  rec.snrTenths = direction == 1 && ok ? static_cast<int16_t>(lroundf(snr * 10.0f)) : FlashLog::NO_SNR;
  rec.deviceStatus = static_cast<uint8_t>(DeviceState::Measuring);
  rec.received = ok ? 1 : 0;
  rec.txTimeUs = txTimeUs;
  log_.logEvent(rec);
}

void SessionState::logTestSummary(const uint8_t direction, const uint8_t sent, const uint8_t recv, const bool hasSignal,
                                  const float rssi, const float snr, const uint32_t txTimeUs) {
  if (!log_.canLogTest()) return; // TEST never fills the log past the limit
  LogRecord rec{};
  rec.sessionId = sessionId_;
  rec.direction = direction;
  rec.type = static_cast<uint8_t>(LogRecordType::TestSummary);
  rec.configIndex = profile_;
  rec.eventKind = sent;
  rec.sessionTimeMs = sessionElapsedMs();
  rec.size = Protocol::BENCH_PACKET_SIZE;
  rec.rssi = hasSignal ? static_cast<int16_t>(lroundf(rssi)) : FlashLog::NO_RSSI;
  rec.snrTenths = hasSignal ? static_cast<int16_t>(lroundf(snr * 10.0f)) : FlashLog::NO_SNR;
  rec.deviceStatus = static_cast<uint8_t>(DeviceState::Testing);
  rec.received = recv;
  rec.txTimeUs = txTimeUs;
  log_.logEvent(rec);
}

// ============================================================================
// State transitions
// ============================================================================

void SessionState::cancelPendingWork() {
  txn_ = Txn::None;
  txnNeedsSend_ = false;
  scheduledMode_ = RunMode::None;
  commitResponder_ = false;
}

void SessionState::dropToIdle() {
  cancelPendingWork();
  runMode_ = RunMode::None;
  runPhase_ = RunPhase::Burst;
  benchTxInFlight_ = false;
  localRequest_ = SlotRequest::None;
  desiredProfile_ = 0;
  replyPending_ = false;
  missedHb_ = 0;
  state_ = DeviceState::Idle;
  radio_.useServiceChannel();
  radio_.startReceive();
  SLOG_D("sess", "-> IDLE");
}

void SessionState::enterSearching(const uint32_t now) {
  cancelPendingWork();
  state_ = DeviceState::Searching;
  searchStartMs_ = now;
  nextSearchTxMs_ = now + jitterMs();
}

void SessionState::enterSeenYou(const uint32_t now) {
  const bool fresh = state_ != DeviceState::Searching; // from IDLE/SYNC: (re)start the search clock
  cancelPendingWork();
  state_ = DeviceState::SeenYou;
  if (fresh) searchStartMs_ = now;
  nextSearchTxMs_ = now + Config::SEARCH_FAST_PERIOD_MS + jitterMs();
}

void SessionState::armSyncedHeartbeat(const uint32_t now) {
  missedHb_ = 0;
  peerDeadlineMs_ = now + Config::HEARTBEAT_INTERVAL_MS + Config::HEARTBEAT_MISS_TOLERANCE_MS;
  // Base owns phase 0 on an absolute schedule, Rover sits half an
  // interval later (and re-anchors on every Base heartbeat it hears).
  nextHbMs_ = kIsBase ? now + Config::HEARTBEAT_BASE_ENTRY_DELAY_MS : now + Config::HEARTBEAT_INTERVAL_MS / 2;
}

void SessionState::enterSyncedViaHandshake(const uint16_t id, const uint32_t now) {
  cancelPendingWork();
  if (id != sessionId_) {
    sessionId_ = id;
    saveSessionId(); // once per session - NVS wear is not a concern (CLAUDE.md rule 7)
  }
  state_ = DeviceState::Synced;
  syncedAtMs_ = now;
  enteredSyncMs_ = now;
  armSyncedHeartbeat(now);
  // Announce SYNCED right away so the peer (still SEEN_YOU) completes too, then continue on
  // the role schedule. The peer just finished transmitting, so it is listening now.
  queueHeartbeatReply(HbState::Synced, now);
  if (kIsBase) nextHbMs_ = now + Config::HEARTBEAT_INTERVAL_MS;
  logSessionEvent(LogEventKind::SyncSuccess);
  notify(Notice::Synced);
  SLOG_I("sess", "-> SYNC id=%u", sessionId_);
}

void SessionState::returnToSynced(const bool newSession, const uint32_t now) {
  runMode_ = RunMode::None;
  runPhase_ = RunPhase::Burst;
  benchTxInFlight_ = false;
  localRequest_ = SlotRequest::None;
  desiredProfile_ = 0;
  testPaused_ = false;
  radio_.useServiceChannel();
  radio_.startReceive();
  state_ = DeviceState::Synced;
  if (newSession) {
    // Both sides already share the id, so +1 on both stays equal; if one
    // side misses this, the heartbeat max-rule realigns it.
    sessionId_ = nextId(sessionId_);
    saveSessionId();
    syncedAtMs_ = now;
    logSessionEvent(LogEventKind::SyncSuccess);
  }
  enteredSyncMs_ = now;
  armSyncedHeartbeat(now);
}

// ============================================================================
// TX
// ============================================================================

void SessionState::queueReply(const ServicePacket &pkt, const uint32_t now) {
  reply_ = pkt;
  replyPending_ = true;
  replyIsStartAck_ = false;
  replyIsSlotReply_ = false;
  replyQueuedMs_ = now;
}

void SessionState::queueHeartbeatReply(const HbState hs, const uint32_t now) {
  queueReply(makePacket(MsgType::Heartbeat, static_cast<uint8_t>(hs), sessionId_), now);
}

bool SessionState::sendService(const ServicePacket &pkt) {
  uint8_t raw[Protocol::SERVICE_PACKET_SIZE];
  Protocol::encodeServicePacket(pkt, raw);
  const bool ok = radio_.startTransmit(raw, sizeof(raw));
  if (ok) SLOG_D("pkt", "tx %02X %02X %02X %02X", raw[0], raw[1], raw[2], raw[3]);
  return ok;
}

void SessionState::serviceTx(const uint32_t now) {
  if (radio_.isTransmitting() || radio_.isPaused()) return;
  // Keep the air (and the radio) free right before a scheduled common start, so the run
  // starts on time on both sides instead of waiting out a heartbeat in flight.
  if (scheduledMode_ != RunMode::None &&
      static_cast<int32_t>(scheduledStartMs_ - now) < static_cast<int32_t>(Airtime::serviceMs() + 30)) {
    return;
  }

  // 1) reactive reply - the peer just finished transmitting and is listening right now
  if (replyPending_) {
    if (now - replyQueuedMs_ > Config::PENDING_REPLY_EXPIRY_MS) {
      replyPending_ = false; // stale - the peer's retry will ask again
    } else {
      ServicePacket pkt = reply_;
      const uint32_t flight = Airtime::serviceMs();
      if (replyIsStartAck_) {
        const int32_t left = static_cast<int32_t>(scheduledStartMs_ - now - flight);
        pkt.value = left > 0 ? static_cast<uint16_t>(left) : 0;
      } else if (replyIsSlotReply_) {
        const int32_t left = static_cast<int32_t>(nextCycleStartMs_ - now - flight);
        pkt.value = left > 0 ? static_cast<uint16_t>(left) : 0;
      }
      if (sendService(pkt)) replyPending_ = false;
      return;
    }
  }

  // 2) transaction request
  if (txn_ != Txn::None && txnNeedsSend_) {
    ServicePacket pkt{};
    if (txn_ == Txn::Abort) pkt = makePacket(MsgType::AbortReq, 0, sessionId_);
    else if (txn_ == Txn::MeasStart) pkt = makePacket(MsgType::MeasStartReq, 0, 0);
    else pkt = makePacket(MsgType::TestStartReq, 1, 0);
    if (sendService(pkt)) {
      txnNeedsSend_ = false;
      txnAttempts_++;
      txnDeadlineMs_ = now + Airtime::serviceMs() + Config::CONTROL_ACK_TIMEOUT_MS;
    }
    return;
  }

  // 3) Rover slot report (once at the slot start, once more if no reply came)
  if (runMode_ != RunMode::None && runPhase_ == RunPhase::Slot) {
    if (!kIsBase && !replyReceived_ &&
        ((reportsSent_ == 0 && reached(now, slotStartMs_ + Config::SLOT_GUARD_MS)) ||
         (reportsSent_ == 1 && reached(now, slotStartMs_ + Config::SLOT_REPORT_RETRY_MS)))) {
      SlotReport r;
      r.profile = profile_;
      r.desiredProfile = runMode_ == RunMode::Test ? desiredProfile_ : 0;
      r.received = rxCount_;
      r.request = localRequest_;
      if (sendService(Protocol::encodeSlotReport(r))) {
        reportsSent_++;
        desiredAtReport_ = r.desiredProfile;
      }
    }
    return; // no heartbeats during a run
  }

  // 4) periodic heartbeat / search
  if (state_ == DeviceState::Searching || state_ == DeviceState::SeenYou) {
    if (reached(now, nextSearchTxMs_)) {
      const HbState hs = state_ == DeviceState::Searching ? HbState::Searching : HbState::SeenYou;
      if (sendService(makePacket(MsgType::Heartbeat, static_cast<uint8_t>(hs), sessionId_))) {
        const bool fast = now - searchStartMs_ < Config::SEARCH_FAST_PHASE_MS;
        nextSearchTxMs_ = now + (fast ? Config::SEARCH_FAST_PERIOD_MS : Config::HEARTBEAT_INTERVAL_MS) + jitterMs();
      }
    }
  } else if (state_ == DeviceState::Synced) {
    if (reached(now, nextHbMs_)) {
      if (sendService(makePacket(MsgType::Heartbeat, static_cast<uint8_t>(HbState::Synced), sessionId_))) {
        // Absolute schedule - never "now + T", or jitter accumulates.
        do {
          nextHbMs_ += Config::HEARTBEAT_INTERVAL_MS;
        } while (reached(now, nextHbMs_));
      }
    }
  }
}

// ============================================================================
// RX
// ============================================================================

void SessionState::pollService(const uint32_t now) {
  uint8_t buf[Protocol::SERVICE_PACKET_SIZE];
  size_t len = 0;
  float rssi = 0, snr = 0;
  if (!radio_.poll(buf, sizeof(buf), len, rssi, snr)) return;
  ServicePacket pkt{};
  if (!Protocol::decodeServicePacket(buf, len, pkt)) return;
  SLOG_D("pkt", "rx %02X %02X %02X %02X rssi %d snr %d", buf[0], buf[1], buf[2], buf[3], static_cast<int>(rssi),
         static_cast<int>(snr));
  lastRssi_ = rssi;
  lastSnr_ = snr;
  handleService(pkt, now);
}

void SessionState::handleService(const ServicePacket &pkt, const uint32_t now) {
  // Any valid packet from the peer proves the link is alive.
  if (state_ == DeviceState::Synced) {
    missedHb_ = 0;
    peerDeadlineMs_ = now + Config::HEARTBEAT_INTERVAL_MS + Config::HEARTBEAT_MISS_TOLERANCE_MS;
  }

  switch (static_cast<MsgType>(pkt.type)) {
    case MsgType::Heartbeat:
      if (pkt.b1 >= 1 && pkt.b1 <= 3) onHeartbeat(static_cast<HbState>(pkt.b1), pkt.value, now);
      break;
    case MsgType::AbortReq:
      onAbortReq(now);
      break;
    case MsgType::AbortAck:
      if (txn_ == Txn::Abort) {
        logSessionEvent(LogEventKind::ManualStop);
        dropToIdle();
        suppressActive_ = true;
        suppressUntilMs_ = now + Config::ABORT_SUPPRESS_MS;
        notify(Notice::AbortDone);
      }
      break;
    case MsgType::MeasStartReq:
      onStartReq(RunMode::Meas, 1, now);
      break;
    case MsgType::TestStartReq:
      onStartReq(RunMode::Test, pkt.b1 >= 1 && pkt.b1 <= 6 ? pkt.b1 : 1, now);
      break;
    case MsgType::MeasStartAck:
      onStartAck(RunMode::Meas, 1, pkt.value, now);
      break;
    case MsgType::TestStartAck:
      onStartAck(RunMode::Test, pkt.b1 >= 1 && pkt.b1 <= 6 ? pkt.b1 : 1, pkt.value, now);
      break;
    case MsgType::StartNak: {
      const Txn expected = pkt.value == 1 ? Txn::MeasStart : Txn::TestStart;
      if (txn_ == expected) {
        txn_ = Txn::None;
        const auto reason = static_cast<Protocol::NakReason>(pkt.b1);
        notify(reason == Protocol::NakReason::Budget    ? Notice::PeerBudget
               : reason == Protocol::NakReason::LogFull ? Notice::PeerLogFull
                                                        : Notice::PeerBusy);
      }
      break;
    }
    case MsgType::SlotReport:
      if (kIsBase) onSlotReport(Protocol::decodeSlotReport(pkt), now);
      break;
    case MsgType::SlotReply:
      if (!kIsBase) onSlotReply(Protocol::decodeSlotReply(pkt), now);
      break;
  }
}

void SessionState::onHeartbeat(const HbState hs, const uint16_t id, const uint32_t now) {
  switch (state_) {
    case DeviceState::Idle:
      // Auto-responder. Heard the peer -> "бачу тебе" + answer at once.
      if (radio_.isPaused()) return;
      if (suppressActive_ && !reached(now, suppressUntilMs_)) return;
      suppressActive_ = false;
      enterSeenYou(now);
      queueHeartbeatReply(HbState::SeenYou, now);
      break;

    case DeviceState::Searching:
      // Step 2 of handshake #1: any heartbeat from the peer.
      enterSeenYou(now);
      queueHeartbeatReply(HbState::SeenYou, now);
      break;

    case DeviceState::SeenYou:
      if (hs == HbState::Searching) {
        queueHeartbeatReply(HbState::SeenYou, now); // peer hasn't heard us yet
      } else if (hs == HbState::SeenYou) {
        // Step 3: the peer hears us too. Both compute the same id from pre-sync values.
        const uint16_t base = sessionId_ > id ? sessionId_ : id;
        enterSyncedViaHandshake(nextId(base), now);
      } else {
        enterSyncedViaHandshake(id, now); // peer already reconciled - adopt its id
      }
      break;

    case DeviceState::Synced:
      if (hs == HbState::Synced) {
        if (!kIsBase) nextHbMs_ = now + Config::HEARTBEAT_INTERVAL_MS / 2; // re-anchor on Base
        if (id > sessionId_ && id <= 9999) {
          sessionId_ = id; // max-rule
          saveSessionId();
        }
      } else if (now - enteredSyncMs_ < Config::HEARTBEAT_INTERVAL_MS) {
        queueHeartbeatReply(HbState::Synced, now); // duplicate: our SYNCED didn't reach it
      } else {
        // The peer lost the session - redo handshake #1 together.
        logSessionEvent(LogEventKind::SyncFail);
        enterSeenYou(now);
        queueHeartbeatReply(HbState::SeenYou, now);
      }
      break;

    case DeviceState::Measuring:
    case DeviceState::Testing:
      // A heartbeat means the peer is no longer in the run.
      endRun(RunEnd::LinkLost, now);
      onHeartbeat(hs, id, now);
      break;
  }
}

void SessionState::onAbortReq(const uint32_t now) {
  // Always answer, in any state - abort beats everything.
  const bool wasActive = state_ != DeviceState::Idle;
  if (wasActive) {
    logSessionEvent(LogEventKind::ManualStop);
    dropToIdle();
    suppressActive_ = true;
    suppressUntilMs_ = now + Config::ABORT_SUPPRESS_MS;
    notify(Notice::AbortByPeer);
  }
  queueReply(makePacket(MsgType::AbortAck, 0, sessionId_), now);
}

void SessionState::onStartReq(const RunMode mode, const uint8_t profile, const uint32_t now) {
  const uint16_t kind = mode == RunMode::Meas ? 1 : 2;

  // Duplicate while we're already committed to the same start -> same answer again.
  if (scheduledMode_ == mode && commitResponder_ && state_ == DeviceState::Synced) {
    queueReply(makePacket(mode == RunMode::Meas ? MsgType::MeasStartAck : MsgType::TestStartAck, scheduledProfile_, 0), now);
    replyIsStartAck_ = true;
    return;
  }
  if (state_ != DeviceState::Synced) return;
  if (txn_ == Txn::Abort) return; // abort wins
  if (txn_ == Txn::MeasStart || txn_ == Txn::TestStart) {
    if (kIsBase) return; // simultaneous requests: Base wins, Rover will answer Base's
    txn_ = Txn::None;    // Rover: drop our own request, answer Base's
    txnNeedsSend_ = false;
  }
  if (scheduledMode_ != RunMode::None) {
    queueReply(makePacket(MsgType::StartNak, static_cast<uint8_t>(Protocol::NakReason::Busy), kind), now);
    return;
  }
  if (mode == RunMode::Meas) {
    if (!log_.hasRoomForMeasRun()) {
      queueReply(makePacket(MsgType::StartNak, static_cast<uint8_t>(Protocol::NakReason::LogFull), kind), now);
      return;
    }
    uint32_t wait = 0;
    if (kIsBase && !baseHasBudgetForMeas(wait)) {
      queueReply(makePacket(MsgType::StartNak, static_cast<uint8_t>(Protocol::NakReason::Budget), kind), now);
      return;
    }
  }

  scheduledMode_ = mode;
  scheduledProfile_ = profile;
  scheduledStartMs_ = now + Config::START_DELAY_MS;
  commitResponder_ = true;
  queueReply(makePacket(mode == RunMode::Meas ? MsgType::MeasStartAck : MsgType::TestStartAck, profile, 0), now);
  replyIsStartAck_ = true;
}

void SessionState::onStartAck(const RunMode mode, const uint8_t profile, const uint16_t msToStart, const uint32_t now) {
  const Txn expected = mode == RunMode::Meas ? Txn::MeasStart : Txn::TestStart;
  if (txn_ != expected || state_ != DeviceState::Synced) return;
  txn_ = Txn::None;
  txnNeedsSend_ = false;
  // The ack's value is already compensated for its own flight time (see serviceTx()).
  scheduledMode_ = mode;
  scheduledProfile_ = profile;
  scheduledStartMs_ = now + msToStart;
  commitResponder_ = false;
}

void SessionState::onSlotReport(const SlotReport &r, const uint32_t now) {
  // Base side. Outside a run: answer strays with the lingering final decision, or STOP.
  if (runMode_ == RunMode::None) {
    if (lingerSlotReply_) {
      queueReply(Protocol::encodeSlotReply(lingerReply_), now);
    } else if (state_ == DeviceState::Synced) {
      SlotReply stop;
      stop.decision = SlotDecision::Stop;
      queueReply(Protocol::encodeSlotReply(stop), now);
    }
    return;
  }
  if (runPhase_ == RunPhase::Burst) {
    // TEST: an early report heard in a listen gap cuts the burst short.
    if (runMode_ != RunMode::Test || !baseInGap_) return;
    enterSlot(now);
  }
  if (runPhase_ != RunPhase::Slot) return;
  reportReceived_ = true;
  lastReport_ = r;
  if (!decided_) {
    decision_ = decide(true, r);
    decided_ = true;
    desiredAtDecision_ = desiredProfile_;
    if (!burstSkipped_) {
      lastBurstRecv_ = r.received;
      lastBurstSent_ = txSent_;
    }
    if (runMode_ == RunMode::Test) {
      // TEST: don't sit out the rest of the fixed slot - start the next burst soon.
      const uint32_t fast = now + Config::TEST_FAST_NEXT_MS;
      if (static_cast<int32_t>(fast - nextCycleStartMs_) < 0) nextCycleStartMs_ = fast;
    }
  }
  queueReply(Protocol::encodeSlotReply(decision_), now);
  replyIsSlotReply_ = true; // every decision carries "ms until the slot ends" for the Rover
}

void SessionState::onSlotReply(const SlotReply &r, const uint32_t now) {
  // Rover side.
  if (runMode_ == RunMode::None) return;
  if (runPhase_ == RunPhase::Burst) {
    if (!roverIrqInFlight_) return;
    roverIrqInFlight_ = false; // our early report got through - the burst is over
    enterSlot(now);
    reportsSent_ = 1;
  }
  if (runPhase_ != RunPhase::Slot || replyReceived_) return;
  replyReceived_ = true;
  decision_ = r;
  nextCycleStartMs_ = now + r.msToNext;

  // TEST: log/show this burst only now that we know it was actually transmitted.
  if (runMode_ == RunMode::Test) {
    testPaused_ = r.lastSkipped || r.nextSkipped;
    if (!r.lastSkipped) {
      lastBurstRecv_ = rxCount_;
      lastBurstSent_ = slotSentEstimate_;
      lastBurstHasSignal_ = rxCount_ > 0;
      lastBurstRssi_ = rxCount_ ? rxRssiSum_ / rxCount_ : 0;
      lastBurstSnr_ = rxCount_ ? rxSnrSum_ / rxCount_ : 0;
      logTestSummary(1, slotSentEstimate_, rxCount_, lastBurstHasSignal_, lastBurstRssi_, lastBurstSnr_, 0);
    }
  }
}

// ============================================================================
// Per-state ticks
// ============================================================================

void SessionState::tickSearch(const uint32_t now) {
  if (now - searchStartMs_ >= Config::SEARCH_TIMEOUT_MS) {
    dropToIdle();
    notify(Notice::SearchTimeout);
  }
}

void SessionState::tickSynced(const uint32_t now) {
  if (scheduledMode_ != RunMode::None && reached(now, scheduledStartMs_) && !radio_.isTransmitting()) {
    const RunMode mode = scheduledMode_;
    const uint8_t profile = scheduledProfile_;
    scheduledMode_ = RunMode::None;
    commitResponder_ = false;
    startRun(mode, profile, now);
    return;
  }

  if (reached(now, peerDeadlineMs_)) {
    missedHb_++;
    peerDeadlineMs_ += Config::HEARTBEAT_INTERVAL_MS;
    if (missedHb_ >= Config::HEARTBEAT_MISS_LIMIT) {
      logSessionEvent(LogEventKind::SyncFail);
      dropToIdle();
      notify(Notice::SyncLost);
    }
  }
}

void SessionState::tickTxn(const uint32_t now) {
  if (txn_ == Txn::None || txnNeedsSend_ || !reached(now, txnDeadlineMs_)) return;
  if (txnAttempts_ < Config::CONTROL_MAX_RETRIES) {
    txnNeedsSend_ = true;
    return;
  }
  const Txn failed = txn_;
  txn_ = Txn::None;
  if (failed == Txn::Abort) {
    // The user asked to stop - never leave them stuck on an unresponsive peer.
    logSessionEvent(LogEventKind::ManualStop);
    dropToIdle();
    suppressActive_ = true;
    suppressUntilMs_ = now + Config::ABORT_SUPPRESS_MS;
    notify(Notice::AbortNoAck);
  } else {
    notify(Notice::NoResponse); // stay in SYNC
  }
}

// ============================================================================
// Burst + slot engine
// ============================================================================

void SessionState::startRun(const RunMode mode, const uint8_t profile, const uint32_t now) {
  runMode_ = mode;
  state_ = mode == RunMode::Meas ? DeviceState::Measuring : DeviceState::Testing;
  profile_ = mode == RunMode::Meas ? 1 : profile;
  burstSize_ = mode == RunMode::Meas ? Config::MEAS_PACKETS_PER_BURST : Config::TEST_PACKETS_PER_BURST;
  missedSlots_ = 0;
  localRequest_ = SlotRequest::None;
  desiredProfile_ = 0;
  lastBurstRecv_ = 0xFF;
  lastBurstHasSignal_ = false;
  testPaused_ = false;
  replyPending_ = false;
  nextBurstSkipped_ = kIsBase && mode == RunMode::Test && !budgetAllowsBurst(profile_);
  notify(mode == RunMode::Meas ? Notice::MeasStarted : Notice::TestStarted);
  SLOG_I("run", "start %s profile=%u", mode == RunMode::Meas ? "MEAS" : "TEST", profile_);
  beginCycle(now);
}

void SessionState::beginCycle(const uint32_t startMs) {
  runPhase_ = RunPhase::Burst;
  cycleStartMs_ = startMs;
  const bool test = runMode_ == RunMode::Test;
  scheduledSlotStartMs_ = startMs + Airtime::burstWindowMs(profile_, burstSize_, test);
  slotStartMs_ = scheduledSlotStartMs_;
  endMarkerInFlight_ = false;
  roverIrqInFlight_ = false;
  lastGapTried_ = -1;
  baseInGap_ = false;
  if (kIsBase && test && nextBurstSkipped_) {
    // Budget pause: nothing to send, so just listen on the service channel the whole window.
    radio_.useServiceChannel();
    radio_.startReceive();
    baseInGap_ = true;
  } else {
    radio_.useBenchConfig(profile_);
  }

  txSent_ = 0;
  nextTxAtMs_ = startMs + Config::BURST_LEAD_MS;
  benchTxInFlight_ = false;
  txTimeSumUs_ = 0;
  txTimeCount_ = 0;
  burstSkipped_ = kIsBase && nextBurstSkipped_;

  for (bool &seen : seqSeen_) seen = false;
  rxCount_ = 0;
  rxRssiSum_ = 0;
  rxSnrSum_ = 0;
  if (!kIsBase) radio_.startReceive();
}

void SessionState::tickRun(const uint32_t now) {
  if (runPhase_ == RunPhase::Burst) {
    if (reached(now, slotStartMs_)) {
      if (radio_.isTransmitting()) return; // never switch channel mid-packet
      enterSlot(now);
      return;
    }
    if (kIsBase) {
      tickBurstBase(now);
    } else if (runMode_ == RunMode::Test) {
      tickRoverInterrupt(now);
    }
    return;
  }
  // Slot phase: end of slot == start of the next cycle.
  if (reached(now, nextCycleStartMs_) && !radio_.isTransmitting()) finalizeSlot(now);
}

bool SessionState::testInterruptWanted() const {
  return runMode_ == RunMode::Test && (desiredProfile_ != 0 || localRequest_ != SlotRequest::None);
}

void SessionState::tickBurstBase(const uint32_t now) {
  if (burstSkipped_ || benchTxInFlight_ || radio_.isTransmitting()) return;
  const bool test = runMode_ == RunMode::Test;
  // TEST uses a fixed, predictable packet schedule (the Rover aims its early report into the
  // gaps); MEAS just keeps the guard gap after each TX-done.
  const uint32_t txAt = test ? cycleStartMs_ + Config::BURST_LEAD_MS + txSent_ * Airtime::packetPeriodMs(profile_, true)
                             : nextTxAtMs_;
  if (txSent_ >= burstSize_) return; // after the last packet: keep listening until the slot
  if (test && baseInGap_) {
    if (!reached(now, txAt - Config::TEST_GAP_SWITCH_MARGIN_MS)) return;
    radio_.useBenchConfig(profile_);
    baseInGap_ = false;
  }
  if (!reached(now, txAt)) return;
  // Don't start a packet that can't finish inside the window.
  if (!reached(scheduledSlotStartMs_, now + Airtime::benchPacketMs(profile_))) return;

  Protocol::BenchPacket pkt{};
  pkt.configIndex = profile_;
  if (testInterruptWanted()) {
    // TEST: the operator on the Base wants out of this burst - send the end marker instead of
    // the next packet; on TX-done we go straight to the slot (see update()).
    pkt.seqNum = Protocol::BURST_END_SEQ;
    uint8_t raw[Protocol::BENCH_PACKET_SIZE];
    Protocol::encodeBenchPacket(pkt, raw);
    if (radio_.startTransmit(raw, sizeof(raw))) {
      benchTxInFlight_ = true;
      endMarkerInFlight_ = true;
    }
    return;
  }

  pkt.seqNum = txSent_;
  Protocol::fillBenchPayload(pkt, static_cast<uint8_t>(txSent_ * 7));
  uint8_t raw[Protocol::BENCH_PACKET_SIZE];
  Protocol::encodeBenchPacket(pkt, raw);

  if (radio_.startTransmit(raw, sizeof(raw))) {
    LogRecord rec{};
    rec.sessionId = sessionId_;
    rec.direction = 0;
    rec.type = static_cast<uint8_t>(LogRecordType::Bench);
    rec.configIndex = profile_;
    rec.sessionTimeMs = sessionElapsedMs();
    rec.size = Protocol::BENCH_PACKET_SIZE;
    rec.rssi = FlashLog::NO_RSSI;
    rec.snrTenths = FlashLog::NO_SNR;
    rec.deviceStatus = static_cast<uint8_t>(state_);
    rec.received = 1;
    pendingTxRecord_ = rec;
    benchTxInFlight_ = true;
  } else {
    if (runMode_ == RunMode::Meas) logBench(0, false, 0, 0, 0);
    nextTxAtMs_ = now + Config::BURST_GUARD_MS;
  }
  txSent_++;
}

void SessionState::tickRoverInterrupt(const uint32_t now) {
  if (roverIrqInFlight_) {
    if (reached(now, roverIrqWaitUntilMs_)) {
      // The Base didn't hear us in that gap - resume listening, try the next gap.
      roverIrqInFlight_ = false;
      radio_.useBenchConfig(profile_);
      radio_.startReceive();
    }
    return;
  }
  if (!testInterruptWanted()) return;

  // Predicted gaps: packet k ends at cycleStart + lead + k*period + airtime.
  const uint32_t period = Airtime::packetPeriodMs(profile_, true);
  const uint32_t firstGap = cycleStartMs_ + Config::BURST_LEAD_MS + Airtime::benchPacketMs(profile_) +
                            Config::TEST_GAP_TX_OFFSET_MS;
  if (!reached(now, firstGap)) return;
  const auto k = static_cast<int16_t>((now - firstGap) / period);
  if (k >= burstSize_ || k == lastGapTried_) return;
  const uint32_t gapAt = firstGap + k * period;
  if (now - gapAt > 60) return; // missed the start of this gap, wait for the next one
  if (!reached(scheduledSlotStartMs_, now + Config::TEST_GAP_REPLY_WAIT_MS)) return; // normal slot is near

  lastGapTried_ = k;
  radio_.useServiceChannel();
  SlotReport r;
  r.profile = profile_;
  r.desiredProfile = desiredProfile_;
  r.received = rxCount_;
  r.request = localRequest_;
  if (sendService(Protocol::encodeSlotReport(r))) {
    desiredAtReport_ = r.desiredProfile;
    roverIrqInFlight_ = true;
    roverIrqWaitUntilMs_ = now + Config::TEST_GAP_REPLY_WAIT_MS;
  } else {
    radio_.useBenchConfig(profile_);
    radio_.startReceive();
  }
}

void SessionState::pollBurstRover(const uint32_t now) {
  uint8_t buf[Protocol::BENCH_PACKET_SIZE];
  size_t len = 0;
  float rssi = 0, snr = 0;
  if (!radio_.poll(buf, sizeof(buf), len, rssi, snr)) return;
  Protocol::BenchPacket pkt{};
  if (!Protocol::decodeBenchPacket(buf, len, pkt)) return;
  if (pkt.configIndex == profile_ && pkt.seqNum == Protocol::BURST_END_SEQ && runMode_ == RunMode::Test) {
    enterSlot(now); // the Base cut the burst short - report right away
    return;
  }
  if (pkt.configIndex != profile_ || pkt.seqNum >= burstSize_ || seqSeen_[pkt.seqNum]) return;
  seqSeen_[pkt.seqNum] = true;
  rxCount_++;
  rxRssiSum_ += rssi;
  rxSnrSum_ += snr;
  if (runMode_ == RunMode::Meas) {
    lastBurstHasSignal_ = true;
    lastBurstRssi_ = rssi;
    lastBurstSnr_ = snr;
    logBench(1, true, rssi, snr, 0);
  }
}

void SessionState::enterSlot(const uint32_t now) {
  // MEAS: one record per bench packet, no gaps - log what the Rover never saw as lost.
  if (!kIsBase && runMode_ == RunMode::Meas) {
    for (uint8_t i = 0; i < burstSize_; i++) {
      if (!seqSeen_[i]) logBench(1, false, 0, 0, 0);
    }
  }
  // How many packets the Base had sent by now (Rover's view, from the fixed TEST schedule).
  if (runMode_ == RunMode::Test && !kIsBase) {
    const uint32_t period = Airtime::packetPeriodMs(profile_, true);
    const uint32_t firstEnd = cycleStartMs_ + Config::BURST_LEAD_MS + Airtime::benchPacketMs(profile_);
    uint8_t sent = 0;
    while (sent < burstSize_ && reached(now + 20, firstEnd + sent * period)) sent++;
    slotSentEstimate_ = sent > rxCount_ ? sent : rxCount_;
  } else {
    slotSentEstimate_ = burstSize_;
  }
  runPhase_ = RunPhase::Slot;
  slotStartMs_ = now;
  // Default end of slot is tied to the *scheduled* window so both sides agree even if one of
  // them entered early; a successful TEST exchange pulls it earlier (onSlotReport()).
  nextCycleStartMs_ = scheduledSlotStartMs_ + Config::SLOT_DURATION_MS;
  if (static_cast<int32_t>(nextCycleStartMs_ - (now + Config::SLOT_DURATION_MS)) < 0 && runMode_ == RunMode::Meas) {
    nextCycleStartMs_ = now + Config::SLOT_DURATION_MS;
  }
  baseInGap_ = false;
  roverIrqInFlight_ = false;
  reportsSent_ = 0;
  replyReceived_ = false;
  decided_ = false;
  reportReceived_ = false;
  replyPending_ = false;
  radio_.useServiceChannel();
  radio_.startReceive();
}

SlotReply SessionState::decide(const bool haveReport, const SlotReport &report) const {
  SlotReply d;
  d.lastSkipped = burstSkipped_;
  const SlotRequest peerReq = haveReport ? report.request : SlotRequest::None;
  // Without a report, queued local requests wait for the next slot the peer can hear.
  const SlotRequest ownReq = haveReport ? localRequest_ : SlotRequest::None;

  if (ownReq == SlotRequest::Abort || peerReq == SlotRequest::Abort) {
    d.decision = SlotDecision::Abort;
  } else if (ownReq == SlotRequest::Stop || peerReq == SlotRequest::Stop) {
    d.decision = SlotDecision::Stop;
  } else if (runMode_ == RunMode::Meas && profile_ >= Config::BENCH_CONFIG_COUNT) {
    d.decision = SlotDecision::Done;
  } else {
    d.decision = SlotDecision::Next;
  }

  if (runMode_ == RunMode::Meas) {
    d.nextProfile = profile_ < Config::BENCH_CONFIG_COUNT ? profile_ + 1 : profile_;
  } else if (haveReport && desiredProfile_ != 0) {
    d.nextProfile = desiredProfile_; // both turned the knob: Base wins
  } else if (haveReport && report.desiredProfile >= 1 && report.desiredProfile <= Config::BENCH_CONFIG_COUNT) {
    d.nextProfile = report.desiredProfile;
  } else {
    d.nextProfile = profile_;
  }
  d.nextSkipped = runMode_ == RunMode::Test && d.decision == SlotDecision::Next && !budgetAllowsBurst(d.nextProfile);
  return d;
}

void SessionState::finalizeSlot(const uint32_t now) {
  SlotReply d;
  if (kIsBase) {
    if (decided_) {
      d = decision_;
      missedSlots_ = 0;
    } else {
      missedSlots_++;
      d = decide(false, lastReport_);
    }
    if (runMode_ == RunMode::Test) {
      const uint32_t avgTx = txTimeCount_ ? txTimeSumUs_ / txTimeCount_ : 0;
      if (!burstSkipped_) {
        logTestSummary(0, txSent_, reportReceived_ ? lastReport_.received : 0xFF, false, 0, 0, avgTx);
        lastBurstSent_ = txSent_;
      }
      testPaused_ = d.nextSkipped;
    }
  } else {
    if (replyReceived_) {
      d = decision_;
      missedSlots_ = 0;
    } else {
      missedSlots_++;
      d.decision = runMode_ == RunMode::Meas && profile_ >= Config::BENCH_CONFIG_COUNT ? SlotDecision::Done
                                                                                      : SlotDecision::Next;
      d.nextProfile = runMode_ == RunMode::Meas ? wrapProfile(profile_ + 1) : profile_;
    }
  }

  SLOG_D("run", "slot p=%u dec=%u next=%u miss=%u rx=%u", profile_, static_cast<unsigned>(d.decision),
         d.nextProfile, missedSlots_, kIsBase ? lastReport_.received : rxCount_);
  if (missedSlots_ >= Config::SLOT_MISS_LIMIT && d.decision == SlotDecision::Next) {
    endRun(localRequest_ == SlotRequest::Abort ? RunEnd::Abort : RunEnd::LinkLost, now);
    return;
  }
  applyDecision(d, now);
}

void SessionState::applyDecision(const SlotReply &d, const uint32_t now) {
  if (kIsBase && d.decision != SlotDecision::Next) {
    // Keep answering the Rover's duplicate reports with the same final word.
    lingerReply_ = d;
    lingerSlotReply_ = true;
    lingerUntilMs_ = now + Config::LINGER_MS;
  }
  switch (d.decision) {
    case SlotDecision::Done:
      endRun(RunEnd::Done, now);
      return;
    case SlotDecision::Stop:
      endRun(RunEnd::Stop, now);
      return;
    case SlotDecision::Abort:
      endRun(RunEnd::Abort, now);
      return;
    case SlotDecision::Next:
      break;
  }
  if (d.nextProfile >= 1 && d.nextProfile <= Config::BENCH_CONFIG_COUNT) {
    if (runMode_ == RunMode::Test) {
      // Clear only the choice that was actually exchanged in this slot; a turn made after the
      // report/decision survives and interrupts the next burst. Without a
      // completed exchange the choice stays queued.
      const bool exchanged = kIsBase ? decided_ : replyReceived_;
      const uint8_t used = kIsBase ? desiredAtDecision_ : desiredAtReport_;
      if (exchanged && desiredProfile_ == used) desiredProfile_ = 0;
    }
    profile_ = d.nextProfile;
    if (desiredProfile_ == profile_) desiredProfile_ = 0;
  }
  nextBurstSkipped_ = d.nextSkipped;
  beginCycle(nextCycleStartMs_);
}

void SessionState::endRun(const RunEnd how, const uint32_t now) {
  const bool wasMeas = runMode_ == RunMode::Meas;
  const bool localAbort = localRequest_ == SlotRequest::Abort;
  SLOG_I("run", "end how=%u", static_cast<unsigned>(how));
  switch (how) {
    case RunEnd::Done:
      logSessionEvent(LogEventKind::MeasureDone);
      returnToSynced(true, now);
      notify(Notice::MeasDone);
      break;
    case RunEnd::Stop:
      if (wasMeas) logSessionEvent(LogEventKind::MeasStopped);
      returnToSynced(wasMeas, now);
      notify(wasMeas ? Notice::MeasStopped : Notice::TestStopped);
      break;
    case RunEnd::LinkLost:
      if (wasMeas) logSessionEvent(LogEventKind::MeasStopped);
      returnToSynced(wasMeas, now);
      notify(wasMeas ? Notice::MeasLinkLost : Notice::TestLinkLost);
      break;
    case RunEnd::Abort:
      logSessionEvent(LogEventKind::ManualStop);
      dropToIdle();
      suppressActive_ = true;
      suppressUntilMs_ = now + Config::ABORT_SUPPRESS_MS;
      notify(localAbort ? Notice::AbortDone : Notice::AbortByPeer);
      break;
  }
}
