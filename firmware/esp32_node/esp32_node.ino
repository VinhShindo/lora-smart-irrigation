#include <LoRa.h>
#include <SPI.h>
#include <DHT.h>

// --- [CẤU HÌNH PINOUT] ---
const int PIN_LORA_SS   = 5;
const int PIN_LORA_RST  = 14;
const int PIN_LORA_DIO0 = 26;
const int PIN_DHT       = 32;
const int PIN_SOIL      = 34;
const int PIN_LDR       = 35;
const int PIN_RELAY     = 22;

// --- [CẤU HÌNH HỆ THỐNG] ---
const String NODE_ID    = "NODE_01"; 
const String SECRET_KEY = "12345";
const long SEND_INTERVAL = 5000; // 5 giây gửi 1 lần

// --- [BIẾN MẶC ĐỊNH / KHỞI TẠO] ---
float default_consumption = 0.5; // Dòng điện (Ampe)
float default_flow_rate   = 1.2; // Lưu lượng (L/phút)
float last_trigger_soil   = 0.0; // Độ ẩm đất lúc kích hoạt bơm

// --- [BIẾN VẬN HÀNH] ---
DHT dht(PIN_DHT, DHT11);  

float ema_t = 0, ema_h = 0, ema_l = 0;
const float alpha = 0.1; 

bool pumpStatus = false;
String mode = "SEN"; // SEN hoặc CLD

int node_rssi = -1;
unsigned long lastSend = 0;

unsigned long manualOverrideUntil = 0;

void processSensors() {
    float rt = dht.readTemperature();
    float rh = dht.readHumidity();
    if(!isnan(rt)) ema_t = (alpha * rt) + ((1 - alpha) * ema_t);
    if(!isnan(rh)) ema_h = (alpha * rh) + ((1 - alpha) * ema_h);
    float rl = map(analogRead(PIN_LDR), 0, 4095, 0, 100);
    ema_l = (alpha * rl) + ((1 - alpha) * ema_l);
}

int getSoil() {
    long sum = 0;
    for(int i=0; i<10; i++) { sum += analogRead(PIN_SOIL); delay(5); }
    return constrain(map(sum/10, 4095, 1000, 0, 100), 0, 100);
}

void setup() {
    Serial.begin(115200);
    Serial.println("\n--- [NODE STARTING] ---");
    dht.begin();
    pinMode(PIN_RELAY, OUTPUT);
    
    LoRa.setPins(PIN_LORA_SS, PIN_LORA_RST, PIN_LORA_DIO0);
    if (!LoRa.begin(433E6)) {
        Serial.println("[ERROR] LoRa Hardware Fail!");
        while(1);
    }
    LoRa.setSyncWord(0xA5);
    LoRa.receive();
    Serial.println("[OK] LoRa Initialized.");
}

void loop() {
    // 1. Nhận lệnh Downlink
    int packetSize = LoRa.parsePacket();
    if (packetSize) {
        String msg = "";
        while (LoRa.available()) msg += (char)LoRa.read();

        if (msg.startsWith("CMD")) {
            // CMD,cmd_id,ON
            int p1 = msg.indexOf(',');           // Vị trí dấu phẩy sau "CMD"
            int p2 = msg.indexOf(',', p1 + 1);   // Vị trí dấu phẩy sau "node"
            int p3 = msg.indexOf(',', p2 + 1);   // Vị trí dấu phẩy sau "cmd_id"

            String cmd_id = "";
            String targetNode = msg.substring(p1 + 1, p2);
            targetNode.trim();

            if (targetNode != NODE_ID) {
                Serial.println("[CMD IGNORED] Not my NODE_ID");
                return;
            }
            Serial.println("[ACT] Đã đúng node: " + targetNode);

            if (p1 != -1 && p2 != -1 && p3 != -1) {
                // Tách cmd_id (nằm giữa p2 và p3)
                cmd_id = msg.substring(p2 + 1, p3);
                
                // Tách action (từ p3 đến hết chuỗi)
                String action = msg.substring(p3 + 1);
                
                // Xóa khoảng trắng thừa (nếu có) để so sánh chính xác
                action.trim(); 

                if (action == "ON") {
                    pumpStatus = true;
                    Serial.println("Lệnh: Bật bơm");
                } else if (action == "OFF") {
                    pumpStatus = false;
                    Serial.println("Lệnh: Tắt bơm");
                }
            }

            mode = "CLD";
            manualOverrideUntil = millis() + 30000;

            // FEEDBACK ACK,NODE_01,cmd_id,ON,CLD
            String ack = "ACK," + NODE_ID + "," + cmd_id + "," +
                         (pumpStatus ? "ON" : "OFF") + "," + mode;

            LoRa.beginPacket();
            LoRa.print(ack);
            LoRa.endPacket();

            Serial.println("[ACK SENT] " + ack);
        }
    }

    if (millis() > manualOverrideUntil) mode = "SEN";

    // 2. Xử lý cảm biến & logic
    processSensors();
    int soil = getSoil();
    if (mode == "SEN") {
        if (soil < 30 && !pumpStatus) { 
            pumpStatus = true; 
            last_trigger_soil = soil; // Lưu lại độ ẩm lúc kích hoạt
            Serial.println("[AUTO] Soil dry -> Pump ON");
        }
        else if (soil > 70 && pumpStatus) { 
            pumpStatus = false; 
            Serial.println("[AUTO] Soil wet -> Pump OFF");
        }
    }
    digitalWrite(PIN_RELAY, pumpStatus);

    // 3. Gửi Uplink (12 TRƯỜNG)
    if (millis() - lastSend > SEND_INTERVAL) {
        lastSend = millis();
        // Cấu trúc: ID, T, H, S, L, Pump, Mode, Key, RSSI, Amp, Flow, LastSoil
        String payload = NODE_ID + "," + 
                         String(ema_t, 1) + "," + 
                         String(ema_h, 1) + "," + 
                         String(soil) + "," + 
                         String((int)ema_l) + "," + 
                         String(pumpStatus ? "1" : "0") + "," + 
                         mode + "," + 
                         SECRET_KEY + "," +
                         String(node_rssi) + "," +
                         String(default_consumption, 1) + "," +
                         String(default_flow_rate, 1) + "," +
                         String(last_trigger_soil, 1);
        
        LoRa.beginPacket();
        LoRa.print(payload);
        LoRa.endPacket();
        LoRa.receive();
        Serial.println("[TX] Payload: " + payload);
    }
}