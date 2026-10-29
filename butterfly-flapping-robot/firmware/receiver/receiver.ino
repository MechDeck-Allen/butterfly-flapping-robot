/*
  Wireless Flapping Butterfly Robot — Receiver
  Board: ESP32-C3 Mini / SuperMini
  Transport: ESP-NOW, channel 1
  Libraries: ESP32 Arduino core (3.x callback API), ESP32Servo

  Behavior:
  - Starts with neutral 90-degree servo commands.
  - Requires valid transmitter packets before motion is enabled.
  - Both joystick deflections must be active to flap.
  - Receiver generates a continuous sine-wave flap motion locally.
  - Mirrored right servo uses the inverse electrical angle.
  - On control-link timeout, both servos return to neutral.
*/

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ESP32Servo.h>
#include <math.h>

// -------- PINOUT --------
constexpr int PIN_LEFT_SERVO = 5;
constexpr int PIN_RIGHT_SERVO = 6;
constexpr int PIN_STATUS_LED = 3;

// -------- RADIO / PROTOCOL --------
constexpr uint8_t ESPNOW_CHANNEL = 1;
constexpr uint32_t CONTROL_MAGIC = 0xB17F2026;
constexpr uint32_t HEARTBEAT_MAGIC = 0xACCE2026;
constexpr uint32_t CONTROL_TIMEOUT_MS = 1000;
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 200;

// -------- SERVO / MOTION LIMITS --------
constexpr int NEUTRAL_ANGLE = 90;
constexpr int MIN_ANGLE = 50;
constexpr int MAX_ANGLE = 130;
constexpr int SERVO_PULSE_MIN_US = 1000;
constexpr int SERVO_PULSE_MAX_US = 2000;

struct ControlPacket {
  uint32_t magic;
  uint32_t sequence;
  uint16_t leftX;
  uint16_t leftY;
  uint16_t rightX;
  uint16_t rightY;
  uint8_t bothMoved;
  uint8_t amplitudeDeg;
  uint8_t frequencyDeciHz;
};

struct HeartbeatPacket {
  uint32_t magic;
  uint32_t receiverMillis;
  uint32_t lastReceivedSequence;
};

Servo leftServo;
Servo rightServo;

portMUX_TYPE packetMux = portMUX_INITIALIZER_UNLOCKED;
ControlPacket latestPacket = {};
bool packetPending = false;
bool transmitterKnown = false;
uint8_t transmitterMac[6] = {};

bool transmitterPeerReady = false;
uint32_t lastControlMs = 0;
uint32_t lastHeartbeatMs = 0;
uint32_t lastReceivedSequence = 0;
uint32_t flapStartMs = 0;
bool flapActive = false;
int flapAmplitudeDeg = 0;
float flapFrequencyHz = 0.0f;

void onDataReceived(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(ControlPacket)) return;

  ControlPacket packet;
  memcpy(&packet, data, sizeof(packet));
  if (packet.magic != CONTROL_MAGIC) return;
  if (packet.bothMoved > 1) return;

  portENTER_CRITICAL(&packetMux);
  latestPacket = packet;
  packetPending = true;
  memcpy(transmitterMac, info->src_addr, 6);
  transmitterKnown = true;
  portEXIT_CRITICAL(&packetMux);
}

void writeNeutral() {
  leftServo.write(NEUTRAL_ANGLE);
  rightServo.write(NEUTRAL_ANGLE);
}

void ensureTransmitterPeer() {
  uint8_t mac[6];
  bool known;

  portENTER_CRITICAL(&packetMux);
  known = transmitterKnown;
  memcpy(mac, transmitterMac, 6);
  portEXIT_CRITICAL(&packetMux);

  if (!known || transmitterPeerReady) return;

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;

  if (esp_now_is_peer_exist(mac) || esp_now_add_peer(&peer) == ESP_OK) {
    transmitterPeerReady = true;
    Serial.println("Transmitter peer added; heartbeat replies enabled.");
  } else {
    Serial.println("WARNING: failed to add transmitter peer; will retry.");
  }
}

