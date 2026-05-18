#include <WiFi.h>
#include <WiFiClient.h>          // plain TCP — no TLS needed for local broker
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ── WIFI ─────────────────────────────────────────
const char* WIFI_SSID     = "RUT_BEF3_2G";
const char* WIFI_PASSWORD = "Admin@123";

// ── MQTT ─────────────────────────────────────────
const char* MQTT_HOST   = "192.168.116.30";
const int   MQTT_PORT   = 1884;
const char* MQTT_CLIENT = "esp32-actuator-01";

const char* TOPIC_SENSORS  = "home/sensors";
const char* TOPIC_COMMANDS = "home/commands";

// ── PINS ─────────────────────────────────────────
// Motor (L298N)
const int MOTOR_IN1 = 18;
const int MOTOR_IN2 = 19;
const int MOTOR_ENA = 25;   // PWM

// Relay (ACTIVE LOW)
const int RELAY_IN1 = 26;
const int RELAY_IN2 = 27;
const int RELAY_IN3 = 32;
const int RELAY_IN4 = 33;

// ── STATE ────────────────────────────────────────
bool flameLatch = false;
int  fanPwm     = 120;
int  lastFlameState = 1;  // for edge detection

WiFiClient espClient;                  // plain WiFiClient — no TLS
PubSubClient mqtt(espClient);

// ── RELAY CONTROL ────────────────────────────────
void setAllRelays(bool ON) {
  uint8_t level = ON ? LOW : HIGH;
  digitalWrite(RELAY_IN1, level);
  digitalWrite(RELAY_IN2, level);
  digitalWrite(RELAY_IN3, level);
  digitalWrite(RELAY_IN4, level);
}

// ── MOTOR CONTROL ────────────────────────────────
void applyFanPwm(int pwm) {
  pwm = constrain(pwm, 0, 255);
  fanPwm = pwm;

  analogWrite(MOTOR_ENA, pwm);

  Serial.printf("[FAN] PWM = %d\n", pwm);
}

// ── FLAME LATCH ──────────────────────────────────
void triggerFlameLatch() {
  if (!flameLatch) {
    flameLatch = true;

    setAllRelays(true);

    Serial.println("[FLAME] LATCH TRIGGERED");

    if (mqtt.connected()) {
      mqtt.publish("home/actuator/status",
        "{\"availability\":\"ONLINE\",\"flame_latch\":true}", true);
    }
  }
}

// ── MQTT CALLBACK ────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {

  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, msg)) return;

  String t = topic;

  // ── SENSOR TOPIC ─────────────────────────────
  if (t == TOPIC_SENSORS) {

    if (doc.containsKey("flame")) {

      int currentFlame = doc["flame"];

      Serial.printf("[SENSOR] Flame = %d\n", currentFlame);

      // EDGE DETECTION (1 → 0)
      if (lastFlameState == 1 && currentFlame == 0) {
        triggerFlameLatch();
      }

      lastFlameState = currentFlame;
    }
    return;
  }

  // ── COMMAND TOPIC ────────────────────────────
  if (t == TOPIC_COMMANDS) {

    // RESET FIRST
    if (doc.containsKey("flame_reset") && doc["flame_reset"]) {
      flameLatch = false;
      setAllRelays(false);

      Serial.println("[CMD] Flame Reset");

      if (mqtt.connected()) {
        mqtt.publish("home/actuator/status",
          "{\"availability\":\"ONLINE\",\"flame_latch\":false}", true);
      }
      return;
    }

    // ── RELAY1+2 CONTROL (IN1 & IN2) — blocked during flame latch ──
    if (doc.containsKey("relay1")) {
      if (!flameLatch) {
        String state = doc["relay1"];

        if (state == "ON") {
          digitalWrite(RELAY_IN1, LOW);
          digitalWrite(RELAY_IN2, LOW);
          Serial.println("[CMD] Relay1&2 ON");
        }
        else if (state == "OFF") {
          digitalWrite(RELAY_IN1, HIGH);
          digitalWrite(RELAY_IN2, HIGH);
          Serial.println("[CMD] Relay1&2 OFF");
        }
      }
      else {
        Serial.println("[CMD] relay1 Ignored (Flame Latch Active)");
      }
    }

    // ── RELAY3+4 CONTROL (IN3 & IN4) — blocked during flame latch ──
    if (doc.containsKey("relay2")) {
      if (!flameLatch) {
        String state = doc["relay2"];

        if (state == "ON") {
          digitalWrite(RELAY_IN3, LOW);
          digitalWrite(RELAY_IN4, LOW);
          Serial.println("[CMD] Relay3&4 ON");
        }
        else if (state == "OFF") {
          digitalWrite(RELAY_IN3, HIGH);
          digitalWrite(RELAY_IN4, HIGH);
          Serial.println("[CMD] Relay3&4 OFF");
        }
      }
      else {
        Serial.println("[CMD] relay2 Ignored (Flame Latch Active)");
      }
    }

    // FAN CONTROL (always allowed)
    if (doc.containsKey("fan_pwm")) {
      applyFanPwm(doc["fan_pwm"]);
    }
  }
}

// ── MQTT CONNECT ─────────────────────────────────
void connectMQTT() {

  while (!mqtt.connected()) {

    Serial.print("[MQTT] Connecting...");

    // No username/password for local broker
    if (mqtt.connect(MQTT_CLIENT)) {

      Serial.println("CONNECTED");

      mqtt.subscribe(TOPIC_SENSORS);
      mqtt.subscribe(TOPIC_COMMANDS);

      mqtt.publish("home/actuator/status",
        "{\"availability\":\"ONLINE\",\"flame_latch\":false}", true);
    }
    else {
      Serial.print("FAILED rc=");
      Serial.println(mqtt.state());
      delay(3000);
    }
  }
}

// ── SETUP ────────────────────────────────────────
void setup() {

  Serial.begin(115200);

  // PIN MODES
  for (int p : {MOTOR_IN1, MOTOR_IN2, MOTOR_ENA,
                RELAY_IN1, RELAY_IN2, RELAY_IN3, RELAY_IN4}) {
    pinMode(p, OUTPUT);
  }

  // MOTOR FIXED DIRECTION
  digitalWrite(MOTOR_IN1, HIGH);
  digitalWrite(MOTOR_IN2, LOW);

  // RELAYS OFF
  setAllRelays(false);

  // WIFI
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] Connected");

  // MQTT — plain TCP, no TLS setup needed
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  // INITIAL MOTOR SPEED
  applyFanPwm(120);
}

// ── LOOP ─────────────────────────────────────────
void loop() {

  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();
}