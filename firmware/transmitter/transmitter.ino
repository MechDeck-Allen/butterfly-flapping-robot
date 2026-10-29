/*
  Wireless Flapping Butterfly Robot — Transmitter
  Board: ESP32-C3 Mini / SuperMini
  Transport: ESP-NOW, channel 1
  Library: ESP32 Arduino core (ESP-NOW callback API used by core 3.x)

  Behavior:
  - Calibrates joystick centers at startup; keep both sticks centered.
  - Flapping is enabled only when BOTH sticks are moved beyond threshold.
  - More movement from the less-deflected stick increases amplitude and speed.
  - LED indicates recent receiver heartbeat.
*/

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// -------- PINOUT (ESP32-C3 ADC-capable GPIOs only) --------
constexpr int PIN_LEFT_X  = 4;
constexpr int PIN_LEFT_Y  = 1;
constexpr int PIN_RIGHT_X = 0;
constexpr int PIN_RIGHT_Y = 2;  // Use GPIO2, not GPIO6: GPIO6 is not ADC-capable.
constexpr int PIN_STATUS_LED = 3;

// -------- RADIO / PROTOCOL --------
constexpr uint8_t ESPNOW_CHANNEL = 1;
constexpr uint32_t CONTROL_MAGIC = 0xB17F2026;
constexpr uint32_t HEARTBEAT_MAGIC = 0xACCE2026;
constexpr uint32_t SEND_INTERVAL_MS = 40;
constexpr uint32_t HEARTBEAT_TIMEOUT_MS = 1200;

constexpr int MOVE_THRESHOLD = 350;
constexpr int MAX_USEFUL_DEFLECTION = 1800;

const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct ControlPacket {
  uint32_t magic;
  uint32_t sequence;
  uint16_t leftX;
  uint16_t leftY;
  uint16_t rightX;
  uint16_t rightY;
  uint8_t bothMoved;
  uint8_t amplitudeDeg;
  uint8_t frequencyDeciHz;  // 8 = 0.8 Hz, 25 = 2.5 Hz
};

struct HeartbeatPacket {
  uint32_t magic;
  uint32_t receiverMillis;
  uint32_t lastReceivedSequence;
};

portMUX_TYPE heartbeatMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t lastHeartbeatMs = 0;
volatile bool heartbeatReceived = false;

uint16_t centerLeftX = 2048;
uint16_t centerLeftY = 2048;
uint16_t centerRightX = 2048;
uint16_t centerRightY = 2048;

uint32_t sequenceNumber = 0;
uint32_t lastSendMs = 0;

uint16_t readAverage(int pin) {
  uint32_t sum = 0;
  for (int i = 0; i < 8; ++i) {
    sum += analogRead(pin);
    delay(2);
  }
  return static_cast<uint16_t>(sum / 8);
}

int stickDeflection(uint16_t x, uint16_t y, uint16_t centerX, uint16_t centerY) {
  const int dx = abs(static_cast<int>(x) - static_cast<int>(centerX));
  const int dy = abs(static_cast<int>(y) - static_cast<int>(centerY));
  return max(dx, dy);
}

void onDataReceived(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  (void)info;
  if (len != sizeof(HeartbeatPacket)) return;

  HeartbeatPacket heartbeat;
  memcpy(&heartbeat, data, sizeof(heartbeat));
  if (heartbeat.magic != HEARTBEAT_MAGIC) return;

  portENTER_CRITICAL(&heartbeatMux);
  lastHeartbeatMs = millis();
  heartbeatReceived = true;
  portEXIT_CRITICAL(&heartbeatMux);
}

