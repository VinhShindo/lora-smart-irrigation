#include <LoRa.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <map>
#include <set>
#include "time.h"

/* =========================================================
   LORA PIN CONFIG
   ========================================================= */
#define LORA_SS 5
#define LORA_RST 14
#define LORA_DIO0 26

#define LORA_RETRY_MAX 5
#define NODE_TIMEOUT 45000

/* =========================================================
   NETWORK CONFIG
   ========================================================= */
const char* WIFI_SSID = "Shindo";
const char* WIFI_PASS = "vinh1230987";
const char* MQTT_SRV = "192.168.0.105";
const char* API_URL = "http://192.168.0.105:5000/api/batch";

WiFiClient espClient;
PubSubClient mqtt(espClient);

/* =========================================================
   LORA STATE
   ========================================================= */
bool loraReady = false;
unsigned long lastLoRaRetry = 0;

/* =========================================================
   SENSOR BUFFER
   ========================================================= */
#define MAX_BUF 50

char buffer[MAX_BUF][128];
int head = 0;
int count = 0;

SemaphoreHandle_t mutex;

/* =========================================================
   ACK STATE
   ========================================================= */
bool waitingAck = false;
unsigned long ackTimeout = 0;
char lastCmdId[32] = "";

/* =========================================================
   NTP CONFIG
   ========================================================= */
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7 * 3600;
const int daylightOffset_sec = 0;

bool ntpReady = false;

/* =========================================================
   NODE REGISTRY
   ========================================================= */
struct NodeState {
  unsigned long lastSeen;
  int rssi;
  bool online;
};

std::map<String, NodeState> nodeRegistry;

/* =========================================================
   SECURITY
   ========================================================= */
const char* whitelist[] = {
  "NODE_01",
  "NODE_02",
  "NODE_03"
};

/* =========================================================
   COMMAND QUEUE
   ========================================================= */
#define CMD_QUEUE_SIZE 10

struct CmdItem {
  char node[16];
  char cmd[16];
  char cid[32];
};

QueueHandle_t cmdQueue;
SemaphoreHandle_t loraMutex;

/* =========================================================
   HEARTBEAT STATE
   ========================================================= */
unsigned long lastHeartbeatSent = 0;
const unsigned long HEARTBEAT_INTERVAL = 25000;


/* =========================================================
   TIME UTIL
   ========================================================= */
String getTimeISO() {
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo, 2000)) {
    return "";
  }

  char buffer[27];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S+07:00", &timeinfo);

  return String(buffer);
}


/* =========================================================
   WHITELIST CHECK
   ========================================================= */
bool isWhitelisted(const char* node) {
  for (int i = 0; i < 3; i++) {
    if (strcmp(node, whitelist[i]) == 0) {
      return true;
    }
  }

  return false;
}

bool validShortNode(const char* node) {

  if (strcmp(node, "01") == 0) return true;
  if (strcmp(node, "02") == 0) return true;
  if (strcmp(node, "03") == 0) return true;

  return false;
}

/* =========================================================
   INIT LORA
   ========================================================= */
void initLoRa() {
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  for (int i = 1; i <= LORA_RETRY_MAX; i++) {

    Serial.printf("[LORA] Init attempt %d/%d...\n", i, LORA_RETRY_MAX);

    if (xSemaphoreTake(loraMutex, portMAX_DELAY)) {

      if (LoRa.begin(433E6)) {

        LoRa.setSyncWord(0xA5);
        LoRa.setSpreadingFactor(9);
        LoRa.setSignalBandwidth(125E3);
        LoRa.setCodingRate4(5);
        LoRa.setTxPower(17);
        LoRa.setPreambleLength(8);
        LoRa.enableCrc();

        LoRa.receive();

        loraReady = true;

        Serial.println("[LORA] Initialized OK (Configured)");

        xSemaphoreGive(loraMutex);
        return;
      }

      xSemaphoreGive(loraMutex);
    }

    delay(1000);
  }

  Serial.println("[LORA][FAIL] Running without LoRa (will retry later)");
  loraReady = false;
}


/* =========================================================
   MQTT RECONNECT
   ========================================================= */
void mqttReconnect() {

  static unsigned long lastTry = 0;

  if (mqtt.connected()) return;

  if (WiFi.status() != WL_CONNECTED) return;

  if (millis() - lastTry < 5000) return;

  lastTry = millis();

  Serial.print("[MQTT] Connecting...");

  if (mqtt.connect("GARDEN_GATEWAY")) {

    mqtt.subscribe("garden/control/+/cmd");

    Serial.println(" OK");

  } else {

    Serial.printf(" FAIL rc=%d\n", mqtt.state());
  }
}

