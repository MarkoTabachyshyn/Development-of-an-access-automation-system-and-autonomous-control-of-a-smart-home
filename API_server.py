from flask import Flask, request, jsonify
from flask_cors import CORS
import sqlite3
import paho.mqtt.client as mqtt
import json
import threading
import time

# Конфігурація
DB_PATH = '/home/marko/SQliteDB_Stuff/smart_home.db'
MQTT_BROKER = "raspberrypi.local"
MQTT_PORT = 1883
MQTT_USER = "rpi"
MQTT_PASSWORD = "rpi"
TOPIC_RESPONSE = "home/door/response"

# Ініціалізація Flask
app = Flask(__name__)
CORS(app)

# Ініціалізація MQTT
mqtt_client = mqtt.Client()
mqtt_client.username_pw_set(MQTT_USER, MQTT_PASSWORD)
mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
mqtt_client.loop_start()

# Підключення до бази даних
def get_db_connection():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn

# Функція для періодичної відправки бажаної температури
def send_desired_temp_periodically():
    while True:
        conn = get_db_connection()
        try:
            table_map = ["Corridor", "Kitchen", "LivingRoom", "Bedroom", "Bathroom"]
            for table in table_map:
                row = conn.execute(f"SELECT desired_temperature FROM {table} WHERE id = 1").fetchone()
                if row:
                    desired_temp = row["desired_temperature"]
                    mqtt_client.publish(f"home/room/{table.lower()}", json.dumps({"desired_temp": desired_temp}))
                    print(f"[INFO] Sent desired_temp={desired_temp} to home/room/{table.lower()}")
        except Exception as e:
            print(f"[ERROR] Failed to send desired temperature: {e}")
        finally:
            conn.close()
        time.sleep(10)

# Запуск потоку для відправки температури
threading.Thread(target=send_desired_temp_periodically, daemon=True).start()

# Функція для обробки повідомлень MQTT
def on_message(client, userdata, message):
    topic = message.topic
    payload = message.payload.decode('utf-8')
    print(f"[DEBUG] Отримано повідомлення: {payload} на топіку {topic}")

    if topic.startswith("home/room/"):
        handle_room_message(topic, payload)
    elif topic == "home/door/check_password":
        check_password_mqtt(payload)
    elif topic == "home/door/check_rfid":
        check_rfid_mqtt(payload)
    elif topic == "home/door/add_master":
        add_master_mqtt(payload)
    elif topic == "home/door/add_user":
        add_user_mqtt(payload)

# Прив’язка обробника повідомлень MQTT
mqtt_client.on_message = on_message
mqtt_client.subscribe("home/room/#")
mqtt_client.subscribe("home/door/check_password")
mqtt_client.subscribe("home/door/check_rfid")
mqtt_client.subscribe("home/door/add_master")
mqtt_client.subscribe("home/door/add_user")

# Обробка повідомлень для кімнат
def handle_room_message(topic, payload):
    room = topic.split('/')[2]
    conn = get_db_connection()

    try:
        data = json.loads(payload)
        table_map = {
            "corridor": "Corridor",
            "kitchen": "Kitchen",
            "livingroom": "LivingRoom",
            "bedroom": "Bedroom",
            "bathroom": "Bathroom",
        }

        table_name = table_map.get(room)
        if not table_name:
            print(f"[WARNING] Unknown room: {room}")
            return

        if room == "kitchen":
            conn.execute(f"""
                UPDATE {table_name}
                SET light_state = ?, current_temperature = ?, fire_detected = ?, timestamp = CURRENT_TIMESTAMP
                WHERE id = 2;
            """, (data.get("light"), data.get("temp"), data.get("fire", "no")))
        elif room == "bedroom":
            conn.execute(f"""
                UPDATE {table_name}
                SET light_state = ?, current_temperature = ?, curtains_state = ?, timestamp = CURRENT_TIMESTAMP
                WHERE id = 2;
            """, (data.get("light"), data.get("temp"), data.get("curtains", "closed")))
        elif room == "bathroom":
            conn.execute(f"""
                UPDATE {table_name}
                SET light_state = ?, current_temperature = ?, fan_state = ?, timestamp = CURRENT_TIMESTAMP
                WHERE id = 2;
            """, (data.get("light"), data.get("temp"), data.get("fan", "off")))
        else:
            conn.execute(f"""
                UPDATE {table_name}
                SET light_state = ?, current_temperature = ?, timestamp = CURRENT_TIMESTAMP
                WHERE id = 2;
            """, (data.get("light"), data.get("temp")))

        conn.commit()
        print(f"[INFO] ESP32 data for {room} updated in row 2.")
    except Exception as e:
        print(f"[ERROR] Failed to process message for {room}: {e}")
    finally:
        conn.close()

# Flask маршрути для встановлення бажаної температури
@app.route('/api/<room>/set-desired-temp', methods=['POST'])
def set_desired_temp(room):
    data = request.json
    desired_temp = data.get('desired_temp')
    if desired_temp is None:
        return jsonify({'error': 'No desired_temp provided'}), 400

    table_map = {
        "corridor": "Corridor",
        "kitchen": "Kitchen",
        "livingroom": "LivingRoom",
        "bedroom": "Bedroom",
        "bathroom": "Bathroom",
    }

    table_name = table_map.get(room)
    if not table_name:
        return jsonify({"error": f"Invalid room: {room}"}), 400

    conn = get_db_connection()
    try:
        conn.execute(f"""
            UPDATE {table_name}
            SET desired_temperature = ?, timestamp = CURRENT_TIMESTAMP
            WHERE id = 1;
        """, (desired_temp,))
        conn.commit()
        mqtt_client.publish(f"home/room/{room}", json.dumps({"desired_temp": desired_temp}))
        print(f"[INFO] Desired temperature for {room} updated and published.")
        return jsonify({'message': f'desired_temp updated for {room}'}), 200
    except Exception as e:
        print(f"[ERROR] Failed to update desired_temp for {room}: {e}")
        return jsonify({'error': str(e)}), 500
    finally:
        conn.close()

