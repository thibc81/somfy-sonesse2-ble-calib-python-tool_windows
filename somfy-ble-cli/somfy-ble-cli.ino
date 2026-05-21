// Somfy Sonesse2 Zigbee BLE Control CLI
// Reverse-engineered BLE GATT interface for motor configuration
// Based on research from mrmelair.com
//
// For M5Stack Atom (ESP32) - connects to Somfy motor via BLE
// and exposes an interactive serial CLI for control.

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEClient.h>

// ─── Somfy BLE GATT Characteristic UUIDs ─────────────────────────
// All characteristics share the base: xxxxxxxx-cad9-46c6-a2ea-2ca16d57b4a5

static BLEUUID UUID_AUTH        ("0000000b-cad9-46c6-a2ea-2ca16d57b4a5");
static BLEUUID UUID_IDENTIFY    ("00000001-cad9-46c6-a2ea-2ca16d57b4a5");
static BLEUUID UUID_GOTO_POS    ("00000005-cad9-46c6-a2ea-2ca16d57b4a5");
static BLEUUID UUID_STOP        ("00000006-cad9-46c6-a2ea-2ca16d57b4a5");
static BLEUUID UUID_DELIVERY    ("0000000a-cad9-46c6-a2ea-2ca16d57b4a5");
static BLEUUID UUID_OPEN        ("0000000d-cad9-46c6-a2ea-2ca16d57b4a5");
static BLEUUID UUID_CLOSE       ("0000000e-cad9-46c6-a2ea-2ca16d57b4a5");

static BLEUUID UUID_FACTORY_RST ("00010001-cad9-46c6-a2ea-2ca16d57b4a5");
static BLEUUID UUID_SET_DIR     ("00010005-cad9-46c6-a2ea-2ca16d57b4a5");
static BLEUUID UUID_SET_LIMIT   ("00010007-cad9-46c6-a2ea-2ca16d57b4a5");
static BLEUUID UUID_MOVE_DOWN   ("00010008-cad9-46c6-a2ea-2ca16d57b4a5");
static BLEUUID UUID_MOVE_UP     ("00010009-cad9-46c6-a2ea-2ca16d57b4a5");
static BLEUUID UUID_CFG_RANGE   ("0001000b-cad9-46c6-a2ea-2ca16d57b4a5");

static BLEUUID UUID_LEAVE_NET   ("00020001-cad9-46c6-a2ea-2ca16d57b4a5");

// We need to discover the service UUID dynamically since Somfy uses
// a custom service. We'll iterate services after connecting.

// ─── State ───────────────────────────────────────────────────────

BLEScan*   pBLEScan   = nullptr;
BLEClient* pClient    = nullptr;
BLERemoteService* pService = nullptr;

bool connected    = false;
bool authenticated = false;
String targetAddr = "";
String pinCode    = "";

// For scan results
struct ScanResult {
  String addr;
  String name;
  int rssi;
};
#define MAX_SCAN_RESULTS 20
ScanResult scanResults[MAX_SCAN_RESULTS];
int scanResultCount = 0;

// ─── CLI helpers ─────────────────────────────────────────────────

void printBanner() {
  Serial.println();
  Serial.println("╔══════════════════════════════════════════╗");
  Serial.println("║   Somfy Sonesse2 BLE Control Tool       ║");
  Serial.println("╠══════════════════════════════════════════╣");
  Serial.println("║  Use 'help' to see available commands    ║");
  Serial.println("╚══════════════════════════════════════════╝");
  Serial.println();
}

void printStatus() {
  Serial.println("┌─ Status ──────────────────────────────────");
  Serial.print("│ BLE Target:    ");
  Serial.println(targetAddr.length() > 0 ? targetAddr : "(not set)");
  Serial.print("│ Connected:     ");
  Serial.println(connected ? "YES" : "no");
  Serial.print("│ Authenticated: ");
  Serial.println(authenticated ? "YES" : "no");
  Serial.print("│ PIN:           ");
  Serial.println(pinCode.length() > 0 ? pinCode : "(not set)");
  Serial.println("└────────────────────────────────────────────");
}

