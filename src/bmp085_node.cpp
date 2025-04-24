#ifdef USE_BMP085

#include <Arduino.h>
#include <painlessMesh.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>
#include <ArduinoJson.h>

// Mesh network credentials - MUST MATCH OTHER NODES
#define MESH_PREFIX     "meshNetwork"
#define MESH_PASSWORD   "MeshPassword"
#define MESH_PORT       5555
#define GATEWAY_ID      1
#define MAX_TTL         5
#define PROBE_INTERVAL  5000  // Send probe every 5 seconds (matching main.cpp)
#define MAX_MSG_SIZE   512   // Increased from 256
#define JSON_RETRIES   5     // Increased from 3
#define MESH_NODE_TIMEOUT 30000 // 30 seconds (matching main.cpp)
#define SENSOR_CHECK_INTERVAL 30000  // 30 seconds (matching main.cpp)
#define GATEWAY_PING_INTERVAL 120000  // Ping gateway every 2 minutes
#define GATEWAY_TIMEOUT 300000      // Reset gateway mesh ID if no contact for 5 minutes

// Pin definitions for ESP8266
#define SDA_PIN 4  // GPIO4 (D2)
#define SCL_PIN 5  // GPIO5 (D1)

// Network metrics structure
struct NodeMetrics {
    uint32_t nodeId;
    int32_t latency;
    uint32_t lastUpdate;
    bool isActive;
    uint32_t hopCount;
    uint32_t hopsToDestination;
};

// Globals
painlessMesh mesh;
Adafruit_BMP085 bmp;
Scheduler userScheduler;
std::vector<NodeMetrics> nodeMetrics;
uint32_t fastestNodeId = 0;
int32_t fastestDelay = INT_MAX;
uint32_t minHops = UINT32_MAX;

// Define a variable to store the gateway's mesh ID
uint32_t gateway_mesh_id = 0;  // Will be populated when we receive a message from gateway
uint32_t lastGatewayContactTime = 0; // Track when we last had contact with the gateway

// Forward declarations
void sendSensorData();
void checkSensor();
void probeNetwork();
void sendGatewayPing();
void processProbeResponse(uint32_t from, int32_t timestamp, uint32_t hopCount, uint32_t hopsToDest);
void updateFastestNode();
void handleMessageError(const String& msg, const char* error);
bool parseJsonMessage(const String& msg, DynamicJsonDocument& doc);

// Tasks
Task sensorTask(SENSOR_CHECK_INTERVAL, TASK_FOREVER, &sendSensorData);  // Send data every 30 seconds
Task sensorCheckTask(SENSOR_CHECK_INTERVAL, TASK_FOREVER, &checkSensor); // Check sensor every 30 seconds
Task probeTask(PROBE_INTERVAL, TASK_FOREVER, &probeNetwork);      // Probe network every 5 seconds
Task gatewayPingTask(GATEWAY_PING_INTERVAL, TASK_FOREVER, &sendGatewayPing); // Ping gateway every 2 minutes

bool hasSensor = false;

void checkSensor() {
    Serial.println("\n🔍 Checking sensor status...");
    
    // Try to initialize the sensor
    if (!bmp.begin()) {
        if (hasSensor) {
            Serial.println("⚠️ BMP085 sensor disconnected!");
            hasSensor = false;
            // Disable sensor task if it was running
            if (sensorTask.isEnabled()) {
                sensorTask.disable();
                Serial.println("❌ Disabled sensor data task");
            }
        } else {
            Serial.println("❌ No BMP085 sensor detected");
        }
        return;
    }
    
    // Try to read from the sensor to verify it's actually working
    float temp = bmp.readTemperature();
    float pressure = bmp.readPressure();
    
    if (isnan(temp) || isnan(pressure)) {
        if (hasSensor) {
            Serial.println("⚠️ BMP085 sensor detected but read failed - marking as disconnected");
            hasSensor = false;
            // Disable sensor task if it was running
            if (sensorTask.isEnabled()) {
                sensorTask.disable();
                Serial.println("❌ Disabled sensor data task");
            }
        } else {
            Serial.println("❌ BMP085 sensor detected but read failed");
        }
    } else {
        if (!hasSensor) {
            Serial.println("✅ BMP085 sensor reconnected!");
            hasSensor = true;
            // Enable sensor task if it was disabled
            if (!sensorTask.isEnabled()) {
                userScheduler.addTask(sensorTask);
                sensorTask.enable();
                Serial.println("✅ Enabled sensor data task");
            }
        }
        Serial.printf("📊 Sensor readings - Temperature: %.2f°C, Pressure: %.1f hPa\n", temp, pressure/100.0);
    }
}

