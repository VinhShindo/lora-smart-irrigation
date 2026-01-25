#include <LoRa.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <map>
#include <set>

// --- [CẤU HÌNH PINOUT] ---
#define LORA_SS 5
#define LORA_RST 14
#define LORA_DIO0 26

// --- [THÔNG SỐ MẠNG] ---
const char* WIFI_SSID = "Shindo";
const char* WIFI_PASS = "vinh1230987";
const char* MQTT_SRV  = "192.168.0.103"; 
const char* API_URL   = "http://192.168.0.103:5000/api/batch";

// --- [QUẢN LÝ BỘ ĐỆM SENSOR] ---
#define MAX_BUF 50
String sensorBuffer[MAX_BUF]; 
int head = 0, count = 0;
SemaphoreHandle_t mutex;

WiFiClient espClient;
PubSubClient mqtt(espClient);

struct NodeState {
  unsigned long lastSeen;
  int lastRSSI;
  bool online;
};

std::map<String, NodeState> nodeRegistry;

std::set<String> nodeWhitelist = {
  "NODE_01",
  "NODE_02",
  "NODE_03"
};

#define NODE_TIMEOUT 10000

// --- MQTT Non-blocking với Log chi tiết (Giữ nguyên) ---
void reconnectMQTT() {
  static unsigned long lastAttempt = 0;
  if (!mqtt.connected() && millis() - lastAttempt > 5000) {
    lastAttempt = millis();
    Serial.print("[MQTT] Đang thử kết nối tới Broker: "); Serial.println(MQTT_SRV);
    
    if (mqtt.connect("GATEWAY_MASTER")) {
      mqtt.subscribe("factory/control/+/cmd");
      Serial.println("[MQTT] => KẾT NỐI THÀNH CÔNG!");
    } else {
      Serial.print("[MQTT] => Thất bại, mã lỗi rc="); Serial.println(mqtt.state());
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  StaticJsonDocument<256> doc;
  deserializeJson(doc, payload, len);

  String cmd_id = doc["cmd_id"];
  String action = doc["action"];
  String node   = doc["node_id"];

  // CMD format: CMD,cmd_id,ON
  String loraCmd = "CMD," + node + "," + cmd_id + "," + action;

  LoRa.beginPacket();
  LoRa.print(loraCmd);
  LoRa.endPacket();

  Serial.println("[DOWNLINK → LORA] " + loraCmd);
}


// --- CORE 0: XỬ LÝ REAL-TIME (LORA & MQTT STATUS) ---
void core0Task(void * p) {
  mqtt.setServer(MQTT_SRV, 1883);
  mqtt.setCallback(mqttCallback);
  Serial.println("[CORE 0] Task Real-time khởi động...");

  for(;;) {
    reconnectMQTT();
    mqtt.loop();

    int packetSize = LoRa.parsePacket();
    if (packetSize) {
      int gateway_measured_rssi = LoRa.packetRssi();
      Serial.print("\n[LORA] >>> ĐÃ NHẬN GÓI TIN! Kích thước: "); Serial.println(packetSize);
      
      String raw = "";
      while (LoRa.available()) raw += (char)LoRa.read();
      Serial.print("[LORA] Nội dung thô: "); Serial.println(raw);

      // --- BÓC TÁCH 12 TRƯỜNG DỮ LIỆU ---
      String v[12]; int idx = 0; int start = 0;
      for(int i=0; i<raw.length() && idx < 12; i++) {
        if(raw[i] == ',') { v[idx++] = raw.substring(start, i); start = i+1; }
      }

      if (idx < 11) {
        Serial.println("[ERROR] Malformed packet");
        return;
      }

      v[idx] = raw.substring(start);

      unsigned long now = millis();
      String node_id = v[0];
      // --- WHITELIST CHECK ---
      if (nodeWhitelist.find(node_id) == nodeWhitelist.end()) {
        Serial.println("[SECURITY] Node rejected (not in whitelist): " + node_id);
        return; // DROP PACKET
      }

      if (!nodeRegistry.count(node_id)) {
        nodeRegistry[node_id] = { now, gateway_measured_rssi, true };
      } else {
        nodeRegistry[node_id].lastSeen = now;
        nodeRegistry[node_id].lastRSSI = gateway_measured_rssi;
        nodeRegistry[node_id].online = true;
      }


      if (raw.startsWith("ACK")) {
        // 1. Tách chuỗi dùng mảng 5 phần tử
        String v[5];
        int idx = 0, start = 0;

        for (int i = 0; i < raw.length() && idx < 4; i++) { // Chỉ chạy đến dấu phẩy cuối cùng
          if (raw[i] == ',') {
            v[idx++] = raw.substring(start, i);
            start = i + 1;
          }
        }
        v[idx] = raw.substring(start); // Phần tử cuối cùng (mode)
        v[idx].trim(); // Xóa ký tự rác nếu có

        // 2. Kiểm tra xem đã tách đủ 5 phần tử chưa (v[0] đến v[4])
        if (idx == 4) { 
            StaticJsonDocument<256> ackDoc;
            
            // Gán đúng theo thứ tự Node gửi
            ackDoc["node_id"]        = v[1]; // NODE_ID
            ackDoc["cmd_id"]         = v[2]; // cmd_id
            ackDoc["component"]      = "PUMP";
            ackDoc["pump"] = v[3]; // ON/OFF
            ackDoc["mode"]           = v[4]; // mode (auto/manual...)
            ackDoc["result"]         = "SUCCESS";

            String ackMsg;
            serializeJson(ackDoc, ackMsg);

            // Topic MQTT: factory/control/NODE_ID/ack
            // Lưu ý: Dùng v[1] (là NODE_ID) thay vì v[0] (là chữ "ACK")
            String topic = "factory/control/" + v[1] + "/ack";
            
            mqtt.publish(topic.c_str(), ackMsg.c_str());

            Serial.println("[MQTT ACK SENT] Topic: " + topic);
            Serial.println("[MQTT ACK DATA] " + ackMsg);
        } else {
            Serial.println("[ERROR] Gói ACK không hợp lệ, thiếu dữ liệu!");
        }
    }

      // --- LUỒNG 1: GỬI TRẠNG THÁI TỨC THỜI QUA MQTT ---
      if (mqtt.connected()) {
        StaticJsonDocument<500> statusDoc;
        String node_id = v[0];
        bool online = nodeRegistry.count(node_id) && nodeRegistry[node_id].online;
        statusDoc["node_id"]    = node_id;
        statusDoc["pump"]       = (v[5] == "1") ? "ON" : "OFF";
        statusDoc["mode"]       = v[6];
        statusDoc["rssi"]       = gateway_measured_rssi;
        statusDoc["amp"]        = v[9].toFloat();
        statusDoc["flow"]       = v[10].toFloat();
        statusDoc["last_soil"]  = v[11].toFloat();
        statusDoc["current_status"] = online ? "ONLINE" : "OFFLINE";
        
        String statusMsg;
        serializeJson(statusDoc, statusMsg);
        if (mqtt.publish("garden/status", statusMsg.c_str())) {
           Serial.println("[MQTT] Đã đẩy trạng thái vận hành (Status) lên Broker.");
        }
      }

      // --- LUỒNG 2: LƯU CẢM BIẾN VÀO BUFFER CHO CORE 1 (HTTP) ---
      xSemaphoreTake(mutex, portMAX_DELAY);
      // Chỉ lưu các trường phục vụ bảng measurements: ID, T, H, S, L
      sensorBuffer[head] = v[0] + "," + v[1] + "," + v[2] + "," + v[3] + "," + v[4];
      head = (head + 1) % MAX_BUF;
      if (count < MAX_BUF) count++;
      xSemaphoreGive(mutex);
      Serial.printf("[BUFFER] Đã lưu cảm biến vào hàng chờ (%d/%d)\n", count, MAX_BUF);
    }
    vTaskDelay(5 / portTICK_PERIOD_MS);
  }
}

// --- CORE 1: XỬ LÝ BACKGROUND (HTTP BATCH SENSOR) ---
void core1Task(void * p) {
  Serial.println("[CORE 1] Task Background khởi động...");
  for(;;) {
    if (count >= 10 && WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.begin(API_URL);
      http.addHeader("Content-Type", "application/json");

      // Sử dụng DynamicJsonDocument cho gói tin lớn (10 mẫu)
      DynamicJsonDocument doc(4096); 
      doc["gateway_id"] = "ESP32_GATEWAY_01";
      JsonArray measurements = doc.createNestedArray("measurements");

      xSemaphoreTake(mutex, portMAX_DELAY);
      int tail = (head - count + MAX_BUF) % MAX_BUF;
      
      for(int i=0; i<10; i++) {
        String raw = sensorBuffer[(tail + i) % MAX_BUF];
        
        char buf[128];
        raw.toCharArray(buf, 128);
        
        float t, h, s, l;
        char id[32];
        
        // sscanf tách dữ liệu từ chuỗi CSV "ID,T,H,S,L"
        if (sscanf(buf, "%[^,],%f,%f,%f,%f", id, &t, &h, &s, &l) == 5) {
            JsonObject mObj = measurements.createNestedObject();
            mObj["node_id"] = String(id);
            mObj["temp"]    = t;
            mObj["humi"]    = h;
            mObj["soil"]    = s;
            mObj["light"]   = l;
        }
      }
      xSemaphoreGive(mutex);

      String jsonOut;
      serializeJson(doc, jsonOut);
      
      // LOG KIỂM TRA TRƯỚC KHI GỬI
      Serial.println("[HTTP] Gửi JSON: " + jsonOut);

      int code = http.POST(jsonOut);
      if (code >= 200 && code < 300) {
        xSemaphoreTake(mutex, portMAX_DELAY); 
        count -= 10; 
        xSemaphoreGive(mutex);
        Serial.printf("[HTTP] Thành công! Mã: %d\n", code);
      } else {
        // Nếu mã 500, xem log ở terminal Flask sẽ thấy nguyên nhân
        Serial.printf("[HTTP] THẤT BẠI! Mã lỗi: %d\n", code);
        String response = http.getString();
        Serial.println("[HTTP] Phản hồi từ Server: " + response);
      }
      http.end();
    }
    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
  unsigned long now = millis();

for (auto &it : nodeRegistry) {
  if (it.second.online && (now - it.second.lastSeen > NODE_TIMEOUT)) {
    it.second.online = false;

    StaticJsonDocument<256> offDoc;
    offDoc["node_id"] = it.first;
    offDoc["gateway_id"] = "ESP32_GATEWAY_01";
    offDoc["rssi"] = it.second.lastRSSI;
    offDoc["current_status"] = "OFFLINE";
    offDoc["updated_at"] = now;

    String offMsg;
    serializeJson(offDoc, offMsg);

    mqtt.publish("factory/node/status", offMsg.c_str());

    Serial.println("[NODE OFFLINE] " + it.first);
  }
}

}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- HỆ THỐNG GATEWAY ĐANG KHỞI ĐỘNG ---");

  mutex = xSemaphoreCreateMutex();

  // 1. Khởi tạo WiFi
  Serial.print("[WIFI] Đang kết nối tới: "); Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500); Serial.print("."); retry++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\n[WIFI] Đã kết nối! IP: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[WIFI] Không thể kết nối WiFi.");
  }

  // 2. Khởi tạo LoRa
  Serial.println("[LORA] Đang cấu hình phần cứng...");
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(433E6)) {
    Serial.println("!!! [LORA] KHÔNG TÌM THẤY MODULE LORA.");
    while (1);
  }
  LoRa.setSyncWord(0xA5);
  Serial.println("[LORA] Khởi tạo thành công (433MHz, SyncWord 0xA5)");

  // 3. Tạo Task Đa Nhân
  xTaskCreatePinnedToCore(core0Task, "RealTime", 8192, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(core1Task, "Batch", 8192, NULL, 1, NULL, 1);
  
  Serial.println("[SYSTEM] Hệ thống đã sẵn sàng vận hành.");
}

void loop() { vTaskDelete(NULL); }