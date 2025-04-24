#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import os
import sys
import json
import time
import pytz
import serial
import threading
import traceback
from flask import Flask, render_template, jsonify, redirect, url_for, request
from flask_cors import CORS
from threading import Thread, Lock, Event
from datetime import datetime, timedelta
import re
import copy

app = Flask(__name__)

# Global variables
nodes = {}
buffer = ""
ser = None  # Global serial connection
serial_lock = threading.Lock()  # Lock for thread-safe serial access
nodes_lock = threading.Lock()  # Lock for thread-safe node data access
mesh_id_to_static_id = {}  # Mapping of mesh IDs to static IDs - will be populated dynamically

# Constants
NODE_TIMEOUT = 120  # 2 minutes before marking a node as inactive
CLEANUP_INTERVAL = 60  # 1 minute between cleanup runs
GHOST_NODE_TIMEOUT = 1800  # 30 minutes before removing a node completely
GATEWAY_ID = 1  # Static ID of the gateway node
MAX_SENSOR_HISTORY = 50  # Maximum number of sensor readings to keep
SENSOR_TIMEOUT = 120  # 2 minutes before considering sensor data stale
MAX_MSG_SIZE = 512  # Maximum message size in bytes
JSON_RETRIES = 5  # Number of JSON parse retries
CHECK_INTERVAL = 30  # 30 seconds between checks
NODE_INACTIVE_TIMEOUT = 120 # 5 minutes before marking a node as inactive
STATUS_CHECK_INTERVAL = 30  # 30 seconds between status checks

# Add datetime filter
@app.template_filter('datetime')
def format_datetime(value):
    if isinstance(value, (int, float)):
        # Convert Unix timestamp to datetime
        value = datetime.fromtimestamp(value)
    if isinstance(value, datetime):
        return value.strftime('%d.%m.%Y %H:%M:%S')
    return str(value)

def format_timestamp(timestamp):
    """Convert Unix timestamp to human readable format."""
    try:
        dt = datetime.fromtimestamp(timestamp)
        return dt.strftime('%Y-%m-%d %H:%M:%S')
    except:
        return str(timestamp)

def is_gateway_node(node_id, data):
    """Determine if a node is a gateway based on ID and message content"""
    # Check if it's the gateway ID
    if node_id == GATEWAY_ID:
        return True
    
    # Check message type
    msg_type = data.get("type")
    if msg_type == "probe_response" and data.get("source") == GATEWAY_ID:
        return True
    
    # Check if node identifies itself as gateway
    if data.get("is_gateway", False):
        return True
        
    return False

def is_valid_message_type(msg_type):
    """Validate if a message type is recognized."""
    if isinstance(msg_type, str):
        # Include a wider range of string message types
        valid_str_types = ["probe", "probe_response", "sensor_data", "route_adv", "route_confirm", 
                          "wrapped", "time_sync", "root_announce", "network_announce"]
        return msg_type.lower() in valid_str_types
    elif isinstance(msg_type, int):
        # Accept more numeric types:
        # 1=probe, 2=probe_response, 3=sensor_data, 4=time_sync, 5/6=routing, 7=root_announce, 8=wrapped
        valid_int_types = [1, 2, 3, 4, 5, 6, 7, 8, 9]
        return msg_type in valid_int_types
    return False

def extract_value(json_text, key):
    """Extract a value from a JSON string using string operations when JSON parsing fails"""
    try:
        # Try different regex patterns for different value types
        
        # For string values: "key":"value"
        string_pattern = f'"{key}"\\s*:\\s*"([^"]*)"'
        string_match = re.search(string_pattern, json_text)
        if string_match:
            return string_match.group(1)
        
        # For numeric or boolean values: "key":value
        numeric_pattern = f'"{key}"\\s*:\\s*([0-9]+)'
        numeric_match = re.search(numeric_pattern, json_text)
        if numeric_match:
            return numeric_match.group(1)
        
        # Special case for wrapped messages - look for source or type inside msg field
        if key in ['source', 'type'] and '"msg"' in json_text:
            # Try to find the value inside the msg field
            msg_pattern = '"msg"\\s*:\\s*".*?\\\\?"' + key + '\\\\?"\\s*:\\s*(?:"?([^,"\\\\}]+)"?|([0-9]+))'
            msg_match = re.search(msg_pattern, json_text)
            if msg_match:
                # Return the first non-None group
                return msg_match.group(1) if msg_match.group(1) else msg_match.group(2)
        
        # For fields at the end of data with possible missing closing quotes
        end_pattern = f'"{key}"\\s*:\\s*"?([^",\\}}\\]]+)'
        end_match = re.search(end_pattern, json_text)
        if end_match:
            return end_match.group(1)
        
        # For quoted meshId and source fields specifically
        if key in ["meshId", "source", "from"]:
            specific_pattern = f'"{key}"\\s*:\\s*([0-9]+)'
            specific_match = re.search(specific_pattern, json_text)
            if specific_match:
                return specific_match.group(1)
        
        # Last resort: try to extract any value
        last_pattern = f'"{key}"\\s*:\\s*([^,\\}}\\]]+)'
        last_match = re.search(last_pattern, json_text)
        if last_match:
            value = last_match.group(1).strip('"')
            return value
    except Exception as e:
        print(f"❌ Error in extract_value for key '{key}': {str(e)}")
    
    return None

