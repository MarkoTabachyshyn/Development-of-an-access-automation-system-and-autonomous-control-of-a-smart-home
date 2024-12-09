#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include <RTClib.h>
#include <Stepper.h>
#include <EEPROM.h>

// WiFi налаштування
const char* ssid = "ASUS";
const char* password = "Ebanina31";

// MQTT налаштування
const char* mqtt_server = "raspberrypi.local";
const int mqtt_port = 1883;
const char* mqtt_user = "esp326";
const char* mqtt_password = "666";
const char* mqtt_topic_publish = "home/room/bedroom";
const char* mqtt_topic_subscribe = "home/room/bedroom";

// Піни
#define PIR_SENSOR_PIN 23
#define LED_PIN 12
#define RED_LED_PIN 16
#define BLUE_LED_PIN 17
#define BUTTON_LIGHT_PIN 13
#define BUTTON_CLOSE_PIN 14
#define BUTTON_OPEN_PIN 27
#define ONE_WIRE_BUS 4

#define IN1 32
#define IN2 33
#define IN3 25
#define IN4 26
#define STEPS_PER_REV 2048

// Налаштування
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
WiFiClient espClient;
PubSubClient client(espClient);
RTC_DS3231 rtc;
Stepper stepper(STEPS_PER_REV, IN1, IN3, IN2, IN4);

// Таймери
unsigned long lastPublishTime = 0;
unsigned long motionDetectedTime = 0;
unsigned long lastMotionCheckTime = 0;
unsigned long lastDebounceTime = 0;
unsigned long lastMotionDetected = 0;

const unsigned long motionTimeout = 60000; // 1 хвилина
const unsigned long motionCheckInterval = 300;
const unsigned long debounceDelay = 50;
const unsigned long motionDebounceDelay = 1000; // 1 секунда

// Змінні
bool lightState = false;
bool buttonState = false;
bool lastButtonState = false;
bool motionLock = false; // Блокування сенсора руху
bool curtainsClosed = false;
float currentTemperature = 0.0;
float desiredTemperature = 22.0;

#define EEPROM_CURTAINS_ADDR 0

// Прототипи функцій
void setupWiFi();
void reconnect();
void callback(char* topic, byte* payload, unsigned int length);
void publishData();
void handleMotion();
void handleButton();
void toggleLight(bool state);
void controlTemperature();
void manageCurtains();
void moveCurtains(int steps);
int getCurrentHour();
bool isNightTime();
void syncTimeWithRTC();

