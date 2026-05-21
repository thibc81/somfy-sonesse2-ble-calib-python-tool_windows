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
// All custom characteristics share the base: xxxxxxxx-cad9-46c6-a2ea-2ca16d57b4a5
// Confidence: ★★★ = confirmed by mrmelair + tested, ★★ = confirmed by mrmelair,
//             ★ = guessed from context/pattern, ? = unknown

// Service 00000000 - Operational commands
static BLEUUID UUID_IDENTIFY    ("00000001-cad9-46c6-a2ea-2ca16d57b4a5"); // ★★★
// 00000002 - ? unknown (write-only, 0 bytes)
// 00000003 - ? unknown (reads 0x00, status flag?)                         ★
// 00000004 - ? unknown (reads 2-byte LE uint, position/counter?)          ★
static BLEUUID UUID_GOTO_POS    ("00000005-cad9-46c6-a2ea-2ca16d57b4a5"); // ★★★ lift goto
static BLEUUID UUID_STOP        ("00000006-cad9-46c6-a2ea-2ca16d57b4a5"); // ★★★
// 00000007 - ? tilt position/state? (reads 1 byte on venetian)            ★
// 00000008 - ? tilt open? (write-only on venetian)                        ★
// 00000009 - ? tilt close? (write-only on venetian)                       ★
static BLEUUID UUID_DELIVERY    ("0000000a-cad9-46c6-a2ea-2ca16d57b4a5"); // ★★
static BLEUUID UUID_AUTH        ("0000000b-cad9-46c6-a2ea-2ca16d57b4a5"); // ★★★
// 0000000d - open (venetian only, not present on all motors)              ★★
// 0000000e - close (venetian only, not present on all motors)             ★★

// Service 00010000 - Configuration commands
static BLEUUID UUID_FACTORY_RST ("00010001-cad9-46c6-a2ea-2ca16d57b4a5"); // ★★
// 00010002 - firmware version string (reads ASCII)                        ★★
static BLEUUID UUID_SET_DIR     ("00010005-cad9-46c6-a2ea-2ca16d57b4a5"); // ★★
static BLEUUID UUID_SET_LIMIT   ("00010007-cad9-46c6-a2ea-2ca16d57b4a5"); // ★★
static BLEUUID UUID_MOVE_DOWN   ("00010008-cad9-46c6-a2ea-2ca16d57b4a5"); // ★★★
static BLEUUID UUID_MOVE_UP     ("00010009-cad9-46c6-a2ea-2ca16d57b4a5"); // ★★★
// 0001000a - ? tilt limit/config? (reads 0x01 on venetian)               ★
static BLEUUID UUID_CFG_RANGE   ("0001000b-cad9-46c6-a2ea-2ca16d57b4a5"); // ★★

// Service 00020000 - Network / Zigbee
static BLEUUID UUID_LEAVE_NET   ("00020001-cad9-46c6-a2ea-2ca16d57b4a5"); // ★★★
// 00020002 - Zigbee channel (reads 1 byte)                                ★★
// 00020003 - Zigbee PAN ID / network addr (reads 2 bytes LE)             ★
// 00020004 - Zigbee network role/type (reads 1 byte)                     ★
// 00020005 - Zigbee EUI64 (reads 8 bytes, little-endian)                 ★★
// 00020006 - Zigbee install code (reads 18 bytes)                        ★
// 00020007 - ? (write-only)                                              ?
// 00020008 - ? (write-only)                                              ?

// Service 00040000 - Config file read/write (WriteData protocol)
// 00040001 - WriteData command channel                                    ★★
// 00040002 - ? (reads empty)                                             ?
// 00040003 - ? status/size (reads 2 bytes)                               ★

// ─── Config File IDs ─────────────────────────────────────────────
#define CFG_FILE_HMI_HI    0x00
#define CFG_FILE_HMI_LO    0xC2
#define CFG_FILE_RADIO_HI  0x00
#define CFG_FILE_RADIO_LO  0xC3
#define CFG_FILE_MOTOR_HI  0x00
#define CFG_FILE_MOTOR_LO  0xC4
#define CFG_FILE_TYPE_HI   0x00
#define CFG_FILE_TYPE_LO   0xD2

// WriteData operations
#define WDATA_OPEN   0x00
#define WDATA_SIZE   0x01
#define WDATA_WRITE  0x02
#define WDATA_READ   0x03
#define WDATA_CLOSE  0x04

// File open modes
#define FMODE_READ   0x00
#define FMODE_WRITE  0x01

static BLEUUID UUID_WRITEDATA  ("00040001-cad9-46c6-a2ea-2ca16d57b4a5");
static BLEUUID UUID_FILESTATUS ("00040003-cad9-46c6-a2ea-2ca16d57b4a5");

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

// Config file read buffer
#define CFG_BUF_SIZE 600
uint8_t cfgBuf[CFG_BUF_SIZE];
int cfgBufLen = 0;

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
  Serial.println("┌─ Commands ──────────────────────────────────────────────────");
  Serial.println("│ Confidence: ★★★=tested ★★=RE'd by mrmelair ★=guessed ?=unknown");
  Serial.println("│");
  Serial.println("│ SETUP");
  Serial.println("│   scan                Scan for Somfy BLE devices");
  Serial.println("│   target <mac|#>      Set target MAC address or scan #");
  Serial.println("│   pin <code>          Set PIN code (from motor label)");
  Serial.println("│   connect             Connect to target motor       ★★★");
  Serial.println("│   disconnect          Disconnect from motor");
  Serial.println("│   auth                Authenticate with PIN         ★★★");
  Serial.println("│   status              Show connection status");
  Serial.println("│");
  Serial.println("│ DIAGNOSTICS");
  Serial.println("│   identify            Make motor jog to identify    ★★★");
  Serial.println("│   services            List BLE services (annotated)");
  Serial.println("│   info                Read device info & network details");
  Serial.println("│");
  Serial.println("│ MOVEMENT");
  Serial.println("│   up [step]           Move up (1-500, default 100)  ★★★");
  Serial.println("│   down [step]         Move down (1-500, default 100)★★★");
  Serial.println("│   stop                Stop movement                 ★★★");
  Serial.println("│   goto <pos>          Go to position (0-32767)      ★★");
  Serial.println("│   open                Full open (venetian?)         ★★");
  Serial.println("│   close               Full close (venetian?)        ★★");
  Serial.println("│");
  Serial.println("│ CALIBRATION");
  Serial.println("│   range start|half|full  Begin range config         ★★");
  Serial.println("│   limit up|down       Set end limit at current pos  ★★");
  Serial.println("│   dir cw|ccw          Set rotation direction        ★★");
  Serial.println("│");
  Serial.println("│ CONFIG FILES (CBOR protocol via WriteData)          ★★");
  Serial.println("│   config list         Show available config files");
  Serial.println("│   config read <file>  Read & decode config file");
  Serial.println("│   config dump <file>  Hex dump of config file");
  Serial.println("│   config set <file> <key> <value>  Set a field");
  Serial.println("│     file: motor, radio, type, hmi");
  Serial.println("│     e.g.: config set motor Application Venetian");
  Serial.println("│");
  Serial.println("│ RAW BLE ACCESS");
  Serial.println("│   read <prefix>       Read characteristic by UUID prefix");
  Serial.println("│   write <prefix> <hex>  Write raw bytes to UUID prefix");
  Serial.println("│");
  Serial.println("│ DANGER ZONE");
  Serial.println("│   factory-reset       Full factory reset            ★★");
  Serial.println("│   leave-network       Leave Zigbee network          ★★★");
  Serial.println("│   delivery-mode       Motor off until button press  ★★");
  Serial.println("│");
  Serial.println("│   help / wizard       Show help / calibration guide");
  Serial.println("└─────────────────────────────────────────────────────────────");
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

void printHex(const uint8_t* data, int len) {
  for (int i = 0; i < len; i++) {
    char hex[4];
    snprintf(hex, sizeof(hex), "%02X ", data[i]);
    Serial.print(hex);
  }
}

// ─── BLE Helpers ─────────────────────────────────────────────────