def unescape_json_string(json_str):
    """Recursively unescape a JSON string until it can be parsed."""
    try:
        # First try to parse as-is
        return json.loads(json_str)
    except json.JSONDecodeError:
        # If that fails, try to unescape once and parse
        try:
            # Remove one level of escaping
            unescaped = json_str.replace('\\"', '"').replace('\\\\', '\\')
            # Try to parse the unescaped string
            return json.loads(unescaped)
        except json.JSONDecodeError:
            # If still fails, try to unescape again
            try:
                # Remove another level of escaping
                double_unescaped = unescaped.replace('\\"', '"').replace('\\\\', '\\')
                return json.loads(double_unescaped)
            except json.JSONDecodeError:
                # If all attempts fail, return None
                return None

def process_json_data(json_str):
    """Process JSON data received from serial."""
    global nodes
    try:
        # Log raw data for debugging
        print(f"📥 Raw data received: {json_str}")
        
        # Try to parse the JSON
        try:
            json_data = json.loads(json_str)
            print(f"✅ Successfully parsed JSON message")
        except json.JSONDecodeError as e:
            print(f"❌ JSON decode error: {str(e)}")
            print(f"   Problematic JSON: {json_str}")
            return False
        
        # Check for message type
        msg_type = json_data.get('type')
        if msg_type is None:
            print("❌ Message missing type field")
            return False
        
        # Extract node identifiers
        mesh_id = json_data.get('meshId', json_data.get('from'))
        static_id = json_data.get('nodeId')
        source_id = json_data.get('source')  # Add source field extraction
        
        # Determine node ID - prioritize static_id, then source_id, then mesh_id
        if static_id:
            node_id = str(static_id)
            print(f"🔍 Using static node ID: {node_id}")
        elif source_id:  # Use source_id if available
            node_id = str(source_id)
            print(f"🔍 Using source ID: {node_id}")
        elif mesh_id:
            node_id = map_mesh_id_to_node(mesh_id)
            print(f"🔍 Mapped mesh ID {mesh_id} to node ID {node_id}")
        else:
            print("❌ No valid node ID found in message")
            return False
        
        # Ensure gateway node exists and is properly configured
        ensure_gateway_node_exists()
        
        # Handle different message types
        if msg_type == 3 or msg_type == "sensor_data":
            # Sensor data message
            print(f"📊 Processing sensor data from node {node_id}")
            
            # Always use current time for sensor readings
            current_time = time.time()
            formatted_time = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
            
            # Create sensor data entry
            sensor_data = {
                'timestamp': current_time,
                'formatted_time': formatted_time
            }
            
            # Extract sensor values
            if 'temp' in json_data:
                sensor_data['temperature'] = float(json_data['temp'])
            elif 'temperature' in json_data:
                sensor_data['temperature'] = float(json_data['temperature'])
                
            if 'pressure' in json_data and json_data['pressure'] is not None:
                sensor_data['pressure'] = float(json_data['pressure'])
                
            if 'humidity' in json_data and json_data['humidity'] is not None:
                sensor_data['humidity'] = float(json_data['humidity'])
            
            # Determine node type based on sensor data
            node_type = json_data.get('node_type')
            if not node_type:
                if 'pressure' in json_data and json_data['pressure'] is not None:
                    node_type = 'BMP085'
                elif 'humidity' in json_data and json_data['humidity'] is not None:
                    node_type = 'DHT11'
                else:
                    node_type = 'Sensor'
            
            with nodes_lock:
                if node_id not in nodes:
                    nodes[node_id] = {
                        'id': node_id,
                        'mesh_id': mesh_id,
                        'status': 'active',
                        'node_type': node_type,
                        'last_seen': current_time,
                        'last_seen_time': formatted_time,
                        'has_sensor': True,
                        'sensor_data': sensor_data.copy(),
                        'sensor_history': [],
                        'active': True
                    }
                    print(f"✨ Created new node {node_id}")
                
                # Update node information
                nodes[node_id].update({
                    'last_seen': current_time,
                    'last_seen_time': formatted_time,
                    'status': 'active',
                    'has_sensor': True,
                    'active': True,
                    'sensor_data': sensor_data.copy(),
                    'node_type': node_type
                                        })
                
                # Initialize sensor_history if needed
                if 'sensor_history' not in nodes[node_id]:
                    nodes[node_id]['sensor_history'] = []
                
                # Add to sensor history
                nodes[node_id]['sensor_history'].append(sensor_data.copy())
                
                # Trim history if too long
                if len(nodes[node_id]['sensor_history']) > MAX_SENSOR_HISTORY:
                    nodes[node_id]['sensor_history'] = nodes[node_id]['sensor_history'][-MAX_SENSOR_HISTORY:]
                
                print(f"✅ Updated node {node_id} with sensor data:")
                print(f"   Temperature: {sensor_data.get('temperature')}°C")
                print(f"   Pressure: {sensor_data.get('pressure')} hPa")
                print(f"   Time: {sensor_data.get('formatted_time')}")
            
            return True
        elif msg_type == 1 or msg_type == "probe":
            # Probe message
            print(f"📡 Processing probe message from node {node_id}")
            return process_probe_message(json_data)
        elif msg_type == 8 or msg_type == "wrapped":
            # Wrapped message - handle the inner message
            print(f"📦 Processing wrapped message from node {node_id}")
            if 'data' in json_data:
                try:
                    inner_data = json_data['data']
                    if isinstance(inner_data, str):
                        # Try to parse the inner message using our unescape function
                        inner_json = unescape_json_string(inner_data)
                        if inner_json is not None:
                            print(f"📥 Successfully parsed inner message: {inner_json}")
                            # Process the inner message recursively
                            return process_json_data(json.dumps(inner_json))
                        else:
                            print(f"❌ Failed to parse inner message after all unescape attempts")
                            print(f"   Inner message: {inner_data}")
                            return False
                    else:
                        # If data is already a dict, process it directly
                        print(f"📥 Processing inner message as dict: {inner_data}")
                        return process_json_data(json.dumps(inner_data))
                except Exception as e:
                    print(f"❌ Error processing wrapped message: {str(e)}")
                    return False
            return False
        
        # Handle other message types...
        return True
        
    except Exception as e:
        print(f"❌ Error processing JSON data: {str(e)}")
        traceback.print_exc()
        return False