void setup() {
  Serial.begin(115200);
  EEPROM.begin(512);
  curtainsClosed = EEPROM.read(EEPROM_CURTAINS_ADDR);

  pinMode(PIR_SENSOR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(BLUE_LED_PIN, OUTPUT);
  pinMode(BUTTON_LIGHT_PIN, INPUT_PULLUP);
  pinMode(BUTTON_CLOSE_PIN, INPUT_PULLUP);
  pinMode(BUTTON_OPEN_PIN, INPUT_PULLUP);

  digitalWrite(LED_PIN, LOW);
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(BLUE_LED_PIN, LOW);
  stepper.setSpeed(10);

  setupWiFi();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  sensors.begin();

  if (!rtc.begin()) {
    Serial.println("DS3231 not found.");
    while (1);
  }

  if (!rtc.lostPower()) {
      DateTime now = rtc.now();
      Serial.print("Current time: ");
      Serial.println(now.timestamp());
  } else {
      Serial.println("RTC not set, syncing with system time.");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  Serial.println("System initialized.");
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  // Зчитування температури
  sensors.requestTemperatures();
  currentTemperature = sensors.getTempCByIndex(0);
  Serial.print("Current Temperature: ");
  Serial.println(currentTemperature);

  handleButton();

  if (!motionLock && !isNightTime()) {
    handleMotion();
  }

  controlTemperature();
  manageCurtains();

  if (millis() - lastPublishTime > 10000) {
    publishData();
    lastPublishTime = millis();
  }
}

void setupWiFi() {
  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Connecting to MQTT...");
    if (client.connect("ESP32_Bedroom", mqtt_user, mqtt_password)) {
      Serial.println("Connected.");
      client.subscribe(mqtt_topic_subscribe);
    } else {
      Serial.print("Failed, rc=");
      Serial.println(client.state());
      delay(5000);
    }
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.print("Message received: ");
  Serial.println(message);

  if (message.startsWith("{") && message.endsWith("}")) {
    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, message);

    if (!error) {
      if (doc.containsKey("desired_temp")) {
        desiredTemperature = doc["desired_temp"];
        Serial.print("Updated desired temperature: ");
        Serial.println(desiredTemperature);
      }
    }
  }
}

void handleMotion() {
  if (millis() - lastMotionCheckTime >= motionCheckInterval) {
    lastMotionCheckTime = millis();

    if (digitalRead(PIR_SENSOR_PIN) == HIGH) {
      if (millis() - lastMotionDetected > motionDebounceDelay) {
        lastMotionDetected = millis();
        Serial.println("Motion detected!");
        motionDetectedTime = millis();
        if (!lightState) {
          toggleLight(true);
          Serial.println("Light turned ON due to motion.");
        }
      }
    }

    if (lightState && millis() - motionDetectedTime > motionTimeout) {
      toggleLight(false);
      Serial.println("Light turned OFF due to inactivity.");
    }
  }
}

void handleButton() {
  bool currentButtonState = !digitalRead(BUTTON_LIGHT_PIN);
  if (currentButtonState != lastButtonState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (currentButtonState != buttonState) {
      buttonState = currentButtonState;
      if (buttonState) {
        motionLock = true; // Заблокувати сенсор руху
        toggleLight(true); // Увімкнути світло
        Serial.println("Light turned ON manually. Motion sensor locked.");
      } else {
        motionLock = false; // Розблокувати сенсор руху
        toggleLight(false); // Вимкнути світло
        Serial.println("Light turned OFF manually. Motion sensor unlocked.");
      }
    }
  }
  lastButtonState = currentButtonState;

  bool closeButtonState = !digitalRead(BUTTON_CLOSE_PIN);
  if (closeButtonState && !curtainsClosed) {
    moveCurtains(512);
    curtainsClosed = true;
    EEPROM.write(EEPROM_CURTAINS_ADDR, curtainsClosed);
    EEPROM.commit();
  }

  bool openButtonState = !digitalRead(BUTTON_OPEN_PIN);
  if (openButtonState && curtainsClosed) {
    moveCurtains(-512);
    curtainsClosed = false;
    EEPROM.write(EEPROM_CURTAINS_ADDR, curtainsClosed);
    EEPROM.commit();
  }
}

void toggleLight(bool state) {
  lightState = state;
  digitalWrite(LED_PIN, state ? HIGH : LOW);
}

void controlTemperature() {
  if (currentTemperature > desiredTemperature) {
    digitalWrite(RED_LED_PIN, HIGH);
    digitalWrite(BLUE_LED_PIN, LOW);
    Serial.println("Cooling activated (RED LED ON).");
  } else if (currentTemperature < desiredTemperature) {
    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(BLUE_LED_PIN, HIGH);
    Serial.println("Heating activated (BLUE LED ON).");
  } else {
    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(BLUE_LED_PIN, LOW);
    Serial.println("Temperature balanced (LEDs OFF).");
  }
}

void manageCurtains() {
  int hour = getCurrentHour();
  if (hour == 22 && !curtainsClosed) {
    moveCurtains(512);
    curtainsClosed = true;
    EEPROM.write(EEPROM_CURTAINS_ADDR, curtainsClosed);
    EEPROM.commit();
  } else if (hour == 7 && curtainsClosed) {
    moveCurtains(-512);
    curtainsClosed = false;
    EEPROM.write(EEPROM_CURTAINS_ADDR, curtainsClosed);
    EEPROM.commit();
  }
}

int getCurrentHour() {
  DateTime now = rtc.now();
  return now.hour();
}

bool isNightTime() {
  int hour = getCurrentHour();
  return (hour >= 22 || hour < 7);
}

void moveCurtains(int steps) {
  stepper.step(steps);
}

void publishData() {
  StaticJsonDocument<200> doc;
  doc["light"] = lightState ? "on" : "off";
  doc["temp"] = currentTemperature;
  doc["desired_temp"] = desiredTemperature;
  doc["curtains"] = curtainsClosed ? "closed" : "open";

  char buffer[256];
  size_t n = serializeJson(doc, buffer);
  client.publish(mqtt_topic_publish, buffer, n);
}