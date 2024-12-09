import cv2
import os
import time
import re
from datetime import datetime
import subprocess
from ultralytics import YOLO
import pytesseract
import sqlite3
import paho.mqtt.client as mqtt

# Шлях до Tesseract OCR
pytesseract.pytesseract.tesseract_cmd = "/usr/bin/tesseract"

# Шлях до бази даних SQLite
DB_PATH = "/home/marko/SQliteDB_Stuff/smart_home.db"

# MQTT налаштування
MQTT_BROKER = "raspberrypi.local"
MQTT_PORT = 1883
MQTT_USER = "rpi"
MQTT_PASSWORD = "rpi"
MQTT_TOPIC_PUBLISH = "home/gate"

# Завантаження моделей YOLO
model_cars = YOLO('yolov8n.pt')  # Використовуйте легку модель
model_plates = YOLO('license_plate_detector.pt')

# Директорії для збереження результатів
CAR_DIR = "processed_cars"
PLATE_DIR = "processed_plates"
os.makedirs(CAR_DIR, exist_ok=True)
os.makedirs(PLATE_DIR, exist_ok=True)

# Ініціалізація MQTT-клієнта
mqtt_client = mqtt.Client()
mqtt_client.username_pw_set(MQTT_USER, MQTT_PASSWORD)
mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)

# Запуск асинхронного циклу MQTT
mqtt_client.loop_start()

def adjust_brightness(image, alpha=1.2, beta=50):
    """Збільшує яскравість та контрастність зображення."""
    adjusted = cv2.convertScaleAbs(image, alpha=alpha, beta=beta)
    return adjusted

def capture_and_recognize_license_plate(output_path):
    """Захоплює зображення за допомогою libcamera-still і зберігає його у файл."""
    print("[INFO] Захоплення зображення з камери...")
    try:
        subprocess.run(["libcamera-still", "-o", output_path, "-n", "--width", "3280", "--height", "2464"], check=True)
        if os.path.exists(output_path):
            print(f"[INFO] Зображення успішно збережено: {output_path}")
            return True
        else:
            print("[ERROR] Файл зображення не знайдено після захоплення.")
            return False
    except subprocess.CalledProcessError as e:
        print(f"[ERROR] Помилка виконання команди libcamera-still: {e}")
        return False

def write_to_file(plate_texts, file_name="recognized_plates.txt"):
    """Записує розпізнані номерні знаки у текстовий файл."""
    with open(file_name, "a") as file:
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        for plate in plate_texts:
            file.write(f"{timestamp}: {plate}\n")
    print("[INFO] Номерні знаки записані у файл.")

def save_cropped_image(image, bbox, output_dir, prefix):
    """Зберігає вирізаний регіон зображення з унікальною назвою."""
    x1, y1, x2, y2 = map(int, bbox)
    cropped = image[y1:y2, x1:x2]
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    filename = f"{prefix}_{timestamp}.jpg"
    filepath = os.path.join(output_dir, filename)
    cv2.imwrite(filepath, cropped)
    print(f"[INFO] Зображення збережено: {filepath}")
    return filepath

def is_plate_allowed(plate_number):
    """Перевіряє, чи дозволений номерний знак у базі даних."""
    conn = sqlite3.connect(DB_PATH)
    cursor = conn.cursor()

    query = "SELECT COUNT(*) FROM allowed_vehicles WHERE plate_number = ?"
    cursor.execute(query, (plate_number,))
    result = cursor.fetchone()

    conn.close()

    return result[0] > 0

def open_gate():
    """Надсилає команду на відкриття воріт через MQTT."""
    mqtt_client.publish(MQTT_TOPIC_PUBLISH, "OPEN")
    print("[INFO] Команда на відкриття воріт надіслана.")

