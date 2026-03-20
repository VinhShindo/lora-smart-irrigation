#ifndef DATA_MANAGER_H
#define DATA_MANAGER_H

#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "config.h"

extern char buffer[MAX_BUF][128];
extern int head, count;

void httpTask(void* p);

// ===== IMPLEMENT =====

char buffer[MAX_BUF][128];
int head = 0, count = 0;

void httpTask(void* p) {
  for (;;) {
    if (count >= 10 && WiFi.status() == WL_CONNECTED) {

      HTTPClient http;
      http.begin("http://192.168.0.105:5000/api/batch");
      http.addHeader("Content-Type", "application/json");

      DynamicJsonDocument doc(2048);
      JsonArray arr = doc.createNestedArray("data");

      for (int i = 0; i < 10; i++) {
        JsonObject o = arr.createNestedObject();
        o["raw"] = buffer[i];
      }

      String out;
      serializeJson(doc, out);

      int code = http.POST(out);
      http.end();

      if (code > 0) {
        count -= 10;
      }
    }

    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}

#endif