from flask import Flask, jsonify, request, render_template
import redis
import json
import paho.mqtt.publish as publish
from datetime import datetime
from supabase import create_client
import uuid

app = Flask(__name__)

# ================= CONFIG =================
REDIS_HOST = "localhost"
REDIS_PORT = 6379

MQTT_BROKER = "192.168.0.103"

SUPABASE_URL = "https://kbfclhdcnttemiwxsezf.supabase.co"
SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImtiZmNsaGRjbnR0ZW1pd3hzZXpmIiwicm9sZSI6ImFub24iLCJpYXQiOjE3Njg4OTU0MzIsImV4cCI6MjA4NDQ3MTQzMn0.mjSevT2RwvHX3hHJLwru0YqkfPxvWOgzcvmOkpBWqbA" # Thay bằng Project API anon key của bạn

NODES = ["NODE_01", "NODE_02", "NODE_03"]
# =========================================

rds = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, decode_responses=True)
supabase = create_client(SUPABASE_URL, SUPABASE_KEY)

# ===================== UI =====================

@app.route("/")
def dashboard():
    return render_template("dashboard.html", nodes=NODES)

# ===================== API =====================

@app.route("/api/node/<node_id>/status")
def node_status(node_id):
    if node_id not in NODES:
        return jsonify({"error": "invalid node"}), 404

    raw = rds.get(f"node:status:{node_id}")
    if not raw:
        return jsonify({
            "node_id": node_id,
            "current_status": "OFFLINE"
        })

    data = json.loads(raw)

    last_update = datetime.fromisoformat(data["updated_at"])
    uptime_sec = int((datetime.now() - last_update).total_seconds())

    rssi = data.get("rssi", -100)
    if rssi > -70:
        quality = "GOOD"
    elif rssi > -90:
        quality = "WEAK"
    else:
        quality = "BAD"

    data.update({
        "node_id": node_id,
        "uptime_sec": uptime_sec,
        "rssi_quality": quality
    })

    return jsonify(data)


@app.route("/api/node/<node_id>/measurements")
def node_measurements(node_id):
    res = (
        supabase
        .table("measurements")
        .select("created_at,temp,humi,soil,light")
        .eq("node_id", node_id)
        .order("created_at", desc=True)
        .limit(60)
        .execute()
    )
    return jsonify(res.data)

# ====== NEW: CHECK PENDING ======
@app.route("/api/node/<node_id>/pending")
def node_pending(node_id):
    cmd_id = rds.get(f"node:pending:{node_id}")
    if not cmd_id:
        return jsonify({"pending": False})

    data = rds.get(f"cmd:pending:{cmd_id}")
    if not data:
        return jsonify({"pending": False})

    return jsonify({
        "pending": True,
        "cmd": json.loads(data)
    })

# ====== CONTROL ======
@app.route("/control", methods=["POST"])
def control():
    data = request.json
    node_id = data["node_id"]
    action = data["action"]

    if node_id not in NODES:
        return jsonify({"error": "invalid node"}), 400

    status_raw = rds.get(f"node:status:{node_id}")
    if not status_raw:
        return jsonify({
            "error": "node offline",
            "status": "REJECTED"
        }), 409
    
    cmd_id = str(uuid.uuid4())
    now = datetime.now().isoformat()

    payload = {
        "cmd_id": cmd_id,
        "node_id": node_id,
        "component": "PUMP",
        "action": action,
        "source": "WEB",
        "ts": now
    }

    # 1️⃣ Save pending command
    rds.set(
        f"cmd:pending:{cmd_id}",
        json.dumps({
            "node_id": node_id,
            "action": action,
            "status": "PENDING",
            "created_at": now
        }),
        ex=10
    )

    # 2️⃣ Map pending by node
    rds.set(
        f"node:pending:{node_id}",
        cmd_id,
        ex=10
    )

    # 3️⃣ Publish MQTT
    publish.single(
        f"factory/control/{node_id}/cmd",
        payload=json.dumps(payload),
        hostname=MQTT_BROKER,
        qos=1
    )

    return jsonify({
        "cmd_id": cmd_id,
        "status": "PENDING"
    })

# ===================== START =====================
if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8080, debug=False)
