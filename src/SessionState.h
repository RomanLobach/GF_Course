// Core protocol state machine.
//
// Owned by and only ever driven from the radio/protocol core (main loop()). Never touched
// from the UI tasks: the menu reads it on the same core and forwards display data through
// DisplaySnapshot.
//
// Shape of the implementation (read before changing anything):
//  - One TX arbiter (serviceTx()) owns every service-channel transmit, in strict priority:
//    reactive reply > transaction request > slot report > heartbeat/search. Nothing else
//    calls radio_.startTransmit() on the service channel, so packets never clobber each
//    other.
//  - Every request is idempotent on the receiving side: a duplicate always gets the same
//    answer again (re-ACK, linger replies).
//  - SYNC heartbeats are slotted by role: Base on its own absolute schedule, Rover T_HB/2
//    after each Base heartbeat it hears.
//  - MEAS and TEST share one "burst + slot" engine: bursts on the
//    profile, all control in a slot on the service channel, Base decides, Rover reports.
#pragma once

#include <Arduino.h>
#include "Config.h"
#include "Protocol.h"
#include "RadioManager.h"
#include "FlashLog.h"
#include "ConfigStore.h"

class SessionState {
public:
  // Things the operator should be told about via a popup (consumed by MenuController).
  enum class Notice : uint8_t {
    Synced,
    SearchTimeout,
    SyncLost,
    AbortDone,
    AbortNoAck,
    AbortByPeer,
    NoResponse,
    PeerBusy,
    PeerBudget,
    PeerLogFull,
    MeasStarted,
    MeasDone,
    MeasStopped,
    MeasLinkLost,
    TestStarted,
    TestStopped,
    TestLinkLost,
  };

  enum class StartResult : uint8_t { Sent, NotSynced, Busy, Budget, LogFull };
  enum class AbortResult : uint8_t { NotApplicable, Stopped, Pending };

  // Read-only view of the running MEAS/TEST cycle for the display.
  struct RunView {
    uint8_t profile = 0;        // 1..6, 0 = no run
    uint8_t desiredProfile = 0; // TEST: profile chosen with the encoder, not applied yet (0 = none)
    uint8_t burstSize = 0;
    uint8_t burstTx = 0;        // Base: packets sent in the current burst
    uint8_t burstRx = 0;        // Rover: packets received in the current burst
    uint8_t lastRecv = 0xFF;    // received count of the last finished burst (0xFF = unknown)
    uint8_t lastSent = 0;       // packets actually sent in that burst (TEST bursts can be cut short)
    bool paused = false;        // TEST: burst skipped because of the duty-cycle budget
    bool hasSignal = false;     // Rover: RSSI/SNR below are valid
    float rssi = 0;
    float snr = 0;
  };

  SessionState(RadioManager &radio, FlashLog &log, ConfigStore &config);

  void begin();
  void update();

  // ---- operator actions (menu) ----
  bool requestStartSearching();          // IDLE only
  AbortResult requestAbort();            // "Синхронізація [завершити]"
  StartResult requestStartMeasurement(); // SYNC, either role
  StartResult requestStartTest();        // SYNC, either role
  bool requestStopRun();                 // "Вимірювання [зупинити]" / "Тест частот [завершити]"
  void requestTestProfileDelta(int8_t delta);
  void resetSessionId();                 // browser only, IDLE only

  bool popNotice(Notice &out);
  uint32_t lastBudgetWaitMinutes() const { return lastBudgetWaitMin_; }

  // ---- state for the UI ----
  Protocol::DeviceState state() const { return state_; }
  uint16_t sessionId() const { return sessionId_; }
  uint32_t sessionElapsedMs() const;
  float rssi() const { return lastRssi_; }
  float snr() const { return lastSnr_; }
  bool isBusy() const; // drives the blinking "pending" dot
  bool isStartPending() const { return txn_ != Txn::None || scheduledMode_ != RunMode::None; }
  bool isTimeCritical() const; // inside a burst window - FlashLog defers flash writes
  RunView runView() const;

private:
  enum class Txn : uint8_t { None, MeasStart, TestStart, Abort };
  enum class RunMode : uint8_t { None, Meas, Test };
  enum class RunPhase : uint8_t { Burst, Slot };
  enum class RunEnd : uint8_t { Done, Stop, LinkLost, Abort };

