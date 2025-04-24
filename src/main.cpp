#ifndef USE_BMP085  // Only compile this file if NOT using BMP085

// Version: 1.0.0-stable (2024-01-24)
// Last stable version with proper message handling and routing

#include <Arduino.h>
#include <painlessMesh.h>
#include <ArduinoJson.h>
#include <DHT.h>

#define MESH_PREFIX "meshNetwork"
#define MESH_PASSWORD "MeshPassword"
#define MESH_PORT 5555
#define MAX_TTL 5
#define PROBE_INTERVAL 5000  // 5 seconds
#define GATEWAY_ID 1
#define DHTPIN D4
#define DHTTYPE DHT11
#define SENSOR_CHECK_INTERVAL 30000  // 30 seconds
#define MESH_NODE_TIMEOUT 30000  // 30 seconds
#define MAX_MSG_SIZE 512    // Increased from default
#define JSON_RETRIES 5      // Increased from default
#define GATEWAY_PING_INTERVAL 120000  // Ping gateway every 2 minutes
#define GATEWAY_TIMEOUT 300000      // Reset gateway mesh ID if no contact for 5 minutes

// Node ID definitions
#ifndef STATIC_NODE_ID
#define STATIC_NODE_ID 2
#endif

#ifndef GATEWAY_NODE_ID
#define GATEWAY_NODE_ID 1
#endif

// Function declarations
void receivedCallback(uint32_t from, const String &msg);
void newConnectionCallback(uint32_t nodeId);
void changedConnectionCallback();
void nodeTimeAdjustedCallback(int32_t offset);
void sendSensorData();
void probeNetwork();
void processProbeResponse(uint32_t from, int32_t offset, uint32_t hopCount, uint32_t hopsToDest);
void updateFastestNode();
void checkSensor();
void checkNetworkHealth();
void sendGatewayPing();

Scheduler userScheduler;
painlessMesh mesh;

// Global variables
DHT dht(DHTPIN, DHTTYPE);
bool hasSensor = false;
Task sensorTask(TASK_SECOND * 10, TASK_FOREVER, &sendSensorData);
Task probeTask(PROBE_INTERVAL, TASK_FOREVER, &probeNetwork);
Task sensorCheckTask(SENSOR_CHECK_INTERVAL, TASK_FOREVER, &checkSensor);
Task gatewayPingTask(GATEWAY_PING_INTERVAL, TASK_FOREVER, &sendGatewayPing);

// Network metrics structure
struct NodeMetrics {
    uint32_t nodeId;
    int32_t latency;
    uint32_t lastUpdate;
    bool isActive;
    uint32_t hopCount;
    uint32_t hopsToDestination;
};

std::vector<NodeMetrics> nodeMetrics;
uint32_t fastestNodeId = 0;
int32_t fastestDelay = INT_MAX;
uint32_t minHops = UINT32_MAX;

// Add gateway tracking
uint32_t gateway_mesh_id = 0;
uint32_t lastGatewayContactTime = 0;

void checkSensor() {
  if (STATIC_NODE_ID == GATEWAY_NODE_ID) {
    Serial.println("Gateway node - skipping sensor check");
    return;
  }
  
  Serial.println("\n🔍 Checking sensor status...");
  
    // Initialize DHT sensor
    dht.begin();
    
    // Try to read the sensor multiple times with  verification
  bool sensorReadSuccess = false;
  for (int i = 0; i < 3; i++) {
    float temp = dht.readTemperature();
    float hum = dht.readHumidity();
    
    if (!isnan(temp) && !isnan(hum)) {
      sensorReadSuccess = true;
            Serial.printf("✅ Sensor readings verified - Temperature: %.2f°C, Humidity: %.2f%%\n", temp, hum);
      break;
    }
        delay(1000);  // Wait between attempts
  }
  
  if (!sensorReadSuccess) {
        if (hasSensor) {
            Serial.println("⚠️ DHT sensor disconnected!");
            hasSensor = false;
            if (sensorTask.isEnabled()) {
                sensorTask.disable();
                Serial.println("❌ Disabled sensor data task");
            }
        } else {
            Serial.println("❌ No DHT sensor detected");
        }
    } else if (!hasSensor) {
        Serial.println("✅ DHT sensor reconnected!");
        hasSensor = true;
      if (!sensorTask.isEnabled()) {
        userScheduler.addTask(sensorTask);
        sensorTask.enable();
            Serial.println("✅ Enabled sensor data task");
        }
    }
}

