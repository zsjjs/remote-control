#include <Arduino.h>
#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>

#include <array>
#include <cctype>
#include <cstring>
#include <string>

#include "local_config.h"

namespace {

constexpr int kPinOpen = 25;
constexpr int kPinClose = 26;
constexpr uint8_t kRelayActiveLevel = LOW;
constexpr uint8_t kRelayIdleLevel = HIGH;
constexpr uint32_t kRelayPulseMs = 500;
constexpr uint32_t kDuplicateIgnoreMs = 3500;
constexpr uint32_t kScanRetryDelayMs = 1000;
constexpr uint16_t kScanIntervalMs = 100;
constexpr uint16_t kScanWindowMs = 100;
constexpr size_t kUuidStringLength = 36;
constexpr size_t kUuidByteCount = 16;

constexpr uint16_t kMajorOpen = 1;
constexpr uint16_t kMajorClose = 2;
constexpr size_t kIBeaconDataSize = 25;
constexpr uint8_t kAppleCompanyIdLowByte = 0x4C;
constexpr uint8_t kAppleCompanyIdHighByte = 0x00;
constexpr uint8_t kIBeaconType = 0x02;
constexpr uint8_t kIBeaconTypeLength = 0x15;
constexpr size_t kRecentCommandCapacity = 4;

enum class RelayState : uint8_t {
  Idle,
  PendingOpen,
  PendingClose,
  PulsingOpen,
  PulsingClose,
};

enum class CommandAction : uint8_t {
  None,
  Open,
  Close,
};

struct RecentCommand {
  bool valid = false;
  uint16_t major = 0;
  uint16_t minor = 0;
  uint32_t timestampMs = 0;
};

BLEScan *g_bleScan = nullptr;

portMUX_TYPE g_stateMutex = portMUX_INITIALIZER_UNLOCKED;
RelayState g_relayState = RelayState::Idle;
uint32_t g_pulseStartedAt = 0;
bool g_scanStarted = false;
uint32_t g_nextScanRetryAt = 0;
std::array<RecentCommand, kRecentCommandCapacity> g_recentCommands;
std::array<uint8_t, kUuidByteCount> g_netIdBytes;

uint16_t readBigEndianU16(const uint8_t *bytes) {
  return (static_cast<uint16_t>(bytes[0]) << 8) | bytes[1];
}

int hexDigitToValue(char digit) {
  if (digit >= '0' && digit <= '9') {
    return digit - '0';
  }

  const char lowerDigit = static_cast<char>(std::tolower(digit));
  if (lowerDigit >= 'a' && lowerDigit <= 'f') {
    return lowerDigit - 'a' + 10;
  }

  return -1;
}

bool parseNetIdUuid(const char *uuid, std::array<uint8_t, kUuidByteCount> *out) {
  if (std::strlen(uuid) != kUuidStringLength) {
    return false;
  }

  size_t byteIndex = 0;
  for (size_t i = 0; i < kUuidStringLength;) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (uuid[i] != '-') {
        return false;
      }
      ++i;
      continue;
    }

    const int highNibble = hexDigitToValue(uuid[i]);
    const int lowNibble = hexDigitToValue(uuid[i + 1]);
    if (highNibble < 0 || lowNibble < 0 || byteIndex >= out->size()) {
      return false;
    }

    (*out)[byteIndex] =
        static_cast<uint8_t>((highNibble << 4) | lowNibble);
    ++byteIndex;
    i += 2;
  }

  return byteIndex == out->size();
}

const char *actionToString(CommandAction action) {
  switch (action) {
    case CommandAction::Open:
      return "open";
    case CommandAction::Close:
      return "close";
    case CommandAction::None:
    default:
      return "none";
  }
}

void setRelayIdle() {
  digitalWrite(kPinOpen, kRelayIdleLevel);
  digitalWrite(kPinClose, kRelayIdleLevel);
}

RelayState getRelayState() {
  portENTER_CRITICAL(&g_stateMutex);
  const RelayState currentState = g_relayState;
  portEXIT_CRITICAL(&g_stateMutex);
  return currentState;
}

