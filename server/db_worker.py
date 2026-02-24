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
                batch.append({
                    "node_id": m["node_id"],
                    "temp": safe_float(m.get("temp")),
                    "humi": safe_float(m.get("humi")),
                    "soil": safe_float(m.get("soil")),
                    "light": safe_float(m.get("light")),
                    "created_at": m.get("created_at") or now_utc()
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
            try:
                node = json.loads(item[1])
                node_id = node["node_id"]
                now = now_utc()

                prev = supabase.table("node_status") \
                    .select("current_status, last_changed") \
                    .eq("node_id", node_id) \
                    .execute()

                prev_status = prev.data[0]["current_status"] if prev.data else None
                last_changed = prev.data[0]["last_changed"] if prev.data else now

                if prev_status != node["current_status"]:
                    last_changed = now

                duration = 0
                try:
                    delta = datetime.fromisoformat(now) - datetime.fromisoformat(last_changed)
                    duration = int(delta.total_seconds() / 60)
                except Exception:
                    pass

                payload = {
                    "node_id": node_id,
                    "current_status": node["current_status"],
                    "previous_status": prev_status,
                    "rssi": safe_float(node.get("rssi")),
                    "up_time_sec": safe_float(node.get("up_time_sec"), 0),
                    "last_changed": last_changed,
                    "duration_minutes": duration,
                    "updated_at": now
                }

                supabase.table("node_status").upsert(
                    payload,
                    on_conflict="node_id"
                ).execute()

                print(f"[DB-NODE] {node_id} = {payload['current_status']}")

            except Exception as e:
                print("[NODE-STATUS][ERROR]", e)

        # ==================================================
        # 3️⃣ DEVICE STATUS (PUMP)
        # ==================================================
        item = rds.brpop("queue:db:device_status", timeout=1)

        if item:
            try:
                dev = json.loads(item[1])
                cid = dev["component_id"]
                now = now_utc()

                prev = supabase.table("device_status") \
                    .select("current_status, last_changed") \
                    .eq("component_id", cid) \
                    .execute()

                prev_status = prev.data[0]["current_status"] if prev.data else None
                last_changed = prev.data[0]["last_changed"] if prev.data else now

                if prev_status != dev["current_status"]:
                    last_changed = now

                duration = 0
                try:
                    delta = datetime.fromisoformat(now) - datetime.fromisoformat(last_changed)
                    duration = int(delta.total_seconds() / 60)
                except Exception:
                    pass

                payload = {
                    "component_id": cid,
                    "node_id": dev["node_id"],
                    "component_type": dev.get("component_type", "PUMP"),
                    "current_status": dev["current_status"],
                    "previous_status": prev_status,
                    "trigger_source": dev.get("trigger_source"),
                    "control_lock": False,
                    "current_consumption": safe_float(dev.get("current_consumption"), 0),
                    "flow_rate": safe_float(dev.get("flow_rate"), 0),
                    "last_value": safe_float(dev.get("last_value")),
                    "last_changed": last_changed,
                    "duration_minutes": duration,
                    "updated_at": now
                }

                supabase.table("device_status").upsert(
                    payload,
                    on_conflict="component_id"
                ).execute()

                print(f"[DB-DEVICE] {cid} = {payload['current_status']}")

            except Exception as e:
                print("[DEVICE-STATUS][ERROR]", e)

        time.sleep(0.3)

    except Exception as e:
        print("[DB-WORKER][FATAL]", e)
        time.sleep(2)