# Функції для дверей
def check_password_mqtt(password):
    conn = get_db_connection()
    stored_password = conn.execute('SELECT password FROM door_passwords LIMIT 1').fetchone()
    conn.close()
    if stored_password and stored_password['password'] == password:
        mqtt_client.publish(TOPIC_RESPONSE, "VALID")
    else:
        mqtt_client.publish(TOPIC_RESPONSE, "INVALID")

def check_rfid_mqtt(card_id):
    conn = get_db_connection()
    card = conn.execute('SELECT * FROM rfid_cards WHERE card_id = ?', (card_id,)).fetchone()
    conn.close()
    if card:
        mqtt_client.publish(TOPIC_RESPONSE, "VALID")
    else:
        mqtt_client.publish(TOPIC_RESPONSE, "INVALID")

def add_master_mqtt(card_id):
    conn = get_db_connection()
    is_master_set = conn.execute('SELECT * FROM rfid_cards WHERE type = "master"').fetchone()
    if is_master_set:
        mqtt_client.publish(TOPIC_RESPONSE, "MASTER_ALREADY_EXISTS")
    else:
        try:
            conn.execute('INSERT INTO rfid_cards (card_id, type) VALUES (?, "master")', (card_id,))
            conn.commit()
            mqtt_client.publish(TOPIC_RESPONSE, "ADD_MASTER")
        except sqlite3.IntegrityError:
            mqtt_client.publish(TOPIC_RESPONSE, "DUPLICATE_CARD")
    conn.close()

def add_user_mqtt(card_id):
    conn = get_db_connection()
    try:
        conn.execute('INSERT INTO rfid_cards (card_id, type) VALUES (?, "user")', (card_id,))
        conn.commit()
        mqtt_client.publish(TOPIC_RESPONSE, "ADD_USER")
    except sqlite3.IntegrityError:
        mqtt_client.publish(TOPIC_RESPONSE, "DUPLICATE_CARD")
    conn.close()

# REST API для роботи з дверима
@app.route('/api/check-password', methods=['POST'])
def check_password():
    data = request.json
    password = data.get('password')
    if not password:
        return jsonify({'status': 'error', 'message': 'Password is required'}), 400
    conn = get_db_connection()
    stored_password = conn.execute('SELECT password FROM door_passwords LIMIT 1').fetchone()
    conn.close()
    if stored_password and stored_password['password'] == password:
        mqtt_client.publish(TOPIC_RESPONSE, "VALID")
        return jsonify({'status': 'success', 'message': 'Password valid'})
    else:
        mqtt_client.publish(TOPIC_RESPONSE, "INVALID")
        return jsonify({'status': 'error', 'message': 'Password invalid'})

@app.route('/api/check-rfid', methods=['POST'])
def check_rfid():
    data = request.json
    card_id = data.get('card_id')
    if not card_id:
        return jsonify({'status': 'error', 'message': 'Card ID is required'}), 400
    conn = get_db_connection()
    card = conn.execute('SELECT * FROM rfid_cards WHERE card_id = ?', (card_id,)).fetchone()
    conn.close()
    if card:
        mqtt_client.publish(TOPIC_RESPONSE, "VALID")
        return jsonify({'status': 'success', 'message': 'Card valid'})
    else:
        mqtt_client.publish(TOPIC_RESPONSE, "INVALID")
        return jsonify({'status': 'error', 'message': 'Card invalid'})

@app.route('/api/add-master', methods=['POST'])
def add_master():
    data = request.json
    card_id = data.get('card_id')
    if not card_id:
        return jsonify({'status': 'error', 'message': 'Card ID is required'}), 400
    conn = get_db_connection()
    is_master_set = conn.execute('SELECT * FROM rfid_cards WHERE type = "master"').fetchone()
    if is_master_set:
        conn.close()
        mqtt_client.publish(TOPIC_RESPONSE, "MASTER_ALREADY_EXISTS")
        return jsonify({'status': 'error', 'message': 'Master card already exists'}), 400
    try:
        conn.execute('INSERT INTO rfid_cards (card_id, type) VALUES (?, "master")', (card_id,))
        conn.commit()
        conn.close()
        mqtt_client.publish(TOPIC_RESPONSE, "ADD_MASTER")
        return jsonify({'status': 'success', 'message': 'Master card added'})
    except sqlite3.IntegrityError:
        conn.close()
        return jsonify({'status': 'error', 'message': 'Card ID already exists'}), 400

@app.route('/api/add-user', methods=['POST'])
def add_user():
    data = request.json
    card_id = data.get('card_id')
    if not card_id:
        return jsonify({'status': 'error', 'message': 'Card ID is required'}), 400
    conn = get_db_connection()
    try:
        conn.execute('INSERT INTO rfid_cards (card_id, type) VALUES (?, "user")', (card_id,))
        conn.commit()
        conn.close()
        mqtt_client.publish(TOPIC_RESPONSE, "ADD_USER")
        return jsonify({'status': 'success', 'message': 'User card added'})
    except sqlite3.IntegrityError:
        conn.close()
        return jsonify({'status': 'error', 'message': 'Card ID already exists'}), 400

# Запуск Flask API
if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000)
