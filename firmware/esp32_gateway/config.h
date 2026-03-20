#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <map>
#include <Preferences.h>
#include <WebServer.h>

// LORA
#define LORA_SS 5
#define LORA_RST 14
#define LORA_DIO0 26
#define LORA_RETRY_MAX 5
#define NODE_TIMEOUT 45000

// NETWORK
#define MQTT_PORT 1883

// BUFFER
#define MAX_BUF 50
#define CMD_QUEUE_SIZE 10

// HEARTBEAT
#define HEARTBEAT_INTERVAL 25000

// NODE STATE
struct NodeState {
  unsigned long lastSeen;
  int rssi;
  bool online;
};

// CMD
struct CmdItem {
  char node[16];
  char cmd[16];
  char cid[32];
};

// GLOBAL
extern std::map<String, NodeState> nodeRegistry;

extern SemaphoreHandle_t mutex;
extern SemaphoreHandle_t loraMutex;
extern QueueHandle_t cmdQueue;

extern bool loraReady;
extern bool waitingAck;
extern uint16_t lastSeqSent;
extern char lastCmdId[32];
extern unsigned long ackTimeout;

extern unsigned long lastHeartbeatSent;

// WIFI CONFIG
extern Preferences preferences;
extern WebServer server;

extern String st_ssid;
extern String st_pass;
extern bool apModeActive;

#endif