  RadioManager &radio_;
  FlashLog &log_;
  ConfigStore &config_;

  Protocol::DeviceState state_ = Protocol::DeviceState::Idle;
  uint16_t sessionId_ = 0;     // last used session id, persisted through ConfigStore
  uint32_t syncedAtMs_ = 0;    // local session timer reference
  uint32_t enteredSyncMs_ = 0; // for the duplicate-SEEN_YOU grace window
  float lastRssi_ = 0;
  float lastSnr_ = 0;

  // --- search / heartbeat ---
  uint32_t searchStartMs_ = 0;
  uint32_t nextSearchTxMs_ = 0;
  uint32_t nextHbMs_ = 0;
  uint32_t peerDeadlineMs_ = 0;
  uint8_t missedHb_ = 0;
  uint32_t suppressUntilMs_ = 0; // IDLE auto-responder off until then (after a manual abort)
  bool suppressActive_ = false;

  // --- reactive reply (highest TX priority) ---
  bool replyPending_ = false;
  Protocol::ServicePacket reply_{};
  bool replyIsStartAck_ = false; // value is filled in at send time (ms until scheduled start)
  bool replyIsSlotReply_ = false; // value is filled in at send time (ms until next cycle)
  uint32_t replyQueuedMs_ = 0;

  // --- outgoing transaction (initiator side) ---
  Txn txn_ = Txn::None;
  bool txnNeedsSend_ = false;
  uint8_t txnAttempts_ = 0;
  uint32_t txnDeadlineMs_ = 0;

  // --- scheduled start (both sides) ---
  RunMode scheduledMode_ = RunMode::None;
  uint32_t scheduledStartMs_ = 0;
  uint8_t scheduledProfile_ = 1;
  bool commitResponder_ = false;

  // --- linger: keep answering duplicates after a final decision ---
  uint32_t lingerUntilMs_ = 0;
  bool lingerSlotReply_ = false;
  Protocol::SlotReply lingerReply_{};

  // --- burst + slot engine ---
  RunMode runMode_ = RunMode::None;
  RunPhase runPhase_ = RunPhase::Burst;
  uint8_t profile_ = 1;
  uint8_t burstSize_ = 0;
  uint32_t cycleStartMs_ = 0;
  uint32_t slotStartMs_ = 0;          // actual slot entry (can be early in TEST)
  uint32_t scheduledSlotStartMs_ = 0; // end of the burst window as both sides compute it
  uint32_t nextCycleStartMs_ = 0;
  uint8_t missedSlots_ = 0;
  Protocol::SlotRequest localRequest_ = Protocol::SlotRequest::None;
  uint8_t desiredProfile_ = 0;
  bool burstSkipped_ = false;     // Base: the current burst is not being transmitted
  bool nextBurstSkipped_ = false; // decided in the slot for the next burst
  // Base, burst
  uint8_t txSent_ = 0;
  uint32_t nextTxAtMs_ = 0;
  bool benchTxInFlight_ = false;
  bool endMarkerInFlight_ = false; // TEST: end-of-burst marker on air instead of a packet
  bool baseInGap_ = false;         // TEST: listening on the service channel between packets
  LogRecord pendingTxRecord_{};
  uint32_t txTimeSumUs_ = 0;
  uint8_t txTimeCount_ = 0;
  // Rover, burst
  bool roverIrqInFlight_ = false;  // TEST: early SLOT_REPORT sent in a gap, waiting for the reply
  uint32_t roverIrqWaitUntilMs_ = 0;
  int16_t lastGapTried_ = -1;
  uint8_t slotSentEstimate_ = 0;   // packets the Base had sent when this slot began
  bool seqSeen_[Config::MAX_PACKETS_PER_BURST] = {};
  uint8_t rxCount_ = 0;
  float rxRssiSum_ = 0;
  float rxSnrSum_ = 0;
  // slot
  uint8_t reportsSent_ = 0;
  bool replyReceived_ = false;
  bool decided_ = false;
  bool reportReceived_ = false;
  Protocol::SlotReport lastReport_{};
  Protocol::SlotReply decision_{};
  // Encoder choice at the moment it was reported (Rover) / used in a decision (Base). Only that
  // value is cleared once applied - a newer turn made meanwhile survives and wins next.
  uint8_t desiredAtReport_ = 0;
  uint8_t desiredAtDecision_ = 0;
  uint8_t lastBurstSent_ = 0;
  // display
  uint8_t lastBurstRecv_ = 0xFF;
  bool testPaused_ = false;
  bool lastBurstHasSignal_ = false;
  float lastBurstRssi_ = 0;
  float lastBurstSnr_ = 0;

