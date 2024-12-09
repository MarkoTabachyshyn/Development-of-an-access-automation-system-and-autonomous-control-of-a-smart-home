#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h> // Бібліотека для дисплея

// Wi-Fi налаштування
const char* ssid = "ASUS";
const char* password = "Ebanina31";

// MQTT налаштування
const char* mqtt_server = "raspberrypi.local";
const int mqtt_port = 1883;
const char* mqtt_user = "esp322";
const char* mqtt_password = "222";

const char* topic_check_rfid = "home/door/check_rfid";
const char* topic_add_master = "home/door/add_master";
const char* topic_add_user = "home/door/add_user";
const char* topic_check_password = "home/door/check_password";
const char* topic_response = "home/door/response";
const char* topic_status = "home/door/status";

// Піни
#define SERVO_PIN 15
#define RST_PIN 16
#define SS_PIN 17
#define DOOR_SENSOR_PIN 4 // Датчик дверей

// Піни для дисплея
#define RS 5
#define EN 18
#define D4 19
#define D5 21
#define D6 22
#define D7 23

// Ініціалізація дисплея (2 рядки, 16 символів)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Клавіатура
#define ROW_NUM 4
#define COL_NUM 4
char keys[ROW_NUM][COL_NUM] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}};
byte rowPins[ROW_NUM] = {32, 33, 25, 26};
byte colPins[COL_NUM] = {27, 14, 12, 13};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROW_NUM, COL_NUM);

// Об'єкти
Servo doorServo;
WiFiClient espClient;
PubSubClient client(espClient);
MFRC522 rfid(SS_PIN, RST_PIN);

// Глобальні змінні
String masterCard = "";
String enteredPassword = "";
bool awaitingUserCard = false;
unsigned long masterCardScanTime = 0;
const unsigned long userCardDelay = 5000;
bool awaitingResponse = false;
bool isMasterCardSet = false;
bool isDoorOpen = false;
bool lastDoorSensorState = true;
unsigned long lastCardScanTime = 0;
unsigned long lastDoorSensorCheckTime = 0;
unsigned long doorOpenTime = 0;
const unsigned long cardScanDelay = 3000;
const unsigned long doorAutoCloseDelay = 10000;
const unsigned long doorSensorCheckInterval = 2000;

// Прототипи функцій
void updateDisplay(const char* line1, const char* line2);
void setupWiFi();
void callback(char* topic, byte* payload, unsigned int length);
void unlockDoor();
void lockDoor();
void reconnect();
void handleKeypadInput(char key);
void handleRFID();
void checkDoorSensor();

void setup() {
    Serial.begin(115200);
    Serial.println("[INFO] Запуск програми...");

    // Ініціалізація дисплея
    lcd.init(); // Initialize the LCD
    lcd.backlight(); // Turn on the backlight

    // Підключення до Wi-Fi
    setupWiFi();

    // Налаштування MQTT
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(callback);

    // Ініціалізація RFID і серво
    rfid.PCD_Init();
    doorServo.attach(SERVO_PIN);

    // Налаштування пінів
    pinMode(DOOR_SENSOR_PIN, INPUT_PULLUP);

    // Тестування дверей
    doorServo.write(180); // Відкрити двері
    delay(3000);
    doorServo.write(0);   // Закрити двері
    delay(3000);

    updateDisplay("System:", "Ready");
}

void loop() {
    if (!client.connected()) reconnect();
    client.loop();

    char key = keypad.getKey();
    if (key) handleKeypadInput(key);

    handleRFID();

    if (isDoorOpen && millis() - doorOpenTime > doorAutoCloseDelay) {
        lockDoor();
        updateDisplay("Door:", "Closed");
    }

    checkDoorSensor();
}

void updateDisplay(const char* line1, const char* line2) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(line1);
    lcd.setCursor(0, 1);
    lcd.print(line2);
}

void setupWiFi() {
    Serial.print("[INFO] Підключення до Wi-Fi: ");
    Serial.println(ssid);
    updateDisplay("WiFi:", "Connecting...");
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    updateDisplay("WiFi:", "Connected");
    Serial.println("\n[INFO] Wi-Fi підключено");
    Serial.print("[INFO] IP-адреса: ");
    Serial.println(WiFi.localIP());
}

void callback(char* topic, byte* payload, unsigned int length) {
    String message;
    for (int i = 0; i < length; i++) {
        message += (char)payload[i];
    }

    Serial.printf("[INFO] Отримано повідомлення на топік: %s, повідомлення: %s\n", topic, message.c_str());

    awaitingResponse = false;

    if (String(topic) == topic_response) {
        if (message == "VALID") {
            unlockDoor();
            updateDisplay("Access:", "Granted");
        } else if (message == "INVALID") {
            updateDisplay("Access:", "Denied");
        } else if (message == "MASTER_ADDED") {
            isMasterCardSet = true;
            updateDisplay("Master:", "Card Added");
        } else if (message == "USER_ADDED") {
            updateDisplay("User:", "Card Added");
        }
    }
}

void unlockDoor() {
    if (!isDoorOpen) {
        for (int pos = 0; pos <= 180; pos++) {
            doorServo.write(pos);
            delay(15);
        }
        isDoorOpen = true;
        doorOpenTime = millis();
        client.publish(topic_status, "DOOR_OPEN");
        updateDisplay("Door:", "Opened");
    }
}

void lockDoor() {
    if (isDoorOpen) {
        for (int pos = 180; pos >= 0; pos--) {
            doorServo.write(pos);
            delay(15);
        }
        isDoorOpen = false;
        client.publish(topic_status, "DOOR_CLOSED");
    }
}

void reconnect() {
    while (!client.connected()) {
        if (client.connect("ESP32_Door", mqtt_user, mqtt_password)) {
            client.subscribe(topic_response);
        } else {
            delay(5000);
        }
    }
}

void handleKeypadInput(char key) {
    if (key == '#') {
        client.publish(topic_check_password, enteredPassword.c_str());
        enteredPassword = "";
    } else if (key == '*') {
        enteredPassword = "";
    } else {
        enteredPassword += key;
    }
}

void handleRFID() {
    if (awaitingResponse) return;
    if (millis() - lastCardScanTime < cardScanDelay) return;

    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
        String cardID = "";
        for (byte i = 0; i < rfid.uid.size; i++) {
            cardID += String(rfid.uid.uidByte[i], HEX);
        }
        cardID.toUpperCase();
        lastCardScanTime = millis();

        if (masterCard == "") {
            masterCard = cardID;
            client.publish(topic_add_master, cardID.c_str());
            awaitingResponse = true;
            awaitingUserCard = true;
            masterCardScanTime = millis();
        } else if (awaitingUserCard) {
            if (millis() - masterCardScanTime >= userCardDelay) {
                client.publish(topic_add_user, cardID.c_str());
                awaitingResponse = true;
                awaitingUserCard = false;
            }
        } else {
            client.publish(topic_check_rfid, cardID.c_str());
            awaitingResponse = true;
        }

        rfid.PICC_HaltA();
    }
}

void checkDoorSensor() {
    if (millis() - lastDoorSensorCheckTime >= doorSensorCheckInterval) {
        lastDoorSensorCheckTime = millis();
        int sensorState = digitalRead(DOOR_SENSOR_PIN);

        if (sensorState == LOW && lastDoorSensorState == true) {
            client.publish(topic_status, "DOOR_CLOSED");
        } else if (sensorState == HIGH && lastDoorSensorState == false) {
            client.publish(topic_status, "DOOR_OPEN");
        }

        lastDoorSensorState = (sensorState == LOW);
    }
}