bool scheduleRelayAction(CommandAction action) {
  bool accepted = false;

  portENTER_CRITICAL(&g_stateMutex);
  if (g_relayState == RelayState::Idle) {
    g_relayState = (action == CommandAction::Open) ? RelayState::PendingOpen
                                                   : RelayState::PendingClose;
    accepted = true;
  }
  portEXIT_CRITICAL(&g_stateMutex);

  return accepted;
}

bool isDuplicateCommand(uint16_t major, uint16_t minor, uint32_t nowMs) {
  bool duplicate = false;

  portENTER_CRITICAL(&g_stateMutex);
  for (const RecentCommand &entry : g_recentCommands) {
    if (!entry.valid) {
      continue;
    }

    if (entry.major != major || entry.minor != minor) {
      continue;
    }

    if (nowMs - entry.timestampMs <= kDuplicateIgnoreMs) {
      duplicate = true;
      break;
    }
  }
  portEXIT_CRITICAL(&g_stateMutex);

  return duplicate;
}

void rememberCommand(uint16_t major, uint16_t minor, uint32_t nowMs) {
  portENTER_CRITICAL(&g_stateMutex);

  size_t replaceIndex = 0;
  uint32_t oldestAge = 0;
  bool foundReplaceSlot = false;

  for (size_t i = 0; i < g_recentCommands.size(); ++i) {
    RecentCommand &entry = g_recentCommands[i];
    if (!entry.valid) {
      replaceIndex = i;
      foundReplaceSlot = true;
      break;
    }

    const uint32_t age = nowMs - entry.timestampMs;
    if (age > kDuplicateIgnoreMs) {
      replaceIndex = i;
      foundReplaceSlot = true;
      break;
    }

    if (!foundReplaceSlot || age > oldestAge) {
      oldestAge = age;
      replaceIndex = i;
      foundReplaceSlot = true;
    }
  }

  RecentCommand recent;
  recent.valid = true;
  recent.major = major;
  recent.minor = minor;
  recent.timestampMs = nowMs;
  g_recentCommands[replaceIndex] = recent;

  portEXIT_CRITICAL(&g_stateMutex);
}

CommandAction actionFromMajor(uint16_t major) {
  if (major == kMajorOpen) {
    return CommandAction::Open;
  }

  if (major == kMajorClose) {
    return CommandAction::Close;
  }

  return CommandAction::None;
}

bool parseTargetBeacon(BLEAdvertisedDevice &device, uint16_t *major,
                       uint16_t *minor) {
  if (!device.haveManufacturerData()) {
    return false;
  }

  const std::string manufacturerData = device.getManufacturerData();
  if (manufacturerData.size() != kIBeaconDataSize) {
    return false;
  }

  const uint8_t *bytes =
      reinterpret_cast<const uint8_t *>(manufacturerData.data());

  if (bytes[0] != kAppleCompanyIdLowByte ||
      bytes[1] != kAppleCompanyIdHighByte || bytes[2] != kIBeaconType ||
      bytes[3] != kIBeaconTypeLength) {
    return false;
  }

  if (std::memcmp(bytes + 4, g_netIdBytes.data(), g_netIdBytes.size()) != 0) {
    return false;
  }

  *major = readBigEndianU16(bytes + 20);
  *minor = readBigEndianU16(bytes + 22);
  return true;
}

class BeaconCallbacks : public BLEAdvertisedDeviceCallbacks {
 public:
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    uint16_t major = 0;
    uint16_t minor = 0;
    if (!parseTargetBeacon(advertisedDevice, &major, &minor)) {
      return;
    }

    const CommandAction action = actionFromMajor(major);
    if (action == CommandAction::None) {
      Serial.printf(
          "Ignoring matched netId beacon with unsupported major=%u minor=%u "
          "rssi=%d\n",
          major, minor, advertisedDevice.getRSSI());
      return;
    }

    const uint32_t nowMs = millis();
    if (isDuplicateCommand(major, minor, nowMs)) {
      return;
    }

    rememberCommand(major, minor, nowMs);

    if (!scheduleRelayAction(action)) {
      Serial.printf(
          "Ignoring beacon while relay busy: netId=%s major=%u minor=%u "
          "action=%s rssi=%d\n",
          kNetIdUuid, major, minor, actionToString(action),
          advertisedDevice.getRSSI());
      return;
    }