def process_inner_message(inner_data, from_id=None):
    """Process inner messages from wrapped messages"""
    try:
        # If inner_data is already a dict, use it directly
        if isinstance(inner_data, dict):
            inner_json = inner_data
        else:
            # Try to parse the inner message
            try:
                inner_json = json.loads(inner_data)
            except json.JSONDecodeError:
                # If parsing fails, try to clean up the string
                cleaned_data = inner_data.strip()
                if cleaned_data.startswith('"') and cleaned_data.endswith('"'):
                    cleaned_data = cleaned_data[1:-1]  # Remove outer quotes
                # Replace escaped quotes with regular quotes
                cleaned_data = cleaned_data.replace('\\"', '"')
                try:
                    inner_json = json.loads(cleaned_data)
                except json.JSONDecodeError as e:
                    logger.error(f"Failed to parse inner message after cleanup: {e}")
                    logger.error(f"Original inner data: {inner_data}")
                    return

        # Process based on message type
        msg_type = inner_json.get('type')
        if msg_type == 1:  # Sensor data
            process_sensor_data_message(inner_json, from_id)
        elif msg_type == 2:  # Node info
            process_node_info_message(inner_json, from_id)
        elif msg_type == 3:  # Gateway announcement
            process_gateway_announcement(inner_json, from_id)
        elif msg_type == 4:  # Probe message
            process_probe_message(inner_json, from_id)
        elif msg_type == 5:  # Probe response
            process_probe_response(inner_json, from_id)
        else:
            logger.warning(f"Unknown inner message type: {msg_type}")
            
    except Exception as e:
        logger.error(f"Error processing inner message: {e}")
        logger.error(f"Message content: {inner_data}")

def send_serial(data):
    """Thread-safe serial write with error handling"""
    global ser
    try:
        with serial_lock:
            if ser and ser.is_open:
                ser.write(data.encode() + b'\n')
                return True
    except Exception as e:
        print(f"Error sending serial data: {str(e)}")
    return False

def check_node_status():
    """
    Periodically check node status, mark inactive nodes, and remove stale nodes.
    """
    while True:
        try:
            current_time = time.time()
            nodes_to_remove = []
            
            with nodes_lock:
                for node_id, node in list(nodes.items()):
                    # Skip the gateway node
                    if node_id == 1:
                        continue
                    
                    # Check if node hasn't been seen for a while
                    if (current_time - node['last_seen']) > NODE_INACTIVE_TIMEOUT:
                        # Mark as inactive
                        node['status'] = 'Offline'
                        print(f"🔴 Node {node_id} marked as offline (last seen {int(current_time - node['last_seen'])} seconds ago)")
                        
                        # Check if node has been inactive for too long
                        if (current_time - node['last_seen']) > GHOST_NODE_TIMEOUT:
                            nodes_to_remove.append(node_id)
                    
                    # Check for stale sensor data
                    if node['node_type'] == 'Sensor' and 'last_sensor_update' in node:
                        # If no sensor data received for a long time, recategorize as forwarder
                        if node['last_sensor_update'] and (current_time - node['last_sensor_update']) > SENSOR_DATA_TIMEOUT:
                            print(f"⚠️ Node {node_id} recategorized from Sensor to Forwarder due to stale sensor data")
                            node['node_type'] = 'Forwarder'
                
                # Remove nodes marked for removal
            for node_id in nodes_to_remove:
                if node_id in nodes:
                    print(f"❌ Removing inactive node {node_id} (last seen {int(current_time - nodes[node_id]['last_seen'])} seconds ago)")
                del nodes[node_id]
            
            # Sleep before next check
            time.sleep(STATUS_CHECK_INTERVAL)
        except Exception as e:
            print(f"❌ Error in node status check: {e}")
            traceback.print_exc()
            time.sleep(STATUS_CHECK_INTERVAL)