// Try writing, first with response, then without. Some Somfy endpoints
// error on the implicit read that write-with-response triggers.
bool bleWriteFlexible(BLEUUID uuid, uint8_t* data, size_t len) {
  if (!connected) {
    printERR("Not connected");
    return false;
  }
  BLERemoteCharacteristic* pChar = nullptr;
  std::map<std::string, BLERemoteService*>* services = pClient->getServices();
  for (auto& kv : *services) {
    try {
      pChar = kv.second->getCharacteristic(uuid);
      if (pChar != nullptr) break;
    } catch (...) { continue; }
  }
  if (pChar == nullptr) {
    Serial.print("  [ERR] Characteristic not found: ");
    Serial.println(uuid.toString().c_str());
    return false;
  }
  try {
    pChar->writeValue(data, len, true);
    return true;
  } catch (...) {
    try {
      pChar->writeValue(data, len, false);
      return true;
    } catch (...) {
      printERR("Write failed");
      return false;
    }
  }
}

// Write without response -- needed for WriteData protocol where write-with-response
// consumes the read buffer (the BLE response read eats the queued config data).
bool bleWriteNoResp(BLEUUID uuid, uint8_t* data, size_t len) {
  if (!connected) { printERR("Not connected"); return false; }
  BLERemoteCharacteristic* pChar = nullptr;
  std::map<std::string, BLERemoteService*>* services = pClient->getServices();
  for (auto& kv : *services) {
    try {
      pChar = kv.second->getCharacteristic(uuid);
      if (pChar != nullptr) break;
    } catch (...) { continue; }
  }
  if (pChar == nullptr) { printERR("Characteristic not found"); return false; }
  try {
    pChar->writeValue(data, len, false);
    return true;
  } catch (...) {
    printERR("Write failed");
    return false;
  }
}

// Read a characteristic, returns length (0 on fail). Data written to outBuf.
int bleReadTo(BLEUUID uuid, uint8_t* outBuf, int maxLen) {
  if (!connected) return 0;
  BLERemoteCharacteristic* pChar = nullptr;
  std::map<std::string, BLERemoteService*>* services = pClient->getServices();
  for (auto& kv : *services) {
    try {
      pChar = kv.second->getCharacteristic(uuid);
      if (pChar != nullptr) break;
    } catch (...) { continue; }
  }
  if (pChar == nullptr) return 0;
  try {
    String val = pChar->readValue();
    int len = val.length();
    if (len > maxLen) len = maxLen;
    for (int i = 0; i < len; i++) outBuf[i] = (uint8_t)val.charAt(i);
    return len;
  } catch (...) {
    return 0;
  }
}

// ─── Simple CBOR Decoder ─────────────────────────────────────────
// Walks CBOR bytes and prints human-readable output.
// Handles the subset used by Somfy config files.

int cborIndent = 0;

void cborPrintIndent() {
  Serial.print("  │ ");
  for (int i = 0; i < cborIndent; i++) Serial.print("  ");
}

// Read CBOR unsigned integer from additional info
uint32_t cborReadUint(const uint8_t* data, int* pos, int len, uint8_t info) {
  if (info < 24) return info;
  if (info == 24 && *pos < len) return data[(*pos)++];
  if (info == 25 && *pos + 1 < len) {
    uint16_t v = ((uint16_t)data[*pos] << 8) | data[*pos + 1];
    *pos += 2;
    return v;
  }
  if (info == 26 && *pos + 3 < len) {
    uint32_t v = ((uint32_t)data[*pos] << 24) | ((uint32_t)data[*pos+1] << 16) |
                 ((uint32_t)data[*pos+2] << 8) | data[*pos+3];
    *pos += 4;
    return v;
  }
  return 0;
}

// Read a CBOR text string, print it, return its content in strBuf (up to strBufMax)
void cborReadString(const uint8_t* data, int* pos, int len, uint8_t info,
                    char* strBuf, int strBufMax) {
  uint32_t slen = cborReadUint(data, pos, len, info);
  int copyLen = (int)slen < strBufMax - 1 ? (int)slen : strBufMax - 1;
  for (int i = 0; i < copyLen && *pos < len; i++) {
    strBuf[i] = (char)data[(*pos)++];
    strBuf[i + 1] = '\0';
  }
  // Skip remaining if string was longer than buffer
  for (uint32_t i = copyLen; i < slen && *pos < len; i++) (*pos)++;
}

// Forward declaration
void cborPrintValue(const uint8_t* data, int* pos, int len, bool topLevel);

// Print CBOR metadata map (the {w:true, l:..., h:..., u:..., e:[...]} part)
void cborPrintMeta(const uint8_t* data, int* pos, int len) {
  if (*pos >= len) return;
  uint8_t b = data[*pos];
  uint8_t major = b >> 5;
  uint8_t info = b & 0x1F;
  if (major != 5) return; // not a map
  (*pos)++;
  uint32_t count = cborReadUint(data, pos, len, info);

  bool writable = false;
  String unit = "";
  String enumVals = "";
  long low = -1, high = -1, initial = -1;
  bool hasLow = false, hasHigh = false, hasInitial = false;

  for (uint32_t i = 0; i < count && *pos < len; i++) {
    // Read key (should be 1-char string)
    char key[8] = {0};
    uint8_t kb = data[(*pos)++];
    uint8_t kinfo = kb & 0x1F;
    cborReadString(data, pos, len, kinfo, key, sizeof(key));

    // Read value
    if (*pos >= len) break;
    uint8_t vb = data[*pos];
    uint8_t vmaj = vb >> 5;
    uint8_t vinf = vb & 0x1F;

    if (strcmp(key, "w") == 0) {
      (*pos)++;
      writable = (vb == 0xF5); // true
    } else if (strcmp(key, "u") == 0) {
      (*pos)++;
      char ubuf[32] = {0};
      cborReadString(data, pos, len, vinf, ubuf, sizeof(ubuf));
      unit = String(ubuf);
    } else if (strcmp(key, "l") == 0) {
      (*pos)++;
      low = cborReadUint(data, pos, len, vinf);
      hasLow = true;
    } else if (strcmp(key, "h") == 0) {
      (*pos)++;
      high = cborReadUint(data, pos, len, vinf);
      hasHigh = true;
    } else if (strcmp(key, "i") == 0) {
      // Initial value - can be various types
      if (vb == 0xF6) { (*pos)++; } // null
      else if (vb == 0xF4) { (*pos)++; hasInitial = true; initial = 0; }
      else if (vb == 0xF5) { (*pos)++; hasInitial = true; initial = 1; }
      else if (vmaj == 0) { (*pos)++; initial = cborReadUint(data, pos, len, vinf); hasInitial = true; }
      else if (vmaj == 3) { // string initial
        (*pos)++;
        char ibuf[32] = {0};
        cborReadString(data, pos, len, vinf, ibuf, sizeof(ibuf));
      }
      else { (*pos)++; } // skip unknown
    } else if (strcmp(key, "e") == 0) {
      // Enum array
      (*pos)++;
      uint32_t ecount = cborReadUint(data, pos, len, vinf);
      enumVals = "";
      for (uint32_t j = 0; j < ecount && *pos < len; j++) {
        uint8_t eb = data[(*pos)++];
        char eval[32] = {0};
        cborReadString(data, pos, len, eb & 0x1F, eval, sizeof(eval));
        if (j > 0) enumVals += ", ";
        enumVals += eval;
      }
    } else {
      // Skip unknown meta value
      cborPrintValue(data, pos, len, false);
    }
  }

  // Print inline annotations
  if (writable) Serial.print(" (writable");
  else Serial.print(" (read-only");
  if (hasLow && hasHigh) {
    Serial.print(", ");
    Serial.print(low);
    Serial.print("-");
    Serial.print(high);
  }
  if (unit.length() > 0) {
    Serial.print(" ");
    Serial.print(unit);
  }
  Serial.print(")");

  if (enumVals.length() > 0) {
    Serial.println();
    cborPrintIndent();
    Serial.print("       enum: ");
    Serial.print(enumVals);
  }
}

