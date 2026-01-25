from flask import Flask, request, jsonify
import paho.mqtt.client as mqtt
import redis
import json
from datetime import datetime

# ================= CONFIG =================
MQTT_BROKER = "localhost"
MQTT_PORT = 1883
MQTT_TOPICS = [
    ("garden/status", 1),
    ("factory/control/+/ack", 1),
    ("factory/control/+/stat", 1)
]

GATEWAY_WHITELIST = {"ESP32_GATEWAY_01"}
NODE_WHITELIST = {"NODE_01", "NODE_02", "NODE_03"}

REDIS_HOST = "localhost"
REDIS_PORT = 6379

HTTP_PORT = 5000   # HTTP ingest riêng
# =========================================

app = Flask(__name__)
rds = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, decode_responses=True)

# ---------- MQTT HANDLER ----------

def handle_operating_status(payload_json):
    # data = json.loads(payload_json)
    data = payload_json
    node_id = data["node_id"]
    now = datetime.now().isoformat()

    # ==============================
    # 1️⃣ Realtime cache cho Flask
    # ==============================
    rds.set(
        f"node:status:{node_id}",
        json.dumps({
            "rssi": data.get("rssi"),
            "pump": data.get("pump"),
            "mode": data.get("mode"),
            "amp": data.get("amp"),
            "flow": data.get("flow"),
            "last_soil": data.get("last_soil"),
            "current_status": data.get("current_status"),
            "updated_at": now
        }),
        ex=15
    )

    # ==============================
    # 2️⃣ DB queues (tách riêng)
    # ==============================
    rds.lpush("queue:db:node_status", json.dumps({
        "node_id": node_id,
        "rssi": data.get("rssi"),
        "current_status": data.get("current_status"),
        "updated_at": now
    }))

    rds.lpush("queue:db:device_status", json.dumps({
        "component_id": f"{node_id}_PUMP_01",
        "node_id": node_id,
        "component_type": "PUMP",
        "current_status": data.get("pump"),
        "trigger_source": data.get("mode"),
        "current_consumption": data.get("amp"),
        "flow_rate": data.get("flow"),
        "last_value": data.get("last_soil"),
        "updated_at": now
    }))

    print(f"[REDIS] Updated realtime & queued status {node_id}")


def handle_command_feedback(payload_json):
    # data = json.loads(payload_json)
    data = payload_json
    cmd_id = data["cmd_id"]

    # update pending → confirmed
    rds.set(
        f"cmd:feedback:{cmd_id}",
        json.dumps(data),
        ex=30
    )

    rds.delete(f"cmd:pending:{cmd_id}")

    print(f"[CMD-ACK] {cmd_id} → {data['result']}")


def on_connect(client, userdata, flags, rc):
    if rc == 0:
        client.subscribe(MQTT_TOPICS)
        print("[MQTT] Connected & subscribed")

# def on_message(client, userdata, msg):
#     payload = msg.payload.decode()
#     if msg.topic == "garden/status":
#         handle_operating_status(payload)
#     elif "stat" in msg.topic:
#         handle_command_feedback(payload)

def on_message(client, userdata, msg):
    data = json.loads(msg.payload.decode())
    now = datetime.now().isoformat()
    node_id = data.get("node_id")

    if node_id not in NODE_WHITELIST:
        print(f"[SECURITY] Reject MQTT from unknown node: {node_id}")
        return
    
    if msg.topic.endswith("/ack"):
        node_id = data["node_id"]
        cmd_id  = data["cmd_id"]

        rds.delete(f"cmd:pending:{cmd_id}")
        rds.delete(f"node:pending:{node_id}")

        current_status_raw = rds.get(f"node:status:{node_id}")
        if current_status_raw:
            status_data = json.loads(current_status_raw)
        else:
            status_data = {}

        status_data.update({
            "pump": data["pump"],
            "mode": data["mode"],
            "updated_at": now
        })

        rds.set(f"node:status:{node_id}", json.dumps(status_data), ex=15)
        handle_command_feedback(data)
        print(f"🚀 [UI FAST-TRACK] Forced update {node_id} pump to {data['pump']}")

    elif msg.topic == "garden/status":
        node_id = data["node_id"]
        rds.set(
            f"node:status:{node_id}",
            json.dumps(data),
            ex=15
        )
        handle_operating_status(data)

# ---------- HTTP INGEST (BATCH) ----------

@app.route("/api/batch", methods=["POST"])
def ingest_batch():
    data = request.json

    gateway_id = data.get("gateway_id")

    if gateway_id not in GATEWAY_WHITELIST:
        return jsonify({"error": "Invalid gateway"}), 403

    measurements = data.get("measurements", [])

    for m in measurements:
        if m["node_id"] not in NODE_WHITELIST:
            print(f"[SECURITY] Drop measurement from {m['node_id']}")
            continue

        rds.lpush("queue:measurements", json.dumps({
            "node_id": m["node_id"],
            "temp": m["temp"],
            "humi": m["humi"],
            "soil": m["soil"],
            "light": m["light"],
            "created_at": m.get("timestamp") or datetime.now().isoformat()
        }))

    print(f"[QUEUE] {len(measurements)} measurements queued")
    return jsonify({"status": "ok", "count": len(measurements)})

# ---------- START ----------

def start():
    mqtt_client = mqtt.Client(client_id="INGESTION_WORKER")
    mqtt_client.on_connect = on_connect
    mqtt_client.on_message = on_message
    mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
    mqtt_client.loop_start()

    app.run(host="0.0.0.0", port=HTTP_PORT)

if __name__ == "__main__":
    start()