bool parseJsonMessage(const String& msg, DynamicJsonDocument& doc) {
    if (msg.length() > MAX_MSG_SIZE) {
        Serial.printf("⚠️ Message too long (%d bytes), dropping\n", msg.length());
        return false;
    }

    for (int i = 0; i < JSON_RETRIES; i++) {
        DeserializationError error = deserializeJson(doc, msg);
        if (!error) {
            return true;
        }
        Serial.printf("⚠️ JSON parse attempt %d failed: %s\n", i + 1, error.c_str());
        delay(10);  // Small delay between retries
    }
    return false;
}

void handleMessageError(const String& msg, const char* error) {
    Serial.printf("❌ Message error: %s\n", error);
    Serial.printf("   Message: %s\n", msg.c_str());
}

// Send a dedicated ping to the gateway
void sendGatewayPing() {
    // Only ping if we have a gateway mesh ID
    if (gateway_mesh_id == 0) {
        Serial.println("⚠️ Gateway mesh ID unknown, can't send ping");
        return;
    }
    
    // Check if we've had contact with the gateway in the last 5 minutes
    if (millis() - lastGatewayContactTime > 300000) {
        Serial.println("⚠️ No gateway contact for 5 minutes, resetting gateway mesh ID");
        gateway_mesh_id = 0;
        return;
    }
    
    // Create a ping message
    DynamicJsonDocument pingDoc(256);
    pingDoc["type"] = 5;  // Ping message type
    pingDoc["nodeId"] = STATIC_NODE_ID;
    pingDoc["meshId"] = mesh.getNodeId();
    pingDoc["timestamp"] = millis();
    pingDoc["ttl"] = MAX_TTL;
    pingDoc["isGateway"] = false;  // We are not the gateway
    
    String pingMsg;
    serializeJson(pingDoc, pingMsg);
    
    // Send ping to gateway
    mesh.sendSingle(gateway_mesh_id, pingMsg);
    Serial.printf("📤 Sent ping to gateway (mesh ID: %u)\n", gateway_mesh_id);
}

void processProbeResponse(uint32_t from, int32_t timestamp, uint32_t hopCount, uint32_t hopsToDest) {
    bool found = false;
    
    // Calculate actual latency in milliseconds (difference between current time and timestamp)
    uint32_t currentTime = (uint32_t)millis();  // Explicitly cast to uint32_t
    
    // Use explicit cast and std::abs to resolve the ambiguity
    int32_t latency = std::abs((int32_t)(currentTime - timestamp));
    
    // Cap maximum latency at 10 seconds to avoid huge values from timestamp wrapping
    if (latency > 10000) {
        latency = 10000;
    }
    
    for (auto& metric : nodeMetrics) {
        if (metric.nodeId == from) {
            metric.latency = latency;
            metric.lastUpdate = currentTime;
            metric.isActive = true;
            metric.hopCount = hopCount;
            metric.hopsToDestination = hopsToDest;
            found = true;
            Serial.printf("📡 Updated route to node %u (hops: %u, latency: %d ms)\n", from, hopCount, latency);
            break;
        }
    }
    
    if (!found) {
        NodeMetrics newMetric = {from, latency, currentTime, true, hopCount, hopsToDest};
        nodeMetrics.push_back(newMetric);
        Serial.printf("📡 New route to node %u (hops: %u, latency: %d ms)\n", from, hopCount, latency);
    }
}

