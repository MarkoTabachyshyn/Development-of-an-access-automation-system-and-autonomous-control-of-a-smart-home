#include <AccelStepper.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <EEPROM.h> // Для роботи з EEPROM

// Налаштування Wi-Fi
const char* ssid = "ASUS"; // Ім'я Wi-Fi
const char* password = "ASUS";             // Пароль Wi-Fi

// Налаштування MQTT
const char* mqtt_server = "raspberrypi.local"; // Адреса MQTT-брокера
const char* mqtt_user = "esp321";                // Користувач MQTT
const char* mqtt_password = "111";            // Пароль MQTT
const int mqtt_port = 1883;                   // Порт MQTT
const char* mqtt_topic = "home/gate";         // Топік для отримання команд

WiFiClient espClient;
PubSubClient client(espClient);

// Налаштування пінів
const int BUTTON_PIN = 12; // Пін для кнопки
const int LED_OPEN = 13;   // LED для відкриття
const int LED_CLOSE = 14;  // LED для закриття

// Налаштування двигуна
const int MOTOR_PIN1 = 5;  // Котушка A+
const int MOTOR_PIN2 = 18; // Котушка A-
const int MOTOR_PIN3 = 19; // Котушка B+
const int MOTOR_PIN4 = 21; // Котушка B-

AccelStepper stepper(AccelStepper::HALF4WIRE, MOTOR_PIN1, MOTOR_PIN3, MOTOR_PIN2, MOTOR_PIN4);

// Константи та змінні
bool isGateOpen = false;       // Статус воріт (відкриті/закриті)
bool isGateMoving = false;     // Статус руху воріт
unsigned long closeTimer = 0;  // Таймер для автоматичного закриття
unsigned long powerOnDelayStart = 0; // Час початку очікування після включення живлення
const unsigned long powerOnDelay = 60000; // 1 хвилина очікування

// EEPROM адреси для збереження станів
const int EEPROM_ADDRESS_GATE_STATE = 0;
const int EEPROM_ADDRESS_GATE_MOVING = 1;

// Збереження стану воріт у EEPROM
void saveGateState(bool state, bool moving) {
  EEPROM.write(EEPROM_ADDRESS_GATE_STATE, state ? 1 : 0);
  EEPROM.write(EEPROM_ADDRESS_GATE_MOVING, moving ? 1 : 0);
  EEPROM.commit();
}

// Завантаження стану воріт з EEPROM
void loadGateState(bool &state, bool &moving) {
  state = EEPROM.read(EEPROM_ADDRESS_GATE_STATE) == 1;
  moving = EEPROM.read(EEPROM_ADDRESS_GATE_MOVING) == 1;
}

// Відкриття воріт
void openGate() {
  Serial.println("[INFO] Відкриття воріт на 90 градусів...");
  digitalWrite(LED_OPEN, HIGH);
  digitalWrite(LED_CLOSE, LOW);

  stepper.moveTo(1024); // 90 градусів
  isGateMoving = true;
  saveGateState(isGateOpen, isGateMoving);

  while (stepper.distanceToGo() != 0) {
    stepper.run();
  }

  isGateOpen = true;
  isGateMoving = false;
  saveGateState(isGateOpen, isGateMoving);
  closeTimer = millis(); // Запускаємо таймер для автоматичного закриття
  Serial.println("[INFO] Ворота відкриті.");
}

// Закриття воріт
void closeGate() {
  Serial.println("[INFO] Закриття воріт...");
  digitalWrite(LED_OPEN, LOW);
  digitalWrite(LED_CLOSE, HIGH);

  stepper.moveTo(0); // Повертаємося до початкового положення
  isGateMoving = true;
  saveGateState(isGateOpen, isGateMoving);

  while (stepper.distanceToGo() != 0) {
    stepper.run();
  }

  isGateOpen = false;
  isGateMoving = false;
  saveGateState(isGateOpen, isGateMoving);
  Serial.println("[INFO] Ворота закриті.");
}

// Функція обробки повідомлень MQTT
void callback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.print("Отримано повідомлення: ");
  Serial.println(message);

  if (message == "OPEN") {
    if (!isGateOpen) {
      openGate(); // Відкрити ворота, якщо вони закриті
    } else {
      closeTimer = millis(); // Обнулення таймера, якщо ворота вже відкриті
      Serial.println("[INFO] Таймер оновлено. Ворота залишаються відкритими.");
    }
  }
}

// Налаштування Wi-Fi
void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Підключення до Wi-Fi: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("Wi-Fi підключено");
  Serial.print("IP-адреса: ");
  Serial.println(WiFi.localIP());
}

// Підключення до MQTT
void reconnect() {
  while (!client.connected()) {
    Serial.print("Підключення до MQTT...");
    if (client.connect("ESP32Client", mqtt_user, mqtt_password)) {
      Serial.println("Підключено до MQTT!");
      client.subscribe(mqtt_topic); // Підписка на топік
    } else {
      Serial.print("Помилка підключення: ");
      Serial.println(client.state());
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("[INFO] Запуск ESP32...");

  // Ініціалізація EEPROM
  EEPROM.begin(2); // Виділяємо 2 байти для збереження стану воріт

  // Налаштування пінів
  pinMode(BUTTON_PIN, INPUT_PULLUP);  // Кнопка з підтяжкою
  pinMode(LED_OPEN, OUTPUT);
  pinMode(LED_CLOSE, OUTPUT);
  digitalWrite(LED_OPEN, LOW);
  digitalWrite(LED_CLOSE, LOW);

  // Налаштування двигуна
  stepper.setMaxSpeed(200);       // Максимальна швидкість
  stepper.setAcceleration(100);   // Прискорення

  // Підключення до Wi-Fi та MQTT
  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  // Відновлення стану воріт з EEPROM
  loadGateState(isGateOpen, isGateMoving);
  powerOnDelayStart = millis(); // Початок відліку затримки після включення живлення

  if (isGateMoving) {
    Serial.println("[INFO] Завершення руху після відновлення живлення...");
    if (isGateOpen) {
      closeGate(); // Завершити закриття
    } else {
      openGate(); // Завершити відкриття
    }
  } else if (isGateOpen) {
    Serial.println("[INFO] Ворота залишаються відкритими після відновлення живлення.");
    stepper.setCurrentPosition(1024); // Ставимо мотор у положення відкритих воріт
  } else {
    Serial.println("[INFO] Ворота залишаються закритими після відновлення живлення.");
    stepper.setCurrentPosition(0); // Ставимо мотор у положення закритих воріт
  }
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  // Перевірка затримки після включення живлення
  if (millis() - powerOnDelayStart < powerOnDelay) {
    return; // Зачекайте перед закриттям воріт
  }

  // Обробка кнопки
  if (digitalRead(BUTTON_PIN) == LOW) {
    Serial.println("[INFO] Кнопка натиснута!");
    delay(50); // Антидребезг
    if (digitalRead(BUTTON_PIN) == LOW) {
      if (!isGateOpen) {
        openGate();
      } else {
        closeGate();
      }
    }
    delay(1000); // Затримка для уникнення повторного натискання
  }

  // Автоматичне закриття воріт
  if (isGateOpen && (millis() - closeTimer > 60000)) { // 1 хвилина
    closeGate();
  }
}