void setup() {
  Serial.begin(115200);
  delay(2000);  // Give serial monitor time to open
  
  // Check if sensor is connected
  Serial.println("\n\n=== Starting ESP Mesh Network ===");
  
  if (STATIC_NODE_ID == GATEWAY_NODE_ID) {
    Serial.println("\n\n=== Gateway Node Startup ===");
    Serial.printf("Gateway Node ID: %d\n", GATEWAY_NODE_ID);
    Serial.printf("Network SSID: %s\n", MESH_PREFIX);
    Serial.println("✅ Gateway mode enabled");
  } else {
    Serial.println("\n\n=== Regular Node Startup ===");
    Serial.printf("Node ID: %d\n", STATIC_NODE_ID);
    Serial.printf("Network SSID: %s\n", MESH_PREFIX);
  }
  
  // Initialize sensor if we're not a gateway
  if (STATIC_NODE_ID != GATEWAY_NODE_ID) {
  Serial.println("Initializing DHT sensor...");
  dht.begin();
  delay(2000);  // Give the sensor time to initialize
  
  // Initial sensor check with detailed logging
  checkSensor();
  
    if (hasSensor) {
      Serial.println("✅ DHT sensor detected");
    } else {
      Serial.println("❌ No DHT sensor detected - operating as forwarder");
    }
  }
  
  // Mesh setup
  Serial.println("Initializing mesh network...");
  
  if (STATIC_NODE_ID == GATEWAY_NODE_ID) {
    // Gateway node setup - MUST be the root node
    Serial.println("Setting up gateway (root) node");
    
    // Enable extensive debug info for the gateway
    mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);
    
    // This is the gateway/root node configuration
    mesh.setContainsRoot(true);  // Network contains a root
    mesh.setRoot(true);          // THIS node is the root
    
    // Initialize the mesh with stable AP/STA configuration and fixed channel
    mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT, WIFI_AP_STA, 6);
    Serial.println("✅ Gateway initialized as ROOT node on channel 6");
  } else {
    // Regular node setup - must NOT be a root node
    Serial.println("Setting up regular non-root node");
    
    // Basic debug info for regular nodes
    mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);
    
    // Regular node configuration - knows there's a root but isn't one
    mesh.setContainsRoot(true);  // Network contains a root
    mesh.setRoot(false);         // This node is NOT the root
    
    // Initialize mesh with same channel as gateway/root
    mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT, WIFI_AP_STA, 6);
    Serial.println("✅ Node initialized as non-root node on channel 6");
  }
  
  // Set up callbacks for all nodes
  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onChangedConnections(&changedConnectionCallback);
  mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);

  if (STATIC_NODE_ID != GATEWAY_NODE_ID) {
    // Set up regular node tasks
    userScheduler.addTask(probeTask);
    probeTask.enable();
    
    userScheduler.addTask(sensorCheckTask);
    sensorCheckTask.enable();
    
    // Add gateway ping task
    userScheduler.addTask(gatewayPingTask);
    gatewayPingTask.enable();
    
    if (hasSensor) {
      Serial.println("✅ Enabling sensor data task");
      userScheduler.addTask(sensorTask);
      sensorTask.enable();
    }
    
    // Force an initial network scan
    Serial.println("🔍 Scanning for mesh network...");
    probeNetwork();
  } else {
    // Gateway only tasks
    Serial.println("✅ Gateway initialized - listening for sensor data");
    Serial.println("✅ Ready to receive messages from nodes");
    
    // Broadcast gateway presence with smaller document size
    DynamicJsonDocument announceDoc(128);  // Reduced from 256
    announceDoc["type"] = 7;  // Root announcement
    announceDoc["nodeId"] = STATIC_NODE_ID;
    announceDoc["meshId"] = mesh.getNodeId();
    announceDoc["timestamp"] = millis();
    announceDoc["node_type"] = "Gateway";
    
    String announceMsg;
    serializeJson(announceDoc, announceMsg);
    
    // Send the announcement a few times to ensure it's received
    for (int i = 0; i < 3; i++) {
      mesh.sendBroadcast(announceMsg);
      Serial.println("📢 Broadcasting gateway presence");
      delay(500);
    }
  }

  Serial.println("Setup complete!");
}