void printHelp() {
  Serial.println();
  Serial.println("┌─ Commands ────────────────────────────────");
  Serial.println("│");
  Serial.println("│ SETUP");
  Serial.println("│   scan              Scan for Somfy BLE devices");
  Serial.println("│   target <mac>      Set target MAC address");
  Serial.println("│   pin <code>        Set PIN code (from motor label)");
  Serial.println("│   connect           Connect to target motor");
  Serial.println("│   disconnect        Disconnect from motor");
  Serial.println("│   auth              Authenticate with PIN");
  Serial.println("│   status            Show connection status");
  Serial.println("│");
  Serial.println("│ DIAGNOSTICS");
  Serial.println("│   identify          Make motor jog to identify");
  Serial.println("│   services          List discovered BLE services");
  Serial.println("│");
  Serial.println("│ MOVEMENT");
  Serial.println("│   up [step]         Move up (step: 1-500, default 100)");
  Serial.println("│   down [step]       Move down (step: 1-500, default 100)");
  Serial.println("│   open              Full open (venetian)");
  Serial.println("│   close             Full close (venetian)");
  Serial.println("│   stop              Stop movement");
  Serial.println("│   goto <pos>        Go to position (0=open, 32767=closed)");
  Serial.println("│");
  Serial.println("│ CALIBRATION");
  Serial.println("│   range start       Begin range configuration");
  Serial.println("│   range half        Half range mode");
  Serial.println("│   range full        Full range mode");
  Serial.println("│   limit up          Set current pos as UPPER limit");
  Serial.println("│   limit down        Set current pos as LOWER limit");
  Serial.println("│   dir cw            Set direction clockwise");
  Serial.println("│   dir ccw           Set direction counter-clockwise");
  Serial.println("│");
  Serial.println("│ DANGER ZONE");
  Serial.println("│   factory-reset     Full factory reset (erases everything)");
  Serial.println("│   leave-network     Leave Zigbee network (will rejoin)");
  Serial.println("│   delivery-mode     Enter delivery mode (motor off)");
  Serial.println("│");
  Serial.println("│   help              Show this help");
  Serial.println("└────────────────────────────────────────────");
  Serial.println();
}

void printOK(const char* msg) {
  Serial.print("  [OK] ");
  Serial.println(msg);
}

void printERR(const char* msg) {
  Serial.print("  [ERR] ");
  Serial.println(msg);
}

void printWARN(const char* msg) {
  Serial.print("  [!] ");
  Serial.println(msg);
}

void printInfo(const char* msg) {
  Serial.print("  [..] ");
  Serial.println(msg);
}

// ─── BLE Write Helper ────────────────────────────────────────────

bool bleWrite(BLEUUID uuid, uint8_t* data, size_t len) {
  if (!connected || !pService) {
    printERR("Not connected");
    return false;
  }

  BLERemoteCharacteristic* pChar = nullptr;
  try {
    pChar = pService->getCharacteristic(uuid);
  } catch (...) {
    // ignore
  }

  if (pChar == nullptr) {
    // Try to find it across all services
    // Some Somfy motors put characteristics in different services
    printERR("Characteristic not found");
    return false;
  }

  try {
    pChar->writeValue(data, len, true);  // response=true
    return true;
  } catch (...) {
    // Retry without response - some endpoints error on read-after-write
    try {
      pChar->writeValue(data, len, false);
      return true;
    } catch (...) {
      printERR("Write failed");
      return false;
    }
  }
}

// Try writing, first with response, then without. Some Somfy endpoints
// error on the implicit read that write-with-response triggers.
bool bleWriteFlexible(BLEUUID uuid, uint8_t* data, size_t len) {
  if (!connected) {
    printERR("Not connected");
    return false;
  }

  // Search across all services for the characteristic
  BLERemoteCharacteristic* pChar = nullptr;

  std::map<std::string, BLERemoteService*>* services = pClient->getServices();
  for (auto& kv : *services) {
    try {
      pChar = kv.second->getCharacteristic(uuid);
      if (pChar != nullptr) break;
    } catch (...) {
      continue;
    }
  }

  if (pChar == nullptr) {
    Serial.print("  [ERR] Characteristic not found: ");
    Serial.println(uuid.toString().c_str());
    return false;
  }

  // Try write-with-response first
  try {
    pChar->writeValue(data, len, true);
    return true;
  } catch (...) {
    // Some Somfy endpoints reject the read-back, try without response
    try {
      pChar->writeValue(data, len, false);
      return true;
    } catch (...) {
      printERR("Write failed (both with and without response)");
      return false;
    }
  }
}