def read_serial_data():
    """Read data from serial port and process it."""
    global buffer, ser

    # List of possible serial ports to try
    possible_ports = ['/dev/ttyUSB0', '/dev/ttyUSB1', '/dev/ttyACM0', '/dev/ttyACM1']
    
    while True:
        try:
            # If serial port is not open or None, try to open it
            if ser is None or not ser.is_open:
                for port in possible_ports:
                    try:
                        print(f"🔌 Attempting to connect to {port}")
                        ser = serial.Serial(
                            port=port,
                            baudrate=115200,
                            timeout=1,
                            write_timeout=1,
                            inter_byte_timeout=0.1
                        )
                        print(f"✅ Connected to {port}")
                        break
                    except (serial.SerialException, OSError) as e:
                        print(f"❌ Failed to connect to {port}: {str(e)}")
                        continue
                
                if ser is None or not ser.is_open:
                    print("⚠️ Could not connect to any serial port, retrying in 5 seconds...")
                    time.sleep(5)
                    continue
            
            # Try to read data if available
            try:
                if ser.in_waiting > 0:
                    data = ser.read(ser.in_waiting).decode('utf-8', errors='replace')
                    buffer += data
                    
                    # Process complete messages
                    while "COMMUNICATION:" in buffer:
                        start_idx = buffer.find("COMMUNICATION:")
                        next_comm = buffer.find("COMMUNICATION:", start_idx + 14)
                        
                        if next_comm != -1:
                            message = buffer[start_idx:next_comm].strip()
                            buffer = buffer[next_comm:]
                        else:
                            message = buffer[start_idx:].strip()
                            buffer = ""
                        
                        if message:
                            json_str = message.replace("COMMUNICATION:", "").strip()
                            try:
                                if json_str and json_str[0] == '{' and json_str[-1] == '}':
                                    print(f"📥 Received message: {json_str}")
                                    process_json_data(json_str)
                            except json.JSONDecodeError as e:
                                print(f"❌ JSON decode error: {e} in '{json_str}'")
                            except Exception as e:
                                print(f"❌ Error processing message: {e}")
                                traceback.print_exc()
                
                # Prevent buffer from growing too large
                if len(buffer) > 2000:
                    buffer = buffer[-1000:]
                
            except (serial.SerialException, OSError) as e:
                print(f"❌ Serial read error: {str(e)}")
                # Close the port and set to None to trigger reconnection
                try:
                    if ser and ser.is_open:
                        ser.close()
                except:
                    pass
                ser = None
                time.sleep(5)
                continue
            
            # Small delay to prevent high CPU usage
            time.sleep(0.1)
            
        except Exception as e:
            print(f"❌ Unexpected error in read_serial_data: {str(e)}")
            traceback.print_exc()
            try:
                if ser and ser.is_open:
                    ser.close()
            except:
                pass
            ser = None
            time.sleep(5)

def ensure_gateway_node_exists():
    """Ensure that the gateway node exists in the nodes dictionary and is always active."""
    with nodes_lock:
        current_time = time.time()
        if "1" not in nodes:
            # Create gateway node entry
            nodes["1"] = {
                'id': "1",
                'mesh_id': "2731576727",  # Gateway's mesh ID from platformio.ini
                'status': 'active',
                'last_seen': current_time,
                'last_seen_time': datetime.now().strftime('%d.%m.%Y %H:%M:%S'),
                'has_sensor': False,
                'sensor_data': {},
                'sensor_history': [],
                'message_count': 0,
                'last_message_type': None,
                'last_message_time': None,
                'node_type': "Gateway",
                'is_gateway': True,
                'route_path': [],
                'first_seen': current_time
            }
            print("✨ Created gateway node (ID 1)")
        else:
            # Update gateway node
            nodes["1"]["is_gateway"] = True
            nodes["1"]["node_type"] = "Gateway"
            nodes["1"]["last_seen"] = current_time
            nodes["1"]["last_seen_time"] = datetime.now().strftime('%d.%m.%Y %H:%M:%S')
            nodes["1"]["status"] = "active"
            print("📡 Gateway node (ID 1) is active")