void loop() {
  // Update mesh
  mesh.update();
  
  // Check network health periodically
  static uint32_t lastNetworkCheck = 0;
  if (millis() - lastNetworkCheck > 30000) {  // Every 30 seconds
    checkNetworkHealth();
    lastNetworkCheck = millis();
  }
}

void checkNetworkHealth() {
    auto nodes = mesh.getNodeList();
    
    if (STATIC_NODE_ID == GATEWAY_NODE_ID) {
        // Gateway/root node checks
        Serial.printf("🔍 Gateway status: %d connected nodes\n", nodes.size());
        for (auto node : nodes) {
            Serial.printf("    - Connected node: %u\n", node);
        }
        
        // Broadcast gateway presence periodically
        DynamicJsonDocument announceDoc(128);
        announceDoc["type"] = 7;
        announceDoc["nodeId"] = STATIC_NODE_ID;
        announceDoc["meshId"] = mesh.getNodeId();
        announceDoc["timestamp"] = millis();
        announceDoc["node_type"] = "Gateway";
        
        String announceMsg;
        serializeJson(announceDoc, announceMsg);
        mesh.sendBroadcast(announceMsg);
        Serial.println("📢 Broadcasting gateway presence");
    } 
    else {
        // Regular node checks
        Serial.printf("🔍 Network status: connected to %d nodes\n", nodes.size());
        
        // Check if we've lost connection
        if (nodes.size() == 0) {
            // No connections, try to reinitialize the mesh network
            Serial.println("⚠️ No mesh connections detected - attempting to reconnect...");
            
            // Stop the mesh
            mesh.stop();
            delay(1000);
            
            // Restart the mesh with same channel as gateway
            mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT, WIFI_AP_STA, 6);
            Serial.println("🔄 Mesh network reinitialized");
            
            // Force a network scan
            probeNetwork();
        }
        else {
            // Check if we can see the gateway
            bool gatewayVisible = false;
            for (auto node : nodes) {
                if (node == GATEWAY_ID) {
                    gatewayVisible = true;
                    Serial.println("✅ Gateway node is directly visible");
                    break;
                }
            }
            
            if (!gatewayVisible) {
                Serial.println("ℹ️ Gateway node not directly visible");
                
                // Check if we have a route to the gateway
                if (fastestNodeId == 0) {
                    Serial.println("⚠️ No route to gateway - scanning network");
                    probeNetwork();
                } else {
                    Serial.printf("✅ Route to gateway exists via node %u\n", fastestNodeId);
                }
            }
        }
    }
}