// ─── Commands ────────────────────────────────────────────────────

void cmdScan() {
  printInfo("Scanning for BLE devices (5 seconds)...");
  scanResultCount = 0;

  pBLEScan->clearResults();
  BLEScanResults* results = pBLEScan->start(5, false);

  int count = results->getCount();
  Serial.println();
  Serial.println("  ┌─ Scan Results ─────────────────────────────────────────┐");
  Serial.println("  │  #  RSSI  MAC Address        Name                     │");
  Serial.println("  ├────────────────────────────────────────────────────────┤");

  int displayed = 0;
  for (int i = 0; i < count && displayed < MAX_SCAN_RESULTS; i++) {
    BLEAdvertisedDevice dev = results->getDevice(i);
    String name = dev.haveName() ? String(dev.getName().c_str()) : "";
    String addr = String(dev.getAddress().toString().c_str());

    // Show all devices with names, or those with "Somfy" / "Sonesse" in name,
    // or all if nothing Somfy-specific found
    bool isSomfy = (name.indexOf("omfy") >= 0 || name.indexOf("onesse") >= 0 ||
                    name.indexOf("SOMFY") >= 0);

    // Store result
    scanResults[displayed].addr = addr;
    scanResults[displayed].name = name;
    scanResults[displayed].rssi = dev.getRSSI();

    char line[80];
    snprintf(line, sizeof(line), "  │ %2d  %4d  %-18s %-24s │",
             displayed + 1, dev.getRSSI(), addr.c_str(),
             name.length() > 0 ? name.substring(0, 24).c_str() : "(unnamed)");
    Serial.println(line);
    displayed++;
  }
  scanResultCount = displayed;

  Serial.println("  └────────────────────────────────────────────────────────┘");
  Serial.print("  Found ");
  Serial.print(displayed);
  Serial.println(" devices. Use 'target <mac>' or 'target <#>' to select.");
  Serial.println();

  pBLEScan->clearResults();
}

void cmdConnect() {
  if (targetAddr.length() == 0) {
    printERR("No target set. Use 'target <mac>' first.");
    return;
  }

  if (connected) {
    printWARN("Already connected, disconnecting first...");
    pClient->disconnect();
    connected = false;
    authenticated = false;
    delay(500);
  }

  Serial.print("  [..] Connecting to ");
  Serial.print(targetAddr);
  Serial.println("...");

  BLEAddress addr(targetAddr.c_str());

  try {
    if (!pClient->connect(addr)) {
      printERR("Connection failed");
      return;
    }
  } catch (...) {
    printERR("Connection failed (exception)");
    return;
  }

  connected = true;
  authenticated = false;
  printOK("Connected!");

  // Discover services
  printInfo("Discovering services...");
  std::map<std::string, BLERemoteService*>* services = pClient->getServices();

  int svcCount = 0;
  for (auto& kv : *services) {
    // Use the first service that has our known characteristics as the primary
    BLERemoteService* svc = kv.second;
    try {
      BLERemoteCharacteristic* testChar = svc->getCharacteristic(UUID_AUTH);
      if (testChar != nullptr) {
        pService = svc;
        Serial.print("  [OK] Found Somfy service: ");
        Serial.println(kv.first.c_str());
      }
    } catch (...) {}
    svcCount++;
  }

  Serial.print("  [OK] Discovered ");
  Serial.print(svcCount);
  Serial.println(" service(s)");

  if (pService == nullptr) {
    printWARN("Somfy control service not auto-detected.");
    printWARN("Use 'services' to inspect, or try 'auth' anyway.");
  }

  Serial.println();
  Serial.println("  Next step: set PIN with 'pin <code>' then run 'auth'");
}

