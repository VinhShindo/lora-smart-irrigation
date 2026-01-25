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

rds = redis.Redis(
    host=REDIS_HOST,
    port=REDIS_PORT,
    decode_responses=True
)

supabase = create_client(SUPABASE_URL, SUPABASE_KEY)

print("[DB-WORKER] Started")

while True:
    try:
        # ==================================================
        # 1️⃣ SENSOR MEASUREMENTS (BATCH → measurements)
        # ==================================================
        measurement_batch = []

        for _ in range(MEASUREMENT_BATCH_SIZE):
            item = rds.brpop("queue:measurements", timeout=1)
            if item:
                measurement_batch.append(json.loads(item[1]))

        if measurement_batch:
            supabase.table("measurements").insert(
                measurement_batch
            ).execute()

            print(f"[DB] Inserted {len(measurement_batch)} measurements")

        # ==================================================
        # 2️⃣ NODE STATUS (NODE HEALTH → node_status)
        # ==================================================
        node_item = rds.brpop("queue:db:node_status", timeout=1)

        if node_item:
            node = json.loads(node_item[1])
            now = datetime.now(timezone.utc).isoformat()            # Lấy trạng thái hiện tại trong DB (nếu có)
            prev = supabase.table("node_status") \
                .select("current_status, last_changed") \
                .eq("node_id", node["node_id"]) \
                .execute()

            prev_status = None
            last_changed = datetime.now().isoformat()

            if prev.data:
                prev_status = prev.data[0]["current_status"]
                if prev_status == node["current_status"]:
                    last_changed = prev.data[0]["last_changed"]

            duration_minutes = 0
            if prev.data and prev_status == node["current_status"]:
                delta = datetime.fromisoformat(now) - datetime.fromisoformat(last_changed)
                duration_minutes = int(delta.total_seconds() / 60)

            node_payload = {
                "node_id": node["node_id"],
                "current_status": node.get("current_status", "ERROR"),
                "previous_status": prev_status,
                "rssi": node.get("rssi"),
                "up_time_sec": node.get("up_time_sec", 0),
                "last_changed": last_changed,
                "duration_minutes": duration_minutes,
                "updated_at": now
            }

            supabase.table("node_status").upsert(
                node_payload,
                on_conflict="node_id"
            ).execute()

            print(f"[DB-NODE] Upserted {node_payload['node_id']}")

        # ==================================================
        # 3️⃣ DEVICE STATUS (PUMP / FAN / LIGHT → device_status)
        # ==================================================
        device_item = rds.brpop("queue:db:device_status", timeout=1)

        if device_item:
            device = json.loads(device_item[1])
            now = datetime.now(timezone.utc).isoformat()

            prev = supabase.table("device_status") \
                .select("current_status, last_changed") \
                .eq("component_id", device["component_id"]) \
                .execute()

            prev_status = None
            last_changed = now

            if prev.data:
                prev_status = prev.data[0]["current_status"]

                if prev_status == device.get("current_status"):
                    last_changed = prev.data[0]["last_changed"]

            duration_minutes = 0
            if prev.data and prev_status == device.get("current_status"):
                delta = datetime.fromisoformat(now) - datetime.fromisoformat(last_changed)
                duration_minutes = int(delta.total_seconds() / 60)

            device_payload = {
                "component_id": device["component_id"],
                "node_id": device["node_id"],
                "component_type": device.get("component_type", "PUMP"),
                "current_status": device.get("current_status", "OFF"),
                "previous_status": prev_status,
                "trigger_source": device.get("trigger_source"),
                "control_lock": device.get("control_lock", False),
                "current_consumption": device.get("current_consumption", 0),
                "flow_rate": device.get("flow_rate", 0),
                "last_value": device.get("last_value"),
                "last_changed": last_changed,
                "duration_minutes": duration_minutes,
                "updated_at": now
            }

            supabase.table("device_status").upsert(
                device_payload,
                on_conflict="component_id"
            ).execute()

            print(f"[DB-DEVICE] Upserted {device_payload['component_id']}")

        time.sleep(0.3)

    except Exception as e:
        print("[DB-WORKER ERROR]", e)
        time.sleep(2)
