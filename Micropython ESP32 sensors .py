#install/preload dht and sdcard modules 
#format sd as fat32!!!
#check/change pins used (18, 23, and 19)
import os
import machine
import time
from machine import Pin, I2C
from sdcard import SDCard

import dht  # DHT11/DHT22

# (DHT22/DHT11)
dht_sensor = dht.DHT22(Pin(4))  # Pin 4 (default) 

# 3rd Sensor                                                           #to be changed
third_sensor_pin = machine.ADC(Pin(36))  # ADC pin 36 - analog sensor 
third_sensor_pin.atten(machine.ADC.ATTN_11DB)  #  for 0-3.3V range

# Initialize SD 
spi = machine.SPI(2, baudrate=1000000, sck=Pin(18), mosi=Pin(23), miso=Pin(19))   
cs = Pin(5, Pin.OUT)
sd = SDCard(spi, cs)

# Mount SD 
vfs = os.VfsFat(sd)
os.mount(vfs, "/sd")

# File paths 
temp_file = "/sd/temperature.txt"
humidity_file = "/sd/humidity.txt"
third_sensor_file = "/sd/third_sensor.txt"

def write_to_file(file_path, data):
    try:
        with open(file_path, "a") as f:
            f.write(data + "\n")
    except Exception as e:
        print(f"Error writing to {file_path}: {e}")

def read_sensors():
    try:
        dht_sensor.measure()
        temperature = dht_sensor.temperature()
        humidity = dht_sensor.humidity()
    except Exception as e:
        print("Error reading sensor:", e)
        temperature = None
        humidity = None

    try:
        third_sensor_value = third_sensor_pin.read()
    except Exception as e:
        print("Error reading 3rd sensor:", e)                           #to be changed 
        third_sensor_value = None

    return temperature, humidity, third_sensor_value

def main():
    while True:
        temperature, humidity, third_sensor_value = read_sensors()

        timestamp = time.strftime("%Y-%m-%d %H:%M:%S")

        if temperature is not None:
            write_to_file(temp_file, f"{timestamp}, {temperature}")
            print(f"Temperature: {temperature}C")

        if humidity is not None:
            write_to_file(humidity_file, f"{timestamp}, {humidity}")
            print(f"Humidity: {humidity}%")

        if third_sensor_value is not None:
            write_to_file(third_sensor_file, f"{timestamp}, {third_sensor_value}")
            print(f"3rd Sensor Value: {third_sensor_value}")              #to be changed

        time.sleep(1)  # Delay readings (time in seconds) 

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("Program stopped.")