def _determine_node_type(node_id, message):
    """Determine the type of node based on message content"""
    # Gateway is always node 1
    if node_id == '1':
        return 'Gateway'
    
    # Check if the message contains a node_type field
    if 'node_type' in message:
        return message['node_type']
    
    # Check for sensor data
    if 'data' in message:
        data = message.get('data', {})
        if isinstance(data, dict):
            # If there are sensor readings, it's a sensor node
            sensor_fields = ['temp', 'humidity', 'pressure', 'gas', 'light']
            for field in sensor_fields:
                if field in data:
                    return 'Sensor'
    
    # Check direct sensor fields
    sensor_fields = ['temp', 'humidity', 'pressure', 'gas', 'light']
    for field in sensor_fields:
        if field in message and isinstance(message[field], (int, float)):
            return 'Sensor'
    
    # Check for 'role' field in message
    if 'role' in message:
        role = message.get('role', '').lower()
        if 'sensor' in role:
            return 'Sensor'
        elif 'gateway' in role:
            return 'Gateway'
        elif 'router' in role or 'forwarder' in role:
            return 'Forwarder'
    
    # Check message type for clues
    msg_type = message.get('type')
    if msg_type == 'probe_response':
        # Probe responses without explicit type or sensor data are likely forwarders
        return 'Forwarder'
    elif msg_type in [8, 9]:
        # Routing message types indicate a forwarder/router
        return 'Forwarder'
    
    # Default to Forwarder if we can't determine
    return 'Forwarder'

@app.route('/')
def index():
    """Render main page with nodes data"""
    global nodes
    current_time = time.time()
    
    with nodes_lock:
        # Create a thread-safe copy of nodes
        nodes_copy = copy.deepcopy(nodes)
    
        # Update node active status based on last seen time
        for node_id, node in nodes_copy.items():
            if node_id == "1":  # Gateway node is always active
                node['active'] = True
                continue
                
            last_seen = node.get('last_seen', 0)
            was_active = node.get('active', False)
            is_active = (current_time - last_seen) <= NODE_TIMEOUT
            
            if was_active and not is_active:
                print(f"⚠️ Node {node_id} marked inactive (last seen {int(current_time - last_seen)}s ago)")
            elif not was_active and is_active:
                print(f"✅ Node {node_id} marked active (last seen {int(current_time - last_seen)}s ago)")
            
            node['active'] = is_active
            node['status'] = 'Online' if is_active else 'Offline'
    
    print("\n🔍 Node data before rendering:")
    for node_id, node_data in nodes_copy.items():
        print(f"Node {node_id}: {node_data.get('node_type', 'Unknown')} - Active: {node_data.get('active', False)} - Status: {node_data.get('status', 'Unknown')}")
        if 'sensor_data' in node_data and node_data['sensor_data']:
            print(f"  - Latest sensor data: {node_data['sensor_data'][-1] if isinstance(node_data['sensor_data'], list) and node_data['sensor_data'] else node_data['sensor_data']}")
        print(f"  - Last seen: {format_timestamp(node_data.get('last_seen', 0))}")
    
    # Count total, active, and inactive nodes
    total_nodes = len(nodes_copy)
    active_nodes = sum(1 for node in nodes_copy.values() if node.get('active', False))
    inactive_nodes = total_nodes - active_nodes
    
    # Process node data for display
    for node_id, node in nodes_copy.items():
        # Format timestamps
        if isinstance(node.get('last_seen'), (int, float)):
            node['last_seen_formatted'] = format_timestamp(node.get('last_seen', 0))
        else:
            node['last_seen_formatted'] = str(node.get('last_seen', 'Unknown'))
        
        # Ensure node type is set
        if not node.get('node_type'):
            if node_id == "1":
                node['node_type'] = "Gateway"
            elif node.get('has_sensor', False) or (isinstance(node.get('sensor_data'), list) and len(node.get('sensor_data', [])) > 0):
                node['node_type'] = "Sensor"
            else:
                node['node_type'] = "Forwarder"
        
        # Make sure gateway is always Gateway type and active
        if node_id == "1":
            node['node_type'] = "Gateway"
            node['active'] = True
            node['status'] = 'Online'
            
        # Normalize sensor_data for the template
        if not node.get('sensor_data'):
            node['sensor_data'] = []
        elif isinstance(node.get('sensor_data'), dict):
            # Convert single reading to list format for consistency
            node['sensor_data'] = [node['sensor_data']]
            
        # Ensure each reading has a formatted timestamp
        if isinstance(node['sensor_data'], list):
            for reading in node['sensor_data']:
                if isinstance(reading, dict):
                    if 'timestamp' in reading and 'timestamp_formatted' not in reading:
                        reading['timestamp_formatted'] = format_timestamp(reading['timestamp']) if isinstance(reading['timestamp'], (int, float)) else str(reading['timestamp'])
                    # Ensure all sensor values are properly formatted
                    if 'temp' in reading:
                        reading['temp'] = float(reading['temp'])
                    if 'pressure' in reading:
                        reading['pressure'] = float(reading['pressure'])
    
    return render_template('index.html',
                           nodes=nodes_copy,
                         total_nodes=total_nodes,
                         active_nodes=active_nodes,
                         inactive_nodes=inactive_nodes)

def initialize_known_nodes():
    """Initialize only the gateway node."""
    print("🔍 Initializing gateway node...")
    
    # Initialize gateway
    ensure_gateway_node_exists()
    
    # Remove DHT sensor node and BMP085 sensor node placeholder initializations
    # as we want to work only with real data from nodes
    
    print("✅ Gateway node initialized")

