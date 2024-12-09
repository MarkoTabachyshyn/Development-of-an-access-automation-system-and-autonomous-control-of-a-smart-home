#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>

// WiFi налаштування
const char* ssid = "ASUS";
const char* password = "ASUS";

// MQTT налаштування
const char* mqtt_server = "raspberrypi.local";
const int mqtt_port = 1883;
const char* mqtt_user = "esp327";
const char* mqtt_password = "777";
const char* mqtt_topic_publish = "home/room/bathroom";
const char* mqtt_topic_subscribe = "home/room/bathroom";

// Піни
#define PIR_SENSOR_PIN 22        // Пін для датчика руху HC-SR501
#define LED_PIN 12               // Світлодіод для управління світлом
#define FAN_PIN 23               // Пін для вентилятора
#define RED_LED_PIN 26           // Червоний світлодіод (охолодження)
#define BLUE_LED_PIN 27          // Синій світлодіод (нагрівання)
#define ONE_WIRE_BUS 14          // Пін для датчика DS18B20
#define BUTTON_LIGHT_PIN 13      // Кнопка для керування світлом
#define BUTTON_FAN_PIN 32        // Кнопка для керування вентилятором

// Налаштування OneWire і DallasTemperature
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// WiFi і MQTT клієнт
WiFiClient espClient;
PubSubClient client(espClient);

// Таймери та змінні
unsigned long lastPublishTime = 0;
unsigned long motionDetectedTime = 0;
unsigned long lastMotionCheckTime = 0;
unsigned long lastDebounceLightTime = 0;

const unsigned long motionTimeout = 5000; // Таймаут для світла
const unsigned long debounceDelay = 50;   // Антидребезг для кнопок

bool lightState = false;
bool fanState = false;
bool motionLock = false;  // Блокування сенсора руху для світла
bool buttonLightState = false;
bool lastButtonLightState = false;

float currentTemperature = 0.0;
float desiredTemperature = 22.0; // За замовчуванням

// Для логіки вентилятора
bool buttonPressed = false;
bool manualControl = false;
bool pirPreviousState = false;
unsigned long fanTimer = 0;
unsigned long lastPIRChange = 0;
unsigned long lastFanOffTime = 0;
const unsigned long FAN_RUN_TIME = 3000;         // Тривалість роботи вентилятора
const unsigned long PIR_DEBOUNCE_TIME = 5000;    // Час стабілізації PIR
const unsigned long MIN_WAIT_AFTER_FAN_OFF = 10000; // Мінімальний час після вимкнення вентилятора
const unsigned long motionCheckInterval = 5000; // Інтервал перевірки руху
unsigned long lastMotionCheck = 0;

// Для стабілізації PIR після вимкнення вентилятора
bool pirStabilizing = false;
unsigned long pirStabilizingStart = 0;
const unsigned long PIR_STABILIZATION_TIME = 3000; // 3 секунди стабілізації

// Прототипи функцій
void publishData();
void handleMotion();
void handleButton();
void controlFan();
void setupWiFi();
void reconnect();
void callback(char* topic, byte* payload, unsigned int length);
void toggleLight(bool state);
void toggleFan(bool state);
void controlTemperature();

