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

MQTT_BROKER = "localhost"

SUPABASE_URL = "https://kbfclhdcnttemiwxsezf.supabase.co"
SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImtiZmNsaGRjbnR0ZW1pd3hzZXpmIiwicm9sZSI6ImFub24iLCJpYXQiOjE3Njg4OTU0MzIsImV4cCI6MjA4NDQ3MTQzMn0.mjSevT2RwvHX3hHJLwru0YqkfPxvWOgzcvmOkpBWqbA" # Thay bằng Project API anon key của bạn

NODES = ["NODE_01", "NODE_02", "NODE_03"]
# =========================================

def now_iso():
    return datetime.now().isoformat()

def safe_json(raw):
    try:
        return json.loads(raw)
    except Exception:
        return None

rds = redis.Redis(
    host=REDIS_HOST,
    port=REDIS_PORT,
    decode_responses=True,
    socket_connect_timeout=2,
    socket_timeout=2
)

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

    # 1️⃣ REALTIME (REDIS)
    raw = rds.get(f"node:status:{node_id}")
    if not raw:
        return jsonify({
            "node_id": node_id,
            "current_status": "OFFLINE"
        })

    realtime = safe_json(raw)
    if not realtime:
        return jsonify({
            "node_id": node_id,
            "current_status": "ERROR"
        })

    # 2️⃣ HISTORY (DB)
    node_db = (
        supabase
        .table("node_status")
        .select("previous_status, duration_minutes")
        .eq("node_id", node_id)
        .limit(1)
        .execute()
    )

    device_db = (
        supabase
        .table("device_status")
        .select("trigger_source")
        .eq("node_id", node_id)
        .eq("component_type", "PUMP")
        .limit(1)
        .execute()
    )

    history = {}
    if node_db.data:
        history.update(node_db.data[0])
    if device_db.data:
        history.update(device_db.data[0])

    # 3️⃣ MERGE
    merged = {
        **realtime,
        "previous_status": history.get("previous_status"),
        "duration_minutes": history.get("duration_minutes"),
        "trigger_source": history.get("trigger_source")
    }

    return jsonify(merged)

# ------------------------------------------------

@app.route("/api/node/<node_id>/measurements")
def node_measurements(node_id):
    if node_id not in NODES:
        return jsonify([])

    try:
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
    except Exception as e:
        print("[MEASUREMENTS][ERROR]", e)
        return jsonify([])

# ====== CHECK PENDING CMD ======
@app.route("/api/node/<node_id>/pending")
def node_pending(node_id):
    try:
        cmd_id = rds.get(f"node:pending:{node_id}")
        if not cmd_id:
            return jsonify({"pending": False})

        raw = rds.get(f"cmd:pending:{cmd_id}")
        if not raw:
            rds.delete(f"node:pending:{node_id}")
            return jsonify({"pending": False})

        return jsonify({
            "pending": True,
            "cmd": safe_json(raw)
        })
    except Exception:
        return jsonify({"pending": False})

# ====== CONTROL ======
@app.route("/control", methods=["POST"])
def control():
    data = request.json or {}
    node_id = data.get("node_id")
    action = data.get("action")

    if node_id not in NODES or action not in ["ON", "OFF"]:
        return jsonify({"error": "invalid request"}), 400

    if not rds.get(f"node:status:{node_id}"):
        return jsonify({
            "error": "node offline",
            "status": "REJECTED"
        }), 409

    cmd_id = str(uuid.uuid4())
    now = now_iso()

    mqtt_payload = {
        "cmd_id": cmd_id,
        "node_id": node_id,
        "component": "PUMP",
        "action": action,
        "source": "WEB",
        "ts": now
    }

    rds.set(
        f"cmd:pending:{cmd_id}",
        json.dumps({
            "node_id": node_id,
            "action": action,
            "status": "PENDING",
            "created_at": now
        }),
        ex=15
    )

    rds.set(
        f"node:pending:{node_id}",
        cmd_id,
        ex=15
    )

    publish.single(
        f"garden/control/{node_id}/cmd",
        payload=json.dumps(mqtt_payload),
        hostname=MQTT_BROKER,
        qos=1
    )

    return jsonify({
        "cmd_id": cmd_id,
        "status": "PENDING"
    })

@app.route("/api/node/<node_id>/feedback")
def node_feedback(node_id):
    try:
        raw = rds.get(f"node:status:{node_id}")
        if not raw:
            return jsonify({"ok": False})

        data = safe_json(raw)
        return jsonify({
            "ok": True,
            "pump": data.get("pump"),
            "mode": data.get("mode"),
            "updated_at": data.get("updated_at")
        })
    except Exception:
        return jsonify({"ok": False})

# ===================== START =====================
if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8080, debug=False)