void cmdDisconnect() {
  if (!connected) {
    printWARN("Not connected");
    return;
  }
  pClient->disconnect();
  connected = false;
  authenticated = false;
  pService = nullptr;
  printOK("Disconnected");
}

void cmdAuth() {
  if (!connected) {
    printERR("Not connected. Use 'connect' first.");
    return;
  }
  if (pinCode.length() == 0) {
    printERR("No PIN set. Use 'pin <code>' first.");
    return;
  }

  // Convert PIN to integer, then to 3-byte little-endian
  uint32_t pin = pinCode.toInt();
  uint8_t pinBytes[3];
  pinBytes[0] = pin & 0xFF;
  pinBytes[1] = (pin >> 8) & 0xFF;
  pinBytes[2] = (pin >> 16) & 0xFF;

  char hexStr[32];
  snprintf(hexStr, sizeof(hexStr), "PIN %lu -> bytes: %02X %02X %02X",
           (unsigned long)pin, pinBytes[0], pinBytes[1], pinBytes[2]);
  printInfo(hexStr);

  if (bleWriteFlexible(UUID_AUTH, pinBytes, 3)) {
    authenticated = true;
    printOK("Authentication sent!");
    Serial.println("  Verify with 'identify' - motor should jog if auth succeeded.");
  }
}

void cmdIdentify() {
  uint8_t val = 0x01;
  if (bleWriteFlexible(UUID_IDENTIFY, &val, 1)) {
    printOK("Identify sent - motor should jog");
  }
}

void cmdServices() {
  if (!connected) {
    printERR("Not connected");
    return;
  }

  std::map<std::string, BLERemoteService*>* services = pClient->getServices();
  Serial.println();
  Serial.println("  ┌─ BLE Services & Characteristics ──────────────────────┐");

  for (auto& kv : *services) {
    Serial.print("  │ Service: ");
    Serial.println(kv.first.c_str());

    std::map<std::string, BLERemoteCharacteristic*>* chars = kv.second->getCharacteristics();
    for (auto& ckv : *chars) {
      Serial.print("  │   └─ ");
      Serial.print(ckv.first.c_str());

      // Label known characteristics
      BLEUUID cUUID = ckv.second->getUUID();
      if (cUUID.equals(UUID_AUTH))        Serial.print("  <- AUTH");
      if (cUUID.equals(UUID_IDENTIFY))    Serial.print("  <- IDENTIFY");
      if (cUUID.equals(UUID_STOP))        Serial.print("  <- STOP");
      if (cUUID.equals(UUID_GOTO_POS))    Serial.print("  <- GOTO_POS");
      if (cUUID.equals(UUID_OPEN))        Serial.print("  <- OPEN");
      if (cUUID.equals(UUID_CLOSE))       Serial.print("  <- CLOSE");
      if (cUUID.equals(UUID_DELIVERY))    Serial.print("  <- DELIVERY_MODE");
      if (cUUID.equals(UUID_FACTORY_RST)) Serial.print("  <- FACTORY_RESET");
      if (cUUID.equals(UUID_SET_DIR))     Serial.print("  <- SET_DIRECTION");
      if (cUUID.equals(UUID_SET_LIMIT))   Serial.print("  <- SET_LIMIT");
      if (cUUID.equals(UUID_MOVE_DOWN))   Serial.print("  <- MOVE_DOWN");
      if (cUUID.equals(UUID_MOVE_UP))     Serial.print("  <- MOVE_UP");
      if (cUUID.equals(UUID_CFG_RANGE))   Serial.print("  <- CONFIG_RANGE");
      if (cUUID.equals(UUID_LEAVE_NET))   Serial.print("  <- LEAVE_NETWORK");
      Serial.println();
    }
  }
  Serial.println("  └────────────────────────────────────────────────────────┘");
}

void cmdMoveUp(int step) {
  // step: 1-500, little-endian 16-bit
  if (step < 1) step = 1;
  if (step > 500) step = 500;
  uint8_t data[2];
  data[0] = step & 0xFF;
  data[1] = (step >> 8) & 0xFF;

  char msg[40];
  snprintf(msg, sizeof(msg), "Moving UP (step=%d)", step);
  printInfo(msg);

  if (bleWriteFlexible(UUID_MOVE_UP, data, 2)) {
    printOK("Move up sent");
  }
}