// Print a CBOR value. If topLevel, prints map keys with nice formatting.
void cborPrintValue(const uint8_t* data, int* pos, int len, bool topLevel) {
  if (*pos >= len) { Serial.print("(truncated)"); return; }

  uint8_t b = data[(*pos)++];
  uint8_t major = b >> 5;
  uint8_t info = b & 0x1F;

  switch (major) {
    case 0: { // unsigned int
      uint32_t v = cborReadUint(data, pos, len, info);
      Serial.print(v);
      break;
    }
    case 1: { // negative int
      uint32_t v = cborReadUint(data, pos, len, info);
      Serial.print("-");
      Serial.print(v + 1);
      break;
    }
    case 2: { // byte string
      uint32_t slen = cborReadUint(data, pos, len, info);
      Serial.print("h'");
      for (uint32_t i = 0; i < slen && *pos < len; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", data[(*pos)++]);
        Serial.print(hex);
      }
      Serial.print("'");
      break;
    }
    case 3: { // text string
      char str[80] = {0};
      cborReadString(data, pos, len, info, str, sizeof(str));
      Serial.print("\"");
      Serial.print(str);
      Serial.print("\"");
      break;
    }
    case 4: { // array
      uint32_t count = cborReadUint(data, pos, len, info);
      if (topLevel && count == 2) {
        // Somfy config pattern: [value, metadata_map]
        // Print value, then parse metadata for annotations
        cborPrintValue(data, pos, len, false);
        // Check if next item is a map (metadata)
        if (*pos < len && (data[*pos] >> 5) == 5) {
          cborPrintMeta(data, pos, len);
        } else {
          // Not metadata, print normally
          Serial.print(", ");
          cborPrintValue(data, pos, len, false);
        }
      } else {
        // Generic array - check if it's an array of [value, meta] pairs
        // (like IntermediatePositionsLift)
        bool isNested = (count > 2);
        if (isNested) {
          Serial.print("[");
          Serial.print(count);
          Serial.print(" items]");
          // Skip the array contents to avoid spam
          for (uint32_t i = 0; i < count && *pos < len; i++) {
            cborPrintValue(data, pos, len, false);
          }
        } else {
          Serial.print("[");
          for (uint32_t i = 0; i < count && *pos < len; i++) {
            if (i > 0) Serial.print(", ");
            cborPrintValue(data, pos, len, false);
          }
          Serial.print("]");
        }
      }
      break;
    }
    case 5: { // map
      uint32_t count = cborReadUint(data, pos, len, info);
      if (topLevel) {
        // Top-level map: print each key-value on its own line
        for (uint32_t i = 0; i < count && *pos < len; i++) {
          cborPrintIndent();
          // Read key
          uint8_t kb = data[(*pos)++];
          char key[64] = {0};
          cborReadString(data, pos, len, kb & 0x1F, key, sizeof(key));
          Serial.print(key);
          Serial.print(": ");
          // Read value (pass topLevel=true for [value, meta] pattern)
          cborPrintValue(data, pos, len, true);
          Serial.println();
        }
      } else {
        Serial.print("{map:");
        Serial.print(count);
        Serial.print("}");
        // Skip contents
        for (uint32_t i = 0; i < count * 2 && *pos < len; i++) {
          cborPrintValue(data, pos, len, false);
        }
      }
      break;
    }
    case 7: { // simple values
      if (info == 20) Serial.print("false");
      else if (info == 21) Serial.print("true");
      else if (info == 22) Serial.print("null");
      else if (info == 23) Serial.print("undefined");
      else { Serial.print("simple("); Serial.print(info); Serial.print(")"); }
      break;
    }
    default:
      Serial.print("?(");
      Serial.print(b, HEX);
      Serial.print(")");
      break;
  }
}

// ─── Config File Helpers ─────────────────────────────────────────

bool getFileId(const String& name, uint8_t* hi, uint8_t* lo) {
  if (name == "motor")      { *hi = CFG_FILE_MOTOR_HI; *lo = CFG_FILE_MOTOR_LO; return true; }
  if (name == "radio")      { *hi = CFG_FILE_RADIO_HI; *lo = CFG_FILE_RADIO_LO; return true; }
  if (name == "type")       { *hi = CFG_FILE_TYPE_HI;  *lo = CFG_FILE_TYPE_LO;  return true; }
  if (name == "hmi")        { *hi = CFG_FILE_HMI_HI;   *lo = CFG_FILE_HMI_LO;   return true; }
  return false;
}

// Poll FileStatus (00040003) for success. Returns true if 0500 or 0564 seen.
bool pollFileStatus(int maxAttempts) {
  uint8_t st[4];
  for (int i = 0; i < maxAttempts; i++) {
    int len = bleReadTo(UUID_FILESTATUS, st, sizeof(st));
    if (len >= 2) {
      // Success codes from TaHoma Pro app: "0500" or "0564"
      if (st[0] == 0x05 && (st[1] == 0x00 || st[1] == 0x64)) {
        return true;
      }
    }
    delay(500);
  }
  return false;
}

// Protocol derived from decompiling TaHoma Pro 2.4.0:
//   OPEN_FILE:  [00] [fileId_hi] [fileId_lo] [mode]  (fileId is little-endian on wire)
//   SIZE:       [01]                                  (no fileId, no mode)
//   WRITE:      [02] [cbor_bytes...]                  (no fileId!)
//   READ:       [03]                                  (no fileId!)
//   CLOSE_FILE: [04]                                  (no fileId!)
//
// Read sequence:  OPEN(read) -> poll status -> SIZE -> read response -> READ -> read response -> CLOSE
// Write sequence: OPEN(write) -> WRITE(cbor) -> CLOSE -> poll status

bool configFileRead(uint8_t fhi, uint8_t flo, uint8_t* buf, int* outLen) {
  *outLen = 0;

  // Step 1: OPEN_FILE in READ mode: [00 fhi flo 00]
  uint8_t openCmd[4] = { WDATA_OPEN, fhi, flo, FMODE_READ };
  if (!bleWriteFlexible(UUID_WRITEDATA, openCmd, 4)) {
    printERR("Failed to open config file");
    return false;
  }

  // Step 2: Poll FileStatus until ready
  if (!pollFileStatus(10)) {
    printWARN("FileStatus not ready, continuing anyway...");
  }

  // Step 3: SIZE command: just [01]
  uint8_t sizeCmd[1] = { WDATA_SIZE };
  if (!bleWriteFlexible(UUID_WRITEDATA, sizeCmd, 1)) {
    printERR("Failed to send SIZE command");
    goto cleanup;
  }
  delay(200);

  // Read size response from 00040001
  {
    uint8_t sizeBuf[4];
    int sizeLen = bleReadTo(UUID_WRITEDATA, sizeBuf, sizeof(sizeBuf));
    if (sizeLen >= 2) {
      uint16_t fileSize = sizeBuf[0] | ((uint16_t)sizeBuf[1] << 8);
      char msg[40];
      snprintf(msg, sizeof(msg), "File size: %u bytes", fileSize);
      printInfo(msg);
    }
  }

  // Step 4: READ command: just [03]
  {
    uint8_t readCmd[1] = { WDATA_READ };
    if (!bleWriteFlexible(UUID_WRITEDATA, readCmd, 1)) {
      printERR("Failed to send READ command");
      goto cleanup;
    }
    delay(200);
  }

  // Step 5: Read data response from 00040001
  *outLen = bleReadTo(UUID_WRITEDATA, buf, CFG_BUF_SIZE);

cleanup:
  // Step 6: CLOSE_FILE: just [04]
  {
    uint8_t closeCmd[1] = { WDATA_CLOSE };
    bleWriteFlexible(UUID_WRITEDATA, closeCmd, 1);
  }

  return *outLen > 0;
}

bool configFileWrite(uint8_t fhi, uint8_t flo, uint8_t* data, int dataLen) {
  // Step 1: OPEN_FILE in WRITE mode: [00 fhi flo 01]
  uint8_t openCmd[4] = { WDATA_OPEN, fhi, flo, FMODE_WRITE };
  if (!bleWriteFlexible(UUID_WRITEDATA, openCmd, 4)) {
    printERR("Failed to open config file for writing");
    return false;
  }
  delay(200);

  // Step 2: WRITE command: [02] + CBOR data (no file ID!)
  uint8_t writeBuf[CFG_BUF_SIZE];
  writeBuf[0] = WDATA_WRITE;
  if (dataLen + 1 > CFG_BUF_SIZE) {
    printERR("Data too large");
    goto fail;
  }
  memcpy(writeBuf + 1, data, dataLen);
  if (!bleWriteFlexible(UUID_WRITEDATA, writeBuf, dataLen + 1)) {
    printERR("Failed to write config data");
    goto fail;
  }
  delay(200);

  // Step 3: CLOSE_FILE: just [04]
  {
    uint8_t closeCmd[1] = { WDATA_CLOSE };
    if (!bleWriteFlexible(UUID_WRITEDATA, closeCmd, 1)) {
      printERR("Failed to close config file");
      return false;
    }
  }

  // Step 4: Poll FileStatus for success
  printInfo("Waiting for motor to confirm write...");
  if (pollFileStatus(10)) {
    return true;
  } else {
    printWARN("FileStatus did not confirm success (may still have worked)");
    return true;  // optimistic -- the write may have succeeded
  }

fail:
  {
    uint8_t closeCmd[1] = { WDATA_CLOSE };
    bleWriteFlexible(UUID_WRITEDATA, closeCmd, 1);
  }
  return false;
}

