import redis
import json
import time
from datetime import datetime, timezone
from supabase import create_client

# ================= CONFIG =================
REDIS_HOST = "localhost"
REDIS_PORT = 6379

SUPABASE_URL = "https://kbfclhdcnttemiwxsezf.supabase.co"
SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImtiZmNsaGRjbnR0ZW1pd3hzZXpmIiwicm9sZSI6ImFub24iLCJpYXQiOjE3Njg4OTU0MzIsImV4cCI6MjA4NDQ3MTQzMn0.mjSevT2RwvHX3hHJLwru0YqkfPxvWOgzcvmOkpBWqbA" # Thay bằng Project API anon key của bạn

MEASUREMENT_BATCH_SIZE = 20
# =========================================

def safe_float(v, default=None):
    try:
        return float(v)
    except Exception:
        return default

def now_utc():
    return datetime.now(timezone.utc).isoformat()

rds = redis.Redis(
    host=REDIS_HOST,
    port=REDIS_PORT,
    decode_responses=True,
    socket_connect_timeout=3,
    socket_timeout=3
)

def safe_int(v, default=None):
    try:
        return int(float(v))
    except Exception:
        return default

supabase = create_client(SUPABASE_URL, SUPABASE_KEY)

print("[DB-WORKER] Started")

while True:
    try:
        # ==================================================
        # 1️⃣ MEASUREMENTS (BATCH)
        # ==================================================
        batch = []

        for _ in range(MEASUREMENT_BATCH_SIZE):
            item = rds.brpop("queue:measurements", timeout=1)
            if not item:
                break

            try:
                m = json.loads(item[1])

                if m.get("type") and m["type"] != "MEASUREMENT":
                    print("[MEASUREMENT][DROP] Invalid type:", m.get("type"))
                    continue

                batch.append({
                    "node_id": m["node_id"],
                    "temp": safe_float(m.get("temp")),
                    "humi": safe_float(m.get("humi")),
                    "soil": safe_float(m.get("soil")),
                    "light": safe_float(m.get("light")),
                    "created_at": m.get("created_at") or now_utc(),
                })
            except Exception as e:
                print("[MEASUREMENT][DROP]", e)

        if batch:
            supabase.table("measurements").insert(batch).execute()
            print(f"[DB] Inserted {len(batch)} measurements")

        # ==================================================
        # 2️⃣ NODE STATUS (ONLINE / OFFLINE)
        # ==================================================
        item = rds.brpop("queue:db:node_status", timeout=1)

        if item:
            node = json.loads(item[1])

            if node.get("type") != "STATUS":
                print("[DB-NODE][DROP] Invalid type:", node.get("type"))
                continue

            node_id = node["node_id"]

            if not node_id:
                print("[DB-NODE][DROP] Missing node_id")
                continue

            now = now_utc()

            exists = supabase.table("devices") \
                .select("node_id") \
                .eq("node_id", node_id) \
                .limit(1) \
                .execute()

            if not exists.data:
                print(f"[DB-NODE][SKIP] Unknown node_id={node_id}")
                continue

            prev = supabase.table("node_status") \
                .select("current_status") \
                .eq("node_id", node_id) \
                .execute()

            prev_status = prev.data[0]["current_status"] if prev.data else None

            if prev_status != node["current_status"]:

                supabase.table("node_status_history") \
                    .update({"ended_at": now}) \
                    .eq("node_id", node_id) \
                    .is_("ended_at", None) \
                    .execute()

                supabase.table("node_status_history").insert({
                    "node_id": node_id,
                    "status": node["current_status"],
                    "rssi": safe_int(node.get("rssi")),
                    "started_at": now
                }).execute()

            payload = {
                "node_id": node_id,
                "current_status": node["current_status"],
                "previous_status": prev_status,
                "rssi": safe_int(node.get("rssi")),
                "up_time_sec": node.get("uptime"),
                "last_changed": now if prev_status != node["current_status"] else None,
                "updated_at": now
            }

            supabase.table("node_status") \
                .upsert(payload, on_conflict="node_id") \
                .execute()

            print(f"[DB-NODE] {node_id} = {payload['current_status']}")

        # ==================================================
        # 3️⃣ DEVICE STATUS (PUMP)
        # ==================================================
        item = rds.brpop("queue:db:device_status", timeout=1)
        if item:
            dev = json.loads(item[1])
            if dev.get("type") != "DEVICE_STATUS":
                print("[DB-DEVICE][DROP] Invalid type:", dev.get("type"))
                continue
            
            cid = dev["component_id"]
            if not cid or not node_id:
                print("[DB-DEVICE][DROP] Missing component_id/node_id")
                continue

            now = now_utc()

            prev = supabase.table("device_status") \
                .select("current_status") \
                .eq("component_id", cid) \
                .execute()

            prev_status = prev.data[0]["current_status"] if prev.data else None

            if prev_status != dev["current_status"]:

                supabase.table("device_status_history") \
                    .update({"ended_at": now}) \
                    .eq("component_id", cid) \
                    .is_("ended_at", None) \
                    .execute()

                supabase.table("device_status_history").insert({
                    "component_id": cid,
                    "node_id": dev["node_id"],
                    "status": dev["current_status"],
                    "trigger_source": dev.get("trigger_source"),
                    "current_consumption": dev.get("amp"),
                    "flow_rate": dev.get("flow"),
                    "started_at": now
                }).execute()

            payload = {
                "component_id": cid,
                "node_id": dev["node_id"],
                "component_type": dev.get("component_type", "PUMP"),
                "current_status": dev["current_status"],
                "previous_status": prev_status,
                "trigger_source": dev.get("trigger_source"),
                "last_changed": now if prev_status != dev["current_status"] else None,
                "updated_at": now
            }

            supabase.table("device_status") \
                .upsert(payload, on_conflict="component_id") \
                .execute()

            print(f"[DB-DEVICE] {cid} = {payload['current_status']}")

        # ================= COMMAND HISTORY =================
        item = rds.brpop("queue:db:command_history", timeout=1)
        if item:
            cmd = json.loads(item[1])
            
            if cmd.get("type") != "ACK":
                print("[DB-CMD][DROP] Invalid type:", cmd.get("type"))
                continue

            supabase.table("command_history").insert({
                "cmd_id": cmd["cmd_id"],
                "node_id": cmd["node_id"],
                "component_id": f"{cmd['node_id']}_PUMP_01",
                "command": cmd["pump"],
                "trigger_source": cmd.get("source", "CLOUD"),
                "success": cmd["success"],
                "error_code": cmd.get("error_code"),
                "message": cmd.get("message"),
                "executed_at": cmd.get("executed_at"),
                "server_time": cmd.get("server_time")
            }).execute()

        time.sleep(0.2)

    except Exception as e:
        print("[DB-WORKER][FATAL]", e)
        time.sleep(2)