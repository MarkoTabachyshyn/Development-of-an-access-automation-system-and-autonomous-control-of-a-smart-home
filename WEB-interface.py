from flask import Flask, render_template, request, redirect, url_for, flash
import sqlite3
import json
import paho.mqtt.client as mqtt
import threading
import time

# Конфігурація
DB_PATH = '/home/marko/SQliteDB_Stuff/smart_home.db'
MQTT_BROKER = "raspberrypi.local"
MQTT_PORT = 1883
MQTT_USER = "rpi"
MQTT_PASSWORD = "rpi"

app = Flask(__name__)
app.secret_key = "supersecretkey"

# MQTT-клієнт
mqtt_client = mqtt.Client()
mqtt_client.username_pw_set(MQTT_USER, MQTT_PASSWORD)
mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
mqtt_client.loop_start()

def get_db_connection():
    """Підключення до бази даних."""
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn

@app.route('/')
def index():
    """Головна сторінка."""
    conn = get_db_connection()
    rooms = {}
    for room in ["Corridor", "Kitchen", "LivingRoom", "Bedroom", "Bathroom"]:
        desired = conn.execute(f"SELECT desired_temperature FROM {room} WHERE id = 1").fetchone()
        current = conn.execute(f"SELECT current_temperature, light_state FROM {room} WHERE id = 2").fetchone()
        additional_data = {}
        if room == "Bedroom":
            additional_data["curtains_state"] = conn.execute(f"SELECT curtains_state FROM {room} WHERE id = 2").fetchone()["curtains_state"]
        elif room == "Bathroom":
            additional_data["fan_state"] = conn.execute(f"SELECT fan_state FROM {room} WHERE id = 2").fetchone()["fan_state"]
        elif room == "Kitchen":
            additional_data["fire_detected"] = conn.execute(f"SELECT fire_detected FROM {room} WHERE id = 2").fetchone()["fire_detected"]
        
        rooms[room] = {
            "desired_temperature": desired["desired_temperature"] if desired else None,
            "current_temperature": current["current_temperature"] if current else None,
            "light_state": current["light_state"] if current else None,
            **additional_data,
        }
    
    password = conn.execute("SELECT password FROM door_passwords WHERE id = 1").fetchone()
    vehicles = conn.execute("SELECT * FROM allowed_vehicles").fetchall()
    conn.close()

    return render_template('index.html', rooms=rooms, password=password, vehicles=vehicles)

@app.route('/update-password', methods=['POST'])
def update_password():
    """Оновлення пароля дверей."""
    new_password = request.form['password']
    conn = get_db_connection()
    try:
        conn.execute("UPDATE door_passwords SET password = ?, updated_at = CURRENT_TIMESTAMP WHERE id = 1", (new_password,))
        conn.commit()
        flash("Пароль дверей успішно оновлено.", "success")
    except Exception as e:
        flash(f"Помилка при оновленні пароля: {e}", "error")
    finally:
        conn.close()
    return redirect(url_for('index'))

@app.route('/update-temperature', methods=['POST'])
def update_temperature():
    """Оновлення бажаної температури в кімнатах."""
    room = request.form['room']
    new_temperature = request.form['temperature']
    table_map = {
        "Corridor": "Corridor",
        "Kitchen": "Kitchen",
        "LivingRoom": "LivingRoom",
        "Bedroom": "Bedroom",
        "Bathroom": "Bathroom"
    }
    if room not in table_map:
        flash(f"Невідома кімната: {room}.", "error")
        return redirect(url_for('index'))

    conn = get_db_connection()
    try:
        conn.execute(f"UPDATE {table_map[room]} SET desired_temperature = ?, timestamp = CURRENT_TIMESTAMP WHERE id = 1", (new_temperature,))
        conn.commit()
        mqtt_client.publish(f"home/room/{room.lower()}", json.dumps({"desired_temp": new_temperature}))
        flash(f"Бажана температура для {room} успішно оновлена.", "success")
    except Exception as e:
        flash(f"Помилка при оновленні температури: {e}", "error")
    finally:
        conn.close()
    return redirect(url_for('index'))

@app.route('/add-vehicle', methods=['POST'])
def add_vehicle():
    """Додавання нового автомобіля."""
    plate_number = request.form['plate_number']
    owner_name = request.form['owner_name']
    conn = get_db_connection()
    try:
        conn.execute("INSERT INTO allowed_vehicles (plate_number, owner_name) VALUES (?, ?)", (plate_number, owner_name))
        conn.commit()
        flash(f"Автомобіль {plate_number} успішно додано.", "success")
    except sqlite3.IntegrityError:
        flash(f"Автомобіль із номером {plate_number} вже існує.", "warning")
    except Exception as e:
        flash(f"Помилка при додаванні автомобіля: {e}", "error")
    finally:
        conn.close()
    return redirect(url_for('index'))

@app.route('/delete-vehicle', methods=['POST'])
def delete_vehicle():
    """Видалення автомобіля."""
    plate_number = request.form['plate_number']
    conn = get_db_connection()
    try:
        cursor = conn.execute("DELETE FROM allowed_vehicles WHERE plate_number = ?", (plate_number,))
        conn.commit()
        if cursor.rowcount > 0:
            flash(f"Автомобіль {plate_number} успішно видалено.", "success")
        else:
            flash(f"Автомобіль із номером {plate_number} не знайдено.", "warning")
    except Exception as e:
        flash(f"Помилка при видаленні автомобіля: {e}", "error")
    finally:
        conn.close()
    return redirect(url_for('index'))

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5001)