// ─── Config Commands ─────────────────────────────────────────────

void cmdConfigList() {
  Serial.println();
  Serial.println("  ┌─ Config Files (CBOR, via WriteData on 00040001) ────────┐");
  Serial.println("  │                                                          │");
  Serial.println("  │  motor (0x00C4)  Motor settings                   ★★    │");
  Serial.println("  │    Application, LiftRange, ReversedDirection,            │");
  Serial.println("  │    NominalSpeed, Ramps, IntermediatePositions            │");
  Serial.println("  │                                                          │");
  Serial.println("  │  radio (0x00C3)  Radio/product settings           ★★    │");
  Serial.println("  │    DeviceName, EndProduct, ZigbeeTxPower,                │");
  Serial.println("  │    BleTxPower, StepLiftConversion, StepTiltConversion    │");
  Serial.println("  │                                                          │");
  Serial.println("  │  type  (0x00D2)  Firmware/hardware info (read-only) ★★  │");
  Serial.println("  │    MediaSoftRelease, Motor, Type, MotorSoftRelease, etc  │");
  Serial.println("  │                                                          │");
  Serial.println("  │  hmi   (0x00C2)  HMI config (often empty)        ★     │");
  Serial.println("  │                                                          │");
  Serial.println("  │  Usage:  config read motor                               │");
  Serial.println("  │          config dump radio                               │");
  Serial.println("  │          config set motor Application Venetian           │");
  Serial.println("  └──────────────────────────────────────────────────────────┘");
}

void cmdConfigRead(const String& fileName) {
  uint8_t fhi, flo;
  if (!getFileId(fileName, &fhi, &flo)) {
    printERR("Unknown file. Use: motor, radio, type, hmi");
    return;
  }

  char msg[48];
  snprintf(msg, sizeof(msg), "Reading %s config (0x%02X%02X)...", fileName.c_str(), fhi, flo);
  printInfo(msg);

  if (!configFileRead(fhi, flo, cfgBuf, &cfgBufLen)) {
    printERR("Failed to read config file (empty or unreadable)");
    return;
  }

  snprintf(msg, sizeof(msg), "Got %d bytes, decoding CBOR:", cfgBufLen);
  printOK(msg);
  Serial.println("  ┌─────────────────────────────────────────────");

  int pos = 0;
  cborIndent = 1;
  cborPrintValue(cfgBuf, &pos, cfgBufLen, true);

  if (pos < cfgBufLen) {
    Serial.print("  │ (");
    Serial.print(cfgBufLen - pos);
    Serial.println(" bytes remaining, possibly truncated by BLE MTU)");
  }
  Serial.println("  └─────────────────────────────────────────────");
}

void cmdConfigDump(const String& fileName) {
  uint8_t fhi, flo;
  if (!getFileId(fileName, &fhi, &flo)) {
    printERR("Unknown file. Use: motor, radio, type, hmi");
    return;
  }

  char msg[48];
  snprintf(msg, sizeof(msg), "Dumping %s config (0x%02X%02X)...", fileName.c_str(), fhi, flo);
  printInfo(msg);

  if (!configFileRead(fhi, flo, cfgBuf, &cfgBufLen)) {
    printERR("Failed to read config file");
    return;
  }

  snprintf(msg, sizeof(msg), "%d bytes:", cfgBufLen);
  printOK(msg);

  // Print hex dump with offset
  for (int i = 0; i < cfgBufLen; i += 16) {
    char offset[8];
    snprintf(offset, sizeof(offset), "  %04X: ", i);
    Serial.print(offset);
    for (int j = 0; j < 16 && i + j < cfgBufLen; j++) {
      char hex[4];
      snprintf(hex, sizeof(hex), "%02X ", cfgBuf[i + j]);
      Serial.print(hex);
    }
    Serial.println();
  }
}

void cmdConfigSet(const String& args) {
  // Parse: <file> <key> <value>
  int sp1 = args.indexOf(' ');
  if (sp1 < 0) {
    printERR("Usage: config set <file> <key> <value>");
    return;
  }
  String fileName = args.substring(0, sp1);
  String rest = args.substring(sp1 + 1);
  rest.trim();
  int sp2 = rest.indexOf(' ');
  if (sp2 < 0) {
    printERR("Usage: config set <file> <key> <value>");
    return;
  }
  String key = rest.substring(0, sp2);
  String value = rest.substring(sp2 + 1);
  value.trim();

  uint8_t fhi, flo;
  if (!getFileId(fileName, &fhi, &flo)) {
    printERR("Unknown file. Use: motor, radio, type, hmi");
    return;
  }

  // Build CBOR: map(1) { key: [value] }
  // TaHoma Pro wraps values in a 1-element array: {"Key": ["Value"]}
  // Value detection: true/false -> bool, digits -> uint, else -> string
  uint8_t cbor[128];
  int cpos = 0;

  // Map with 1 entry
  cbor[cpos++] = 0xA1;

  // Key: text string
  int keyLen = key.length();
  if (keyLen < 24) {
    cbor[cpos++] = 0x60 | keyLen;
  } else {
    cbor[cpos++] = 0x78;
    cbor[cpos++] = keyLen;
  }
  for (int i = 0; i < keyLen; i++) cbor[cpos++] = key.charAt(i);

  // Value wrapped in array(1)
  cbor[cpos++] = 0x81;  // array of 1 element

  if (value == "true") {
    cbor[cpos++] = 0xF5;
  } else if (value == "false") {
    cbor[cpos++] = 0xF4;
  } else if (value == "null") {
    cbor[cpos++] = 0xF6;
  } else {
    // Check if numeric
    bool isNum = true;
    for (unsigned int i = 0; i < value.length(); i++) {
      if (!isDigit(value.charAt(i))) { isNum = false; break; }
    }
    if (isNum && value.length() > 0) {
      uint32_t v = value.toInt();
      if (v < 24) {
        cbor[cpos++] = v;
      } else if (v < 256) {
        cbor[cpos++] = 0x18;
        cbor[cpos++] = v;
      } else if (v < 65536) {
        cbor[cpos++] = 0x19;
        cbor[cpos++] = (v >> 8) & 0xFF;
        cbor[cpos++] = v & 0xFF;
      } else {
        cbor[cpos++] = 0x1A;
        cbor[cpos++] = (v >> 24) & 0xFF;
        cbor[cpos++] = (v >> 16) & 0xFF;
        cbor[cpos++] = (v >> 8) & 0xFF;
        cbor[cpos++] = v & 0xFF;
      }
    } else {
      // String value
      int vLen = value.length();
      if (vLen < 24) {
        cbor[cpos++] = 0x60 | vLen;
      } else {
        cbor[cpos++] = 0x78;
        cbor[cpos++] = vLen;
      }
      for (int i = 0; i < vLen; i++) cbor[cpos++] = value.charAt(i);
    }
  }

  // Show what we're writing
  Serial.print("  [..] Setting ");
  Serial.print(fileName);
  Serial.print(".");
  Serial.print(key);
  Serial.print(" = ");
  Serial.println(value);
  Serial.print("  [..] CBOR (");
  Serial.print(cpos);
  Serial.print(" bytes): ");
  printHex(cbor, cpos);
  Serial.println();

  // Confirm
  Serial.println("  [!] This writes to motor config via CBOR protocol.");
  Serial.println("  [!] Type 'yes-config-write' to confirm.");

  // Store for confirmation
  memcpy(cfgBuf, cbor, cpos);
  cfgBufLen = cpos;
  // Store file ID in the buffer after CBOR for retrieval
  cfgBuf[cpos] = fhi;
  cfgBuf[cpos + 1] = flo;
}

void cmdConfigSetConfirm() {
  if (cfgBufLen == 0) {
    printERR("No pending config write. Use 'config set' first.");
    return;
  }
  // Retrieve file ID from after CBOR data
  uint8_t fhi = cfgBuf[cfgBufLen];
  uint8_t flo = cfgBuf[cfgBufLen + 1];

  printInfo("Writing config...");
  if (configFileWrite(fhi, flo, cfgBuf, cfgBufLen)) {
    printOK("Config written! Power-cycle the motor for changes to take effect.");
  }
  cfgBufLen = 0;
}

// ─── Info Command ────────────────────────────────────────────────