/* =========================================================
   SEND LORA
   ========================================================= */
void sendLoRa(const char* msg) {
  if (!loraReady) return;
  if (xSemaphoreTake(loraMutex, portMAX_DELAY)) {

    LoRa.idle();
    delay(2);

    LoRa.beginPacket();
    LoRa.print(msg);
    LoRa.endPacket();

    delay(2);
    LoRa.receive();

    xSemaphoreGive(loraMutex);
  }
}


/* =========================================================
   WIFI RECONNECT
   ========================================================= */
void wifiReconnect() {
  static unsigned long lastTry = 0;

  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastTry < 5000) return;

  lastTry = millis();

  Serial.println("[WIFI] Reconnecting...");

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}


/* =========================================================
   MQTT CALLBACK
   ========================================================= */
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  Serial.printf("[MQTT][RX CMD] topic=%s\n", topic);

  StaticJsonDocument<256> doc;

  if (deserializeJson(doc, payload, len) != DeserializationError::Ok) {
    Serial.println("[MQTT][CMD] JSON parse fail");
    return;
  }

  CmdItem item;

  strncpy(item.node, doc["node_id"], sizeof(item.node) - 1);
  item.node[sizeof(item.node) - 1] = '\0';

  strncpy(item.cmd, doc["action"], sizeof(item.cmd) - 1);
  item.cmd[sizeof(item.cmd) - 1] = '\0';

  strncpy(item.cid, doc["cmd_id"], sizeof(item.cid) - 1);
  item.cid[sizeof(item.cid) - 1] = '\0';

  Serial.printf(
    "[MQTT][CMD DATA] node=%s cmd=%s cid=%s\n",
    item.node,
    item.cmd,
    item.cid);

  if (!isWhitelisted(item.node)) {
    Serial.println("[SECURITY] CMD rejected");
    return;
  }

  if (xQueueSend(cmdQueue, &item, pdMS_TO_TICKS(50)) != pdPASS) {

    Serial.println("[CMD QUEUE] FULL - drop CMD");

  } else {

    Serial.println("[CMD QUEUE] Enqueued");
  }
}


/* =========================================================
   CMD TASK
   ========================================================= */
void cmdTask(void* p) {
  CmdItem item;

  for (;;) {

    if (xQueueReceive(cmdQueue, &item, portMAX_DELAY) == pdTRUE) {

      if (!loraReady) {
        Serial.println("[CMD TASK] LoRa not ready - skip");
        continue;
      }

      if (waitingAck) {
        Serial.println("[CMD TASK] Waiting ACK - skip new CMD");
        continue;
      }

      char loraCmd[128];

      snprintf(
        loraCmd,
        sizeof(loraCmd),
        "C,GW,%s,3,%s,%s",
        item.node,
        item.cid,
        item.cmd);

      if (xSemaphoreTake(loraMutex, portMAX_DELAY)) {

        Serial.println("[CMD] Preempt radio");

        LoRa.idle();

        delay(5);

        while (LoRa.available()) LoRa.read();

        delay(5);

        if (LoRa.parsePacket()) {

          Serial.println("[CMD] Channel busy");

          xSemaphoreGive(loraMutex);

          vTaskDelay(50 / portTICK_PERIOD_MS);

          continue;
        }

        LoRa.beginPacket();
        LoRa.print(loraCmd);
        LoRa.endPacket();

        LoRa.receive();

        lastHeartbeatSent = millis();

        xSemaphoreGive(loraMutex);
      }

      waitingAck = true;
      ackTimeout = millis() + 8000;

      strncpy(lastCmdId, item.cid, sizeof(lastCmdId));

      Serial.print("[CMD→LORA] ");
      Serial.println(loraCmd);
    }
  }
}


/* =========================================================
   CORE0 TASK
   ========================================================= */