void updateFastestNode() {
    fastestNodeId = 0;
    fastestDelay = INT_MAX;
    minHops = UINT32_MAX;
    
    // Get current time for checking expiration
    uint32_t currentTime = millis();
    
    // Remove inactive nodes (not seen for at least 2 probe intervals)
    nodeMetrics.erase(
        std::remove_if(nodeMetrics.begin(), nodeMetrics.end(),
            [currentTime](const NodeMetrics& m) {
                return (currentTime - m.lastUpdate) > (2 * PROBE_INTERVAL);
            }),
        nodeMetrics.end()
    );

    // Compare all routes including direct and indirect
    for (const auto& metric : nodeMetrics) {
        if (!metric.isActive || metric.hopsToDestination == UINT32_MAX) {
            continue;
        }

        // Calculate effective latency with hop penalty (2ms per hop)
        int32_t effectiveLatency = metric.latency + (metric.hopsToDestination * 2);

        // Update fastest route if this one is better
        if (effectiveLatency < fastestDelay) {
            fastestDelay = effectiveLatency;
            fastestNodeId = metric.nodeId;
            minHops = metric.hopsToDestination;
            
            Serial.printf("📡 Found faster route through node %u:\n", fastestNodeId);
            Serial.printf("   Hops: %u\n", minHops);
            Serial.printf("   Raw latency: %d ms\n", metric.latency);
            Serial.printf("   Effective latency: %d ms\n", effectiveLatency);
        }
    }

    if (fastestNodeId != 0) {
        if (fastestNodeId == GATEWAY_ID) {
            Serial.printf("📡 Using direct route to gateway (latency: %d ms)\n", fastestDelay);
        } else {
            Serial.printf("📡 Best route through node %u (hops: %u, effective latency: %d ms)\n", 
                         fastestNodeId, minHops, fastestDelay);
        }
    } else {
        Serial.println("⚠️ No route to gateway found");
        Serial.println("   Will try to find route on next network scan");
    }
}

void probeNetwork() {
    // Look for direct connection to the gateway by mesh ID if known
    if (gateway_mesh_id != 0 && mesh.isConnected(gateway_mesh_id)) {
        Serial.printf("✅ Direct connection to gateway (mesh ID %u) detected\n", gateway_mesh_id);
    } else if (mesh.isConnected(GATEWAY_ID)) {
        Serial.printf("✅ Direct connection to gateway (node %u) detected\n", GATEWAY_ID);
    } else {
        Serial.println("⚠️ No direct connection to gateway");
    }
    
    // Print node list
    auto nodes = mesh.getNodeList();
    Serial.printf("🔍 Mesh network: %d nodes found\n", nodes.size());
    for (auto node : nodes) {
        Serial.printf("    - Node: %u\n", node);
        // Check if this is the gateway
        if (node == gateway_mesh_id) {
            Serial.println("    ✅ This is the gateway node!");
        }
    }
    
    // Create probe message
    DynamicJsonDocument probeDoc(256);
    probeDoc["type"] = 1;  // Use numeric type for probe
    probeDoc["source"] = STATIC_NODE_ID;
    probeDoc["nodeId"] = STATIC_NODE_ID;
    probeDoc["meshId"] = mesh.getNodeId();
    probeDoc["timestamp"] = millis();  // Use milliseconds consistently
    probeDoc["hopCount"] = 0;
    probeDoc["hopsToDest"] = UINT32_MAX;
    probeDoc["ttl"] = MAX_TTL;
    probeDoc["node_type"] = "BMP085";  // Identify ourselves as BMP085 sensor
    probeDoc["has_sensor"] = hasSensor;  // Indicate if sensor is active

    String probeMsg;
    if (serializeJson(probeDoc, probeMsg) == 0) {
        Serial.println("❌ Failed to serialize probe message");
        return;
    }
    
    // Try to send a probe to the gateway by mesh ID if known
    if (gateway_mesh_id != 0) {
        mesh.sendSingle(gateway_mesh_id, probeMsg);
        Serial.printf("📤 Sending probe directly to gateway mesh ID (%u)\n", gateway_mesh_id);
    } else if (mesh.isConnected(GATEWAY_ID)) {
        // Try direct connection to gateway by static ID
        mesh.sendSingle(GATEWAY_ID, probeMsg);
        Serial.printf("📤 Sending probe directly to gateway (node %u)\n", GATEWAY_ID);
    } else {
        // Try sending to the gateway even if not directly connected
        Serial.printf("📤 Attempting to send probe to gateway (node %u) without direct connection\n", GATEWAY_ID);
        mesh.sendSingle(GATEWAY_ID, probeMsg);
    }
    
    // Also broadcast to find alternative routes
    mesh.sendBroadcast(probeMsg);
    Serial.println("📤 Broadcasting probe message");
    
    // Check if we have any route to the gateway
    updateFastestNode();
}