void cmdInfo() {
  if (!connected) { printERR("Not connected"); return; }

  Serial.println();
  Serial.println("  ┌─ Device Information ───────────────────────");

  // Standard BLE: Device Name (0x2A00)
  BLEUUID uuidDevName("00002a00-0000-1000-8000-00805f9b34fb");
  uint8_t tmp[64];
  int len;

  len = bleReadTo(uuidDevName, tmp, sizeof(tmp) - 1);
  if (len > 0) {
    tmp[len] = 0;
    Serial.print("  │ Device Name:   ");
    Serial.println((char*)tmp);
  }

  // Standard BLE: HW Revision (0x2A27)
  BLEUUID uuidHwRev("00002a27-0000-1000-8000-00805f9b34fb");
  len = bleReadTo(uuidHwRev, tmp, sizeof(tmp) - 1);
  if (len > 0) {
    tmp[len] = 0;
    Serial.print("  │ HW Revision:   ");
    Serial.println((char*)tmp);
  }

  // Standard BLE: SW Revision (0x2A28)
  BLEUUID uuidSwRev("00002a28-0000-1000-8000-00805f9b34fb");
  len = bleReadTo(uuidSwRev, tmp, sizeof(tmp) - 1);
  if (len > 0) {
    tmp[len] = 0;
    Serial.print("  │ SW Revision:   ");
    Serial.println((char*)tmp);
  }

  // Standard BLE: Manufacturer (0x2A29)
  BLEUUID uuidMfr("00002a29-0000-1000-8000-00805f9b34fb");
  len = bleReadTo(uuidMfr, tmp, sizeof(tmp) - 1);
  if (len > 0) {
    tmp[len] = 0;
    Serial.print("  │ Manufacturer:  ");
    Serial.println((char*)tmp);
  }

  // Standard BLE: PnP ID (0x2A50)
  BLEUUID uuidPnp("00002a50-0000-1000-8000-00805f9b34fb");
  len = bleReadTo(uuidPnp, tmp, sizeof(tmp));
  if (len > 0) {
    Serial.print("  │ PnP ID:        ");
    printHex(tmp, len);
    Serial.println();
  }

  // Standard BLE: Battery Level (0x2A19)
  BLEUUID uuidBatt("00002a19-0000-1000-8000-00805f9b34fb");
  len = bleReadTo(uuidBatt, tmp, sizeof(tmp));
  if (len > 0) {
    Serial.print("  │ Battery:       ");
    Serial.print(tmp[0]);
    Serial.println("%");
  }

  // Standard BLE: Battery Power State (0x2A1A)
  BLEUUID uuidBattSt("00002a1a-0000-1000-8000-00805f9b34fb");
  len = bleReadTo(uuidBattSt, tmp, sizeof(tmp));
  if (len > 0) {
    Serial.print("  │ Battery State: 0x");
    char hex[4]; snprintf(hex, sizeof(hex), "%02X", tmp[0]);
    Serial.println(hex);
  }

  Serial.println("  │");
  Serial.println("  │ Network (Zigbee):");

  // Zigbee channel (00020002)
  BLEUUID uuidCh("00020002-cad9-46c6-a2ea-2ca16d57b4a5");
  len = bleReadTo(uuidCh, tmp, sizeof(tmp));
  if (len > 0) {
    Serial.print("  │   Channel:     ");
    Serial.println(tmp[0]);
  }

  // Zigbee PAN ID (00020003)
  BLEUUID uuidPan("00020003-cad9-46c6-a2ea-2ca16d57b4a5");
  len = bleReadTo(uuidPan, tmp, sizeof(tmp));
  if (len >= 2) {
    uint16_t pan = tmp[0] | ((uint16_t)tmp[1] << 8);
    char panStr[8];
    snprintf(panStr, sizeof(panStr), "0x%04X", pan);
    Serial.print("  │   PAN ID:      ");
    Serial.println(panStr);
  }

  // Zigbee EUI64 (00020005)
  BLEUUID uuidEui("00020005-cad9-46c6-a2ea-2ca16d57b4a5");
  len = bleReadTo(uuidEui, tmp, sizeof(tmp));
  if (len >= 8) {
    Serial.print("  │   EUI64:       ");
    // Print in big-endian (human-readable) order
    for (int i = 7; i >= 0; i--) {
      char hex[3];
      snprintf(hex, sizeof(hex), "%02X", tmp[i]);
      Serial.print(hex);
      if (i == 4) Serial.print(":");  // visual separator for FFFE
    }
    Serial.println();
    // Also show derived BLE MAC
    Serial.print("  │   BLE MAC:     ");
    char mac[24];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             tmp[7], tmp[6], tmp[5], tmp[2], tmp[1], tmp[0]);
    Serial.print(mac);
    Serial.println(" (derived, removing FFFE)");
  }

  // Zigbee install code (00020006)
  BLEUUID uuidIC("00020006-cad9-46c6-a2ea-2ca16d57b4a5");
  len = bleReadTo(uuidIC, tmp, sizeof(tmp));
  if (len > 0) {
    Serial.print("  │   Install Code: ");
    printHex(tmp, len);
    Serial.println();
  }

  // Firmware string (00010002)
  BLEUUID uuidFw("00010002-cad9-46c6-a2ea-2ca16d57b4a5");
  len = bleReadTo(uuidFw, tmp, sizeof(tmp) - 1);
  if (len > 0) {
    tmp[len] = 0;
    Serial.print("  │ FW Release:    ");
    Serial.println((char*)tmp);
  }

  Serial.println("  └──────────────────────────────────────────");
}

// ─── Enhanced Services Display ───────────────────────────────────

struct CharLabel {
  const char* prefix;  // 8-char hex prefix
  const char* label;
  const char* confidence;
};

// Somfy custom characteristic labels
static const CharLabel somfyLabels[] = {
  {"00000001", "IDENTIFY",         "★★★"},
  {"00000002", "?unknown",         "?  "},
  {"00000003", "?status_flag",     "★  "},
  {"00000004", "?position_counter","★  "},
  {"00000005", "GOTO_POS (lift)",  "★★★"},
  {"00000006", "STOP",             "★★★"},
  {"00000007", "?tilt_position",   "★  "},
  {"00000008", "?tilt_open",       "★  "},
  {"00000009", "?tilt_close",      "★  "},
  {"0000000a", "DELIVERY_MODE",    "★★ "},
  {"0000000b", "AUTH",             "★★★"},
  {"0000000d", "OPEN (venetian)",  "★★ "},
  {"0000000e", "CLOSE (venetian)", "★★ "},
  {"00010001", "FACTORY_RESET",    "★★ "},
  {"00010002", "FW_VERSION",       "★★ "},
  {"00010005", "SET_DIRECTION",    "★★ "},
  {"00010007", "SET_LIMIT (lift)", "★★ "},
  {"00010008", "MOVE_DOWN (lift)", "★★★"},
  {"00010009", "MOVE_UP (lift)",   "★★★"},
  {"0001000a", "?tilt_limit",      "★  "},
  {"0001000b", "CONFIG_RANGE",     "★★ "},
  {"00020001", "LEAVE_NETWORK",    "★★★"},
  {"00020002", "ZB_CHANNEL",       "★★ "},
  {"00020003", "ZB_PAN_ID",        "★  "},
  {"00020004", "ZB_ROLE",          "★  "},
  {"00020005", "ZB_EUI64",         "★★ "},
  {"00020006", "ZB_INSTALL_CODE",  "★  "},
  {"00020007", "?unknown",         "?  "},
  {"00020008", "?unknown",         "?  "},
  {"00040001", "CONFIG_RW",        "★★ "},
  {"00040002", "?config_data",     "★  "},
  {"00040003", "?config_status",   "★  "},
  {nullptr, nullptr, nullptr}
};

// Standard BLE service names
const char* getStdServiceName(const char* uuid) {
  if (strstr(uuid, "00001800")) return "Generic Access";
  if (strstr(uuid, "00001801")) return "Generic Attribute";
  if (strstr(uuid, "0000180a")) return "Device Information";
  if (strstr(uuid, "0000180f")) return "Battery Service";
  return nullptr;
}

// Standard BLE characteristic names
const char* getStdCharName(const char* uuid) {
  if (strstr(uuid, "00002a00")) return "Device Name";
  if (strstr(uuid, "00002a01")) return "Appearance";
  if (strstr(uuid, "00002a05")) return "Service Changed";
  if (strstr(uuid, "00002a19")) return "Battery Level";
  if (strstr(uuid, "00002a1a")) return "Battery Power State";
  if (strstr(uuid, "00002a27")) return "HW Revision";
  if (strstr(uuid, "00002a28")) return "SW Revision";
  if (strstr(uuid, "00002a29")) return "Manufacturer Name";
  if (strstr(uuid, "00002a50")) return "PnP ID";
  if (strstr(uuid, "00002b29")) return "Client Supported Features";
  if (strstr(uuid, "00002b2a")) return "Database Hash";
  return nullptr;
}

