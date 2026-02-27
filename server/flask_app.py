import json
import uuid
import time
import redis
import paho.mqtt.publish as publish
from flask import Flask, render_template, jsonify
from flask_socketio import SocketIO, emit
from datetime import datetime
from supabase import create_client
import paho.mqtt.client as mqtt
from flask import request

REDIS_HOST = "127.0.0.1"
REDIS_PORT = 6379
MQTT_BROKER = "localhost"

SUPABASE_URL = "https://kbfclhdcnttemiwxsezf.supabase.co"
SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImtiZmNsaGRjbnR0ZW1pd3hzZXpmIiwicm9sZSI6ImFub24iLCJpYXQiOjE3Njg4OTU0MzIsImV4cCI6MjA4NDQ3MTQzMn0.mjSevT2RwvHX3hHJLwru0YqkfPxvWOgzcvmOkpBWqbA" # Thay bằng Project API anon key của bạn

NODES = ["NODE_01", "NODE_02", "NODE_03"]

app = Flask(__name__)
socketio = SocketIO(app, cors_allowed_origins="*", async_mode="threading")

rds = redis.Redis(
    host=REDIS_HOST,
    port=REDIS_PORT,
    decode_responses=True,
    socket_keepalive=True,
    health_check_interval=30,
    max_connections=20
)

rds_pub = redis.Redis(
    host=REDIS_HOST,
    port=REDIS_PORT,
    decode_responses=True,
    socket_keepalive=True,
    health_check_interval=30,
    max_connections=20
)

mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)

def on_connect(client, userdata, flags, reason_code, properties):
    print("MQTT connected:", reason_code)

mqtt_client.on_connect = on_connect
mqtt_client.connect(MQTT_BROKER, 1883, 60)
mqtt_client.loop_start()

supabase = create_client(SUPABASE_URL, SUPABASE_KEY)

def now_iso():
    return datetime.now().isoformat()

def listen_to_redis_events():
    while True:
        try:
            pubsub = rds_pub.pubsub(ignore_subscribe_messages=True)
            pubsub.subscribe("node_updates", "node_ack")
            print("[REDIS] PubSub connected")

            for message in pubsub.listen():
                if message["type"] != "message":
                    continue

                channel = message["channel"]
                data = json.loads(message["data"])

                # STATUS
                if channel == "node_updates":
                    socketio.emit("node_realtime", data)

                # ACK
                elif channel == "node_ack":
                    socketio.emit("node_ack", data)

        except redis.exceptions.ConnectionError as e:
            print("[REDIS] Reconnecting in 3s...", e)
            time.sleep(3)

def check_timeout():
    while True:
        time.sleep(5)

        pending = supabase.table("command_history") \
            .select("cmd_id") \
            .eq("status", "PENDING") \
            .execute()

        for cmd in pending.data:
            cmd_id = cmd["cmd_id"]

            if not rds.exists(f"cmd:pending:{cmd_id}"):
                supabase.table("command_history") \
                    .update({"status": "TIMEOUT"}) \
                    .eq("cmd_id", cmd_id) \
                    .execute()

socketio.start_background_task(check_timeout)
socketio.start_background_task(listen_to_redis_events)

@app.route("/")
def dashboard():
    return render_template("dashboard.html", nodes=NODES)

@socketio.on("subscribe_node")
def handle_subscribe(data):
    node_id = data.get("node_id")
    if node_id not in NODES:
        return

    raw = rds.get(f"node:status:{node_id}")
    payload = json.loads(raw) if raw else {
        "node_id": node_id,
        "current_status": "OFFLINE"
    }

    payload["type"] = "STATUS"
    emit("node_realtime", payload)

@socketio.on("control")
def handle_control(data):
    node_id = data.get("node_id")
    action = data.get("action")

    if node_id not in NODES or action not in ["ON", "OFF"]:
        return

    cmd_id = data.get("cmd_id") or str(uuid.uuid4())
    component_id = f"{node_id}_PUMP_01"

    supabase.table("command_history").insert({
        "cmd_id": cmd_id,
        "node_id": node_id,
        "component_id": component_id,
        "command": action,
        "trigger_source": "CLOUD",
        "status": "PENDING",
        "sent_at": now_iso(),
        "retry_count": 0
    }).execute()

    emit("node_pending", {
        "node_id": node_id,
        "cmd_id": cmd_id
    }, to=request.sid)

    mqtt_payload = {
        "cmd_id": cmd_id,
        "node_id": node_id,
        "component": "PUMP",
        "action": action,
        "source": "WEB",
        "ts": now_iso()
    }
    
    rds.set(f"node:pending:{node_id}", cmd_id, ex=20)
    rds.set(
        f"cmd:pending:{cmd_id}",
        json.dumps({
            "node_id": node_id,
            "created_at": now_iso()
        }),
        ex=20
    )

    mqtt_client.publish(
        f"garden/control/{node_id}/cmd",
        json.dumps(mqtt_payload),
        qos=1
    )

@app.route("/api/node/<node_id>/measurements")
def get_measurements_api(node_id):
    if node_id not in NODES:
        return jsonify([])

    try:
        res = supabase.table("measurements") \
            .select("temp, humi, soil, light, created_at") \
            .eq("node_id", node_id) \
            .order("created_at", desc=True) \
            .limit(20).execute()
        return jsonify(res.data)
    except: 
        return jsonify([])

if __name__ == "__main__":
    socketio.run(app, host="0.0.0.0", port=8080, debug=False)