void sendHeartbeat() {
  if (!transmitterPeerReady) return;

  uint8_t mac[6];
  portENTER_CRITICAL(&packetMux);
  memcpy(mac, transmitterMac, 6);
  portEXIT_CRITICAL(&packetMux);

  HeartbeatPacket heartbeat = {};
  heartbeat.magic = HEARTBEAT_MAGIC;
  heartbeat.receiverMillis = millis();
  heartbeat.lastReceivedSequence = lastReceivedSequence;

  const esp_err_t result = esp_now_send(
    mac, reinterpret_cast<const uint8_t *>(&heartbeat), sizeof(heartbeat));

  if (result != ESP_OK) {
    Serial.printf("Heartbeat send error: %d\n", result);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);

  // LED wiring check at boot.
  digitalWrite(PIN_STATUS_LED, HIGH);
  delay(350);
  digitalWrite(PIN_STATUS_LED, LOW);

  leftServo.setPeriodHertz(50);
  rightServo.setPeriodHertz(50);
  leftServo.attach(PIN_LEFT_SERVO, SERVO_PULSE_MIN_US, SERVO_PULSE_MAX_US);
  rightServo.attach(PIN_RIGHT_SERVO, SERVO_PULSE_MIN_US, SERVO_PULSE_MAX_US);
  writeNeutral();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    Serial.println("ERROR: unable to set ESP-NOW channel.");
    while (true) delay(1000);
  }

  Serial.print("Receiver MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ERROR: ESP-NOW initialization failed.");
    while (true) delay(1000);
  }

  esp_now_register_recv_cb(onDataReceived);
  Serial.println("Receiver ready. Waiting for transmitter packets.");
}

void loop() {
  const uint32_t now = millis();

  ControlPacket packet;
  bool gotPacket = false;

  portENTER_CRITICAL(&packetMux);
  if (packetPending) {
    packet = latestPacket;
    packetPending = false;
    gotPacket = true;
  }
  portEXIT_CRITICAL(&packetMux);

  ensureTransmitterPeer();

  if (gotPacket) {
    lastControlMs = now;
    lastReceivedSequence = packet.sequence;

    const bool shouldFlap = packet.bothMoved == 1 &&
                             packet.amplitudeDeg > 0 &&
                             packet.frequencyDeciHz > 0;

    if (shouldFlap && !flapActive) {
      flapStartMs = now;
    }
    if (!shouldFlap) {
      writeNeutral();
    }

    flapActive = shouldFlap;
    flapAmplitudeDeg = constrain(static_cast<int>(packet.amplitudeDeg), 0, 40);
    flapFrequencyHz = constrain(static_cast<int>(packet.frequencyDeciHz), 0, 25) / 10.0f;
  }

  const bool connected =
    lastControlMs != 0 && (now - lastControlMs <= CONTROL_TIMEOUT_MS);

  digitalWrite(PIN_STATUS_LED, connected ? HIGH : LOW);

  if (!connected) {
    flapActive = false;
    flapAmplitudeDeg = 0;
    flapFrequencyHz = 0.0f;
    writeNeutral();
  } else if (flapActive && flapAmplitudeDeg > 0 && flapFrequencyHz > 0.0f) {
    const float elapsedSeconds = (now - flapStartMs) / 1000.0f;
    const float offset = flapAmplitudeDeg *
      sinf(2.0f * PI * flapFrequencyHz * elapsedSeconds);

    const int leftAngle = constrain(
      static_cast<int>(lroundf(NEUTRAL_ANGLE + offset)), MIN_ANGLE, MAX_ANGLE);

    // Mirrored installation: opposite electrical angles create matching physical motion.
    const int rightAngle = constrain(
      static_cast<int>(lroundf(NEUTRAL_ANGLE - offset)), MIN_ANGLE, MAX_ANGLE);

    leftServo.write(leftAngle);
    rightServo.write(rightAngle);
  }

  if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = now;
    sendHeartbeat();
  }

  delay(5);
}
