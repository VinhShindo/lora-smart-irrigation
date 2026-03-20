#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "config.h"

extern WiFiClient espClient;
extern PubSubClient mqtt;

const char* DEFAULT_SSID = "Garden_Gateway_Config";
const char* DEFAULT_PASS = "12345678";

void startConfigPortal();
void wifiReconnect();
void mqttReconnect();
void mqttCallback(char* topic, byte* payload, unsigned int len);

// ===== IMPLEMENT =====

WiFiClient espClient;
PubSubClient mqtt(espClient);

void handleSave() {
  String n_ssid = server.arg("ssid");
  String n_pass = server.arg("pass");

  if (n_ssid.length() > 0) {
    preferences.begin("wifi-config", false);
    preferences.putString("ssid", n_ssid);
    preferences.putString("pass", n_pass);
    preferences.end();

    server.send(200, "text/html", "Saved. Restarting...");
    delay(2000);
    ESP.restart();
  } else {
    server.send(400, "text/html", "Invalid SSID");
  }
}

void startConfigPortal() {
  if (apModeActive) return;

  apModeActive = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(DEFAULT_SSID, DEFAULT_PASS);

  server.on("/", []() {
    server.send(200, "text/html", "<form method='POST' action='/save'>SSID:<input name='ssid'><br>PASS:<input name='pass'><br><button>Save</button></form>");
  });

  server.on("/save", HTTP_POST, handleSave);
  server.begin();
}

void wifiReconnect() {
  if (apModeActive) return;
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.begin(st_ssid.c_str(), st_pass.c_str());
}

void mqttReconnect() {
  if (mqtt.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (mqtt.connect("GARDEN_GATEWAY")) {
    mqtt.subscribe("garden/control/+/cmd");
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload, len)) return;

  CmdItem item;
  strcpy(item.node, doc["node_id"]);
  strcpy(item.cmd, doc["action"]);
  strcpy(item.cid, doc["cmd_id"]);

  xQueueSend(cmdQueue, &item, 0);
}

#endif