void core0Task(void* p) {
  mqtt.setServer(MQTT_SRV, 1883);
  mqtt.setCallback(mqttCallback);

  Serial.println("[CORE0] Real-time task started");

  for (;;) {

    static unsigned long lastNtpTry = 0;

    if (WiFi.status() == WL_CONNECTED && !ntpReady && millis() - lastNtpTry > 10000) {

      lastNtpTry = millis();

      Serial.println("[NTP] Retry sync");

      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

      struct tm timeinfo;

      if (getLocalTime(&timeinfo, 3000)) {

        ntpReady = true;
        Serial.println("[NTP] Sync OK");

      } else {

        Serial.println("[NTP] Sync fail");
      }
    }

    /* ======================================================
           P0 – LORA RX
           ====================================================== */

    if (loraReady && xSemaphoreTake(loraMutex, pdMS_TO_TICKS(10))) {

      int packetSize = LoRa.parsePacket();

      if (!packetSize) {
        xSemaphoreGive(loraMutex);
        vTaskDelay(2);
        continue;
      }

      int rssi = LoRa.packetRssi();

      char buf[256];
      int i = 0;

      while (LoRa.available() && i < sizeof(buf) - 1) {
        buf[i++] = (char)LoRa.read();
      }

      buf[i] = '\0';

      xSemaphoreGive(loraMutex);

      if (i < 3) {
        Serial.println("[LORA] short packet");
        continue;
      }

      Serial.print("[LORA][RX] ");
      Serial.println(buf);

      /* ======================================================
     HELLO
     ====================================================== */

      if (strncmp(buf, "H,", 2) == 0) {
        char node[4];

        if (sscanf(buf, "H,%3[^,\r\n]", node) == 1) {

          Serial.printf("[HELLO] node=%s RSSI=%d\n", node, rssi);

          char reply[32];

          snprintf(reply, sizeof(reply), "R,%s,%d", node, rssi);

          delay(100);
          sendLoRa(reply);

          char nodeId[12];
          snprintf(nodeId, sizeof(nodeId), "NODE_%s", node);

          nodeRegistry[nodeId].lastSeen = millis();
          nodeRegistry[nodeId].rssi = rssi;
          nodeRegistry[nodeId].online = true;

          StaticJsonDocument<256> status;

          status["type"] = "STATUS";
          status["node_id"] = nodeId;
          status["current_status"] = "ONLINE";
          status["rssi"] = rssi;
          status["pump"] = "UNKNOWN";
          status["mode"] = "R";
          status["gateway_time"] = getTimeISO();

          char msg[256];
          serializeJson(status, msg);

          if (mqtt.connected()) {
            mqtt.publish("garden/status", msg);
          }

          Serial.print("[HELLO REPLY] ");
          Serial.println(reply);
        } else {
          Serial.println("[HELLO] malformed");
        }

        continue;
      }

      /* ======================================================
     BATCH SENSOR
     BATCHSEN,D,01,28,70,500,800;D,02,...
     ====================================================== */

      if (strncmp(buf, "BS,", 3) == 0) {

        Serial.println("[GW] SENSOR BATCH RX");

        char* record = buf + 3;
        char* entry = strtok(record, ";");

        while (entry) {

          char node[4];
          float t, h, soil, light;

          if (sscanf(entry, "%3[^,],%f,%f,%f,%f",
                     node, &t, &h, &soil, &light)
              == 5) {

            if (!validShortNode(node)) {
              Serial.println("[SECURITY] invalid node");
              entry = strtok(NULL, ";");
              continue;
            }

            char nodeId[12];
            snprintf(nodeId, sizeof(nodeId), "NODE_%s", node);

            xSemaphoreTake(mutex, portMAX_DELAY);

            String measuredAt = getTimeISO();

            char line[128];

            snprintf(line, sizeof(line),
                     "%s,%.1f,%.1f,%f,%f,%s",
                     nodeId,
                     t, h, soil, light,
                     measuredAt.c_str());

            strncpy(buffer[head], line, sizeof(buffer[head]) - 1);
            buffer[head][sizeof(buffer[head]) - 1] = '\0';

            head = (head + 1) % MAX_BUF;

            if (count < MAX_BUF) count++;

            xSemaphoreGive(mutex);

            Serial.printf("[BATCH SENSOR] %s stored (%d/%d)\n",
                          node, count, MAX_BUF);

            nodeRegistry[nodeId].lastSeen = millis();
            nodeRegistry[nodeId].rssi = rssi;
            nodeRegistry[nodeId].online = true;
          }

          entry = strtok(NULL, ";");
        }

        continue;
      }

      /* ======================================================
        BATCH STATUS
        BATCHSTA,S,01,1,A,500;S,02,0,M,450
        ====================================================== */

      if (strncmp(buf, "BT,", 3) == 0) {

        Serial.println("[GW] STATUS BATCH RX");

        char* record = buf + 3;
        char* entry = strtok(record, ";");

        while (entry) {

          char node[4];
          int pump;
          char mode;
          int soil;
          float amp;
          float flow;

          if (sscanf(entry,
                     "%3[^,],%d,%c,%d,%f,%f",
                     node, &pump, &mode, &soil, &amp, &flow)
              >= 4) {

            char nodeId[12];
            snprintf(nodeId, sizeof(nodeId), "NODE_%s", node);


            StaticJsonDocument<256> status;

            status["type"] = "STATUS";
            status["node_id"] = nodeId;
            status["pump"] = pump ? "ON" : "OFF";
            char modeStr[2] = { mode, '\0' };
            status["mode"] = modeStr;
            status["last_soil"] = soil;
            status["amp"] = amp;
            status["flow"] = flow;
            status["rssi"] = rssi;
            status["current_status"] = "ONLINE";
            status["gateway_time"] = getTimeISO();

            char msg[256];
            serializeJson(status, msg);

            if (mqtt.connected()) {
              mqtt.publish("garden/status", msg);
            }

            nodeRegistry[nodeId].lastSeen = millis();
            nodeRegistry[nodeId].rssi = rssi;
            nodeRegistry[nodeId].online = true;

            Serial.printf("[BATCH STATUS] %s\n", node);
          }

          entry = strtok(NULL, ";");
        }

        continue;
      }

      /* ======================================================
     ACK
     A,02,GW,03,CMD123,1,OK,...
     ====================================================== */

      if (strncmp(buf, "A,", 2) == 0) {

        char* v[16];
        int idx = 0;

        char tmp[256];
        strncpy(tmp, buf, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';

        char* token = strtok(tmp, ",");

        while (token && idx < 16) {
          v[idx++] = token;
          token = strtok(NULL, ",");
        }

        if (idx < 6) {
          Serial.println("[ACK] malformed");
          continue;
        }

        if (!validShortNode(v[1])) {
          Serial.println("[SECURITY] invalid ACK node");
          continue;
        }

        char nodeId[12];
        snprintf(nodeId, sizeof(nodeId), "NODE_%s", v[1]);

        StaticJsonDocument<512> ack;

        ack["type"] = "ACK";
        ack["node_id"] = nodeId;
        ack["cmd_id"] = v[4];

        if (waitingAck && strcmp(v[4], lastCmdId) == 0) {

          waitingAck = false;
          ackTimeout = 0;

          Serial.println("[ACK MATCHED]");
        }

        ack["success"] = (strcmp(v[5], "1") == 0);

        if (idx > 6) ack["message"] = v[6];
        if (idx > 7) ack["pump"] = v[7];
        if (idx > 8) ack["mode"] = v[8];
        if (idx > 9) ack["flow"] = atof(v[9]);
        if (idx > 10) ack["amp"] = atof(v[10]);
        if (idx > 11) ack["last_soil"] = atof(v[11]);

        ack["executed_at"] = getTimeISO();
        ack["gateway_time"] = getTimeISO();
        ack["rssi"] = rssi;

        char out[512];

        serializeJson(ack, out);

        if (mqtt.connected()) {
          mqtt.publish("garden/control/ack", out);
        }

        Serial.println("[ACK RX]");

        continue;
      }

      /* ======================================================
     UNKNOWN PACKET
     ====================================================== */

      Serial.println("[LORA] UNKNOWN PACKET");
    }

    wifiReconnect();
    mqttReconnect();
    mqtt.loop();

    /* ======================================================
           HEARTBEAT
           ====================================================== */

    if (loraReady && millis() - lastHeartbeatSent > HEARTBEAT_INTERVAL && xSemaphoreTake(loraMutex, 0)) {

      LoRa.idle();
      delay(2);

      LoRa.beginPacket();

      if (loraReady) {

        char hb[32];

        snprintf(hb, sizeof(hb), "B,%s", "ALL");

        LoRa.print(hb);
      }

      LoRa.endPacket();

      LoRa.receive();

      lastHeartbeatSent = millis();

      xSemaphoreGive(loraMutex);
    }

    if (!loraReady && millis() - lastLoRaRetry > 10000) {

      Serial.println("[LORA] Retry init...");

      lastLoRaRetry = millis();

      initLoRa();
    }

    if (waitingAck && millis() > ackTimeout) {

      Serial.println("[ACK] Timeout - clear waiting");

      waitingAck = false;
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}


/* =========================================================
   CORE1 TASK
   ========================================================= */
void core1Task(void* p) {
  Serial.println("[CORE1] Background task started");

  static uint32_t batch_id = 0;

  for (;;) {

    if (count >= 10 && WiFi.status() == WL_CONNECTED) {

      String nowISO = getTimeISO();

      if (nowISO == "") {
        nowISO = "1970-01-01T00:00:00";
      }

      xSemaphoreTake(mutex, portMAX_DELAY);

      int tail = (head - count + MAX_BUF) % MAX_BUF;

      DynamicJsonDocument doc(4096);

      doc["gateway_id"] = "GW_01";
      doc["batch_id"] = ++batch_id;
      doc["sent_at"] = nowISO.length() ? nowISO : "1970-01-01T00:00:00";

      JsonArray arr = doc.createNestedArray("measurements");

      for (int i = 0; i < 10; i++) {

        char id[16];
        float t, h, s, l;
        char measuredAt[32];

        sscanf(
          buffer[(tail + i) % MAX_BUF],
          "%15[^,],%f,%f,%f,%f,%31[^,\n]",
          id,
          &t,
          &h,
          &s,
          &l,
          measuredAt);

        JsonObject o = arr.createNestedObject();

        o["node_id"] = id;
        o["t"] = t;
        o["h"] = h;
        o["s"] = s;
        o["l"] = l;
        o["ts"] = measuredAt;
      }

      char out[4096];
      serializeJson(doc, out);

      xSemaphoreGive(mutex);

      Serial.println("[HTTP] POST attempt:");
      Serial.println(out);

      HTTPClient http;

      http.setTimeout(5000);

      http.begin(API_URL);
      http.addHeader("Content-Type", "application/json");

      int code = http.POST(out);

      http.end();

      if (code >= 200 && code < 300) {

        xSemaphoreTake(mutex, portMAX_DELAY);
        count -= 10;
        xSemaphoreGive(mutex);

        Serial.println("[HTTP] SUCCESS - batch committed");

      } else {

        Serial.printf("[HTTP] FAIL (code=%d) - retry later\n", code);
      }
    }

    unsigned long now = millis();

    for (auto& it : nodeRegistry) {

      if (it.second.online && now - it.second.lastSeen > NODE_TIMEOUT) {

        it.second.online = false;

        StaticJsonDocument<256> off;

        off["type"] = "STATUS";
        off["node_id"] = it.first;
        off["current_status"] = "OFFLINE";
        off["rssi"] = it.second.rssi;
        off["uptime"] = 0;
        off["gateway_time"] = getTimeISO();

        String msg;

        serializeJson(off, msg);

        if (mqtt.connected()) {
          mqtt.publish("garden/status", msg.c_str(), false);
        }

        Serial.println("[NODE OFFLINE] " + it.first);
      }
    }

    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}


/* =========================================================
   SETUP
   ========================================================= */
void setup() {
  Serial.begin(115200);

  mqtt.setBufferSize(1024);

  Serial.println("\n=== ESP32 GARDEN GATEWAY ===");

  mutex = xSemaphoreCreateMutex();
  loraMutex = xSemaphoreCreateMutex();

  if (!loraMutex) {
    Serial.println("[FATAL] LoRa mutex create failed");
    esp_restart();
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 0, NULL, true);

  Serial.print("[WIFI] Connecting");

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED) {

    delay(500);

    Serial.print(".");

    if (millis() - start > 20000) {

      Serial.println("\n[WIFI] TIMEOUT");

      break;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {

    Serial.println("\n[WIFI] OK");
    Serial.println(WiFi.localIP());

  } else {

    Serial.println("[WIFI] FAIL");
  }

  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  if (WiFi.status() == WL_CONNECTED) {

    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    Serial.print("[NTP] Waiting sync");

    for (int i = 0; i < 10; i++) {

      struct tm timeinfo;

      if (getLocalTime(&timeinfo)) {

        ntpReady = true;

        Serial.println(" OK");

        break;
      }

      Serial.print(".");
      delay(1000);
    }

    if (!ntpReady) {
      Serial.println(" FAIL");
    }
  }

  initLoRa();
  mqtt.setKeepAlive(30);

  cmdQueue = xQueueCreate(CMD_QUEUE_SIZE, sizeof(CmdItem));

  if (!cmdQueue) {
    Serial.println("[FATAL] CMD Queue create failed");
    esp_restart();
  }

  xTaskCreatePinnedToCore(cmdTask, "CMD", 4096, NULL, 6, NULL, 0);
  xTaskCreatePinnedToCore(core0Task, "CORE0", 8192, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(core1Task, "CORE1", 8192, NULL, 1, NULL, 1);
}


/* =========================================================
   LOOP
   ========================================================= */
void loop() {
  vTaskDelete(NULL);
}