def map_mesh_id_to_node(mesh_id, possible_node_id=None):
    """Map a mesh ID to a node ID, using known mappings or creating a new one"""
    if mesh_id is None and possible_node_id is None:
        return "unknown"
        
    # Always convert to strings for consistent comparison
    str_mesh_id = str(mesh_id) if mesh_id is not None else None
    str_node_id = str(possible_node_id) if possible_node_id is not None else None
    
    # Priority 1: If we have a valid static node ID, use it directly
    if str_node_id is not None:
        if str_node_id == "1" or str_node_id == "3" or str_node_id == "4":
            print(f"🔍 Using provided static node ID: {str_node_id}")
            # Store the mapping for future use
            if str_mesh_id:
                mesh_id_to_static_id[str_mesh_id] = str_node_id
                print(f"🔗 Mapped mesh ID {str_mesh_id} → Node ID {str_node_id}")
            return str_node_id
    
    # Priority 2: Check existing mesh ID mapping
    if str_mesh_id and str_mesh_id in mesh_id_to_static_id:
        node_id = mesh_id_to_static_id[str_mesh_id]
        print(f"🔍 Using existing mapping: Mesh ID {mesh_id} → Node ID {node_id}")
        return node_id
    
    # Priority 3: Check for known mesh IDs from platformio.ini
    if mesh_id == 109443093:
        node_id = "4"  # Node 4 from platformio.ini (BMP085 sensor node)
        mesh_id_to_static_id[str_mesh_id] = node_id
        print(f"🔗 Mapped mesh ID {mesh_id} to Node {node_id} (based on platformio.ini - BMP085 node)")
        return node_id
    elif mesh_id == 2440625153:
        node_id = "3"  # Node 3 from platformio.ini
        mesh_id_to_static_id[str_mesh_id] = node_id
        print(f"🔗 Mapped mesh ID {mesh_id} to Node {node_id} (based on platformio.ini)")
        return node_id
    elif mesh_id == 2731576727:
        node_id = "1"  # Gateway Node
        mesh_id_to_static_id[str_mesh_id] = node_id
        print(f"🔗 Mapped mesh ID {mesh_id} to Node {node_id} (Gateway)")
        return node_id
        
    # Priority 4: Use provided node ID if available
    if str_node_id is not None:
        # If we get here, it means the provided node ID wasn't a known static ID
        # But we'll still use it if we have nothing better
        mesh_id_to_static_id[str_mesh_id] = str_node_id
        print(f"🔗 Mapped mesh ID {mesh_id} to Node {str_node_id} (from message)")
        return str_node_id
    
    # Last resort: use mesh ID as node ID
    if str_mesh_id:
        print(f"⚠️ No known mapping for mesh ID {mesh_id}, using it as node ID")
        return str_mesh_id
        
    return "unknown"

def process_probe_message(data):
    """Process probe or probe_response messages and update node information."""
    print(f"📡 Processing probe message: {data}")
    
    # Extract mesh ID and try to map to a node ID
    mesh_id = data.get('meshId')
    if not mesh_id:
        print("⚠️ Missing mesh ID in probe message")
        return
    
    # Determine node ID - use static ID if available, otherwise map the mesh ID
    node_id = data.get('nodeId')
    if not node_id:
        # Try to get from source field
        node_id = data.get('source')
        if not node_id:
            # Try to map mesh ID to node ID
            node_id = map_mesh_id_to_node(mesh_id)
    
    print(f"🔍 Processing probe for node {node_id} (mesh ID: {mesh_id})")
    
    # Check for node type information
    node_type = None
    has_sensor = False
    
    # Check for node_type field
    if 'node_type' in data:
        node_type = data['node_type']
        print(f"📝 Node identifies as {node_type}")
        
        # If BMP085 or sensor is in the type, it's a sensor node
        if 'BMP085' in node_type or 'sensor' in node_type.lower() or 'DHT11' in node_type:
            has_sensor = True
    
    # Check for has_sensor field
    if 'has_sensor' in data:
        has_sensor = bool(data['has_sensor'])
        if has_sensor:
            print(f"✅ Node {node_id} reports having a sensor")
            # Ensure node type reflects sensor capability
            if not node_type or node_type == "Forwarder":
                node_type = "Sensor"
    
    # Update the node in our database
    with nodes_lock:
        # Check if this node already exists
        if str(node_id) not in nodes:
            # Create new node entry
            nodes[str(node_id)] = {
                'id': node_id,
                'mesh_id': mesh_id,
                'status': 'active',
                'last_seen': time.time(),
                'probe_count': 1,
                'has_sensor': has_sensor,
                'route_path': [],
                'sensor_history': []
            }
            
            # Set node type
            if node_type:
                nodes[str(node_id)]['node_type'] = node_type
            elif str(node_id) == "1":
                nodes[str(node_id)]['node_type'] = "Gateway"
            elif has_sensor:
                nodes[str(node_id)]['node_type'] = "Sensor"
            else:
                nodes[str(node_id)]['node_type'] = "Forwarder"
            
            print(f"✨ Created new node {node_id} as {nodes[str(node_id)]['node_type']}")
        else:
            # Update existing node
            nodes[str(node_id)]['last_seen'] = time.time()
            nodes[str(node_id)]['status'] = 'active'
            nodes[str(node_id)]['probe_count'] = nodes[str(node_id)].get('probe_count', 0) + 1
            
            # Update mesh ID if not already set
            if 'mesh_id' not in nodes[str(node_id)] or not nodes[str(node_id)]['mesh_id']:
                nodes[str(node_id)]['mesh_id'] = mesh_id
            
            # Update sensor status if node reports having a sensor
            if has_sensor:
                nodes[str(node_id)]['has_sensor'] = True
                if 'node_type' not in nodes[str(node_id)] or nodes[str(node_id)]['node_type'] == "Forwarder":
                    nodes[str(node_id)]['node_type'] = "Sensor"
            
            # Update node type if provided
            if node_type and (
                'node_type' not in nodes[str(node_id)] or 
                (node_type != "Forwarder" and nodes[str(node_id)]['node_type'] == "Forwarder")
            ):
                nodes[str(node_id)]['node_type'] = node_type
            
            print(f"🔄 Updated node {node_id} ({nodes[str(node_id)]['node_type']})")
        
        # Update RSSI and hops if available
        if 'rssi' in data:
            nodes[str(node_id)]['rssi'] = data['rssi']
        if 'hops' in data:
            nodes[str(node_id)]['hops'] = data['hops']
    
    return True