// Somfy custom service names
const char* getSomfyServiceName(const char* uuid) {
  if (strstr(uuid, "00000000-cad9")) return "Somfy Operational";
  if (strstr(uuid, "00010000-cad9")) return "Somfy Configuration";
  if (strstr(uuid, "00020000-cad9")) return "Somfy Network/Zigbee";
  if (strstr(uuid, "00040000-cad9")) return "Somfy Config Files (WriteData)";
  return nullptr;
}

void cmdServices() {
  if (!connected) { printERR("Not connected"); return; }

  std::map<std::string, BLERemoteService*>* services = pClient->getServices();
  Serial.println();
  Serial.println("  ┌─ BLE Services ─────────────────────────────────────────────┐");
  Serial.println("  │ Confidence: ★★★=tested ★★=from RE ★=guessed ?=unknown     │");
  Serial.println("  ├─────────────────────────────────────────────────────────────┤");

  for (auto& kv : *services) {
    const char* svcUuid = kv.first.c_str();
    Serial.print("  │ ");

    // Check for known names
    const char* stdName = getStdServiceName(svcUuid);
    const char* somfyName = getSomfyServiceName(svcUuid);
    if (somfyName) {
      Serial.print(somfyName);
    } else if (stdName) {
      Serial.print(stdName);
    } else {
      Serial.print(svcUuid);
    }
    Serial.println();

    std::map<std::string, BLERemoteCharacteristic*>* chars = kv.second->getCharacteristics();
    for (auto& ckv : *chars) {
      const char* charUuid = ckv.first.c_str();
      Serial.print("  │   ");

      // Check standard BLE char names
      const char* stdCharName = getStdCharName(charUuid);
      if (stdCharName) {
        Serial.print(charUuid);
        Serial.print("  ");
        Serial.println(stdCharName);
        continue;
      }

      // Check Somfy labels by prefix (first 8 chars)
      char prefix[9] = {0};
      strncpy(prefix, charUuid, 8);

      bool found = false;
      for (int i = 0; somfyLabels[i].prefix != nullptr; i++) {
        if (strcmp(prefix, somfyLabels[i].prefix) == 0) {
          Serial.print(prefix);
          Serial.print("  ");
          Serial.print(somfyLabels[i].confidence);
          Serial.print(" ");
          Serial.println(somfyLabels[i].label);
          found = true;
          break;
        }
      }
      if (!found) {
        Serial.print(charUuid);
        Serial.println("  ?");
      }
    }
    Serial.println("  │");
  }
  Serial.println("  └─────────────────────────────────────────────────────────────┘");
}

// ─── Movement Commands ───────────────────────────────────────────

void cmdMoveUp(int step) {
  if (step < 1) step = 1;
  if (step > 500) step = 500;
  uint8_t data[2] = { (uint8_t)(step & 0xFF), (uint8_t)((step >> 8) & 0xFF) };
  char msg[40]; snprintf(msg, sizeof(msg), "Moving UP (step=%d)", step);
  printInfo(msg);
  if (bleWriteFlexible(UUID_MOVE_UP, data, 2)) printOK("Move up sent");
}

void cmdMoveDown(int step) {
  if (step < 1) step = 1;
  if (step > 500) step = 500;
  uint8_t data[2] = { (uint8_t)(step & 0xFF), (uint8_t)((step >> 8) & 0xFF) };
  char msg[40]; snprintf(msg, sizeof(msg), "Moving DOWN (step=%d)", step);
  printInfo(msg);
  if (bleWriteFlexible(UUID_MOVE_DOWN, data, 2)) printOK("Move down sent");
}

void cmdStop() {
  uint8_t val = 0x01;
  if (bleWriteFlexible(UUID_STOP, &val, 1)) printOK("Stop sent");
}

void cmdGoto(int pos) {
  if (pos < 0) pos = 0;
  if (pos > 32767) pos = 32767;
  uint8_t data[2] = { (uint8_t)(pos & 0xFF), (uint8_t)((pos >> 8) & 0xFF) };
  char msg[48]; snprintf(msg, sizeof(msg), "Going to position %d", pos);
  printInfo(msg);
  if (bleWriteFlexible(UUID_GOTO_POS, data, 2)) printOK("Goto position sent");
}

void cmdConfigRange(const String& mode) {
  uint8_t data[2] = {0, 0};
  if (mode == "start")     { data[0] = 0x00; }
  else if (mode == "half") { data[0] = 0x02; }
  else if (mode == "full") { data[0] = 0x01; }
  else { printERR("Usage: range start|half|full"); return; }
  char msg[48]; snprintf(msg, sizeof(msg), "Configure range: %s", mode.c_str());
  printInfo(msg);
  if (bleWriteFlexible(UUID_CFG_RANGE, data, 2)) printOK("Range config sent");
}

void cmdSetLimit(const String& which) {
  uint8_t val = (which == "up") ? 0x00 : (which == "down") ? 0x01 : 0xFF;
  if (val == 0xFF) { printERR("Usage: limit up|down"); return; }
  char msg[48]; snprintf(msg, sizeof(msg), "Setting %s limit at current position", which.c_str());
  printInfo(msg);
  if (bleWriteFlexible(UUID_SET_LIMIT, &val, 1)) printOK("Limit set!");
}

void cmdSetDir(const String& dir) {
  uint8_t val = (dir == "cw") ? 0x01 : (dir == "ccw") ? 0x00 : 0xFF;
  if (val == 0xFF) { printERR("Usage: dir cw|ccw"); return; }
  char msg[48]; snprintf(msg, sizeof(msg), "Setting direction: %s", dir.c_str());
  printInfo(msg);
  if (bleWriteFlexible(UUID_SET_DIR, &val, 1)) printOK("Direction set!");
}

// ─── Danger Zone Commands ────────────────────────────────────────

void cmdFactoryReset() {
  Serial.println();
  Serial.println("  ╔════════════════════════════════════════╗");
  Serial.println("  ║  WARNING: FACTORY RESET                ║");
  Serial.println("  ║  Erases limits, network, all settings  ║");
  Serial.println("  ║  Motor enters programming mode 3 min   ║");
  Serial.println("  ║  Type 'yes-reset' to confirm           ║");
  Serial.println("  ╚════════════════════════════════════════╝");
}

void cmdLeaveNetwork() {
  Serial.println();
  Serial.println("  ╔════════════════════════════════════════╗");
  Serial.println("  ║  WARNING: LEAVE NETWORK                ║");
  Serial.println("  ║  Motor leaves Zigbee, will try rejoin  ║");
  Serial.println("  ║  Type 'yes-leave' to confirm           ║");
  Serial.println("  ╚════════════════════════════════════════╝");
}

// ─── Raw BLE Access ──────────────────────────────────────────────

void cmdWriteRaw(const String& args) {
  int spaceIdx = args.indexOf(' ');
  if (spaceIdx < 0) {
    printERR("Usage: write <uuid-prefix> <hex bytes...>");
    return;
  }
  String prefix = args.substring(0, spaceIdx);
  String hexPart = args.substring(spaceIdx + 1);
  hexPart.trim();

  String fullUUID = prefix + "-cad9-46c6-a2ea-2ca16d57b4a5";
  BLEUUID uuid(fullUUID.c_str());

  uint8_t data[64];
  int dataLen = 0;
  int pos = 0;
  while (pos < (int)hexPart.length() && dataLen < 64) {
    while (pos < (int)hexPart.length() && hexPart.charAt(pos) == ' ') pos++;
    if (pos >= (int)hexPart.length()) break;
    char hi = hexPart.charAt(pos);
    char lo = (pos + 1 < (int)hexPart.length()) ? hexPart.charAt(pos + 1) : '0';
    uint8_t val = 0;
    if (hi >= '0' && hi <= '9') val = (hi - '0') << 4;
    else if (hi >= 'a' && hi <= 'f') val = (hi - 'a' + 10) << 4;
    else if (hi >= 'A' && hi <= 'F') val = (hi - 'A' + 10) << 4;
    if (lo >= '0' && lo <= '9') val |= (lo - '0');
    else if (lo >= 'a' && lo <= 'f') val |= (lo - 'a' + 10);
    else if (lo >= 'A' && lo <= 'F') val |= (lo - 'A' + 10);
    data[dataLen++] = val;
    pos += 2;
  }
  if (dataLen == 0) { printERR("No data bytes parsed"); return; }

  Serial.print("  [..] Writing to ");
  Serial.print(prefix);
  Serial.print(": ");
  printHex(data, dataLen);
  Serial.println();
  if (bleWriteFlexible(uuid, data, dataLen)) printOK("Write succeeded");
}

