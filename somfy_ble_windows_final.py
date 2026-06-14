#!/usr/bin/env python3
import argparse
import asyncio
import sys
from dataclasses import dataclass
from typing import List, Optional

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("Install bleak first: py -3 -m pip install bleak")
    sys.exit(1)

BASE = "cad9-46c6-a2ea-2ca16d57b4a5"
UUID_IDENTIFY = f"00000001-{BASE}"
UUID_GOTO_POS = f"00000005-{BASE}"
UUID_STOP = f"00000006-{BASE}"
UUID_ORIENTATION = f"00000008-{BASE}"
UUID_TILT_POS = f"00000009-{BASE}"
UUID_DELIVERY = f"0000000a-{BASE}"
UUID_AUTH = f"0000000b-{BASE}"
UUID_OPEN = f"0000000d-{BASE}"
UUID_CLOSE = f"0000000e-{BASE}"
UUID_FACTORY_RST = f"00010001-{BASE}"
UUID_FW_VERSION = f"00010002-{BASE}"
UUID_SET_DIR = f"00010005-{BASE}"
UUID_SET_LIMIT = f"00010007-{BASE}"
UUID_MOVE_DOWN = f"00010008-{BASE}"
UUID_MOVE_UP = f"00010009-{BASE}"
UUID_CFG_RANGE = f"0001000b-{BASE}"
UUID_LEAVE_NET = f"00020001-{BASE}"
UUID_ZB_CHANNEL = f"00020002-{BASE}"
UUID_ZB_PAN = f"00020003-{BASE}"
UUID_ZB_ROLE = f"00020004-{BASE}"
UUID_ZB_EUI64 = f"00020005-{BASE}"
UUID_ZB_INSTALL = f"00020006-{BASE}"
UUID_WRITEDATA = f"00040001-{BASE}"
UUID_FILESTATUS = f"00040003-{BASE}"

CFG_FILES = {
    "hmi": (0x00, 0xC2),
    "radio": (0x00, 0xC3),
    "motor": (0x00, 0xC4),
    "type": (0x00, 0xD2),
}

WDATA_OPEN = 0x00
WDATA_WRITE = 0x02
WDATA_READ = 0x03
WDATA_CLOSE = 0x04
FMODE_READ = 0x00
FMODE_WRITE = 0x01


@dataclass
class ScanResult:
    address: str
    name: str
    rssi: int


