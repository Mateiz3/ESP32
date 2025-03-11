#include "painlessMesh.h"

#define MESH_PREFIX     "ESP_mesh"
#define MESH_PASSWORD   "123"
#define MESH_PORT       5555

Scheduler userScheduler;
painlessMesh mesh;

#define STATIC_NODE_ID  3  // Change this per node (1, 2, 3, etc.)

void receivedCallback(uint32_t from, String &msg);
void sendMessage();

Task taskSendMessage(TASK_SECOND * 5, TASK_FOREVER, &sendMessage);

void sendMessage() {
    uint32_t timestamp = millis();  
    uint32_t myNodeId = STATIC_NODE_ID;

    Serial.printf(" Node %u sending message: %s\n", myNodeId, String(myNodeId) + ":" + String(timestamp));

    auto nodes = mesh.getNodeList();
    if (nodes.size() > 0) {
        mesh.sendBroadcast(String(myNodeId) + ":" + String(timestamp));
        Serial.printf("✅ Message sent to %d nodes.\n", nodes.size());
    } else {
        Serial.println("❌ No other nodes found. Waiting for connections...");
    }
}

void receivedCallback(uint32_t from, String &msg) {
    Serial.printf(" Message received from %u: %s\n", from, msg.c_str());
}

void newConnectionCallback(uint32_t nodeId) {
    Serial.printf(" New node joined: %u\n", nodeId);
}

void changedConnectionCallback() {
    Serial.println("⚡ Mesh network connections changed!");
    Serial.print(" Connected nodes: ");

    auto nodes = mesh.getNodeList();
    for (auto id : nodes) {
        Serial.printf("%u ", id);
    }
    Serial.println();

    if (nodes.size() == 0) {
        Serial.println("❌ No other nodes detected!");
    }
}

void scanWiFiNetworks() {
    Serial.println(" Scanning WiFi networks...");
    int numNetworks = WiFi.scanNetworks();
    
    for (int i = 0; i < numNetworks; i++) {
        Serial.printf(" SSID: %s, RSSI: %d dBm, Channel: %d\n", 
                      WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
    }
}

void setup() {
    Serial.begin(115200);
    scanWiFiNetworks();  // Check WiFi availability

    Serial.println(" Starting mesh network...");
    mesh.setDebugMsgTypes(ERROR | STARTUP);
    mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT, WIFI_AP_STA);

    mesh.onReceive(&receivedCallback);
    mesh.onNewConnection(&newConnectionCallback);
    mesh.onChangedConnections(&changedConnectionCallback);

    userScheduler.addTask(taskSendMessage);
    taskSendMessage.enable();

    Serial.printf("✅ Mesh initialized. Node ID: %u\n", STATIC_NODE_ID);
}

void loop() {
    mesh.update();
}