void cmdRead(const String& args) {
  String prefix = args;
  prefix.trim();
  if (prefix.length() < 8) {
    printERR("Usage: read <uuid-prefix>  e.g.: read 00000007");
    return;
  }
  if (!connected) { printERR("Not connected"); return; }

  String fullUUID = prefix + "-cad9-46c6-a2ea-2ca16d57b4a5";
  BLEUUID uuid(fullUUID.c_str());

  Serial.print("  [..] Reading ");
  Serial.print(prefix);
  Serial.println("...");

  uint8_t buf[256];
  int len = bleReadTo(uuid, buf, sizeof(buf));
  if (len < 0) {
    printERR("Read failed");
    return;
  }

  Serial.print("  [OK] ");
  Serial.print(len);
  Serial.print(" bytes: ");
  printHex(buf, len);
  Serial.println();

  // Try ASCII
  bool printable = true;
  for (int i = 0; i < len; i++) {
    if (buf[i] < 32 || buf[i] > 126) { printable = false; break; }
  }
  if (printable && len > 0) {
    buf[len] = 0;
    Serial.print("       ASCII: ");
    Serial.println((char*)buf);
  }
}

// ─── Command Parsing ─────────────────────────────────────────────

String inputBuffer = "";

// ─── Command History ─────────────────────────────────────────────
#define HISTORY_SIZE 16
#define MAX_CMD_LEN 128
char history[HISTORY_SIZE][MAX_CMD_LEN];
int historyCount = 0;    // total commands stored
int historyPos = -1;     // current browse position (-1 = typing new)
String savedInput = "";  // saved input when browsing history

void historyAdd(const String& cmd) {
  if (cmd.length() == 0) return;
  // Don't add duplicates of the last entry
  if (historyCount > 0) {
    int lastIdx = (historyCount - 1) % HISTORY_SIZE;
    if (strcmp(history[lastIdx], cmd.c_str()) == 0) return;
  }
  int idx = historyCount % HISTORY_SIZE;
  cmd.toCharArray(history[idx], MAX_CMD_LEN);
  historyCount++;
}

// Clear current line and redraw with new content
void lineReplace(const String& newContent) {
  // Erase current input from display
  for (unsigned int i = 0; i < inputBuffer.length(); i++) Serial.print("\b \b");
  inputBuffer = newContent;
  Serial.print(inputBuffer);
}

void historyUp() {
  if (historyCount == 0) return;
  int available = historyCount < HISTORY_SIZE ? historyCount : HISTORY_SIZE;
  if (historyPos == -1) {
    // First press: save current input, go to most recent
    savedInput = inputBuffer;
    historyPos = historyCount - 1;
  } else if (historyPos > historyCount - available) {
    historyPos--;
  } else {
    return; // at oldest
  }
  lineReplace(String(history[historyPos % HISTORY_SIZE]));
}

void historyDown() {
  if (historyPos == -1) return;
  if (historyPos < historyCount - 1) {
    historyPos++;
    lineReplace(String(history[historyPos % HISTORY_SIZE]));
  } else {
    // Back to the input being typed
    historyPos = -1;
    lineReplace(savedInput);
  }
}

// ─── Escape Sequence Parser ──────────────────────────────────────
// Handles VT100 arrow keys: ESC [ A (up), ESC [ B (down)
enum EscState { ESC_NONE, ESC_GOT_ESC, ESC_GOT_BRACKET };
EscState escState = ESC_NONE;

// Returns true if the character was consumed by the escape parser
bool handleEscape(char c) {
  switch (escState) {
    case ESC_NONE:
      if (c == 27) { escState = ESC_GOT_ESC; return true; }
      return false;
    case ESC_GOT_ESC:
      if (c == '[') { escState = ESC_GOT_BRACKET; return true; }
      escState = ESC_NONE;
      return false;  // wasn't an escape sequence, let it through
    case ESC_GOT_BRACKET:
      escState = ESC_NONE;
      if (c == 'A') { historyUp(); return true; }    // Up arrow
      if (c == 'B') { historyDown(); return true; }   // Down arrow
      if (c == 'C') { return true; }                  // Right arrow (ignore)
      if (c == 'D') { return true; }                  // Left arrow (ignore)
      return true;  // consume unknown sequence chars
  }
  return false;
}