  // --- notices ---
  static constexpr uint8_t NOTICE_CAPACITY = 8;
  Notice notices_[NOTICE_CAPACITY]{};
  uint8_t noticeHead_ = 0;
  uint8_t noticeTail_ = 0;
  uint32_t lastBudgetWaitMin_ = 0;

  // helpers
  static bool reached(uint32_t now, uint32_t t) { return static_cast<int32_t>(now - t) >= 0; }
  void notify(Notice n);
  void loadSessionId();
  void saveSessionId();
  static uint16_t nextId(uint16_t id);
  void logSessionEvent(LogEventKind kind);

  // state transitions
  void dropToIdle();
  void enterSearching(uint32_t now);
  void enterSeenYou(uint32_t now);
  void enterSyncedViaHandshake(uint16_t id, uint32_t now);
  void returnToSynced(bool newSession, uint32_t now);
  void armSyncedHeartbeat(uint32_t now);
  void cancelPendingWork();

  // TX
  void queueReply(const Protocol::ServicePacket &pkt, uint32_t now);
  void queueHeartbeatReply(Protocol::HbState hs, uint32_t now);
  bool sendService(const Protocol::ServicePacket &pkt);
  void serviceTx(uint32_t now);

  // RX
  void pollService(uint32_t now);
  void handleService(const Protocol::ServicePacket &pkt, uint32_t now);
  void onHeartbeat(Protocol::HbState hs, uint16_t id, uint32_t now);
  void onStartReq(RunMode mode, uint8_t profile, uint32_t now);
  void onStartAck(RunMode mode, uint8_t profile, uint16_t msToStart, uint32_t now);
  void onSlotReport(const Protocol::SlotReport &r, uint32_t now);
  void onSlotReply(const Protocol::SlotReply &r, uint32_t now);
  void onAbortReq(uint32_t now);

  // per-state ticks
  void tickSearch(uint32_t now);
  void tickSynced(uint32_t now);
  void tickTxn(uint32_t now);
  void tickRun(uint32_t now);

  // run engine
  bool baseHasBudgetForMeas(uint32_t &waitMinutes) const;
  bool budgetAllowsBurst(uint8_t profile) const;
  void startRun(RunMode mode, uint8_t profile, uint32_t now);
  void beginCycle(uint32_t startMs);
  void tickBurstBase(uint32_t now);
  void pollBurstRover(uint32_t now);
  void tickRoverInterrupt(uint32_t now);
  bool testInterruptWanted() const;
  void enterSlot(uint32_t now);
  Protocol::SlotReply decide(bool haveReport, const Protocol::SlotReport &report) const;
  void finalizeSlot(uint32_t now);
  void applyDecision(const Protocol::SlotReply &d, uint32_t now);
  void endRun(RunEnd how, uint32_t now);
  void logBench(uint8_t direction, bool ok, float rssi, float snr, uint32_t txTimeUs);
  void logTestSummary(uint8_t direction, uint8_t sent, uint8_t recv, bool hasSignal, float rssi, float snr,
                      uint32_t txTimeUs);
};