void cmdMoveDown(int step) {
  if (step < 1) step = 1;
  if (step > 500) step = 500;
  uint8_t data[2];
  data[0] = step & 0xFF;
  data[1] = (step >> 8) & 0xFF;

  char msg[40];
  snprintf(msg, sizeof(msg), "Moving DOWN (step=%d)", step);
  printInfo(msg);

  if (bleWriteFlexible(UUID_MOVE_DOWN, data, 2)) {
    printOK("Move down sent");
  }
}

void cmdStop() {
  uint8_t val = 0x01;
  if (bleWriteFlexible(UUID_STOP, &val, 1)) {
    printOK("Stop sent");
  }
}

void cmdOpen() {
  uint8_t val = 0x01;
  if (bleWriteFlexible(UUID_OPEN, &val, 1)) {
    printOK("Open sent");
  }
}

void cmdClose() {
  uint8_t val = 0x01;
  if (bleWriteFlexible(UUID_CLOSE, &val, 1)) {
    printOK("Close sent");
  }
}

void cmdGoto(int pos) {
  if (pos < 0) pos = 0;
  if (pos > 32767) pos = 32767;
  uint8_t data[2];
  data[0] = pos & 0xFF;
  data[1] = (pos >> 8) & 0xFF;

  char msg[48];
  snprintf(msg, sizeof(msg), "Going to position %d", pos);
  printInfo(msg);

  if (bleWriteFlexible(UUID_GOTO_POS, data, 2)) {
    printOK("Goto position sent");
  }
}

void cmdConfigRange(const String& mode) {
  uint8_t data[2] = {0, 0};

  if (mode == "start") {
    data[0] = 0x00; data[1] = 0x00;
  } else if (mode == "half") {
    data[0] = 0x02; data[1] = 0x00;
  } else if (mode == "full") {
    data[0] = 0x01; data[1] = 0x00;
  } else {
    printERR("Usage: range start|half|full");
    return;
  }

  char msg[48];
  snprintf(msg, sizeof(msg), "Configure range: %s", mode.c_str());
  printInfo(msg);

  if (bleWriteFlexible(UUID_CFG_RANGE, data, 2)) {
    printOK("Range config sent");
  }
}

void cmdSetLimit(const String& which) {
  uint8_t val;
  if (which == "up") {
    val = 0x00;
  } else if (which == "down") {
    val = 0x01;
  } else {
    printERR("Usage: limit up|down");
    return;
  }

  char msg[48];
  snprintf(msg, sizeof(msg), "Setting %s limit at current position", which.c_str());
  printInfo(msg);

  if (bleWriteFlexible(UUID_SET_LIMIT, &val, 1)) {
    printOK("Limit set!");
  }
}

void cmdSetDir(const String& dir) {
  uint8_t val;
  if (dir == "cw") {
    val = 0x01;
  } else if (dir == "ccw") {
    val = 0x00;
  } else {
    printERR("Usage: dir cw|ccw");
    return;
  }

  char msg[48];
  snprintf(msg, sizeof(msg), "Setting direction: %s", dir.c_str());
  printInfo(msg);

  if (bleWriteFlexible(UUID_SET_DIR, &val, 1)) {
    printOK("Direction set!");
  }
}

void cmdFactoryReset() {
  Serial.println();
  Serial.println("  ╔════════════════════════════════════════╗");
  Serial.println("  ║  WARNING: FACTORY RESET                ║");
  Serial.println("  ║                                        ║");
  Serial.println("  ║  This will erase ALL settings:         ║");
  Serial.println("  ║  - End limits                          ║");
  Serial.println("  ║  - Network pairing                     ║");
  Serial.println("  ║  - Speed settings                      ║");
  Serial.println("  ║  - Direction                           ║");
  Serial.println("  ║                                        ║");
  Serial.println("  ║  Motor will enter programming mode     ║");
  Serial.println("  ║  for 3 minutes after reset.            ║");
  Serial.println("  ║                                        ║");
  Serial.println("  ║  Type 'yes-reset' to confirm           ║");
  Serial.println("  ╚════════════════════════════════════════╝");
}