void sendSensorData() {
    // Skip if the sensor isn't detected
    if (!hasSensor) {
        Serial.println("❌ No sensor detected - skipping data transmission");
        return;
    }

    // Get sensor readings
    float temperature = bmp.readTemperature();
    float pressure = bmp.readPressure() / 100.0F;  // Convert Pa to hPa
    
    // Check if readings are valid
    if (isnan(temperature) || isnan(pressure)) {
        hasSensor = false;
        Serial.println("❌ Sensor read failed - marking sensor as disconnected");
        return;
    }
    
    Serial.print("Temperature: ");
    Serial.print(temperature);
    Serial.print(" °C, Pressure: ");
    Serial.print(pressure);
    Serial.println(" hPa");

    // Create JSON document - use larger size to ensure enough space
    DynamicJsonDocument jsonDoc(512);
    
    // Set message metadata
    jsonDoc["type"] = 3;  // Sensor data message type (numeric)
    jsonDoc["nodeId"] = STATIC_NODE_ID;
    jsonDoc["meshId"] = mesh.getNodeId();
    jsonDoc["ttl"] = MAX_TTL;  // Always set TTL to maximum for new messages
    jsonDoc["timestamp"] = millis();  // Current time in milliseconds
    
    // Add node info
    jsonDoc["has_sensor"] = hasSensor;  // Use current sensor status
    jsonDoc["node_type"] = "BMP085";
    
    // Add sensor data at the root level for maximum compatibility
    jsonDoc["temp"] = temperature;
    jsonDoc["temperature"] = temperature;
    jsonDoc["pressure"] = pressure;
    
    // Add sensor data as structured object for better organization
    JsonObject sensorObj = jsonDoc.createNestedObject("sensor");
    sensorObj["temperature"] = temperature;
    sensorObj["pressure"] = pressure;
    sensorObj["nodeType"] = "BMP085";
    
    // Log the TTL we're setting
    Serial.printf("Setting message TTL to %d\n", MAX_TTL);
    
    // Serialize JSON to string
    String jsonString;
    serializeJson(jsonDoc, jsonString);

    // Check if we have a path to the gateway either by direct connection or known path
    if (gateway_mesh_id != 0 && mesh.isConnected(gateway_mesh_id)) {
        // Direct connection to gateway by mesh ID
        Serial.println("📤 Sending sensor data directly to gateway by mesh ID");
        mesh.sendSingle(gateway_mesh_id, jsonString);
    }
    else if (fastestNodeId != 0) {
        // Send sensor data to fastest route
        if (fastestNodeId == GATEWAY_ID && gateway_mesh_id != 0) {
            Serial.println("📤 Sending sensor data to gateway using stored mesh ID");
            mesh.sendSingle(gateway_mesh_id, jsonString);
        } else {
            Serial.printf("📤 Forwarding sensor data through node %u\n", fastestNodeId);
            
            // Create a wrapped message when sending to intermediate nodes
            DynamicJsonDocument wrapperDoc(1024);
            wrapperDoc["type"] = 8;  // Wrapped message type
            wrapperDoc["from"] = mesh.getNodeId();
            wrapperDoc["dest"] = fastestNodeId;
            wrapperDoc["data"] = jsonString;  // Include the original message as data
            
            String wrappedMsg;
            serializeJson(wrapperDoc, wrappedMsg);
            
            mesh.sendSingle(fastestNodeId, wrappedMsg);
            Serial.printf("📤 Sent wrapped sensor data to node %u with TTL %d\n", 
                        fastestNodeId, MAX_TTL);
        }
    } else {
        Serial.println("⚠️ No route to gateway - BROADCASTING sensor data");
        
        // Try sending directly to gateway by mesh ID if we have it
        if (gateway_mesh_id != 0) {
            mesh.sendSingle(gateway_mesh_id, jsonString);
            Serial.println("📤 Attempted direct send to gateway by mesh ID");
        } else {
            // Try sending directly to gateway anyway using static ID
            mesh.sendSingle(GATEWAY_ID, jsonString);
            Serial.println("📤 Attempted direct send to gateway by static ID");
        }
        
        // Also broadcast to all nodes
        mesh.sendBroadcast(jsonString);
        Serial.println("📤 Broadcasted sensor data to all nodes");
        
        // Try to find a route for next time
        probeNetwork();
    }
}

