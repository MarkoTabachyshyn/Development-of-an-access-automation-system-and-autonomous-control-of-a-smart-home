#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>

// WiFi налаштування
const char* ssid = "ASUS";
const char* password = "Ebanina31";

// MQTT налаштування
const char* mqtt_server = "raspberrypi.local";
const int mqtt_port = 1883;
const char* mqtt_user = "esp324";
const char* mqtt_password = "444";
const char* mqtt_topic_publish = "home/room/kitchen";
const char* mqtt_topic_subscribe = "home/room/kitchen";

// Піни
#define PIR_SENSOR_PIN 22        // Пін для датчика руху HC-SR501
#define LED_PIN 12               // Світлодіод для управління світлом
#define RED_LED_PIN 26           // Червоний світлодіод (охолодження)
#define BLUE_LED_PIN 27          // Синій світлодіод (нагрівання)
#define ONE_WIRE_BUS 14          // Пін для датчика DS18B20
#define MQ2_PIN 34               // Пін для датчика диму MQ2
#define SPEAKER_PIN 33           // Пін для спікера
#define BUTTON_PIN 13            // Пін для кнопки

// Налаштування OneWire і DallasTemperature
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// WiFi і MQTT клієнт
WiFiClient espClient;
PubSubClient client(espClient);

// Таймери
unsigned long lastPublishTime = 0;
unsigned long lastSmokeCheckTime = 0;
unsigned long lastMotionCheckTime = 0;
unsigned long motionDetectedTime = 0;
const unsigned long motionTimeout = 60000; // 1 хвилина
const unsigned long motionCheckInterval = 5000; // 5 секунд
const unsigned long smokeCheckInterval = 5000; // 5 секунд
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50; // Антидребезг для кнопки

// Змінні
bool lightState = false;
bool buttonState = false;
bool lastButtonState = false;
bool motionLock = false; // Блокування сенсора руху кнопкою
bool fireDetected = false;
float currentTemperature = 0.0;
float desiredTemperature = 22.0; // За замовчуванням

// Прототипи функцій
void publishData();
void handleMotion();
void handleButton();
void controlTemperature();
void checkSmoke();
void toggleLight(bool state);
void setupWiFi();
void reconnect();
void callback(char* topic, byte* payload, unsigned int length);

void setup() {
  Serial.begin(115200);

  // Ініціалізація пінів
  pinMode(PIR_SENSOR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(BLUE_LED_PIN, OUTPUT);
  pinMode(SPEAKER_PIN, OUTPUT);
  pinMode(MQ2_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP); // Кнопка з pull-up резистором

  digitalWrite(LED_PIN, LOW);
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(BLUE_LED_PIN, LOW);
  digitalWrite(SPEAKER_PIN, LOW);

  setupWiFi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  sensors.begin();
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  sensors.requestTemperatures();
  currentTemperature = sensors.getTempCByIndex(0);

  handleButton();

  if (!motionLock) {
    handleMotion();
  }

  controlTemperature();

  if (millis() - lastSmokeCheckTime >= smokeCheckInterval) {
    lastSmokeCheckTime = millis();
    checkSmoke();
  }

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
    if (client.connect("ESP32_Kitchen", mqtt_user, mqtt_password)) {
      Serial.println("connected");
      client.subscribe(mqtt_topic_subscribe);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
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
      Serial.println("Motion detected!");
      motionDetectedTime = millis();
      if (!lightState) {
        toggleLight(true);
        Serial.println("Light turned ON due to motion.");
      }
    }

    if (lightState && millis() - motionDetectedTime > motionTimeout) {
      toggleLight(false);
      Serial.println("Light turned OFF due to inactivity.");
    }
  }
}

void handleButton() {
  bool currentButtonState = !digitalRead(BUTTON_PIN); // Інверсія через pull-up
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

void checkSmoke() {
  int mq2Value = analogRead(MQ2_PIN);
  if (mq2Value > 500) { // Поріг для пожежі
    fireDetected = true;
    digitalWrite(SPEAKER_PIN, HIGH); // Увімкнути спікер
    Serial.println("Fire detected! Speaker ON.");
  } else {
    fireDetected = false;
    digitalWrite(SPEAKER_PIN, LOW); // Вимкнути спікер
    Serial.println("No fire detected. Speaker OFF.");
  }
}

void publishData() {
  StaticJsonDocument<200> doc;
  doc["light"] = lightState ? "on" : "off";
  doc["temp"] = currentTemperature;
  doc["desired_temp"] = desiredTemperature;
  doc["fire"] = fireDetected ? "yes" : "no";

  char buffer[256];
  size_t n = serializeJson(doc, buffer);

  if (client.publish(mqtt_topic_publish, buffer, n)) {
    Serial.println("Data published successfully");
  } else {
    Serial.println("Failed to publish data");
  }
}
