/*
 * ============================================================
 *  ESP32  –  Sensor + PWM Motor Controller
 *
 *  Publishes:  home/sensors
 *    { "flame": 0|1, "voltage": 12.34, "temperature": 31.2, "fan_pwm": 100 }
 *
 *  Subscribes: home/commands
 *    { "fan_pwm": 0-255 }
 *    { "fan_pwm": -1 }              → fallback to base speed
 *    { "target_voltage": 12.5 }     → inverse calibration → PWM
 *
 *  Voltage calibration (quadratic):
 *    V = 0.0157·Vpin² + 5.1177·Vpin + 0.6332
 *    where Vpin = raw_adc * (3.3 / 4095)
 *
 *  Temperature sensor:
 *    Thermistor on GPIO27
 *    Steinhart-Hart / B-parameter approximation
 *
 *  WebSocket port 81 – local LAN fallback for fan_pwm
 * ============================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <WebSocketsServer.h>
#include <math.h>

// ── CONFIG ────────────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "RUT_BEF3_2G";
const char* WIFI_PASSWORD = "Admin@123";

const char* MQTT_HOST   = "192.168.116.30";
const int   MQTT_PORT   = 1884;
const char* MQTT_USER   = "";
const char* MQTT_PASS   = "";
const char* MQTT_CLIENT = "esp32-sensor-01";

const char* TOPIC_SENSORS  = "home/sensors";
const char* TOPIC_COMMANDS = "home/commands";

const unsigned long PUB_INTERVAL = 2000;  // publish every 2 s

// ── PINS ──────────────────────────────────────────────────────────────────────
#define FLAME_PIN    25   // Digital input (active-LOW: 0 = fire)
#define VOLTAGE_PIN  35   // Input-only ADC
#define TEMP_PIN     34   // Thermistor input

// Motor / fan driver
#define MOTOR_IN1    32
#define MOTOR_IN2    33
#define MOTOR_ENA    26   // PWM speed

// ── THERMISTOR CONSTANTS ──────────────────────────────────────────────────────
#define VCC                3.3f
#define THERMISTOR_RSERIES 10000.0f
#define THERMISTOR_R0      10000.0f
#define THERMISTOR_B       3950.0f
#define THERMISTOR_T0      298.15f   // 25°C in Kelvin

// ── FAN ───────────────────────────────────────────────────────────────────────
const int FAN_BASE_SPEED = 100;   // minimum / default PWM
int       fanPwm         = FAN_BASE_SPEED;

// ── GLOBALS ───────────────────────────────────────────────────────────────────
WiFiClient       wifiClient;        // plain TCP — no TLS needed for local broker
PubSubClient     mqtt(wifiClient);
WebSocketsServer webSocket(81);
unsigned long    lastPublish = 0;

// ── VOLTAGE READING ───────────────────────────────────────────────────────────
float readVoltage() {
  int   raw  = analogRead(VOLTAGE_PIN);
  float vpin = raw * (3.3f / 4095.0f);
  return 0.0157f * vpin * vpin + 5.1177f * vpin + 0.6332f;
}

// ── TEMPERATURE READING ───────────────────────────────────────────────────────
float readTemperature() {
  long sum = 0;

  // Average 10 samples to reduce ADC noise
  for (int i = 0; i < 10; i++) {
    sum += analogRead(TEMP_PIN);
    delay(2);
  }

  float raw = sum / 10.0f;

  // Convert ADC → voltage
  float vadc = raw * (VCC / 4095.0f);

  // Guard against divide-by-zero / disconnected sensor
  if (vadc <= 0.01f || vadc >= (VCC - 0.01f)) {
    Serial.println("[TEMP] ADC out of range – sensor disconnected?");
    return -999.0f;
  }

  // Voltage divider → thermistor resistance
  // Vout = Vcc * R_therm / (R_series + R_therm)
  // → R_therm = R_series * Vout / (Vcc - Vout)
  float rTherm = THERMISTOR_RSERIES * (vadc / (VCC - vadc));

  // B-parameter equation
  float tempK = 1.0f / ((logf(rTherm / THERMISTOR_R0) / THERMISTOR_B) + (1.0f / THERMISTOR_T0));
  float tempC = tempK - 273.15f;

  return tempC;
}

// ── TEMPERATURE → PWM ─────────────────────────────────────────────────────────
int temperatureToPwm(float tempC) {
  // Fail-safe: keep the fan at minimum speed if the sensor is invalid
  if (tempC < -100.0f) return FAN_BASE_SPEED;

  // Tune these two numbers to suit your thermistor / cooling needs
  const float T_MIN = 30.0f;   // at or below this → minimum PWM
  const float T_MAX = 60.0f;   // at or above this → full PWM

  if (tempC <= T_MIN) return FAN_BASE_SPEED;
  if (tempC >= T_MAX) return 255;

  float ratio = (tempC - T_MIN) / (T_MAX - T_MIN);
  int pwm = FAN_BASE_SPEED + (int)roundf(ratio * (255 - FAN_BASE_SPEED));
  return constrain(pwm, FAN_BASE_SPEED, 255);
}

// ── INVERSE CALIBRATION: target voltage → PWM ────────────────────────────────
int voltageTargetToPwm(float targetV) {
  const float a = 0.0157f;
  const float b = 5.1177f;
  const float c = 0.6332f - targetV;

  float discriminant = b * b - 4.0f * a * c;
  if (discriminant < 0.0f) {
    Serial.printf("[VOLT] target_voltage %.2f out of range\n", targetV);
    return -1;
  }

  float vpin = (-b + sqrtf(discriminant)) / (2.0f * a);
  int   pwm  = (int)roundf((vpin / 3.3f) * 255.0f);
  return constrain(pwm, 0, 255);
}

// ── FAN OUTPUT ────────────────────────────────────────────────────────────────
void applyFanPwm(int pwm) {
  pwm = constrain(pwm, FAN_BASE_SPEED, 255);
  digitalWrite(MOTOR_IN1, HIGH);
  digitalWrite(MOTOR_IN2, LOW);
  analogWrite(MOTOR_ENA, pwm);
  Serial.printf("[FAN] PWM = %d\n", pwm);
}

// ── WEBSOCKET (LAN fallback) ───────────────────────────────────────────────────
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type != WStype_TEXT) return;

  StaticJsonDocument<128> doc;
  if (!deserializeJson(doc, (char*)payload) && doc.containsKey("fan_pwm")) {
    int v = doc["fan_pwm"].as<int>();
    fanPwm = (v < 0) ? FAN_BASE_SPEED : constrain(v, FAN_BASE_SPEED, 255);
    applyFanPwm(fanPwm);
    Serial.printf("[WS] fan_pwm → %d\n", fanPwm);
  }
}

// ── MQTT CALLBACK ─────────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  if (String(topic) != TOPIC_COMMANDS) return;

  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, msg)) return;

  // Direct PWM override
  if (doc.containsKey("fan_pwm")) {
    int v = doc["fan_pwm"].as<int>();
    fanPwm = (v < 0) ? FAN_BASE_SPEED : constrain(v, FAN_BASE_SPEED, 255);
    applyFanPwm(fanPwm);
    Serial.printf("[CMD] fan_pwm → %d\n", fanPwm);
  }

  // Target voltage → inverse calibration → PWM
  if (doc.containsKey("target_voltage")) {
    float tv  = doc["target_voltage"].as<float>();
    int   pwm = voltageTargetToPwm(tv);
    if (pwm >= 0) {
      fanPwm = constrain(pwm, FAN_BASE_SPEED, 255);
      applyFanPwm(fanPwm);
      Serial.printf("[CMD] target_voltage %.2fV → PWM %d\n", tv, fanPwm);
    }
  }
}

// ── MQTT CONNECT ──────────────────────────────────────────────────────────────
void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;

  Serial.print("[MQTT] Connecting…");

  bool ok = (strlen(MQTT_USER) > 0)
              ? mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS)
              : mqtt.connect(MQTT_CLIENT);

  if (ok) {
    Serial.println(" OK");
    mqtt.subscribe(TOPIC_COMMANDS);
    Serial.println("[MQTT] Subscribed → " + String(TOPIC_COMMANDS));
  } else {
    Serial.printf(" FAILED rc=%d\n", mqtt.state());
  }
}

// ── SETUP ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  analogSetPinAttenuation(TEMP_PIN, ADC_11db);

  pinMode(FLAME_PIN, INPUT);
  pinMode(TEMP_PIN, INPUT);
  pinMode(MOTOR_IN1, OUTPUT);
  pinMode(MOTOR_IN2, OUTPUT);
  pinMode(MOTOR_ENA, OUTPUT);

  // Safe defaults – motor minimum speed
  digitalWrite(MOTOR_IN1, LOW);
  digitalWrite(MOTOR_IN2, LOW);
  analogWrite(MOTOR_ENA, 0);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] Connected  IP: " + WiFi.localIP().toString());

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println("[WS] Server started on port 81");

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setKeepAlive(60);
  mqtt.setCallback(mqttCallback);

  connectMQTT();

  // Start fan at minimum/base speed
  applyFanPwm(FAN_BASE_SPEED);
}

// ── LOOP ──────────────────────────────────────────────────────────────────────
void loop() {
  webSocket.loop();

  if (!mqtt.connected()) connectMQTT();
  else                   mqtt.loop();

  unsigned long now = millis();
  if (now - lastPublish >= PUB_INTERVAL) {
    lastPublish = now;

    int   flame      = digitalRead(FLAME_PIN);
    float voltage    = readVoltage();
    float temperature = readTemperature();

    // Update fan speed from temperature
    int tempPwm = temperatureToPwm(temperature);
    fanPwm = tempPwm;
    applyFanPwm(fanPwm);

    char payload[128];
    snprintf(payload, sizeof(payload),
      "{\"flame\":%d,\"voltage\":%.2f,\"temperature\":%.2f,\"fan_pwm\":%d}",
      flame, voltage, temperature, fanPwm);

    if (mqtt.connected()) mqtt.publish(TOPIC_SENSORS, payload);
    Serial.println("[PUB] " + String(payload));
  }
}