    Serial.printf(
        "Accepted beacon: netId=%s major=%u minor=%u action=%s rssi=%d\n",
        kNetIdUuid, major, minor, actionToString(action),
        advertisedDevice.getRSSI());
  }
};

void beginPulse(RelayState state) {
  setRelayIdle();

  if (state == RelayState::PendingOpen) {
    digitalWrite(kPinOpen, kRelayActiveLevel);
    Serial.println("Executing open pulse on GPIO25");
  } else if (state == RelayState::PendingClose) {
    digitalWrite(kPinClose, kRelayActiveLevel);
    Serial.println("Executing close pulse on GPIO26");
  } else {
    return;
  }

  portENTER_CRITICAL(&g_stateMutex);
  if (g_relayState == RelayState::PendingOpen && state == RelayState::PendingOpen) {
    g_relayState = RelayState::PulsingOpen;
    g_pulseStartedAt = millis();
  } else if (g_relayState == RelayState::PendingClose &&
             state == RelayState::PendingClose) {
    g_relayState = RelayState::PulsingClose;
    g_pulseStartedAt = millis();
  }
  portEXIT_CRITICAL(&g_stateMutex);
}

void finishPulse(RelayState state) {
  setRelayIdle();

  if (state == RelayState::PulsingOpen) {
    Serial.println("Open pulse complete");
  } else if (state == RelayState::PulsingClose) {
    Serial.println("Close pulse complete");
  }

  portENTER_CRITICAL(&g_stateMutex);
  if (g_relayState == state) {
    g_relayState = RelayState::Idle;
    g_pulseStartedAt = 0;
  }
  portEXIT_CRITICAL(&g_stateMutex);
}

bool startBeaconScan() {
  if (g_bleScan == nullptr) {
    g_bleScan = BLEDevice::getScan();
    g_bleScan->setAdvertisedDeviceCallbacks(new BeaconCallbacks(), true, true);
    g_bleScan->setActiveScan(false);
    g_bleScan->setInterval(kScanIntervalMs);
    g_bleScan->setWindow(kScanWindowMs);
  }

  const bool started = g_bleScan->start(0, nullptr, false);
  if (started) {
    Serial.println("BLE passive scan started");
  } else {
    Serial.println("BLE passive scan failed to start");
  }
  return started;
}

void setupBleScanner() {
  BLEDevice::init("");

  if (!parseNetIdUuid(kNetIdUuid, &g_netIdBytes)) {
    Serial.printf("Invalid netId UUID in local_config.h: %s\n", kNetIdUuid);
    return;
  }

  Serial.printf("Configured netId UUID: %s\n", kNetIdUuid);
  Serial.printf("Command mapping: major %u=open, major %u=close\n", kMajorOpen,
                kMajorClose);
  Serial.printf("Scan window=%ums, scan interval=%ums, duplicate ignore=%ums\n",
                kScanWindowMs, kScanIntervalMs, kDuplicateIgnoreMs);

  g_scanStarted = startBeaconScan();
  g_nextScanRetryAt = g_scanStarted ? 0 : millis() + kScanRetryDelayMs;
}

void handleRelayState() {
  const RelayState state = getRelayState();

  if (state == RelayState::PendingOpen || state == RelayState::PendingClose) {
    beginPulse(state);
    return;
  }

  if (state != RelayState::PulsingOpen && state != RelayState::PulsingClose) {
    return;
  }

  const uint32_t nowMs = millis();
  if (nowMs - g_pulseStartedAt < kRelayPulseMs) {
    return;
  }

  finishPulse(state);
}

void retryScanIfNeeded() {
  if (g_scanStarted) {
    return;
  }

  const uint32_t nowMs = millis();
  if (nowMs < g_nextScanRetryAt) {
    return;
  }

  g_scanStarted = startBeaconScan();
  g_nextScanRetryAt = g_scanStarted ? 0 : nowMs + kScanRetryDelayMs;
}

}  // namespace

void setup() {
  Serial.begin(115200);

  pinMode(kPinOpen, OUTPUT);
  pinMode(kPinClose, OUTPUT);
  setRelayIdle();

  Serial.println("Booting BLE beacon relay scanner...");
  setupBleScanner();
}

void loop() {
  retryScanIfNeeded();
  handleRelayState();
  delay(5);
}