void cmdFactoryResetConfirm() {
  printInfo("Sending factory reset...");
  uint8_t val = 0x01;
  if (bleWriteFlexible(UUID_FACTORY_RST, &val, 1)) {
    printOK("Factory reset sent!");
    Serial.println("  Motor will enter programming mode for 3 minutes.");
    Serial.println("  It should try to join a Zigbee network.");
    Serial.println("  Enable permit-join on your coordinator now!");
  }
}

void cmdLeaveNetwork() {
  Serial.println();
  Serial.println("  ╔════════════════════════════════════════╗");
  Serial.println("  ║  WARNING: LEAVE NETWORK                ║");
  Serial.println("  ║                                        ║");
  Serial.println("  ║  Motor will leave current Zigbee       ║");
  Serial.println("  ║  network and try to join a new one.    ║");
  Serial.println("  ║                                        ║");
  Serial.println("  ║  Type 'yes-leave' to confirm           ║");
  Serial.println("  ╚════════════════════════════════════════╝");
}

void cmdLeaveNetworkConfirm() {
  printInfo("Sending leave network...");
  uint8_t val = 0x01;
  if (bleWriteFlexible(UUID_LEAVE_NET, &val, 1)) {
    printOK("Leave network sent!");
    Serial.println("  Enable permit-join on your coordinator now.");
  }
}

void cmdDeliveryMode() {
  printWARN("Delivery mode will switch the motor OFF.");
  printWARN("It won't respond until the physical button is pressed.");
  printInfo("Sending delivery mode...");
  uint8_t val = 0x01;
  if (bleWriteFlexible(UUID_DELIVERY, &val, 1)) {
    printOK("Delivery mode sent");
  }
}

// ─── Command Parsing ─────────────────────────────────────────────

String inputBuffer = "";

