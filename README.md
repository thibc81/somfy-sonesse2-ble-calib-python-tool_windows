# somfy-sonesse2-ble-calib-tool-esp

ESP32-based BLE tool for configuring and recovering **Somfy Sonesse2 Zigbee** motors
(Roller, Venetian, and other variants) without the official Somfy remote or TaHoma Pro
app.

Exposes an interactive serial CLI over USB that lets you authenticate, calibrate limits,
control movement, read/write internal config files, and factory-reset the motor -- all
through the motor's Bluetooth Low Energy (BLE) GATT interface.

## Why does this exist?

Somfy Sonesse2 Zigbee motors use the standard Zigbee Window Covering cluster, but they
have a catch: **the motor will not respond to Zigbee movement commands unless it has valid
end limits configured**. If the motor loses its calibration (e.g. after a power outage,
firmware glitch, or accidental reset), it enters a state where:

- It stays on the Zigbee network (LED blinks on commands)
- It responds to Zigbee attribute reads (partially)
- `configStatus` reports bit 0 = 0 (**not operational**)
- Position/limit attributes return `UNSUPPORTED_ATTRIBUTE` or `CALIBRATION_ERROR`
- **All movement commands are silently ignored**

Additionally, **Venetian blind motors may ship with Application type set to "Roller"**
in their internal config, which disables tilt functionality entirely. This can only be
changed through the BLE config file interface.

The official recovery paths all require hardware or accounts most users don't have:

| Method | Requirement |
|--------|-------------|
| TaHoma Pro app | Somfy installer customer number (gated) |
| Ysia Zigbee remote | Purchased separately, ~$80+ |
| Physical PROG button | Often buried inside the blind tube, inaccessible |

However, the motors also expose a **BLE GATT interface** alongside Zigbee. This interface
accepts configuration commands after authentication with a PIN code printed on the motor
label. This tool uses that interface.

## BLE Protocol