// Callback for receiving messages
void receivedCallback(uint32_t from, String &msg) {
    Serial.printf("📥 Received message from %u: %s\n", from, msg.c_str());
    
    DynamicJsonDocument doc(MAX_MSG_SIZE + 256); // Add extra space for parsing
    if (!parseJsonMessage(msg, doc)) {
        handleMessageError(msg, "Failed to parse JSON");
        return;
    }
    
    // Check if this message is from the gateway (either by static ID or gateway announcement)
    if (doc.containsKey("type")) {
        int type = doc["type"];
        
        // Check if this is a gateway announcement message
        if (type == 7) { // Gateway announcement type
            if (doc.containsKey("isGateway") && doc["isGateway"].as<bool>()) {
                gateway_mesh_id = from;
                lastGatewayContactTime = millis();
                Serial.printf("✅ Gateway identified at mesh ID %u\n", gateway_mesh_id);
                return;  // Processed gateway announcement
            }
        }
        
        // If message is from gateway, update contact time
        if (from == gateway_mesh_id || (doc.containsKey("nodeId") && doc["nodeId"] == GATEWAY_ID)) {
            lastGatewayContactTime = millis();
            
            // If we didn't know gateway mesh ID before, store it now
            if (gateway_mesh_id == 0) {
                gateway_mesh_id = from;
                Serial.printf("✅ Gateway mesh ID learned: %u\n", gateway_mesh_id);
            }
        }

        // Process message based on type
        switch (type) {
            case 1: // Probe
                {
                    // A probe message asking us to respond with our delay
                    if (!doc.containsKey("timestamp")) {
                        handleMessageError(msg, "Missing timestamp in probe");
                        return;
                    }
                    
                    int32_t timestamp = doc["timestamp"];
                    uint32_t hopCount = doc.containsKey("hopCount") ? doc["hopCount"].as<uint32_t>() : 0;
                    
                    // Create a response
                    DynamicJsonDocument responseDoc(256);
                    responseDoc["type"] = 2; // Type 2 is probe_response
                    responseDoc["nodeId"] = STATIC_NODE_ID;
                    responseDoc["meshId"] = mesh.getNodeId();
                    responseDoc["timestamp"] = timestamp; // Return original timestamp
                    responseDoc["hopCount"] = hopCount + 1; // Increment hop count
                    
                    // Add hops to gateway if we know them
                    uint32_t hopsToGateway = UINT32_MAX;
                    if (gateway_mesh_id == mesh.getNodeId()) { // We are the gateway
                        hopsToGateway = 0;
                    } else if (gateway_mesh_id == from) { // Directly connected to gateway
                        hopsToGateway = 1; 
                    } else if (fastestNodeId != 0 && minHops != UINT32_MAX) { // We have a route
                        hopsToGateway = minHops;
                    }
                    responseDoc["hopsToDest"] = hopsToGateway;
                    
                    String response;
                    serializeJson(responseDoc, response);
                    
                    // Send response back to the original sender
                    mesh.sendSingle(from, response);
                    Serial.printf("📤 Sent probe response to %u (hops: %u, dest_hops: %u)\n", 
                                 from, hopCount + 1, hopsToGateway);
                }
                break;
                
            case 2: // Probe response
                {
                    // Process probe response to update our routing
                    if (!doc.containsKey("timestamp") || !doc.containsKey("nodeId")) {
                        handleMessageError(msg, "Missing fields in probe response");
                        return;
                    }
                    
                    int32_t timestamp = doc["timestamp"];
                    uint32_t hopCount = doc.containsKey("hopCount") ? doc["hopCount"].as<uint32_t>() : 1;
                    uint32_t hopsToDest = doc.containsKey("hopsToDest") ? 
                                         doc["hopsToDest"].as<uint32_t>() : UINT32_MAX;
                    
                    processProbeResponse(from, timestamp, hopCount, hopsToDest);
                    updateFastestNode();
                }
                break;
                
            case 3: // Sensor data
                // We don't need to process sensor data, but we might forward it
                // Check if this is a message that needs to be forwarded
                if (doc.containsKey("ttl")) {
                    int ttl = doc["ttl"];
                    if (ttl > 0) {
                        // Decrement TTL
                        doc["ttl"] = ttl - 1;
                        
                        // Serialize the updated message
                        String updatedMsg;
                        serializeJson(doc, updatedMsg);
                        
                        // Forward message if we know a route to the gateway and TTL allows it
                        if (gateway_mesh_id != 0) {
                            // Forward directly to gateway if we know its mesh ID
                            mesh.sendSingle(gateway_mesh_id, updatedMsg);
                            Serial.printf("📤 Forwarded sensor data to gateway (mesh ID: %u)\n", gateway_mesh_id);
                        } else if (fastestNodeId != 0) {
                            // Forward to fastest node
                            mesh.sendSingle(fastestNodeId, updatedMsg);
                            Serial.printf("📤 Forwarded sensor data to fastest node %u\n", fastestNodeId);
                        } else {
                            // Broadcast if we don't know a specific route
                            mesh.sendBroadcast(updatedMsg);
                            Serial.println("📤 Broadcasted forwarded sensor data (no known route)");
                        }
                    } else {
                        Serial.println("⚠️ TTL expired, dropping message");
                    }
                }
                break;
                
            case 5: // Ping response
                // This would be a response to our ping, update gateway contact time
                if (from == gateway_mesh_id || (doc.containsKey("nodeId") && doc["nodeId"] == GATEWAY_ID)) {
                    lastGatewayContactTime = millis();
                    Serial.println("✅ Received ping response from gateway");
                }
                break;
                
            case 8: // Wrapped message (encapsulated in another message)
                {
                    if (!doc.containsKey("data")) {
                        handleMessageError(msg, "Missing data in wrapped message");
                        return;
                    }
                    
                    // Extract inner message
                    String innerMsg;
                    if (doc["data"].is<String>()) {
                        innerMsg = doc["data"].as<String>();
                    } else {
                        DynamicJsonDocument innerDoc(MAX_MSG_SIZE);
                        innerDoc = doc["data"];
                        serializeJson(innerDoc, innerMsg);
                    }
                    
                    // Process inner message recursively
                    receivedCallback(from, innerMsg);
                }
                break;
                
            default:
                Serial.printf("⚠️ Unknown message type: %d\n", type);
                break;
        }
    } else {
        handleMessageError(msg, "Missing type field");
    }
}