void sendSensorData() {
  if (!hasSensor) {
    Serial.println("❌ No sensor detected - skipping data transmission");
    return;
  }

  float temp = dht.readTemperature();
  float hum = dht.readHumidity();

  if (isnan(temp) || isnan(hum)) {
    Serial.println("❌ Sensor read failed!");
    return;
  }

  Serial.printf("\n📊 Sensor Data:\n");
  Serial.printf("   Temperature: %.2f°C\n", temp);
  Serial.printf("   Humidity: %.2f%%\n", hum);

    DynamicJsonDocument doc(512);
  doc["nodeId"] = STATIC_NODE_ID;
  doc["meshId"] = mesh.getNodeId();
    doc["type"] = 3;
  doc["temp"] = temp;
  doc["humidity"] = hum;
    doc["pressure"] = nullptr;
  doc["ttl"] = MAX_TTL;
  doc["timestamp"] = millis();
    doc["node_type"] = "DHT11";
    doc["has_sensor"] = true;

  JsonArray path = doc.createNestedArray("path");
  path.add(STATIC_NODE_ID);

    String jsonString;
    serializeJson(doc, jsonString);

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
            
            // Create wrapped message for forwarding
            DynamicJsonDocument wrapperDoc(1024);
            wrapperDoc["type"] = 8;
            wrapperDoc["from"] = mesh.getNodeId();
            wrapperDoc["dest"] = fastestNodeId;
            
            // Store the original JSON string directly - ArduinoJson will handle proper escaping
            wrapperDoc["data"] = jsonString;
            
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

void probeNetwork() {
  DynamicJsonDocument probeDoc(128);
  probeDoc["type"] = 1;  // Use numeric type for probe consistently
  probeDoc["source"] = STATIC_NODE_ID;
  probeDoc["meshId"] = mesh.getNodeId();
  probeDoc["timestamp"] = millis();  // Use milliseconds consistently
  probeDoc["hopCount"] = 0;
  probeDoc["hopsToDest"] = UINT32_MAX;

  String probeMsg;
  if (serializeJson(probeDoc, probeMsg) == 0) {
    Serial.println("❌ Failed to serialize probe message");
    return;
  }
  
  // Send probe to gateway if directly connected
  if (mesh.isConnected(GATEWAY_ID)) {
    mesh.sendSingle(GATEWAY_ID, probeMsg);
    Serial.printf("📤 Sending probe directly to gateway (node %u)\n", GATEWAY_ID);
  }
  
  // Also broadcast to find alternative routes
  mesh.sendBroadcast(probeMsg);
  Serial.println("📤 Broadcasting probe message");
}

void processProbeResponse(uint32_t from, int32_t offset, uint32_t hopCount, uint32_t hopsToDest) {
    bool found = false;
    // Convert to milliseconds and ensure positive value
    int32_t latencyMs = abs(offset);
    
    // Cap maximum latency at 10 seconds
    if (latencyMs > 10000) {
        latencyMs = 10000;
    }
    
    for (auto& metric : nodeMetrics) {
        if (metric.nodeId == from) {
            metric.latency = latencyMs;
            metric.lastUpdate = millis();
            metric.isActive = true;
            metric.hopCount = hopCount;
            metric.hopsToDestination = hopsToDest;
            found = true;
            Serial.printf("📡 Updated route to node %u (hops: %u, latency: %d ms)\n", 
                        from, hopCount, latencyMs);
            break;
        }
    }
    
    if (!found) {
        NodeMetrics newMetric = {from, latencyMs, millis(), true, hopCount, hopsToDest};
        nodeMetrics.push_back(newMetric);
        Serial.printf("📡 New route to node %u (hops: %u, latency: %d ms)\n", 
                     from, hopCount, latencyMs);
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

void receivedCallback(uint32_t from, const String &msg) {
    if (STATIC_NODE_ID == GATEWAY_NODE_ID) {
        Serial.printf("COMMUNICATION:%s\n", msg.c_str());
        return;
    }

    Serial.printf("📥 Received from %u: %s\n", from, msg.c_str());
    
    DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, msg);
  
  if (error) {
        Serial.printf("❌ JSON parsing failed: %s\n", error.c_str());
    return;
  }

    // Update gateway contact if message is from gateway
    if (doc.containsKey("type")) {
        int type = doc["type"];
        
        // Check for gateway announcement
        if (type == 7 && doc.containsKey("nodeId") && doc["nodeId"] == GATEWAY_ID) {
            gateway_mesh_id = from;
            lastGatewayContactTime = millis();
            Serial.printf("✅ Gateway identified at mesh ID %u\n", gateway_mesh_id);
        }
        
        // Update contact time if message is from known gateway
        if (from == gateway_mesh_id || (doc.containsKey("nodeId") && doc["nodeId"] == GATEWAY_ID)) {
            lastGatewayContactTime = millis();
        }
    }

    // Handle both string and numeric message types
    int msgTypeNum = -1;
    String msgTypeStr = "";
    
    if (doc["type"].is<int>()) {
        msgTypeNum = doc["type"].as<int>();
        // Convert numeric type to string equivalent for compatibility
        if (msgTypeNum == 1) msgTypeStr = "probe";
        else if (msgTypeNum == 2) msgTypeStr = "probe_response";
        else if (msgTypeNum == 3) msgTypeStr = "sensor_data";
        else if (msgTypeNum == 8) msgTypeStr = "wrapped";
        Serial.printf("🔍 Received numeric message type %d (as %s)\n", msgTypeNum, msgTypeStr.c_str());
    } else {
        msgTypeStr = doc["type"].as<String>();
        // Convert string type to numeric equivalent
        if (msgTypeStr == "probe") msgTypeNum = 1;
        else if (msgTypeStr == "probe_response") msgTypeNum = 2;
        else if (msgTypeStr == "sensor_data") msgTypeNum = 3;
        else if (msgTypeStr == "wrapped") msgTypeNum = 8;
        Serial.printf("🔍 Received string message type %s (as %d)\n", msgTypeStr.c_str(), msgTypeNum);
    }
    
    // Process based on message type
    if (msgTypeNum == 1 || msgTypeStr == "probe") {
        // Handle probe messages only if they contain required fields
        if (doc.containsKey("source") && doc.containsKey("meshId")) {
            uint32_t source = doc["source"];
            uint32_t meshId = doc["meshId"];
            
            // Forward probe messages with reduced document sizes
            DynamicJsonDocument forwardDoc(192);
            forwardDoc["type"] = 8;  // Use numeric type for wrapped message
            forwardDoc["dest"] = 0;  // Broadcast
            forwardDoc["from"] = mesh.getNodeId();
            
            // Create inner message with reduced size
            DynamicJsonDocument innerDoc(192);
            innerDoc["type"] = 1;  // Use numeric type for probe
            innerDoc["source"] = source;
            innerDoc["meshId"] = meshId;
            innerDoc["timestamp"] = millis();
            innerDoc["hopCount"] = doc["hopCount"].as<uint32_t>() + 1;
            innerDoc["hopsToDest"] = doc["hopsToDest"].as<uint32_t>();
            
            // Store the inner document directly in the wrapper
            forwardDoc["data"] = innerDoc;
            
            String forwardMsg;
            serializeJson(forwardDoc, forwardMsg);
            
            // Send probe response with reduced document size
            DynamicJsonDocument responseDoc(192);
            responseDoc["type"] = 2;  // Use numeric type for probe_response
            responseDoc["source"] = STATIC_NODE_ID;
            responseDoc["nodeId"] = STATIC_NODE_ID;
            responseDoc["meshId"] = mesh.getNodeId();
            responseDoc["timestamp"] = millis();
            responseDoc["hopCount"] = 1;
            responseDoc["hopsToDest"] = doc["hopCount"].as<uint32_t>() + 1;
            responseDoc["has_sensor"] = hasSensor;

            String responseMsg;
            serializeJson(responseDoc, responseMsg);
            
            mesh.sendSingle(source, responseMsg);
            Serial.printf("📤 Sent probe response to node %u\n", source);
            
            // Forward probe if not at max hops
            if (doc["hopCount"].as<uint32_t>() < MAX_TTL) {
                mesh.sendBroadcast(forwardMsg);
                Serial.println("📤 Forwarding probe message");
            }
        }
    }
    else if (msgTypeNum == 2 || msgTypeStr == "probe_response") {
        // Handle probe responses only with required fields
        if (doc.containsKey("source") && doc.containsKey("meshId")) {
            uint32_t source = doc["source"];
            uint32_t meshId = doc["meshId"];
            uint32_t hopCount = doc.containsKey("hopCount") ? doc["hopCount"].as<uint32_t>() : 0;
            uint32_t hopsToDest = doc.containsKey("hopsToDest") ? doc["hopsToDest"].as<uint32_t>() : UINT32_MAX;
            
            // Process the response
            processProbeResponse(source, doc["timestamp"].as<int32_t>(), hopCount, hopsToDest);
            
            // Forward response if not at max hops
            if (hopCount < MAX_TTL) {
                // Create wrapped message with reduced size
                DynamicJsonDocument forwardDoc(192);
                forwardDoc["type"] = 8;
                forwardDoc["dest"] = 0;
                forwardDoc["from"] = mesh.getNodeId();
                
                // Create inner message with reduced size
                DynamicJsonDocument innerDoc(192);
                innerDoc["type"] = 2;
                innerDoc["source"] = source;
                innerDoc["meshId"] = meshId;
                innerDoc["timestamp"] = millis();
                innerDoc["hopCount"] = hopCount + 1;
                innerDoc["hopsToDest"] = hopsToDest;
                
                String innerMsg;
                serializeJson(innerDoc, innerMsg);
                
                forwardDoc["data"] = innerMsg;
                
                String forwardMsg;
                serializeJson(forwardDoc, forwardMsg);
                
                mesh.sendBroadcast(forwardMsg);
                Serial.println("📤 Forwarding probe response");
            }
        }
    }
    else if (msgTypeNum == 3 || msgTypeStr == "sensor_data") {
        // Only forward sensor data if it has required fields
        if (doc.containsKey("nodeId") || doc.containsKey("meshId")) {
            // Use smaller document sizes to avoid memory issues
            DynamicJsonDocument forwardDoc(512);
            forwardDoc["type"] = 8;
            forwardDoc["dest"] = 0;
            forwardDoc["from"] = mesh.getNodeId();
            
            // Store the original document directly in the wrapper
            forwardDoc["data"] = doc;

      String forwardMsg;
            serializeJson(forwardDoc, forwardMsg);

            // Forward only if TTL is valid
            if (doc["ttl"].as<uint32_t>() > 0) {
        if (fastestNodeId != 0) {
          mesh.sendSingle(fastestNodeId, forwardMsg);
                    Serial.printf("📤 Forwarding sensor data to node %u (ttl: %u)\n", fastestNodeId, doc["ttl"].as<uint32_t>());
        } else {
                    mesh.sendBroadcast(forwardMsg);
                    Serial.println("📤 Broadcasting sensor data (no route found)");
        }
      } else {
                Serial.println("⛔ TTL expired - not forwarding sensor data");
            }
        }
    }
    else if (msgTypeNum == 8 || msgTypeStr == "wrapped") {
        // Carefully handle wrapped messages
        if (doc.containsKey("data")) {
            String innerData;
            
            if (doc["data"].is<JsonObject>()) {
                // Just serialize the object to string
                DynamicJsonDocument tmpDoc(384);
                JsonObject obj = doc["data"].as<JsonObject>();
                for (JsonPair p : obj) {
                    tmpDoc[p.key()] = p.value();
                }
                serializeJson(tmpDoc, innerData);
            } else if (doc["data"].is<String>()) {
                innerData = doc["data"].as<String>();
  } else {
                Serial.println("❌ Wrapped message data has unsupported format");
        return;
      }
      
            Serial.println("🔍 Unwrapping message");
            receivedCallback(from, innerData);
        }
    }
    else {
        Serial.printf("⚠️ Unknown message type: %d/%s\n", msgTypeNum, msgTypeStr.c_str());
  }
}

void newConnectionCallback(uint32_t nodeId) {
  if (STATIC_NODE_ID != GATEWAY_NODE_ID) {
    Serial.printf("🔗 New connection from node: %u\n", nodeId);
    probeNetwork();
  }
}

void changedConnectionCallback() {
  if (STATIC_NODE_ID != GATEWAY_NODE_ID) {
    Serial.println("🔄 Mesh topology changed");
    updateFastestNode();
  }
}

void nodeTimeAdjustedCallback(int32_t offset) {
  if (STATIC_NODE_ID != GATEWAY_NODE_ID) {
    Serial.printf("⏰ Time adjusted: %d\n", offset);
  }
}

// Function to ping gateway periodically
void sendGatewayPing() {
    if (gateway_mesh_id == 0) {
        Serial.println("⚠️ Gateway mesh ID unknown, can't send ping");
        return;
    }
    
    // Check if we've lost contact with gateway
    if (millis() - lastGatewayContactTime > GATEWAY_TIMEOUT) {
        Serial.println("⚠️ No gateway contact for 5 minutes, resetting gateway mesh ID");
        gateway_mesh_id = 0;
        return;
    }
    
    // Create ping message
    DynamicJsonDocument pingDoc(256);
    pingDoc["type"] = 5;  // Ping message type
    pingDoc["nodeId"] = STATIC_NODE_ID;
    pingDoc["meshId"] = mesh.getNodeId();
    pingDoc["timestamp"] = millis();
    pingDoc["ttl"] = MAX_TTL;
    pingDoc["isGateway"] = false;
    
    String pingMsg;
    serializeJson(pingDoc, pingMsg);
    
    mesh.sendSingle(gateway_mesh_id, pingMsg);
    Serial.printf("📤 Sent ping to gateway (mesh ID: %u)\n", gateway_mesh_id);
}

#endif  // USE_BMP085