The BLE protocol was reverse-engineered by
[mrmelair](https://mrmelair.com/2024/07/10/hacking-the-somfy-sonesse2-zigbee-motors/)
by decompiling the TaHoma Pro Android APK. All characteristics share the base UUID
`xxxxxxxx-cad9-46c6-a2ea-2ca16d57b4a5`.

### Known Characteristics

Confidence: `***` = tested, `**` = confirmed by reverse engineering, `*` = guessed from
context, `?` = unknown.

#### Service 00000000 -- Operational

| UUID Prefix | Function | Confidence | Parameters |
|-------------|----------|:----------:|------------|
| `0000000b` | **Authentication** | *** | PIN as 3-byte little-endian int |
| `00000001` | Identify (jog) | *** | `0x01` |
| `00000005` | Go to position (lift) | *** | 16-bit LE, 0=open, 32767=closed |
| `00000006` | Stop | *** | `0x01` |
| `00000007` | Tilt position/state? | * | Reads 1 byte on venetian motors |
| `00000008` | Tilt open? | * | Write-only on venetian motors |
| `00000009` | Tilt close? | * | Write-only on venetian motors |
| `0000000a` | Delivery mode | ** | `0x01` (motor off until button press) |
| `0000000d` | Open (venetian) | ** | `0x01` -- not present on all motors |
| `0000000e` | Close (venetian) | ** | `0x01` -- not present on all motors |

#### Service 00010000 -- Configuration

| UUID Prefix | Function | Confidence | Parameters |
|-------------|----------|:----------:|------------|
| `00010001` | **Factory reset** | ** | `0x01` |
| `00010002` | Firmware version | ** | Reads ASCII string |
| `00010005` | Set direction | ** | `0x00`=CCW, `0x01`=CW |
| `00010007` | Set limit (lift) | ** | `0x00`=upper, `0x01`=lower |
| `00010008` | Move down (lift) | *** | 16-bit LE step (1-500) |
| `00010009` | Move up (lift) | *** | 16-bit LE step (1-500) |
| `0001000a` | Tilt limit? | * | Reads `0x01` on venetian motors |
| `0001000b` | Configure range | ** | `0x0000`=Start, `0x0001`=Full, `0x0002`=Half |

#### Service 00020000 -- Network / Zigbee

| UUID Prefix | Function | Confidence | Notes |
|-------------|----------|:----------:|-------|
| `00020001` | **Leave Zigbee network** | *** | `0x01` -- triggers rejoin |
| `00020002` | Zigbee channel | ** | Reads 1 byte |
| `00020003` | PAN ID / network addr | * | Reads 2 bytes LE |
| `00020005` | Zigbee EUI64 | ** | Reads 8 bytes, little-endian |
| `00020006` | Install code | * | Reads 18 bytes |

#### Service 00040000 -- Config Files (WriteData/CBOR)

| UUID Prefix | Function | Confidence | Notes |
|-------------|----------|:----------:|-------|
| `00040001` | Config file R/W | ** | CBOR protocol for motor config |

### Config Files (CBOR Protocol)

The motor stores configuration in files accessible through a binary protocol on
characteristic `00040001`. Files are encoded in CBOR (Concise Binary Object
Representation). Each value is stored as `[current_value, metadata]` where metadata
includes writability, valid ranges, units, and enum options.

| File ID | Name | Contents |
|---------|------|----------|
| `0x00C4` | **motor** | Application type, LiftRange, ReversedDirection, NominalSpeed, ramps, intermediate positions |
| `0x00C3` | **radio** | DeviceName, EndProduct, ZigbeeTxPower, BleTxPower, StepLiftConversion, StepTiltConversion |
| `0x00D2` | **type** | Firmware versions, hardware info, motor model (read-only) |
| `0x00C2` | **hmi** | HMI config (often empty) |

The `Application` field in the motor config is particularly important: it controls
whether the motor operates as `"Roller"`, `"Venetian"`, `"Sheer"`, or `"Zebra"`.
A venetian motor with Application set to "Roller" will have tilt disabled.

## Hardware

Any ESP32 board with BLE support will work. Tested on:

- **M5Stack Atom Lite** (ESP32-PICO-D4)

Other boards: change the FQBN in `flash.sh` or pass it as the second argument. Common
FQBNs:

```
esp32:esp32:m5stack_atom        # M5Stack Atom
esp32:esp32:esp32               # Generic ESP32 DevKit
esp32:esp32:m5stack_core        # M5Stack Core
esp32:esp32:esp32s3             # ESP32-S3 boards
```

## Prerequisites

- [arduino-cli](https://arduino.github.io/arduino-cli/) installed and in PATH
- ESP32 Arduino core (`arduino-cli core install esp32:esp32` -- the flash script will
  install it automatically if missing)
- A serial terminal: `picocom`, `screen`, or `minicom`

## Flashing

```bash
# Auto-detect port, default board (M5Stack Atom)
./flash.sh

# Specify port
./flash.sh /dev/ttyUSB0

# Specify port and board
./flash.sh /dev/ttyUSB0 esp32:esp32:esp32

# macOS example
./flash.sh /dev/cu.usbserial-0001
```

Then connect:

```bash
picocom /dev/ttyUSB0 -b 115200
```

## Motor Label

Before using the tool, locate the label on your motor (usually on the tube head or body).
You need:

- **PIN code** -- numeric code (e.g. `123456`), used for BLE authentication
- **EUI48** -- BLE MAC address (e.g. `4C:C2:06:AA:BB:CC`)
- **EUI64** -- Zigbee IEEE address (e.g. `4CC206FFFEAABBCC`)

If you can't read the label but know the Zigbee EUI64 (visible in zigbee2mqtt), the BLE
MAC is usually the EUI64 with the `FFFE` middle bytes removed:

```
Zigbee: 4CC206 FFFE AABBCC
BLE:    4C:C2:06:AA:BB:CC
```

## CLI Reference

The CLI supports command history (arrow up/down), backspace editing, Ctrl-U to clear
the line, and Ctrl-A to recall the last command.

### Setup Commands

```
scan                    Scan for nearby BLE devices (5 seconds)
target <mac|#>          Set target by MAC address or scan result number
pin <code>              Set PIN code (from motor label)
connect                 Connect to target motor via BLE
disconnect              Disconnect
auth                    Authenticate with PIN (required before any control)
status                  Show connection status
```

### Diagnostics

```
identify                Make the motor jog (brief up/down) to identify itself
services                List all BLE services with annotated characteristics
                        (shows confidence levels and labels for known UUIDs)
info                    Read device info: name, manufacturer, firmware, battery,
                        Zigbee channel, PAN ID, EUI64, install code
```

### Movement

```
up [step]               Move up (step: 1-500, default 100)
down [step]             Move down (step: 1-500, default 100)
stop                    Stop motor movement
goto <pos>              Go to position (0 = fully open, 32767 = fully closed)
open                    Full open (venetian blinds, not on all motors)
close                   Full close (venetian blinds, not on all motors)
```

### Calibration

```
range start             Begin range configuration (required before setting limits)
range half              Half range mode
range full              Full range mode
limit up                Set current position as UPPER end limit
limit down              Set current position as LOWER end limit
dir cw                  Set motor direction to clockwise
dir ccw                 Set motor direction to counter-clockwise
```

### Config Files

Read and write the motor's internal CBOR configuration files.

```
config list             Show available config files with descriptions
config read <file>      Read and decode a config file (pretty-printed CBOR)
config dump <file>      Raw hex dump of a config file (for backup)
config set <file> <key> <value>
                        Set a field in a config file (with confirmation)
                        Requires typing 'yes-config-write' to confirm

Files: motor, radio, type, hmi
```

Example:

```
somfy> config read motor
  [OK] Got 245 bytes, decoding CBOR:
  ┌─────────────────────────────────────────────
  │ Application: "Roller" (writable)
  │        enum: Roller, Venetian, Zebra, Sheer
  │ LiftRange: 4635 (writable, 480-48000 pulse)
  │ ReversedDirection: false (writable)
  │ IntermediatePositionsLift: [16 items]
  └─────────────────────────────────────────────

somfy> config set motor Application Venetian
  [..] Setting motor.Application = Venetian
  [..] CBOR (21 bytes): A1 6B 41 70 70 6C 69 ...
  [!] This writes to motor config via CBOR protocol.
  [!] Type 'yes-config-write' to confirm.

somfy> yes-config-write
  [OK] Config written! Power-cycle the motor for changes to take effect.
```

### Raw BLE Access

For exploring unknown characteristics or debugging.

```
read <prefix>           Read a characteristic by UUID prefix
                        e.g.: read 00000007
write <prefix> <hex>    Write raw bytes to a characteristic
                        e.g.: write 00000007 01
                        e.g.: write 0001000a 00 01
```

### Zigbee Helpers

```
calibration on|off      Show MQTT command for Zigbee calibration mode
```

### Danger Zone

```
factory-reset           Full factory reset (erases limits, network, all settings)
                        Requires typing 'yes-reset' to confirm
leave-network           Leave current Zigbee network (motor will try to rejoin)
                        Requires typing 'yes-leave' to confirm
delivery-mode           Switch motor off until physical button is pressed
```

### Other

```
help                    Show command list
wizard                  Show step-by-step calibration guide
```

## Typical Workflows

### Recovering a motor that stopped responding after power outage

This is the most common scenario. The motor is on the Zigbee network, LED blinks on
commands, but it won't move. The `configStatus` attribute reads with bit 0 = 0
(not operational) and position attributes return errors.

```
somfy> scan
  (find your motor in the list)

somfy> target AA:BB:CC:DD:EE:FF
somfy> pin 123456
somfy> connect
somfy> auth
somfy> identify
  (motor should jog -- confirms BLE auth works)

somfy> range start
somfy> down 100
  (repeat 'down 100' until the blind reaches the desired lower position)
  (use 'down 10' for fine adjustment near the end)
somfy> limit down

somfy> up 100
  (repeat 'up 100' until the blind reaches the desired upper position)
somfy> limit up

somfy> dir cw
  (if the motor moved the wrong direction, use 'dir ccw' instead)
```

After setting limits, the motor should become operational again and respond to Zigbee
commands from zigbee2mqtt / Home Assistant / ZHA.

### Fixing tilt on a Venetian motor

If your Venetian motor has working lift (up/down) but tilt doesn't work, check the
Application type:

```
somfy> config read motor
  (look for Application: "Roller")

somfy> config set motor Application Venetian
somfy> yes-config-write
  (power-cycle the motor after this)
```

### Factory reset and re-pair to zigbee2mqtt

If the motor is in a completely broken state and you want to start fresh:

```
somfy> target AA:BB:CC:DD:EE:FF
somfy> pin 123456
somfy> connect
somfy> auth
somfy> factory-reset
somfy> yes-reset
```

The motor will:
1. Erase all settings (limits, network, direction, speed)
2. Enter programming mode for 3 minutes (LED blinks amber)
3. Attempt to join a Zigbee network

On your coordinator, enable permit-join within those 3 minutes:

```bash
# zigbee2mqtt via MQTT
mosquitto_pub -t 'zigbee2mqtt/bridge/request/permit_join' -m '{"value": true, "time": 180}'
```

After it joins, set limits via BLE (see workflow above).

### Pairing a brand-new motor to zigbee2mqtt

New Somfy motors ship as their own Zigbee network coordinator. They won't join your
network until told to leave their own:

```
somfy> scan
somfy> target <mac>
somfy> pin <pin from label>
somfy> connect
somfy> auth
somfy> leave-network
somfy> yes-leave
```

Enable permit-join on your coordinator. The motor should appear within seconds.

### Inspecting motor details

```
somfy> info
  ┌─ Device Information ───────────────────────
  │ Device Name:   Sonesse2 40 Zigbee
  │ HW Revision:   ...
  │ Manufacturer:  Somfy
  │ Battery:       100%
  │
  │ Network (Zigbee):
  │   Channel:     11
  │   PAN ID:      0x1A62
  │   EUI64:       4CC206FF:FE702CA3
  │   BLE MAC:     4C:C2:06:70:2C:A3 (derived, removing FFFE)
  └──────────────────────────────────────────
```

## Troubleshooting

### Motor doesn't show up in BLE scan

- Ensure the motor has power (mains connected or battery charged)
- Press the PROG button on the motor head (if accessible) to wake BLE
- The ESP32 BLE range is limited; bring it within 2-3 meters of the motor
- The motor may only advertise BLE for a limited time after power-on; try
  power-cycling the motor

### Connection fails

- Make sure no other BLE client is connected (phone, ESPHome proxy, etc.)
- BLE connections are 1:1; only one client can connect at a time
- Try power-cycling the motor, then connect within 30 seconds

### Auth succeeds but commands don't work

- Verify the PIN is correct (from the motor label, not the Zigbee install code)
- Use `identify` to test -- if the motor jogs, auth worked
- Some commands may not exist on your motor variant (e.g. `open`/`close` are
  venetian-only); use `services` to see what's available

### Motor moves but Zigbee still shows non-operational

- After setting limits via BLE, you may need to power-cycle the motor
- Remove and re-pair the device in zigbee2mqtt if the state doesn't update
- Read `configStatus` via MQTT to verify:
  ```bash
  mosquitto_pub -t 'zigbee2mqtt/DEVICE/set' \
    -m '{"read":{"cluster":258,"attributes":[7]}}'
  ```
  Value should have bit 0 set (odd number = operational)

### Tilt doesn't work on Venetian motor

- Check `config read motor` -- if Application is "Roller", change it:
  `config set motor Application Venetian` + `yes-config-write`
- Power-cycle the motor after changing the Application type
- Tilt may need separate limit calibration

## Zigbee Diagnostics (via zigbee2mqtt)

Useful MQTT commands for diagnosing the motor state alongside this tool:

```bash
# Read configStatus (attribute 7 on cluster 258 / WindowCovering)
mosquitto_pub -t 'zigbee2mqtt/DEVICE/set' \
  -m '{"read":{"cluster":258,"attributes":[7]}}'

# configStatus bitmap:
#   bit 0: operational (1=yes, 0=NO -- this is your problem)
#   bit 1: online
#   bit 2: commands reversed
#   bit 3: lift closed-loop
#   bit 4: tilt closed-loop
#   bit 5: lift encoder
#   bit 6: tilt encoder

# Read windowCoveringMode (attribute 23)
mosquitto_pub -t 'zigbee2mqtt/DEVICE/set' \
  -m '{"read":{"cluster":258,"attributes":[23]}}'

# Read windowCoveringType (attribute 0)
mosquitto_pub -t 'zigbee2mqtt/DEVICE/set' \
  -m '{"read":{"cluster":258,"attributes":[0]}}'
# Type 0=Roller, 8=Tilt Blind Lift+Tilt (Venetian)

# Read basic info
mosquitto_pub -t 'zigbee2mqtt/DEVICE/set' \
  -m '{"read":{"cluster":"genBasic","attributes":["modelId","manufacturerName","swBuildId"]}}'
```

## Credits

- BLE protocol reverse engineering: [mrmelair](https://mrmelair.com/2024/07/10/hacking-the-somfy-sonesse2-zigbee-motors/) and [follow-up post](https://mrmelair.com/2024/08/22/more-ble-for-the-somfy-sonesse2-zigbee-motors/)
- Zigbee calibration insight: [LogicMachine forum thread](https://forum.logicmachine.net/showthread.php?tid=5807)
- zigbee2mqtt Somfy device support: [zigbee2mqtt #24607](https://github.com/Koenkk/zigbee2mqtt/issues/24607), [zigbee-herdsman-converters #10565](https://github.com/Koenkk/zigbee-herdsman-converters/issues/10565)

## License

MIT
