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

REDIS_HOST = "127.0.0.1"
REDIS_PORT = 6379
HTTP_PORT = 5000

MODE_MAP = {
    "SEN": "SENSOR",
    "CLD": "CLOUD"
}
# =========================================

app = Flask(__name__)

# ✅ Redis cấu hình chuẩn cho Windows + Docker
def create_redis():
    return redis.Redis(
        host=REDIS_HOST,
        port=REDIS_PORT,
        decode_responses=True,
        socket_keepalive=True,
        socket_timeout=5,
        retry_on_timeout=True,
        health_check_interval=30
    )

rds = create_redis()

# ---------- REDIS SAFE WRAPPER ----------

def redis_safe_call(func, *args, **kwargs):
    global rds
    try:
        return func(*args, **kwargs)
    except (redis.exceptions.ConnectionError,
            redis.exceptions.TimeoutError):
        print("[REDIS] Reconnecting...")
        rds = create_redis()
        return func(*args, **kwargs)

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
        
        old_raw = redis_safe_call(rds.get, f"node:status:{node_id}")
        old_status = json.loads(old_raw) if old_raw else {}

        payload = {
            "node_id": node_id,
            "rssi": safe_int(data.get("rssi")),
            "pump": data.get("pump"),
            "mode": data.get("mode"),
            "amp": safe_float(data.get("amp")),
            "flow": safe_float(data.get("flow")),
            "last_soil": safe_float(data.get("last_soil")),
            "uptime": safe_int(data.get("uptime")),
            "current_status": data.get("current_status"),
            "updated_at": now_iso(),
            "previous_status": "UNKNOWN",
            "source": "realtime",
            "type": "STATUS"
        }

        redis_safe_call(
            rds.set,
            f"node:status:{node_id}",
            json.dumps(payload),
            ex=30
        )

        redis_safe_call(
            rds.publish,
            "node_updates",
            json.dumps(payload)
        )

        redis_safe_call(
                rds.lpush,
                "queue:db:node_status",
                json.dumps({
                    "type": "STATUS",
                    "node_id": node_id,
                    "rssi": payload["rssi"],
                    "current_status": payload["current_status"],
                    "uptime": payload["uptime"],
                    "updated_at": payload["updated_at"]
                })
            )

        redis_safe_call(
            rds.lpush,
            "queue:db:device_status",
            json.dumps({
                "type": "DEVICE_STATUS",
                "component_id": f"{node_id}_PUMP_01",
                "node_id": node_id,
                "component_type": "PUMP",
                "current_status": payload["pump"],
                "trigger_source": MODE_MAP.get(payload["mode"], "UNKNOWN"),
                "amp": payload.get("amp"),
                "flow": payload.get("flow"),
                "updated_at": payload["updated_at"]
            })
        )

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

        pending_node = redis_safe_call(rds.get, f"cmd:pending:{cmd_id}")
        if not pending_node:
            print(f"[ACK] Unknown cmd_id {cmd_id}")
            return

        # XÓA pending
        redis_safe_call(rds.delete, f"cmd:pending:{cmd_id}")
        redis_safe_call(rds.delete, f"node:pending:{node_id}")

        ack_payload = {
            "type": "ACK",
            "node_id": node_id,
            "cmd_id": cmd_id,

            # Kết quả thực thi
            "success": data.get("success", True),  # True / False
            "error_code": data.get("error_code"),  # VD: RELAY_FAIL, LOW_VOLT
            "message": data.get("message"),        # Thông báo chi tiết

            # Trạng thái thiết bị sau khi thực thi
            "pump": data.get("pump"),
            "mode": data.get("mode"),
            "flow": safe_float(data.get("flow")),
            "amp": safe_float(data.get("amp")),
            "last_soil": safe_float(data.get("last_soil")),

            # Metadata
            "executed_at": data.get("executed_at"),
            "server_time": now_iso()
        }

        redis_safe_call(
            rds.publish,
            "node_ack",
            json.dumps(ack_payload)
        )

        redis_safe_call(
            rds.lpush,
            "queue:db:command_history",
            json.dumps({
                "cmd_id": cmd_id,
                "status": "SUCCESS" if ack_payload["success"] else "FAILED",
                "success": ack_payload["success"],
                "error_code": ack_payload.get("error_code"),
                "message": ack_payload.get("message"),
                "executed_at": ack_payload.get("executed_at"),
                "server_time": ack_payload["server_time"]
            })
        )

        redis_safe_call(
            rds.lpush,
            "queue:db:device_status",
            json.dumps({
                "type": "DEVICE_STATUS",
                "component_id": f"{node_id}_PUMP_01",
                "node_id": node_id,
                "component_type": "PUMP",
                "current_status": data.get("pump"),
                "trigger_source": "CLOUD",
                "updated_at": now_iso()
            })
        )
        print(f"[MQTT][ACK OK] cmd={cmd_id} node={node_id}")

    except Exception as e:
        print(f"[ERROR][ACK] {e} data={data}")

# ---------- MQTT CALLBACKS ----------

def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
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

# ---------- HTTP INGEST ----------

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

        if redis_safe_call(rds.exists, batch_key):
            print(f"[HTTP][DUPLICATE] batch_id={batch_id}")
            return jsonify({"status": "duplicate"}), 409

        redis_safe_call(rds.set, batch_key, now_iso(), ex=3600)

        measurements = data.get("measurements", [])
        accepted = 0
        batch_time = data.get("sent_at") or now_iso()

        for m in measurements:
            node_id = m.get("node_id")
            if node_id not in NODE_WHITELIST:
                continue

            measured_at = m.get("measured_at") or batch_time

            record = {
                "node_id": node_id,
                "temp": safe_float(m.get("temp")),
                "humi": safe_float(m.get("humi")),
                "soil": safe_float(m.get("soil")),
                "light": safe_float(m.get("light")),
                "created_at": measured_at,
                "batch_sent_at": batch_time   # nếu muốn lưu
            }
    
            redis_safe_call(
                rds.lpush,
                "queue:measurements",
                json.dumps(record)
            )

            accepted += 1

        print(f"[HTTP][BATCH OK] gateway={gateway_id} batch={batch_id} records={accepted}")
        return jsonify({"status": "ok", "count": accepted})

    except Exception as e:
        print(f"[HTTP][ERROR] {e}")
        return jsonify({"error": "server error"}), 500



# ---------- START ----------

def start():
    mqtt_client = mqtt.Client(
        client_id="INGESTION_WORKER",
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2
    )

    mqtt_client.on_connect = on_connect
    mqtt_client.on_message = on_message

    mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
    mqtt_client.loop_start()
    mqtt_client.reconnect_delay_set(min_delay=1, max_delay=5)

    app.run(host="0.0.0.0", port=HTTP_PORT)

if __name__ == "__main__":
    start()