def update_node_from_message(node_id, data):
    """Update or create a node entry based on message data"""
    try:
        # Get current time
        current_time = time.time()
        
        # Extract mesh ID and static ID
        mesh_id = str(data.get('meshId', '')) if data.get('meshId') else None
        static_id = str(data.get('staticId', '')) if data.get('staticId') else None
        
        # Thread-safe node update
        with nodes_lock:
            # Create new node if it doesn't exist
            if node_id not in nodes:
                nodes[node_id] = {
                    'id': node_id,
                    'mesh_id': mesh_id,
                    'static_id': static_id,
                    'status': 'online',
                    'last_seen': current_time,
                    'last_seen_time': datetime.now().strftime('%d.%m.%Y %H:%M:%S'),
                    'has_sensor': False,
                    'sensor_data': {},
                    'sensor_history': [],
                    'message_count': 0,
                    'last_message_type': None,
                    'last_message_time': None,
                    'node_type': None,
                    'is_gateway': False,
                    'route_path': [],
                    'first_seen': current_time
                }
                print(f"➕ Created new node entry for Node {node_id}")
            
            # Update existing node
            nodes[node_id]['last_seen'] = current_time
            nodes[node_id]['last_seen_time'] = datetime.now().strftime('%d.%m.%Y %H:%M:%S')
            nodes[node_id]['status'] = 'online'
            
            # Update IDs if provided
            if mesh_id:
                nodes[node_id]['mesh_id'] = mesh_id
            if static_id:
                nodes[node_id]['static_id'] = static_id
                
            # Update message tracking
            nodes[node_id]['message_count'] += 1
            nodes[node_id]['last_message_type'] = data.get('type')
            nodes[node_id]['last_message_time'] = current_time
            
            # Update node type based on message content
            msg_type = data.get('type')
            if msg_type == 7:  # Gateway announcement
                nodes[node_id]['node_type'] = 'Gateway'
                nodes[node_id]['is_gateway'] = True
            elif msg_type == 3:  # Sensor data
                nodes[node_id]['node_type'] = 'Sensor'
                nodes[node_id]['has_sensor'] = True
            elif not nodes[node_id]['node_type']:  # Only set if not already determined
                nodes[node_id]['node_type'] = _determine_node_type(node_id, data)
            
            # Process sensor data if present
            if 'sensor' in data:
                sensor_data = extract_sensor_data(data['sensor'])
                if sensor_data:
                    nodes[node_id]['has_sensor'] = True
                    nodes[node_id]['sensor_data'] = sensor_data
                    nodes[node_id]['sensor_history'].append(sensor_data)
                    # Keep only the last MAX_SENSOR_HISTORY readings
                    if len(nodes[node_id]['sensor_history']) > MAX_SENSOR_HISTORY:
                        nodes[node_id]['sensor_history'] = nodes[node_id]['sensor_history'][-MAX_SENSOR_HISTORY:]
            
            # Update route path if available
            if 'path' in data:
                nodes[node_id]['route_path'] = data['path']
                    
            print(f"📝 Updated Node {node_id} (Type: {nodes[node_id]['node_type']})")
            return True
            
    except Exception as e:
        print(f"❌ Error updating node {node_id}: {str(e)}")
        traceback.print_exc()
        return False