void setup() {
  Serial.begin(115200);

  // Ініціалізація пінів
  pinMode(PIR_SENSOR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(BLUE_LED_PIN, OUTPUT);
  pinMode(BUTTON_LIGHT_PIN, INPUT_PULLUP);
  pinMode(BUTTON_FAN_PIN, INPUT_PULLUP);

  digitalWrite(LED_PIN, LOW);
  digitalWrite(FAN_PIN, HIGH); // Початковий стан вентилятора (вимкнено)
  fanState = false;
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(BLUE_LED_PIN, LOW);

  // Підключення до WiFi
  setupWiFi();

  // Ініціалізація MQTT
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  // Ініціалізація DS18B20
  sensors.begin();

  Serial.println("System initialized:");
  Serial.println("Light OFF");
  Serial.println("Fan OFF");
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
  handleMotion();
  controlFan();
  controlTemperature();

  if (millis() - lastPublishTime > 10000) {
    publishData();
    lastPublishTime = millis();
  }
}

void controlFan() {
  unsigned long currentMillis = millis();
  bool buttonState = digitalRead(BUTTON_FAN_PIN);

  // Перевірка кнопки
  if (buttonState == LOW) {
    if (!buttonPressed) {
      buttonPressed = true;
      manualControl = !manualControl;

      if (manualControl) {
        Serial.println("Manual mode ON. Fan always ON.");
        digitalWrite(FAN_PIN, LOW); // Вентилятор увімкнено
        fanState = true;
      } else {
        Serial.println("Manual mode OFF. Returning to PIR control.");
        digitalWrite(FAN_PIN, HIGH); // Вентилятор вимкнено
        fanState = false;

        // Початок стабілізації PIR після вимкнення вентилятора
        pirStabilizing = true;
        pirStabilizingStart = currentMillis;
        Serial.println("PIR stabilizing...");
      }
    }
  } else {
    buttonPressed = false;
  }

  if (!manualControl) {
    if (pirStabilizing) {
      if (currentMillis - pirStabilizingStart >= PIR_STABILIZATION_TIME + 1000) {
        pirStabilizing = false;
        Serial.println("PIR stabilized.");
      }
      return;
    }

    if (currentMillis - lastMotionCheck >= motionCheckInterval) {
      lastMotionCheck = currentMillis;
      bool pirState = digitalRead(PIR_SENSOR_PIN);

      if (pirState != pirPreviousState && currentMillis - lastPIRChange >= PIR_DEBOUNCE_TIME) {
        lastPIRChange = currentMillis;

        if (pirState == HIGH) {
          digitalWrite(FAN_PIN, HIGH); // Вентилятор вимкнено
          fanState = false;
          Serial.println("Motion detected. Fan OFF.");
        } else if (pirState == LOW && currentMillis - lastFanOffTime >= MIN_WAIT_AFTER_FAN_OFF) {
          fanTimer = millis();
          digitalWrite(FAN_PIN, LOW); // Вентилятор увімкнено
          fanState = true;
          Serial.println("No motion. Fan ON for 3 seconds.");
        }

        pirPreviousState = pirState;
      }

      if (fanState && millis() - fanTimer >= FAN_RUN_TIME) {
        digitalWrite(FAN_PIN, HIGH); // Вимкнути вентилятор
        fanState = false;
        lastFanOffTime = millis();
        Serial.println("Fan runtime ended. Fan OFF.");
      }
    }
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
    if (client.connect("ESP32_Bathroom", mqtt_user, mqtt_password)) {
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
  unsigned long currentMillis = millis();

  // Якщо PIR знаходиться в стадії стабілізації, ігнорувати рух
  if (pirStabilizing && (currentMillis - pirStabilizingStart < PIR_STABILIZATION_TIME + 1000)) {
    Serial.println("Motion ignored (PIR stabilizing).");
    return;
  }

  if (currentMillis - lastMotionCheckTime >= motionCheckInterval) {
    lastMotionCheckTime = currentMillis;

    if (digitalRead(PIR_SENSOR_PIN) == HIGH) {
      Serial.println("Motion detected!");
      motionDetectedTime = millis();
      if (!lightState) {
        toggleLight(true);
      }
    }

    if (lightState && millis() - motionDetectedTime > motionTimeout) {
      toggleLight(false);
    }
  }
}

void handleButton() {
  unsigned long currentMillis = millis();

  if (currentMillis - lastDebounceLightTime > debounceDelay) {
    bool currentButtonLightState = !digitalRead(BUTTON_LIGHT_PIN);
    if (currentButtonLightState != lastButtonLightState) {
      lastDebounceLightTime = currentMillis;
      if (currentButtonLightState) {
        motionLock = !motionLock;
        toggleLight(motionLock);
      }
      lastButtonLightState = currentButtonLightState;
    }
  }
}

void toggleLight(bool state) {
  lightState = state;
  digitalWrite(LED_PIN, state ? HIGH : LOW);
  Serial.print("Light ");
  Serial.println(state ? "ON" : "OFF");
}

void toggleFan(bool state) {
  fanState = state;
  digitalWrite(FAN_PIN, state ? LOW : HIGH);
  Serial.print("Fan ");
  Serial.println(state ? "ON" : "OFF");
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

void publishData() {
  StaticJsonDocument<200> doc;
  doc["light"] = lightState ? "on" : "off";
  doc["fan"] = fanState ? "on" : "off";
  doc["temp"] = currentTemperature;
  doc["desired_temp"] = desiredTemperature;

  char buffer[256];
  size_t n = serializeJson(doc, buffer);

  if (client.publish(mqtt_topic_publish, buffer, n)) {
    Serial.println("Data published successfully");
  } else {
    Serial.println("Failed to publish data");
  }
}