class SomfyWindowsCLI:
    def __init__(self):
        self.client: Optional[BleakClient] = None
        self.target_addr = ""
        self.pin_code = ""
        self.authenticated = False
        self.scan_results: List[ScanResult] = []

    @property
    def connected(self) -> bool:
        return self.client is not None and self.client.is_connected

    def banner(self):
        print("\nSomfy Sonesse2 BLE Control - Windows PC\n")

    def help(self):
        print("Commands:")
        print("  scan")
        print("  target <mac|#>")
        print("  pin <code>")
        print("  connect")
        print("  disconnect")
        print("  auth")
        print("  identify")
        print("  info")
        print("  up [step]")
        print("  down [step]")
        print("  open")
        print("  close")
        print("  stop")
        print("  goto <pos>")
        print("  orient <0-100>")
        print("  tilt-read")
        print("  range start|half|full")
        print("  limit up|down")
        print("  dir cw|ccw")
        print("  config read <motor|radio|type|hmi>")
        print("  config set <file> <key> <value>")
        print("  read <uuid-prefix|full-uuid>")
        print("  write <uuid-prefix|full-uuid> <hex-bytes>")
        print("  leave-network")
        print("  delivery-mode")
        print("  factory-reset")
        print("  status")
        print("  quit")
        print()

    def status(self):
        print(f"Target: {self.target_addr or '(not set)'}")
        print(f"Connected: {'YES' if self.connected else 'no'}")
        print(f"Authenticated: {'YES' if self.authenticated else 'no'}")
        print(f"PIN: {self.pin_code or '(not set)'}")

    async def scan(self):
        print("Scanning for BLE devices for 5 seconds...")
        found = await BleakScanner.discover(timeout=5.0, return_adv=True)
        self.scan_results = []
        idx = 1
        for address, (device, adv) in found.items():
            name = device.name or adv.local_name or ""
            rssi = adv.rssi if adv else 0
            self.scan_results.append(ScanResult(address=address, name=name, rssi=rssi))
            print(f"{idx:2d}. {address}  RSSI={rssi:4d}  {name or '(unnamed)'}")
            idx += 1
        if not self.scan_results:
            print("No BLE devices found")

    async def resolve_target_by_name(self, wanted_name: str) -> bool:
        found = await BleakScanner.discover(timeout=5.0, return_adv=True)
        for address, (device, adv) in found.items():
            name = device.name or adv.local_name or ""
            if wanted_name.lower() in name.lower():
                self.target_addr = address
                print(f"Matched device '{name}' at {address}")
                return True
        print(f"No BLE device matched name: {wanted_name}")
        return False

    async def connect(self):
        if not self.target_addr:
            print("Set a target first with: target <mac|#>")
            return False
        if self.connected:
            await self.disconnect()
        print(f"Connecting to {self.target_addr}...")
        self.client = BleakClient(self.target_addr)
        try:
            await self.client.connect()
            self.authenticated = False
            print("Connected")
            return True
        except Exception as e:
            self.client = None
            print(f"Connection failed: {e}")
            return False

    async def disconnect(self):
        if self.client is not None:
            try:
                await self.client.disconnect()
            except Exception:
                pass
        self.client = None
        self.authenticated = False
        print("Disconnected")

    async def write_flexible(self, uuid: str, data: bytes) -> bool:
        if not self.connected:
            print("Not connected")
            return False
        try:
            await self.client.write_gatt_char(uuid, data, response=True)
            return True
        except Exception:
            try:
                await self.client.write_gatt_char(uuid, data, response=False)
                return True
            except Exception as e:
                print(f"Write failed: {e}")
                return False

    async def read_char(self, uuid: str) -> bytes:
        if not self.connected:
            print("Not connected")
            return b""
        try:
            return bytes(await self.client.read_gatt_char(uuid))
        except Exception as e:
            print(f"Read failed: {e}")
            return b""

    async def auth(self):
        if not self.pin_code:
            print("Set PIN first: pin <code>")
            return False
        pin = int(self.pin_code)
        payload = bytes((pin & 0xFF, (pin >> 8) & 0xFF, (pin >> 16) & 0xFF))
        print(f"AUTH payload: {' '.join(f'{b:02X}' for b in payload)}")
        ok = await self.write_flexible(UUID_AUTH, payload)
        if ok:
            self.authenticated = True
            print("Auth sent")
        return ok

    async def identify(self):
        if await self.write_flexible(UUID_IDENTIFY, b"\x01"):
            print("Identify sent")

    async def move_up(self, step: int = 100):
        step = max(1, min(500, step))
        if await self.write_flexible(UUID_MOVE_UP, step.to_bytes(2, "little")):
            print(f"Move up sent ({step})")

    async def move_down(self, step: int = 100):
        step = max(1, min(500, step))
        if await self.write_flexible(UUID_MOVE_DOWN, step.to_bytes(2, "little")):
            print(f"Move down sent ({step})")

    async def open_cmd(self):
        if await self.write_flexible(UUID_OPEN, b"\x01"):
            print("Open sent")

    async def close_cmd(self):
        if await self.write_flexible(UUID_CLOSE, b"\x01"):
            print("Close sent")

    async def stop(self):
        if await self.write_flexible(UUID_STOP, b"\x01"):
            print("Stop sent")

    async def goto(self, pos: int):
        pos = max(0, min(32767, pos))
        if await self.write_flexible(UUID_GOTO_POS, pos.to_bytes(2, "little")):
            print(f"Goto sent ({pos})")

    async def orient(self, pct: int):
        pct = max(0, min(100, pct))
        value = int(pct * 32767 / 100)
        if await self.write_flexible(UUID_ORIENTATION, value.to_bytes(2, "little")):
            print(f"Orientation sent ({pct}%)")

    async def tilt_read(self):
        data = await self.read_char(UUID_TILT_POS)
        print(f"Tilt raw ({len(data)} bytes): {' '.join(f'{b:02X}' for b in data)}")

    async def range_config(self, mode: str):
        if mode == "start":
            payload = b"\x00\x00"
        elif mode == "half":
            payload = b"\x00\x02"
        elif mode == "full":
            payload = b"\x00\x01"
        else:
            print("Usage: range start|half|full")
            return
        if await self.write_flexible(UUID_CFG_RANGE, payload):
            print(f"Range config sent ({mode})")

    async def set_limit(self, which: str):
        if which == "up":
            payload = b"\x00"
        elif which == "down":
            payload = b"\x01"
        else:
            print("Usage: limit up|down")
            return
        if await self.write_flexible(UUID_SET_LIMIT, payload):
            print(f"Limit sent ({which})")

    async def set_dir(self, which: str):
        if which == "cw":
            payload = b"\x01"
        elif which == "ccw":
            payload = b"\x00"
        else:
            print("Usage: dir cw|ccw")
            return
        if await self.write_flexible(UUID_SET_DIR, payload):
            print(f"Direction sent ({which})")

    async def leave_network(self):
        if await self.write_flexible(UUID_LEAVE_NET, b"\x01"):
            print("Leave network sent")
            return True
        return False

    async def delivery_mode(self):
        if await self.write_flexible(UUID_DELIVERY, b"\x01"):
            print("Delivery mode sent")

    async def factory_reset(self):
        if await self.write_flexible(UUID_FACTORY_RST, b"\x01"):
            print("Factory reset sent")

    async def info(self):
        uuids = [
            ("Device Name", "00002a00-0000-1000-8000-00805f9b34fb"),
            ("HW Revision", "00002a27-0000-1000-8000-00805f9b34fb"),
            ("SW Revision", "00002a28-0000-1000-8000-00805f9b34fb"),
            ("Manufacturer", "00002a29-0000-1000-8000-00805f9b34fb"),
            ("Battery", "00002a19-0000-1000-8000-00805f9b34fb"),
            ("FW Release", UUID_FW_VERSION),
            ("Zigbee Channel", UUID_ZB_CHANNEL),
            ("Zigbee PAN", UUID_ZB_PAN),
            ("Zigbee Role", UUID_ZB_ROLE),
            ("Zigbee EUI64", UUID_ZB_EUI64),
            ("Install Code", UUID_ZB_INSTALL),
        ]
        for label, uuid in uuids:
            data = await self.read_char(uuid)
            if not data:
                continue
            if label == "Battery" and len(data) >= 1:
                print(f"{label}: {data[0]}%")
            elif all(32 <= b < 127 for b in data):
                print(f"{label}: {data.decode(errors='ignore')}")
            else:
                print(f"{label}: {' '.join(f'{b:02X}' for b in data)}")

    async def poll_file_status(self, attempts: int = 5) -> bool:
        for _ in range(attempts):
            data = await self.read_char(UUID_FILESTATUS)
            if len(data) >= 2 and data[0] == 0x05 and data[1] in (0x00, 0x64):
                return True
            await asyncio.sleep(0.5)
        return False

    def cbor_text(self, s: str) -> bytes:
        raw = s.encode()
        if len(raw) < 24:
            return bytes([0x60 | len(raw)]) + raw
        return bytes([0x78, len(raw)]) + raw

    def cbor_uint(self, value: int) -> bytes:
        if value < 24:
            return bytes([value])
        if value < 256:
            return bytes([0x18, value])
        if value < 65536:
            return bytes([0x19, (value >> 8) & 0xFF, value & 0xFF])
        return bytes([0x1A, (value >> 24) & 0xFF, (value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF])

    def build_simple_cbor_map(self, key: str, value: str) -> bytes:
        out = bytearray([0xA1])
        out.extend(self.cbor_text(key))
        out.append(0x81)
        if value == "true":
            out.append(0xF5)
        elif value == "false":
            out.append(0xF4)
        elif value == "null":
            out.append(0xF6)
        elif value.isdigit():
            out.extend(self.cbor_uint(int(value)))
        else:
            out.extend(self.cbor_text(value))
        return bytes(out)

    async def config_read(self, file_name: str):
        if file_name not in CFG_FILES:
            print("Unknown file. Use: motor, radio, type, hmi")
            return
        hi, lo = CFG_FILES[file_name]
        print(f"Reading config file {file_name} (0x{hi:02X}{lo:02X})")
        if not await self.write_flexible(UUID_WRITEDATA, bytes([WDATA_OPEN, lo, hi, FMODE_READ])):
            return
        await asyncio.sleep(0.6)
        if not await self.write_flexible(UUID_WRITEDATA, bytes([WDATA_READ, lo, hi])):
            return
        await asyncio.sleep(0.6)
        data = await self.read_char(UUID_WRITEDATA)
        print(f"Read {len(data)} bytes: {' '.join(f'{b:02X}' for b in data)}")
        await asyncio.sleep(0.2)
        await self.write_flexible(UUID_WRITEDATA, bytes([WDATA_CLOSE, lo, hi]))

    async def config_set(self, file_name: str, key: str, value: str):
        if file_name not in CFG_FILES:
            print("Unknown file. Use: motor, radio, type, hmi")
            return
        hi, lo = CFG_FILES[file_name]
        cbor = self.build_simple_cbor_map(key, value)
        print(f"Writing {file_name}.{key} = {value}")
        print(f"CBOR: {' '.join(f'{b:02X}' for b in cbor)}")
        if not await self.write_flexible(UUID_WRITEDATA, bytes([WDATA_OPEN, lo, hi, FMODE_WRITE])):
            return
        await asyncio.sleep(0.6)
        if not await self.write_flexible(UUID_WRITEDATA, bytes([WDATA_WRITE]) + cbor):
            return
        await asyncio.sleep(0.6)
        await self.write_flexible(UUID_WRITEDATA, bytes([WDATA_CLOSE]))
        ok = await self.poll_file_status()
        print("Config written" if ok else "FileStatus did not confirm")

    def expand_uuid(self, value: str) -> str:
        return value if "-" in value else f"{value}-{BASE}"

    async def raw_read(self, value: str):
        uuid = self.expand_uuid(value)
        data = await self.read_char(uuid)
        print(f"{len(data)} bytes: {' '.join(f'{b:02X}' for b in data)}")
        if data and all(32 <= b < 127 for b in data):
            print(data.decode(errors="ignore"))

    async def raw_write(self, uuid_value: str, hex_bytes: str):
        uuid = self.expand_uuid(uuid_value)
        try:
            payload = bytes(int(x, 16) for x in hex_bytes.split())
        except ValueError:
            print("Use hex bytes like: 01 00 FF")
            return
        if await self.write_flexible(uuid, payload):
            print("Write succeeded")

    async def run_command(self, line: str) -> bool:
        line = line.strip()
        if not line:
            return True
        parts = line.split()
        cmd = parts[0].lower()
        args = parts[1:]
        try:
            if cmd in ("quit", "exit"):
                return False
            if cmd == "help":
                self.help()
            elif cmd == "status":
                self.status()
            elif cmd == "scan":
                await self.scan()
            elif cmd == "target":
                if not args:
                    print("Usage: target <mac|#>")
                elif args[0].isdigit() and 1 <= int(args[0]) <= len(self.scan_results):
                    self.target_addr = self.scan_results[int(args[0]) - 1].address
                    print(f"Target set to {self.target_addr}")
                else:
                    self.target_addr = args[0]
                    print(f"Target set to {self.target_addr}")
            elif cmd == "pin":
                if not args:
                    print("Usage: pin <code>")
                else:
                    self.pin_code = args[0]
                    print(f"PIN stored: {self.pin_code}")
            elif cmd == "connect":
                await self.connect()
            elif cmd == "disconnect":
                await self.disconnect()
            elif cmd == "auth":
                await self.auth()
            elif cmd == "identify":
                await self.identify()
            elif cmd == "info":
                await self.info()
            elif cmd == "up":
                await self.move_up(int(args[0]) if args else 100)
            elif cmd == "down":
                await self.move_down(int(args[0]) if args else 100)
            elif cmd == "open":
                await self.open_cmd()
            elif cmd == "close":
                await self.close_cmd()
            elif cmd == "stop":
                await self.stop()
            elif cmd == "goto":
                await self.goto(int(args[0]))
            elif cmd == "orient":
                await self.orient(int(args[0]))
            elif cmd == "tilt-read":
                await self.tilt_read()
            elif cmd == "range":
                await self.range_config(args[0] if args else "")
            elif cmd == "limit":
                await self.set_limit(args[0] if args else "")
            elif cmd == "dir":
                await self.set_dir(args[0] if args else "")
            elif cmd == "leave-network":
                await self.leave_network()
            elif cmd == "delivery-mode":
                await self.delivery_mode()
            elif cmd == "factory-reset":
                await self.factory_reset()
            elif cmd == "read":
                if not args:
                    print("Usage: read <uuid-prefix|full-uuid>")
                else:
                    await self.raw_read(args[0])
            elif cmd == "write":
                if len(args) < 2:
                    print("Usage: write <uuid-prefix|full-uuid> <hex bytes>")
                else:
                    await self.raw_write(args[0], " ".join(args[1:]))
            elif cmd == "config":
                if not args:
                    print("Usage: config read|set ...")
                elif args[0] == "read" and len(args) >= 2:
                    await self.config_read(args[1].lower())
                elif args[0] == "set" and len(args) >= 4:
                    await self.config_set(args[1].lower(), args[2], " ".join(args[3:]))
                else:
                    print("Usage: config read <file> | config set <file> <key> <value>")
            else:
                print(f"Unknown command: {cmd}")
        except Exception as e:
            print(f"Command failed: {e}")
        return True


async def run_one_shot(args):
    cli = SomfyWindowsCLI()
    if args.address:
        cli.target_addr = args.address
    elif args.name:
        ok = await cli.resolve_target_by_name(args.name)
        if not ok:
            return 1
    else:
        print("One-shot mode requires --address or --name")
        return 1

    if args.pin:
        cli.pin_code = args.pin

    if not await cli.connect():
        return 1

    try:
        if args.auth or args.pin:
            ok = await cli.auth()
            if not ok:
                return 1
            await asyncio.sleep(0.5)

        if args.identify:
            await cli.identify()
        if args.leave_network:
            ok = await cli.leave_network()
            return 0 if ok else 1
        if args.info:
            await cli.info()
            return 0
        print("Connected, but no one-shot action requested")
        return 0
    finally:
        if cli.connected:
            await cli.disconnect()


async def run_interactive():
    cli = SomfyWindowsCLI()
    cli.banner()
    cli.help()
    try:
        while True:
            try:
                line = await asyncio.to_thread(input, "somfy-win> ")
            except EOFError:
                break
            if not await cli.run_command(line):
                break
    finally:
        if cli.connected:
            await cli.disconnect()


def build_parser():
    p = argparse.ArgumentParser(description="Somfy Sonesse2 BLE control for Windows PC")
    p.add_argument("--address", help="BLE MAC address of the motor")
    p.add_argument("--name", help="Scan and match a BLE device by name substring")
    p.add_argument("--pin", help="Somfy PIN code from the motor label")
    p.add_argument("--auth", action="store_true", help="Send AUTH before action")
    p.add_argument("--identify", action="store_true", help="Send identify command")
    p.add_argument("--info", action="store_true", help="Read basic device info")
    p.add_argument("--leave-network", action="store_true", help="Send leave-network in one-shot mode")
    return p


async def main_async():
    args = build_parser().parse_args()
    if any([args.address, args.name, args.pin, args.auth, args.identify, args.info, args.leave_network]):
        code = await run_one_shot(args)
        raise SystemExit(code)
    await run_interactive()


if __name__ == "__main__":
    asyncio.run(main_async())