void newConnectionCallback(uint32_t nodeId) {
    Serial.printf("✨ New connection from node %u\n", nodeId);
}

void changedConnectionCallback() {
    Serial.println("🔄 Changed connections");
    updateFastestNode();
}

void nodeTimeAdjustedCallback(int32_t offset) {
    Serial.printf("⏰ Time adjusted by %d ms\n", offset);
}

void setup() {
    Serial.begin(115200);
    delay(2000);  // Give serial monitor time to open

    Serial.println("\n\n=== BMP085 Node Starting ===");
    Serial.printf("Static Node ID: %d\n", STATIC_NODE_ID);
    Serial.printf("Mesh will be initialized on: %s\n", MESH_PREFIX);

    // Initialize I2C
    Wire.begin(SDA_PIN, SCL_PIN);
    delay(1000);

    // Check sensor
    checkSensor();

    // Improved mesh setup
    Serial.println("Setting up mesh network...");
    
    // Important: Only one node should be root (normally the gateway)
    // Since this is a sensor node, it should NOT be root
    mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);
    mesh.setContainsRoot(true);  // We know there's a root in the network (gateway)
    mesh.setRoot(false);         // This node is NOT the root
    
    // Initialize mesh with same configuration as main.cpp
    mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT, WIFI_AP_STA, 6);
    Serial.println("✅ Mesh initialized on channel 6");
    
    // Set callbacks
    mesh.onReceive(&receivedCallback);
    mesh.onNewConnection(&newConnectionCallback);
    mesh.onChangedConnections(&changedConnectionCallback);
    mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);

    // Add tasks
    userScheduler.addTask(probeTask);
    probeTask.enable();
    
    userScheduler.addTask(gatewayPingTask);
    gatewayPingTask.enable();
    
    if (hasSensor) {
        Serial.println("✅ Sensor detected, enabling sensor data task");
        userScheduler.addTask(sensorTask);
        sensorTask.enable();
    } else {
        Serial.println("❌ No sensor detected, operating in forwarder mode only");
    }

    // Force a network scan
    Serial.println("🔍 Scanning for mesh network...");
    probeNetwork();
    Serial.println("Setup complete!");
}

// Add a reconnection helper function
void checkNetworkStatus() {
    if (mesh.getNodeList().size() == 0) {
        // No connections, try to reconnect
        Serial.println("No mesh connections - attempting to reconnect...");
        mesh.stop();
        delay(1000);
        mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT, WIFI_AP_STA, 6);
    }
}

void loop() {
    // Update mesh network
    mesh.update();
    
    // Check network status every 30 seconds
    static uint32_t lastNetworkCheck = 0;
    if (millis() - lastNetworkCheck > 30000) {
        checkNetworkStatus();
        lastNetworkCheck = millis();
    }
}

#endif 