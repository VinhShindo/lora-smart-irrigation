#include "config.h"
#include "network_manager.h"
#include "lora_manager.h"
#include "data_manager.h"

// GLOBAL DEFINE
std::map<String, NodeState> nodeRegistry;

SemaphoreHandle_t mutex;
SemaphoreHandle_t loraMutex;
QueueHandle_t cmdQueue;

Preferences preferences;
WebServer server(80);

bool loraReady = false;
bool waitingAck = false;
uint16_t lastSeqSent = 0;
char lastCmdId[32];

unsigned long ackTimeout = 0;
unsigned long lastHeartbeatSent = 0;

String st_ssid = "";
String st_pass = "";
bool apModeActive = false;

void setup() {
  Serial.begin(115200);

  preferences.begin("wifi-config", true);
  st_ssid = preferences.getString("ssid", "");
  st_pass = preferences.getString("pass", "");
  preferences.end();

  WiFi.begin(st_ssid.c_str(), st_pass.c_str());

  mutex = xSemaphoreCreateMutex();
  loraMutex = xSemaphoreCreateMutex();
  cmdQueue = xQueueCreate(CMD_QUEUE_SIZE, sizeof(CmdItem));

  initLoRa();

  mqtt.setServer("192.168.0.105", 1883);
  mqtt.setCallback(mqttCallback);

  xTaskCreatePinnedToCore(cmdTask, "cmd", 4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(loraRxTask, "lora", 4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(httpTask, "http", 4096, NULL, 1, NULL, 1);
}

void loop() {
  wifiReconnect();
  mqttReconnect();
  mqtt.loop();
}