void calibrateCenters() {
  Serial.println("\nCenter BOTH joysticks and leave them untouched for 2 seconds...");
  delay(2000);

  uint32_t lx = 0, ly = 0, rx = 0, ry = 0;
  for (int i = 0; i < 40; ++i) {
    lx += analogRead(PIN_LEFT_X);
    ly += analogRead(PIN_LEFT_Y);
    rx += analogRead(PIN_RIGHT_X);
    ry += analogRead(PIN_RIGHT_Y);
    delay(10);
  }

  centerLeftX = lx / 40;
  centerLeftY = ly / 40;
  centerRightX = rx / 40;
  centerRightY = ry / 40;

  Serial.printf("Centers: LX=%u LY=%u RX=%u RY=%u\n",
                centerLeftX, centerLeftY, centerRightX, centerRightY);
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

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_LEFT_X, ADC_11db);
  analogSetPinAttenuation(PIN_LEFT_Y, ADC_11db);
  analogSetPinAttenuation(PIN_RIGHT_X, ADC_11db);
  analogSetPinAttenuation(PIN_RIGHT_Y, ADC_11db);

  calibrateCenters();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    Serial.println("ERROR: unable to set ESP-NOW channel.");
    while (true) delay(1000);
  }

  Serial.print("Transmitter MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ERROR: ESP-NOW initialization failed.");
    while (true) delay(1000);
  }

  esp_now_register_recv_cb(onDataReceived);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_MAC, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;

  if (!esp_now_is_peer_exist(BROADCAST_MAC) &&
      esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("ERROR: failed to add broadcast peer.");
    while (true) delay(1000);
  }

  Serial.println("Transmitter ready. Waiting for receiver heartbeat.");
}

void loop() {
  const uint32_t now = millis();

  if (now - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = now;

    const uint16_t lx = readAverage(PIN_LEFT_X);
    const uint16_t ly = readAverage(PIN_LEFT_Y);
    const uint16_t rx = readAverage(PIN_RIGHT_X);
    const uint16_t ry = readAverage(PIN_RIGHT_Y);

    const int leftMovement = stickDeflection(lx, ly, centerLeftX, centerLeftY);
    const int rightMovement = stickDeflection(rx, ry, centerRightX, centerRightY);
    const bool bothMoved =
      leftMovement > MOVE_THRESHOLD && rightMovement > MOVE_THRESHOLD;

    const int limitingMovement = min(leftMovement, rightMovement);
    const int amplitude = bothMoved
      ? map(constrain(limitingMovement, MOVE_THRESHOLD, MAX_USEFUL_DEFLECTION),
            MOVE_THRESHOLD, MAX_USEFUL_DEFLECTION, 20, 40)
      : 0;
    const int frequencyDeciHz = bothMoved
      ? map(constrain(limitingMovement, MOVE_THRESHOLD, MAX_USEFUL_DEFLECTION),
            MOVE_THRESHOLD, MAX_USEFUL_DEFLECTION, 8, 25)
      : 0;

    ControlPacket packet = {};
    packet.magic = CONTROL_MAGIC;
    packet.sequence = ++sequenceNumber;
    packet.leftX = lx;
    packet.leftY = ly;
    packet.rightX = rx;
    packet.rightY = ry;
    packet.bothMoved = bothMoved ? 1 : 0;
    packet.amplitudeDeg = static_cast<uint8_t>(amplitude);
    packet.frequencyDeciHz = static_cast<uint8_t>(frequencyDeciHz);

    const esp_err_t result = esp_now_send(
      BROADCAST_MAC, reinterpret_cast<const uint8_t *>(&packet), sizeof(packet));

    if (result != ESP_OK) {
      Serial.printf("Control packet send error: %d\n", result);
    }

    Serial.printf("L=%d R=%d flap=%s amplitude=%d deg speed=%.1f Hz\n",
                  leftMovement, rightMovement, bothMoved ? "ON" : "OFF",
                  amplitude, frequencyDeciHz / 10.0f);
  }

  uint32_t heartbeatTime;
  bool gotHeartbeat;
  portENTER_CRITICAL(&heartbeatMux);
  heartbeatTime = lastHeartbeatMs;
  gotHeartbeat = heartbeatReceived;
  portEXIT_CRITICAL(&heartbeatMux);

  const bool connected = gotHeartbeat && (now - heartbeatTime <= HEARTBEAT_TIMEOUT_MS);
  digitalWrite(PIN_STATUS_LED, connected ? HIGH : LOW);

  delay(2);
}