void processCommand(String input) {
  input.trim();
  if (input.length() == 0) return;

  int spaceIdx = input.indexOf(' ');
  String cmd = (spaceIdx >= 0) ? input.substring(0, spaceIdx) : input;
  String arg = (spaceIdx >= 0) ? input.substring(spaceIdx + 1) : "";
  arg.trim();
  cmd.toLowerCase();

  if (cmd == "help" || cmd == "?") { printHelp(); }
  else if (cmd == "status") { printStatus(); }
  else if (cmd == "scan") {
    printInfo("Scanning for BLE devices (5 seconds)...");
    scanResultCount = 0;
    pBLEScan->clearResults();
    BLEScanResults* results = pBLEScan->start(5, false);
    int count = results->getCount();
    Serial.println();
    Serial.println("  ┌─ Scan Results ─────────────────────────────────────────┐");
    Serial.println("  │  #  RSSI  MAC Address        Name                     │");
    Serial.println("  ├────────────────────────────────────────────────────────┤");
    for (int i = 0; i < count && scanResultCount < MAX_SCAN_RESULTS; i++) {
      BLEAdvertisedDevice dev = results->getDevice(i);
      String name = dev.haveName() ? String(dev.getName().c_str()) : "";
      String addr = String(dev.getAddress().toString().c_str());
      scanResults[scanResultCount].addr = addr;
      scanResults[scanResultCount].name = name;
      scanResults[scanResultCount].rssi = dev.getRSSI();
      char line[80];
      snprintf(line, sizeof(line), "  │ %2d  %4d  %-18s %-24s │",
               scanResultCount + 1, dev.getRSSI(), addr.c_str(),
               name.length() > 0 ? name.substring(0, 24).c_str() : "(unnamed)");
      Serial.println(line);
      scanResultCount++;
    }
    Serial.println("  └────────────────────────────────────────────────────────┘");
    Serial.print("  Found "); Serial.print(scanResultCount);
    Serial.println(" devices. Use 'target <mac>' or 'target <#>'.");
    pBLEScan->clearResults();
  }
  else if (cmd == "target") {
    if (arg.length() == 0) { printERR("Usage: target <mac|#>"); return; }
    bool isNumber = true;
    for (unsigned int i = 0; i < arg.length(); i++) {
      if (!isDigit(arg.charAt(i))) { isNumber = false; break; }
    }
    if (isNumber && arg.toInt() > 0 && arg.toInt() <= scanResultCount) {
      targetAddr = scanResults[arg.toInt() - 1].addr;
    } else {
      targetAddr = arg;
    }
    Serial.print("  [OK] Target: "); Serial.println(targetAddr);
  }
  else if (cmd == "pin") {
    if (arg.length() == 0) { printERR("Usage: pin <code>"); return; }
    pinCode = arg;
    Serial.print("  [OK] PIN: "); Serial.println(pinCode);
  }
  else if (cmd == "connect") {
    if (targetAddr.length() == 0) { printERR("No target. Use 'target <mac>'."); return; }
    if (connected) { pClient->disconnect(); connected = false; authenticated = false; delay(500); }
    Serial.print("  [..] Connecting to "); Serial.print(targetAddr); Serial.println("...");
    BLEAddress addr(targetAddr.c_str());
    try {
      if (!pClient->connect(addr)) { printERR("Connection failed"); return; }
    } catch (...) { printERR("Connection failed (exception)"); return; }
    connected = true; authenticated = false; printOK("Connected!");
    printInfo("Discovering services...");
    std::map<std::string, BLERemoteService*>* services = pClient->getServices();
    for (auto& kv : *services) {
      try {
        if (kv.second->getCharacteristic(UUID_AUTH) != nullptr) {
          pService = kv.second;
          Serial.print("  [OK] Found Somfy service: "); Serial.println(kv.first.c_str());
        }
      } catch (...) {}
    }
    Serial.println("  Next: pin <code>, then auth");
  }
  else if (cmd == "disconnect" || cmd == "dc") {
    if (!connected) { printWARN("Not connected"); return; }
    pClient->disconnect(); connected = false; authenticated = false; pService = nullptr;
    printOK("Disconnected");
  }
  else if (cmd == "auth") {
    if (!connected) { printERR("Not connected"); return; }
    if (pinCode.length() == 0) { printERR("No PIN set"); return; }
    uint32_t pin = pinCode.toInt();
    uint8_t pinBytes[3] = { (uint8_t)(pin & 0xFF), (uint8_t)((pin >> 8) & 0xFF), (uint8_t)((pin >> 16) & 0xFF) };
    char hexStr[32]; snprintf(hexStr, sizeof(hexStr), "PIN %lu -> %02X %02X %02X", (unsigned long)pin, pinBytes[0], pinBytes[1], pinBytes[2]);
    printInfo(hexStr);
    if (bleWriteFlexible(UUID_AUTH, pinBytes, 3)) {
      authenticated = true;
      printOK("Auth sent! Use 'identify' to verify.");
    }
  }
  else if (cmd == "identify" || cmd == "id") {
    uint8_t v = 0x01;
    if (bleWriteFlexible(UUID_IDENTIFY, &v, 1)) printOK("Identify sent - motor should jog");
  }
  else if (cmd == "services" || cmd == "svc") { cmdServices(); }
  else if (cmd == "info") { cmdInfo(); }
  else if (cmd == "up") { cmdMoveUp(arg.length() > 0 ? arg.toInt() : 100); }
  else if (cmd == "down") { cmdMoveDown(arg.length() > 0 ? arg.toInt() : 100); }
  else if (cmd == "stop" || cmd == "s") { cmdStop(); }
  else if (cmd == "open") { uint8_t v=0x01; if(bleWriteFlexible(BLEUUID("0000000d-cad9-46c6-a2ea-2ca16d57b4a5"),&v,1)) printOK("Open sent"); }
  else if (cmd == "close") { uint8_t v=0x01; if(bleWriteFlexible(BLEUUID("0000000e-cad9-46c6-a2ea-2ca16d57b4a5"),&v,1)) printOK("Close sent"); }
  else if (cmd == "goto") {
    if (arg.length() == 0) { printERR("Usage: goto <0-32767>"); return; }
    cmdGoto(arg.toInt());
  }
  else if (cmd == "range") { if (arg.length() == 0) { printERR("Usage: range start|half|full"); return; } arg.toLowerCase(); cmdConfigRange(arg); }
  else if (cmd == "limit") { if (arg.length() == 0) { printERR("Usage: limit up|down"); return; } arg.toLowerCase(); cmdSetLimit(arg); }
  else if (cmd == "dir") { if (arg.length() == 0) { printERR("Usage: dir cw|ccw"); return; } arg.toLowerCase(); cmdSetDir(arg); }
  else if (cmd == "factory-reset") { cmdFactoryReset(); }
  else if (cmd == "yes-reset") {
    printInfo("Sending factory reset...");
    uint8_t v=0x01;
    if (bleWriteFlexible(UUID_FACTORY_RST, &v, 1)) {
      printOK("Factory reset sent!"); Serial.println("  Enable permit-join on coordinator now!");
    }
  }
  else if (cmd == "leave-network") { cmdLeaveNetwork(); }
  else if (cmd == "yes-leave") {
    printInfo("Sending leave network...");
    uint8_t v=0x01;
    if (bleWriteFlexible(UUID_LEAVE_NET, &v, 1)) {
      printOK("Leave network sent!"); Serial.println("  Enable permit-join now.");
    }
  }
  else if (cmd == "delivery-mode") {
    printWARN("Motor will switch OFF until button press.");
    uint8_t v=0x01;
    if (bleWriteFlexible(UUID_DELIVERY, &v, 1)) printOK("Delivery mode sent");
  }
  else if (cmd == "config") {
    if (arg.length() == 0) { printERR("Usage: config list|read|dump|set"); return; }
    int sp = arg.indexOf(' ');
    String subcmd = (sp >= 0) ? arg.substring(0, sp) : arg;
    String subarg = (sp >= 0) ? arg.substring(sp + 1) : "";
    subarg.trim();
    subcmd.toLowerCase();

    if (subcmd == "list") { cmdConfigList(); }
    else if (subcmd == "read") { if (subarg.length()==0) { printERR("Usage: config read motor|radio|type|hmi"); return; } subarg.toLowerCase(); cmdConfigRead(subarg); }
    else if (subcmd == "dump") { if (subarg.length()==0) { printERR("Usage: config dump motor|radio|type|hmi"); return; } subarg.toLowerCase(); cmdConfigDump(subarg); }
    else if (subcmd == "set") { if (subarg.length()==0) { printERR("Usage: config set <file> <key> <value>"); return; } cmdConfigSet(subarg); }
    else { printERR("Usage: config list|read|dump|set"); }
  }
  else if (cmd == "yes-config-write") { cmdConfigSetConfirm(); }
  else if (cmd == "calibration") {
    if (arg == "on") {
      Serial.println("  Run via MQTT:");
      Serial.println("  mosquitto_pub -t 'zigbee2mqtt/DEVICE/set' \\");
      Serial.println("    -m '{\"write\":{\"cluster\":258,\"options\":{},\"payload\":{\"23\":{\"value\":2,\"type\":24}}}}'");
    } else if (arg == "off") {
      Serial.println("  Run via MQTT:");
      Serial.println("  mosquitto_pub -t 'zigbee2mqtt/DEVICE/set' \\");
      Serial.println("    -m '{\"write\":{\"cluster\":258,\"options\":{},\"payload\":{\"23\":{\"value\":0,\"type\":24}}}}'");
    } else { printERR("Usage: calibration on|off"); }
  }
  else if (cmd == "write") { if (arg.length()==0) { printERR("Usage: write <prefix> <hex>"); return; } cmdWriteRaw(arg); }
  else if (cmd == "read") { if (arg.length()==0) { printERR("Usage: read <prefix>"); return; } cmdRead(arg); }
  else if (cmd == "wizard") {
    Serial.println();
    Serial.println("  ┌─ Calibration Wizard ─────────────────────");
    Serial.println("  │ 1. scan / target / pin / connect / auth");
    Serial.println("  │ 2. identify         - Verify (motor jogs)");
    Serial.println("  │ 3. config read motor - Check Application type");
    Serial.println("  │    If 'Roller', set to 'Venetian' for tilt:");
    Serial.println("  │    config set motor Application Venetian");
    Serial.println("  │ 4. range start      - Begin calibration");
    Serial.println("  │ 5. down 100 (repeat) -> limit down");
    Serial.println("  │ 6. up 100 (repeat)   -> limit up");
    Serial.println("  │ 7. dir cw/ccw       - Fix direction");
    Serial.println("  │ Motor should now be operational.");
    Serial.println("  └─────────────────────────────────────────────");
  }
  else {
    Serial.print("  Unknown: '"); Serial.print(cmd); Serial.println("'. Type 'help'.");
  }
}

// ─── Arduino Setup & Loop ────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(1000);
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

    // Handle escape sequences (arrow keys)
    if (handleEscape(c)) continue;

    if (c == '\n' || c == '\r') {
      Serial.println();
      if (inputBuffer.length() > 0) {
        historyAdd(inputBuffer);
        processCommand(inputBuffer);
        inputBuffer = "";
        historyPos = -1;
        savedInput = "";
      }
      Serial.print("somfy> ");
    } else if (c == 127 || c == 8) {
      // Backspace
      if (inputBuffer.length() > 0) {
        inputBuffer.remove(inputBuffer.length() - 1);
        Serial.print("\b \b");
      }
    } else if (c == 0x15) {
      // Ctrl-U: clear line
      for (unsigned int i = 0; i < inputBuffer.length(); i++) Serial.print("\b \b");
      inputBuffer = "";
    } else if (c == 0x01) {
      // Ctrl-A: repeat last command
      if (historyCount > 0) {
        lineReplace(String(history[(historyCount - 1) % HISTORY_SIZE]));
      }
    } else if (c >= 32 && c < 127) {
      inputBuffer += c;
      Serial.print(c);
    }
  }
  if (connected && !pClient->isConnected()) {
    connected = false; authenticated = false; pService = nullptr;
    Serial.println(); printWARN("BLE connection lost!"); Serial.print("somfy> ");
  }
  delay(10);
}