def extract_sensor_data(data):
    """Extract and format sensor data from message"""
    try:
        if not data:
            return None
            
        sensor_data = {}
        current_time = time.time()
        
        # Handle nested sensor data
        if isinstance(data, dict):
            # Temperature
            if 'temperature' in data:
                sensor_data['temperature'] = float(data['temperature'])
            elif 'temp' in data:
                sensor_data['temperature'] = float(data['temp'])
                
            # Pressure
            if 'pressure' in data:
                sensor_data['pressure'] = float(data['pressure'])
            elif 'press' in data:
                sensor_data['pressure'] = float(data['press'])
                
            # Humidity
            if 'humidity' in data:
                sensor_data['humidity'] = float(data['humidity'])
            elif 'hum' in data:
                sensor_data['humidity'] = float(data['hum'])
                
            # Add timestamp if not present
            if 'timestamp' not in data:
                sensor_data['timestamp'] = current_time
            else:
                timestamp = data['timestamp']
                # Convert millis to seconds if needed
                if timestamp > 1000000000000:  # If timestamp is in milliseconds
                    timestamp = timestamp / 1000
                sensor_data['timestamp'] = timestamp
                
            # Format timestamp
            sensor_data['formatted_time'] = format_timestamp(sensor_data['timestamp'])
            
            return sensor_data
            
        return None
        
    except Exception as e:
        print(f"❌ Error extracting sensor data: {str(e)}")
        traceback.print_exc()
        return None

def process_sensor_data_message(data):
    """Process sensor data messages (type 3)."""
    print(f"📊 Processing sensor data from node {data.get('nodeId', 'unknown')}")
    
    # Extract node identifiers
    mesh_id = data.get('meshId', data.get('from'))
    static_id = data.get('nodeId')
    source_id = data.get('source')
    
    # Determine which ID to use for node identification
    if static_id:
        node_id = str(static_id)
        print(f"🔍 Using static node ID: {node_id}")
    elif mesh_id:
        node_id = str(map_mesh_id_to_node(mesh_id))
        print(f"🔍 Mapped mesh ID {mesh_id} to node ID {node_id}")
    else:
        print("❌ No valid node ID found in sensor data message")
        return
    
    # Create sensor data entry with proper timestamp handling
    current_time = time.time()
    msg_timestamp = data.get('timestamp', current_time)
    
    # Convert milliseconds to seconds if needed
    if isinstance(msg_timestamp, (int, float)) and msg_timestamp > 1000000000000:  # If timestamp is in milliseconds
        msg_timestamp = msg_timestamp / 1000
    
    # Create a new sensor data reading
    sensor_data = {
        'timestamp': msg_timestamp,
        'formatted_time': format_timestamp(msg_timestamp)
    }
    
    # Extract sensor values with proper type conversion
    if 'temp' in data:
        sensor_data['temperature'] = float(data['temp'])
    elif 'temperature' in data:
        sensor_data['temperature'] = float(data['temperature'])
    
    if 'pressure' in data:
        sensor_data['pressure'] = float(data['pressure'])
    
    if 'humidity' in data:
        sensor_data['humidity'] = float(data['humidity'])
    
    # Update node information
    with nodes_lock:
        if node_id not in nodes:
            # Create new node entry
            nodes[node_id] = {
                'id': node_id,
                'mesh_id': mesh_id,
                'status': 'active',
                'node_type': data.get('node_type', 'BMP085' if node_id == '4' else 'Sensor'),
                'last_seen': current_time,
                'last_seen_time': format_timestamp(current_time),
                'has_sensor': True,
                'sensor_data': sensor_data.copy(),  # Store a copy of the current reading
                'sensor_history': [],
                'active': True
            }
            print(f"✨ Created new node {node_id}")
        
        # Update existing node
        nodes[node_id].update({
            'last_seen': current_time,
            'last_seen_time': format_timestamp(current_time),
            'status': 'active',
            'has_sensor': True,
            'active': True,
            'sensor_data': sensor_data.copy()  # Update current reading
        })
        
        # Initialize sensor_history if it doesn't exist
        if 'sensor_history' not in nodes[node_id]:
            nodes[node_id]['sensor_history'] = []
        
        # Add new reading to history
        nodes[node_id]['sensor_history'].append(sensor_data.copy())
        
        # Keep only last MAX_SENSOR_HISTORY readings
        if len(nodes[node_id]['sensor_history']) > MAX_SENSOR_HISTORY:
            nodes[node_id]['sensor_history'] = nodes[node_id]['sensor_history'][-MAX_SENSOR_HISTORY:]
        
        print(f"✅ Updated node {node_id} with new sensor data")
        print(f"   Temperature: {sensor_data.get('temperature')}°C")
        print(f"   Pressure: {sensor_data.get('pressure')} hPa")
        print(f"   Time: {sensor_data.get('formatted_time')}")
    
    # WebSocket support if available
    try:
        if 'web_socket' in globals() and web_socket:
            web_socket.send_update('sensor_data', {
                'node_id': node_id,
                'data': sensor_data
            })
    except NameError:
        # web_socket may not be defined, which is fine
        pass

if __name__ == '__main__':
    # Initialize gateway node
    ensure_gateway_node_exists()
    
    # Start the node status checker thread
    status_thread = threading.Thread(target=check_node_status)
    status_thread.daemon = True
    status_thread.start()
    
    # Start the serial reader thread
    serial_thread = threading.Thread(target=read_serial_data)
    serial_thread.daemon = True
    serial_thread.start()
    
    print("✅ Gateway server started - Open http://localhost:5000 in your browser")
    app.run(host='0.0.0.0', port=5000)