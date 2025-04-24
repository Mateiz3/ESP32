

1. Install Python dependencies:
```bash
pip install -r requirements.txt
```


2. Run the server:
```bash
python gateway.py
```

3. Open  `http://localhost:5000`

## Features

- Real-time display of sensor data from multiple ESP32/ESP8266 nodes
- Automatic detection of new nodes in the mesh network
- Web interface for easy monitoring
- Support for multiple sensor types (temperature, humidity, air pressure etc.)

## Requirements

- Python 3.6 or higher
- ESP32 / ESP8266 device with painlessmesh


## Notes

- For ESP8266: Make sure to adjust the serial port name in the gateway code i(default is `/dev/ttyUSB0`)
- The mesh network code is compatible with both ESP32 and ESP8266
- DHT/BMP sensor pin assignments may need to be adjusted based on your specific ESP8266 board 
