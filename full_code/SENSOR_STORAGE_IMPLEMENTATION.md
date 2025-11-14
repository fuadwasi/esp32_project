# Sensor Data Storage and Sync Implementation

## Overview

This document describes the implementation of sensor data storage and synchronization system for the ESP32 IoT device. Instead of sending sensor readings immediately, the system now stores them locally and syncs periodically or on-demand.

## Key Changes

### 1. **File-Based Storage**
- **Library Added**: `LittleFS` for file system operations
- **Storage File**: `/sensor_data.json` - Contains all unsent sensor readings
- **Format**: JSON array of sensor reading objects with timestamps

### 2. **Sensor Data Collection**
- **Frequency**: Every `sensorInterval` milliseconds (default: 60 seconds)
- **Storage**: Each reading is immediately saved to the JSON file
- **No Immediate API Calls**: Sensor readings are no longer sent directly to the API

### 3. **Data Synchronization Triggers**

#### **Periodic Sync (5 Minutes)**
- **Interval**: `dataSyncInterval = 5UL * 60UL * 1000UL` (5 minutes)
- **Function**: Automatically sends all stored sensor data to the API
- **Cleanup**: Deletes the JSON file after successful transmission

#### **MQTT Sync Message**
- **Trigger**: `{"t":"Sync","d":null}` message on MQTT topic
- **Function**: Immediately sends all stored sensor data to the API
- **Cleanup**: Deletes the JSON file after successful transmission

#### **MQTT Ping Message**
- **Trigger**: `{"t":"Ping","d":null}` message on MQTT topic
- **Function**: Sends stored sensor data + triggers immediate sensor reading + sends API ping
- **Cleanup**: Deletes the JSON file after successful transmission

#### **Startup Check**
- **Trigger**: Device boot-up
- **Function**: If `/sensor_data.json` exists, sends all stored data to API
- **Cleanup**: Deletes the JSON file after successful transmission

### 4. **Data Format**

#### **Stored JSON Structure**
```json
[
  {
    "sensorIdentifier": "dht11-temperature",
    "numericValue": 25.5,
    "textValue": "25.50 °C",
    "capturedOnUtc": "2024-11-14T10:30:00Z"
  },
  {
    "sensorIdentifier": "dht11-humidity",
    "numericValue": 65.2,
    "textValue": "65.20 %",
    "capturedOnUtc": "2024-11-14T10:30:00Z"
  }
]
```

#### **API Payload**
Same JSON array format sent to the sensor readings endpoint.

## Functions Added

### **Storage Functions**
- `saveSensorReadingToFile(SensorReading reading)` - Saves individual reading to JSON file
- `sendStoredSensorData()` - Sends all stored data to API and deletes file
- `checkAndSendStoredDataOnStartup()` - Checks for stored data on device startup

### **Configuration**
- `SENSOR_DATA_FILE "/sensor_data.json"` - File path constant
- `dataSyncInterval` - 5-minute periodic sync interval
- `lastDataSyncMillis` - Timestamp tracking for periodic syncs

## Behavior Changes

### **Before (Immediate Sending)**
1. Read sensors every 60 seconds
2. Send readings directly to API
3. Buffer failed transmissions for retry

### **After (Storage + Sync)**
1. Read sensors every 60 seconds
2. Save readings to local JSON file
3. Send stored data every 5 minutes OR on MQTT triggers
4. Send any existing data on device startup

## MQTT Commands

### **Trigger Data Sync**
```bash
mosquitto_pub -h 103.181.42.126 -u fuad -P orbiqlabsMqtt159 -t "devices/{device_id}" -m '{"t":"Sync","d":null}'
```

### **Trigger Ping + Data Sync**
```bash
mosquitto_pub -h 103.181.42.126 -u fuad -P orbiqlabsMqtt159 -t "devices/{device_id}" -m '{"t":"Ping","d":null}'
```

## Benefits

1. **Offline Resilience**: Device can collect data even when API is unreachable
2. **Batch Processing**: Reduces API call frequency and network overhead
3. **Data Preservation**: No data loss during network outages
4. **On-Demand Sync**: Remote triggering of data transmission
5. **Startup Recovery**: Automatically recovers unsent data after restarts

## Error Handling

- **File Operations**: Logged failures for file read/write operations
- **API Failures**: Stored data remains in file for retry on next sync
- **JSON Parsing**: Corrupted files are deleted and recreated
- **Network Issues**: Data accumulates until connection is restored

## Monitoring

### **Serial Output Examples**
```
[2024-11-14 10:30:00] Sensor reading saved to file: dht11-temperature = 25.50
[2024-11-14 10:35:00] 🔄 Periodic data sync - sending stored sensor data
[2024-11-14 10:35:00] ✅ Stored sensor data sent successfully and file deleted
```

### **File System Usage**
- Check available space with LittleFS.info()
- Monitor file size growth during network outages
- Automatic cleanup after successful transmissions

## Configuration Options

### **Timing Configuration**
```cpp
unsigned long sensorInterval = 60UL * 1000UL;     // Sensor reading interval (60 seconds)
unsigned long dataSyncInterval = 5UL * 60UL * 1000UL; // Data sync interval (5 minutes)
```

### **File System**
- Uses LittleFS for reliable file operations
- Automatic initialization on startup
- Error handling for file system failures

## Testing

### **Verify Storage**
1. Disconnect network connectivity
2. Monitor sensor readings in serial output
3. Check that readings are saved to file (no API calls)

### **Verify Sync**
1. Reconnect network
2. Wait for 5-minute interval or send MQTT sync command
3. Verify data transmission and file cleanup

### **Verify Startup Recovery**
1. Power cycle device with stored data
2. Check serial output for startup data sync
3. Verify file is deleted after successful transmission