#ifndef MESH_H
#define MESH_H

#include "config.h"

MeshNode meshTable[MAX_MESH_NODE];
int meshCount = 0;

NodeMetric nodeTable[5];
int nodeCount = 0;

char leaderId[4] = "";
bool isLeader = false;
bool leaderLocked = false;
bool networkFormed = false;

unsigned long lastHello = 0;

// ===== MESH FUNCTIONS =====

void updateLastSeen(const char* node) {
  if (strcmp(node, "GW") == 0) return;
  if (strcmp(node, NODE_SHORT) == 0) return;

  for (int i = 0; i < meshCount; i++) {
    if (strcmp(meshTable[i].id, node) == 0) {
      meshTable[i].lastSeen = millis();
      return;
    }
  }

  if (meshCount < MAX_MESH_NODE) {
    strcpy(meshTable[meshCount].id, node);
    meshTable[meshCount].rssi = -120;
    meshTable[meshCount].lastSeen = millis();
    meshCount++;
  }
}

void updateMesh(const char* node, int rssi) {
  for (int i = 0; i < meshCount; i++) {
    if (strcmp(meshTable[i].id, node) == 0) {
      meshTable[i].rssi = rssi;
      meshTable[i].lastSeen = millis();
      return;
    }
  }

  if (meshCount < MAX_MESH_NODE) {
    strcpy(meshTable[meshCount].id, node);
    meshTable[meshCount].rssi = rssi;
    meshTable[meshCount].lastSeen = millis();
    meshCount++;
  }
}

void updateGatewayRSSI(const char* node, int rssi) {
  for (int i = 0; i < nodeCount; i++) {
    if (strcmp(nodeTable[i].id, node) == 0) {
      nodeTable[i].gwRssi = rssi;
      nodeTable[i].lastUpdate = millis();
      return;
    }
  }

  if (nodeCount < 5) {
    strcpy(nodeTable[nodeCount].id, node);
    nodeTable[nodeCount].gwRssi = rssi;
    nodeTable[nodeCount].lastUpdate = millis();
    nodeCount++;
  }
}

void electLeader() {
  if (leaderLocked || networkFormed) return;

  int best = -200;
  char bestNode[4] = "";

  for (int i = 0; i < nodeCount; i++) {
    if (nodeTable[i].gwRssi > best) {
      best = nodeTable[i].gwRssi;
      strcpy(bestNode, nodeTable[i].id);
    }
  }

  if (strlen(bestNode) == 0) return;

  strcpy(leaderId, bestNode);
  isLeader = strcmp(leaderId, NODE_SHORT) == 0;
  leaderLocked = true;
  networkFormed = true;
  nodeState = NODE_NETWORK_READY;
}

#endif