def detect_cars_and_plates(image_path):
    """Обробляє зображення: автомобілі та номерні знаки."""
    image = cv2.imread(image_path)
    if image is None:
        print(f"[ERROR] Не вдалося завантажити зображення {image_path}.")
        return

    bright_image = adjust_brightness(image)

    print("[INFO] Розпочато обробку зображення для пошуку автомобілів...")
    results_cars = model_cars(bright_image)
    detections_cars = results_cars[0].boxes.data.numpy() if results_cars[0].boxes is not None else []

    if detections_cars.size == 0:
        print("[INFO] Автомобілі не знайдено!")
        return

    plate_texts = []

    for detection in detections_cars:
        x1, y1, x2, y2, conf, cls = detection
        car_roi = bright_image[int(y1):int(y2), int(x1):int(x2)]
        save_cropped_image(bright_image, (x1, y1, x2, y2), CAR_DIR, "car")

        plate_text = detect_plate(car_roi)
        if plate_text:
            plate_text_cleaned = plate_text.replace(" ", "")
            plate_texts.append(plate_text_cleaned)

            if is_plate_allowed(plate_text_cleaned):
                print(f"[ACCESS GRANTED] Номер дозволений: {plate_text_cleaned}")
                print("[INFO] Дозволено в'їзд.")
                open_gate()
            else:
                print(f"[ACCESS DENIED] Номер не дозволений: {plate_text_cleaned}")
                print("[INFO] Заборонено в'їзд.")

    if plate_texts:
        result_line = ", ".join(plate_texts)
        print(f"[RESULT] Розпізнані номерні знаки: {result_line}")
        write_to_file(plate_texts)
    else:
        print("[RESULT] Жодного номерного знака не виявлено.")

def detect_plate(car_image):
    """Розпізнає номерний знак з області автомобіля."""
    try:
        results_plates = model_plates(car_image)
        detections_plates = results_plates[0].boxes.data.numpy() if results_plates[0].boxes is not None else []

        if detections_plates.size == 0:
            return ""

        for detection in detections_plates:
            x1, y1, x2, y2, conf, cls = detection
            plate_roi = car_image[int(y1):int(y2), int(x1):int(x2)]
            save_cropped_image(car_image, (x1, y1, x2, y2), PLATE_DIR, "plate")
            text = recognize_plate_text(plate_roi)
            return text
    except Exception as e:
        print(f"[ERROR] Помилка обробки номерного знака: {e}")
        return ""

def recognize_plate_text(plate_roi):
    """Розпізнає текст номерного знака за допомогою Tesseract OCR."""
    try:
        gray = cv2.cvtColor(plate_roi, cv2.COLOR_BGR2GRAY)
        blurred = cv2.GaussianBlur(gray, (5, 5), 0)
        _, binary = cv2.threshold(blurred, 0, 255, cv2.THRESH_BINARY | cv2.THRESH_OTSU)

        # Динамічне масштабування
        height, width = binary.shape[:2]
        if width < 100 or height < 50:
            scale = 8  # Якщо зображення дуже маленьке
        elif width > 400 or height > 200:
            scale = 2  # Якщо зображення велике
        else:
            scale = 4  # Середній випадок
        
        # Масштабування
        binary_resized = cv2.resize(binary, None, fx=scale, fy=scale, interpolation=cv2.INTER_CUBIC)
        resized_path = "processed_plates/plate_resized.jpg"
        cv2.imwrite(resized_path, binary_resized)
        print(f"[INFO] Збережено масштабоване зображення: {resized_path}")
        config = '--psm 7 --oem 3'
        raw_text = pytesseract.image_to_string(binary_resized, config=config).strip()

        return correct_plate_format(raw_text)
    except Exception as e:
        print(f"[ERROR] Помилка розпізнавання тексту: {e}")
        return ""

def correct_plate_format(raw_text):
    """Коригує текст до формату номерного знака."""
    cleaned_text = re.sub(r'[^A-ZА-ЯІЇЄ0-9]', '', raw_text.upper())
    match = re.search(r'[A-ZА-ЯІЇЄ]{2}\d{4}[A-ZА-ЯІЇЄ]{2}', cleaned_text)

    if match:
        return match.group(0)
    else:
        possible_numbers = re.findall(r'[A-ZА-ЯІЇЄ]{1,2}|\d{1,4}', cleaned_text)
        corrected = ''.join(possible_numbers[:2] + possible_numbers[-2:])
        return corrected if len(corrected) == 8 else cleaned_text

if __name__ == "__main__":
    image_path = "current_image.jpg"
    while True:
        print("[INFO] Запуск циклу...")
        if capture_and_recognize_license_plate(image_path):
            detect_cars_and_plates(image_path)
        else:
            print("[ERROR] Зображення не вдалося зберегти.")
        print("[INFO] Завершення циклу. Очікування 3 секунди...")
        time.sleep(3)