void processCommand(String input) {
  input.trim();
  if (input.length() == 0) return;

  // Split into command and argument
  int spaceIdx = input.indexOf(' ');
  String cmd = (spaceIdx >= 0) ? input.substring(0, spaceIdx) : input;
  String arg = (spaceIdx >= 0) ? input.substring(spaceIdx + 1) : "";
  arg.trim();
  cmd.toLowerCase();

  if (cmd == "help" || cmd == "?") {
    printHelp();
  }
  else if (cmd == "status") {
    printStatus();
  }
  else if (cmd == "scan") {
    cmdScan();
  }
  else if (cmd == "target") {
    if (arg.length() == 0) {
      printERR("Usage: target <mac>  or  target <scan#>");
      return;
    }
    // Allow selecting by scan result number (only if pure digits)
    bool isNumber = true;
    for (unsigned int i = 0; i < arg.length(); i++) {
      if (!isDigit(arg.charAt(i))) { isNumber = false; break; }
    }
    if (isNumber && arg.toInt() > 0 && arg.toInt() <= scanResultCount) {
      targetAddr = scanResults[arg.toInt() - 1].addr;
    } else {
      targetAddr = arg;
    }
    Serial.print("  [OK] Target set to: ");
    Serial.println(targetAddr);
  }
  else if (cmd == "pin") {
    if (arg.length() == 0) {
      printERR("Usage: pin <code>  (numeric PIN from motor label)");
      return;
    }
    pinCode = arg;
    Serial.print("  [OK] PIN set to: ");
    Serial.println(pinCode);
  }
  else if (cmd == "connect") {
    cmdConnect();
  }
  else if (cmd == "disconnect" || cmd == "dc") {
    cmdDisconnect();
  }
  else if (cmd == "auth") {
    cmdAuth();
  }
  else if (cmd == "identify" || cmd == "id") {
    cmdIdentify();
  }
  else if (cmd == "services" || cmd == "svc") {
    cmdServices();
  }
  else if (cmd == "up") {
    int step = arg.length() > 0 ? arg.toInt() : 100;
    cmdMoveUp(step);
  }
  else if (cmd == "down") {
    int step = arg.length() > 0 ? arg.toInt() : 100;
    cmdMoveDown(step);
  }
  else if (cmd == "stop" || cmd == "s") {
    cmdStop();
  }
  else if (cmd == "open") {
    cmdOpen();
  }
  else if (cmd == "close") {
    cmdClose();
  }
  else if (cmd == "goto") {
    if (arg.length() == 0) {
      printERR("Usage: goto <position>  (0=open, 32767=closed)");
      return;
    }
    cmdGoto(arg.toInt());
  }
  else if (cmd == "range") {
    if (arg.length() == 0) {
      printERR("Usage: range start|half|full");
      return;
    }
    arg.toLowerCase();
    cmdConfigRange(arg);
  }
  else if (cmd == "limit") {
    if (arg.length() == 0) {
      printERR("Usage: limit up|down");
      return;
    }
    arg.toLowerCase();
    cmdSetLimit(arg);
  }
  else if (cmd == "dir") {
    if (arg.length() == 0) {
      printERR("Usage: dir cw|ccw");
      return;
    }
    arg.toLowerCase();
    cmdSetDir(arg);
  }
  else if (cmd == "factory-reset") {
    cmdFactoryReset();
  }
  else if (cmd == "yes-reset") {
    cmdFactoryResetConfirm();
  }
  else if (cmd == "leave-network") {
    cmdLeaveNetwork();
  }
  else if (cmd == "yes-leave") {
    cmdLeaveNetworkConfirm();
  }
  else if (cmd == "delivery-mode") {
    cmdDeliveryMode();
  }
  // Quick-setup wizard
  else if (cmd == "wizard") {
    Serial.println();
    Serial.println("  ┌─ Calibration Wizard ─────────────────────");
    Serial.println("  │");
    Serial.println("  │ Follow these steps in order:");
    Serial.println("  │");
    Serial.println("  │ 1. scan             - Find your motor");
    Serial.println("  │ 2. target <mac>     - Select it");
    Serial.println("  │ 3. pin <code>       - Set PIN from label");
    Serial.println("  │ 4. connect          - Connect via BLE");
    Serial.println("  │ 5. auth             - Authenticate");
    Serial.println("  │ 6. identify         - Verify (motor jogs)");
    Serial.println("  │ 7. range start      - Begin calibration");
    Serial.println("  │ 8. down 100         - Move to lower limit");
    Serial.println("  │    (repeat until at bottom)");
    Serial.println("  │ 9. limit down       - Set lower limit");
    Serial.println("  │10. up 100           - Move to upper limit");
    Serial.println("  │    (repeat until at top)");
    Serial.println("  │11. limit up         - Set upper limit");
    Serial.println("  │12. dir cw/ccw       - Fix direction if needed");
    Serial.println("  │");
    Serial.println("  │ After this, the motor should be operational");
    Serial.println("  │ and respond to Zigbee commands again.");
    Serial.println("  └─────────────────────────────────────────────");
    Serial.println();
  }
  else {
    Serial.print("  Unknown command: '");
    Serial.print(cmd);
    Serial.println("'. Type 'help' for commands.");
  }
}

// ─── Arduino Setup & Loop ────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(1000);  // Let serial settle

  BLEDevice::init("SomfyCLI");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  pBLEScan->setWindow(99);
  pBLEScan->setInterval(100);

  pClient = BLEDevice::createClient();

  printBanner();
  printHelp();
  Serial.print("somfy> ");
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();

    if (c == '\n' || c == '\r') {
      if (inputBuffer.length() > 0) {
        Serial.println();  // Echo newline
        processCommand(inputBuffer);
        inputBuffer = "";
        Serial.print("somfy> ");
      }
    } else if (c == 127 || c == 8) {
      // Backspace
      if (inputBuffer.length() > 0) {
        inputBuffer.remove(inputBuffer.length() - 1);
        Serial.print("\b \b");
      }
    } else if (c >= 32 && c < 127) {
      inputBuffer += c;
      Serial.print(c);  // Echo
    }
  }

  // Check if BLE connection dropped
  if (connected && !pClient->isConnected()) {
    connected = false;
    authenticated = false;
    pService = nullptr;
    Serial.println();
    printWARN("BLE connection lost!");
    Serial.print("somfy> ");
  }

  delay(10);
}
