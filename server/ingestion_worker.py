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
    ("garden/control/ack", 1)
]

GATEWAY_WHITELIST = {"ESP32_GATEWAY_01"}
NODE_WHITELIST = {"NODE_01", "NODE_02", "NODE_03"}

REDIS_HOST = "localhost"
REDIS_PORT = 6379
HTTP_PORT = 5000
# =========================================

app = Flask(__name__)
rds = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, decode_responses=True)

# ---------- HELPERS ----------

def safe_float(val):
    try:
        return float(val)
    except Exception:
        return None

def safe_int(val):
    try:
        return int(val)
    except Exception:
        return None

def now_iso():
    return datetime.now().isoformat()

# ---------- MQTT HANDLERS ----------

def handle_operating_status(data: dict):
    try:
        node_id = data.get("node_id")
        if node_id not in NODE_WHITELIST:
            print(f"[SECURITY] Reject status from {node_id}")
            return

        payload = {
            "rssi": safe_int(data.get("rssi")),
            "pump": data.get("pump"),
            "mode": data.get("mode"),
            "amp": safe_float(data.get("amp")),
            "flow": safe_float(data.get("flow")),
            "last_soil": safe_float(data.get("last_soil")),
            "current_status": data.get("current_status"),
            "updated_at": now_iso()
        }

        # Realtime cache
        rds.set(f"node:status:{node_id}", json.dumps(payload), ex=15)

        # DB queues
        rds.lpush("queue:db:node_status", json.dumps({
            "node_id": node_id,
            "rssi": payload["rssi"],
            "current_status": payload["current_status"],
            "updated_at": payload["updated_at"]
        }))

        rds.lpush("queue:db:device_status", json.dumps({
            "component_id": f"{node_id}_PUMP_01",
            "node_id": node_id,
            "component_type": "PUMP",
            "current_status": payload["pump"],
            "trigger_source": payload["mode"],
            "current_consumption": payload["amp"],
            "flow_rate": payload["flow"],
            "last_value": payload["last_soil"],
            "updated_at": payload["updated_at"]
        }))

        print(f"[MQTT][STATUS] {node_id} pump={payload['pump']} mode={payload['mode']}")

    except Exception as e:
        print(f"[ERROR][STATUS] {e} data={data}")


def handle_command_ack(data: dict):
    try:
        node_id = data.get("node_id")
        cmd_id  = data.get("cmd_id")

        if node_id not in NODE_WHITELIST or not cmd_id:
            print("[ACK] Invalid ACK payload")
            return

        rds.delete(f"cmd:pending:{cmd_id}")
        rds.delete(f"node:pending:{node_id}")
        
        # rds.set(f"cmd:feedback:{cmd_id}", json.dumps(data), ex=30)

        # Fast UI update
        status_raw = rds.get(f"node:status:{node_id}")
        status = json.loads(status_raw) if status_raw else {}

        status.update({
            "pump": data.get("pump"),
            "mode": data.get("mode"),
            "last_soil": safe_float(data.get("last_soil")),
            "updated_at": now_iso()
        })

        rds.set(f"node:status:{node_id}", json.dumps(status), ex=15)

        print(f"[MQTT][ACK] cmd={cmd_id} node={node_id} pump={data.get('pump')}")

    except Exception as e:
        print(f"[ERROR][ACK] {e} data={data}")

# ---------- MQTT CALLBACKS ----------

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        client.subscribe(MQTT_TOPICS)
        print("[MQTT] Connected & subscribed")


def on_message(client, userdata, msg):
    try:
        data = json.loads(msg.payload.decode())
    except Exception:
        print("[MQTT][DROP] Invalid JSON")
        return

    if msg.topic == "garden/status":
        handle_operating_status(data)

    elif msg.topic == "garden/control/ack":
        handle_command_ack(data)

# ---------- HTTP INGEST (BATCH) ----------

@app.route("/api/batch", methods=["POST"])
def ingest_batch():
    try:
        data = request.json or {}
        gateway_id = data.get("gateway_id")
        batch_id   = data.get("batch_id")

        if gateway_id not in GATEWAY_WHITELIST:
            return jsonify({"error": "Invalid gateway"}), 403

        if not batch_id:
            return jsonify({"error": "Missing batch_id"}), 400

        batch_key = f"batch:{gateway_id}:{batch_id}"
        if rds.exists(batch_key):
            print(f"[HTTP][DUPLICATE] batch_id={batch_id}")
            return jsonify({"status": "duplicate"}), 409

        rds.set(batch_key, now_iso(), ex=3600)

        measurements = data.get("measurements", [])
        accepted = 0

        for m in measurements:
            try:
                node_id = m.get("node_id")
                if node_id not in NODE_WHITELIST:
                    continue

                record = {
                    "node_id": node_id,
                    "temp": safe_float(m.get("temp")),
                    "humi": safe_float(m.get("humi")),
                    "soil": safe_float(m.get("soil")),
                    "light": safe_float(m.get("light")),
                    "created_at": m.get("timestamp") or now_iso()
                }

                rds.lpush("queue:measurements", json.dumps(record))
                accepted += 1

            except Exception as e:
                print(f"[HTTP][MEAS DROP] {e} data={m}")

        print(f"[HTTP][BATCH OK] gateway={gateway_id} batch={batch_id} records={accepted}")
        return jsonify({"status": "ok", "count": accepted})

    except Exception as e:
        print(f"[HTTP][ERROR] {e}")
        return jsonify({"error": "server error"}), 500

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