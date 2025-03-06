#include <ESP8266WiFi.h>  // WiFi  //"Copyright [year] <Copyright Owner>"
#include <PubSubClient.h>  // mqtt
#include <DHT.h>   // DHT sensor
#include <Adafruit_BMP085.h>  // Adafruit BMP280 for bmp sensor

// WiFi & MQTT Server Details
// WiFi details
const char* ssid = "******";                   // to be changed acordingly
const char* password = "******";                // to be changed acordingly
// MQTT details
const char* mqtt_server = "192.168.36.252";  // Replace with MQTT broker IP
const int mqtt_port = 1883;                 // default port

// WiFi & MQTT Clients
WiFiClient espClient;
PubSubClient client(espClient);

// DHT Sensor Setup
#define DHTPIN D3
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// BMP Sensor Setup
Adafruit_BMP085 bmp;  // Use Adafruit_BMP280 bmp;

void connectToWiFi() {
    Serial.print("Connecting to WiFi...");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n✅ WiFi Connected! IP: " + WiFi.localIP().toString());
}

void reconnectMQTT() {
    while (!client.connected()) {
        Serial.print("Attempting MQTT connection...");
        if (client.connect("ESP8266Client")) {  // No authentication; to be changed acordingly
            Serial.println("✅ Connected to MQTT broker!");
        } else {
            Serial.print("❌ Failed, rc=");
            Serial.print(client.state());
            Serial.println(" Retrying in 5 seconds...");
            delay(5000);
        }
    }
}

void setup() {
    Serial.begin(115200);
    connectToWiFi();
    client.setServer(mqtt_server, mqtt_port);
    reconnectMQTT();
    
    dht.begin();
    
    if (!bmp.begin()) {
        Serial.println("❌ BMP sensor not detected!");
    } else {
        Serial.println("✅ BMP sensor initialized!");
    }
}

void loop() {
    if (!client.connected()) {
        reconnectMQTT();
    }

    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();
    float pressure = bmp.readPressure() / 100.0F;  // Convert to hPa
    float altitude = bmp.readAltitude();

    if (!isnan(temperature) && !isnan(humidity)) {
        String payload = "{";
        payload += "\"temperature\": " + String(temperature, 1) + ", ";
        payload += "\"humidity\": " + String(humidity, 1);

        if (bmp.begin()) {
            payload += ", \"pressure\": " + String(pressure, 1);
            payload += ", \"altitude\": " + String(altitude, 1);
        }

        payload += "}";

        client.publish("sensor/data", payload.c_str());  // mqtt channel publish
        Serial.println("📡 Published: " + payload);
    } else {
        Serial.println("❌ Failed to read from DHT sensor! check wiring ; ");
    }

    client.loop();
    delay(5000);  // Send data every 5 seconds
}
