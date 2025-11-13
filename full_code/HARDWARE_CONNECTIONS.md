# ESP32 IoT System - Hardware Connections Documentation

## Pin Assignments Overview

### I2C Communication (LCD Display)
- **SDA (Data)**: GPIO 21
- **SCL (Clock)**: GPIO 22
- **Purpose**: Connected to 16x2 I2C LCD display (address 0x27)

### Sensors

#### DHT11 Temperature & Humidity Sensor
- **Data Pin**: GPIO 4
- **Power**: 3.3V (from ESP32)
- **Ground**: GND
- **Pull-up Resistor**: 4.7KΩ between data pin and 3.3V (recommended)
- **Sensor Identifier**: `dht11-temperature`, `dht11-humidity`

#### DS18B20 Waterproof Temperature Sensor
- **Data Pin**: GPIO 5
- **Power**: 3.3V (from ESP32)
- **Ground**: GND
- **Pull-up Resistor**: 4.7KΩ between data pin and 3.3V (required for OneWire protocol)
- **Sensor Identifier**: `ds18b20-temperature`

### Relays (8-Channel Relay Module)

#### Relay Pin Assignments
| Relay Number | ESP32 GPIO Pin | Physical Relay Channel |
|-------------|----------------|----------------------|
| Relay 1     | GPIO 12       | Channel 1           |
| Relay 2     | GPIO 13       | Channel 2           |
| Relay 3     | GPIO 14       | Channel 3           |
| Relay 4     | GPIO 27       | Channel 4           |
| Relay 5     | GPIO 26       | Channel 5           |
| Relay 6     | GPIO 25       | Channel 6           |
| Relay 7     | GPIO 33       | Channel 7           |
| Relay 8     | GPIO 32       | Channel 8           |

#### Relay Module Specifications
- **Type**: Active LOW relay module
- **Logic**:
  - LOW (0V) = Relay ON (energized)
  - HIGH (3.3V) = Relay OFF (de-energized)
- **Power Supply**:
  - VCC: 5V (external power supply recommended)
  - GND: Common ground with ESP32
  - JD-VCC: 5V (relay coil power)
- **Contact Rating**: Typically 10A @ 250VAC / 10A @ 30VDC per channel

### Switches/Buttons

#### WiFi Reset Button
- **Button Pin**: GPIO 18
- **Configuration**: INPUT_PULLUP (internal pull-up resistor enabled)
- **Logic**: Active LOW (button press = LOW)
- **Function**:
  - Short press: No action
  - Long press (5+ seconds): Reset WiFi settings and restart ESP32

### Indicators

#### WiFi Status LED
- **LED Pin**: GPIO 2
- **Logic**: Active HIGH
- **States**:
  - HIGH: WiFi connected
  - LOW: WiFi disconnected/not connected

## Wiring Diagram Summary

```
ESP32 Dev Board
├── GPIO 2  ────────────────► WiFi Status LED ───┬─► 330Ω Resistor ──► GND
├── GPIO 4  ────────────────► DHT11 Data Pin ────┼─► 4.7KΩ Pull-up ──► 3.3V
├── GPIO 5  ────────────────► DS18B20 Data Pin ──┼─► 4.7KΩ Pull-up ──► 3.3V
├── GPIO 12 ────────────────► Relay Channel 1
├── GPIO 13 ────────────────► Relay Channel 2
├── GPIO 14 ────────────────► Relay Channel 3
├── GPIO 21 ────────────────► I2C SDA (LCD)
├── GPIO 22 ────────────────► I2C SCL (LCD)
├── GPIO 23 ────────────────► WiFi Reset Button ─┼─► GND (when pressed)
├── GPIO 25 ────────────────► Relay Channel 6
├── GPIO 26 ────────────────► Relay Channel 5
├── GPIO 27 ────────────────► Relay Channel 4
├── GPIO 32 ────────────────► Relay Channel 8
├── GPIO 33 ────────────────► Relay Channel 7
├── 3.3V   ────────────────► Sensor Power (DHT11, DS18B20)
├── 5V     ────────────────► Relay Module VCC
└── GND    ────────────────► Common Ground
```

## Hardware Requirements

### Sensors
1. **DHT11 Module**: Temperature & humidity sensor
2. **DS18B20 Waterproof Sensor**: High-precision temperature sensor
3. **16x2 I2C LCD Display**: Character LCD with I2C backpack (address 0x27)

### Actuators
1. **8-Channel Relay Module**: Active LOW type (most common)
   - Operating Voltage: 5V
   - Control Signal: 3.3V logic level compatible

### Miscellaneous
1. **Push Button**: Momentary contact switch for WiFi reset
2. **LED**: Standard 3mm/5mm LED with appropriate resistor
3. **Power Supply**:
   - ESP32: 5V USB or regulated 3.3V
   - Relay Module: Separate 5V supply (recommended for stability)

## Important Notes

### Relay Module Compatibility
- The code is configured for **Active LOW** relay modules (`RELAY_ACTIVE_LOW = true`)
- If using Active HIGH relays, change the constant to `false` and recompile
- Always use external power supply for relay coils to avoid ESP32 power issues

### Sensor Placement
- DHT11: Indoor use, avoid direct sunlight or moisture
- DS18B20: Waterproof, suitable for outdoor or wet environments
- Ensure proper pull-up resistors for reliable communication

### Button Functionality
- WiFi reset button requires 5-second long press to activate
- Button is polled in the main loop, not interrupt-driven

### Power Considerations
- Relay module draws significant current when multiple relays are active
- Use separate power supplies for ESP32 and relay module
- Ensure common ground connection between all components

## Troubleshooting

### Common Issues
1. **Relays not activating**: Check relay module type (Active LOW vs HIGH)
2. **Sensor reading failures**: Verify pull-up resistors and wiring
3. **LCD not displaying**: Check I2C address (0x27) and connections
4. **WiFi reset not working**: Ensure button is properly connected to GPIO 23

This documentation covers all hardware connections used in the ESP32 IoT system firmware. All pin assignments are configurable in the code if needed for different hardware setups.