// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  Meshtastic PSP  —  BT-Edition (Oberfläche von v24, Funk per Bluetooth)  ║
// ║                                                                          ║
// ║  Board:   ESP32 Dev Module (ESP32 mit BLE, kein S2)                      ║
// ║  Library: NimBLE-Arduino von h2zero, Version 2.x                         ║
// ║                                                                          ║
// ║  WLAN:    offener AP "meshtastic-gate"  (unverändert, PSP-Profil bleibt) ║
// ║  Web:     http://192.168.4.1                                             ║
// ║                                                                          ║
// ║  Übernommen aus v24:  Frameset-UI, CH1/CH2, PN, Nodes, Telemetrie,       ║
// ║    Favoriten, Presets, Traceroute, ACK-Anzeige, MAC-Whitelist,           ║
// ║    WLAN-Einstellungen, 3 Anzeigemodi (PSP/Netbook/Smartphone), Shutdown  ║
// ║  Neu:  Tab "BT" (Scan, Gerät wählen, PIN eingeben, Trennen)              ║
// ║        Funk läuft über BLE statt Serial2 (Mini-Protobuf, kein nanopb)    ║
// ║  Entfällt: Kamera / Foto-Versand (keine Kamera am Dev-Board)             ║
// ╚══════════════════════════════════════════════════════════════════════════╝

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <Preferences.h>
#include <NimBLEDevice.h>

// ======================= Konfiguration =======================
#define AP_SSID        "meshtastic-gate"   // offenes WLAN, kein Passwort
#define FIXED_PIN      0                   // 0 = PIN im Webinterface eingeben, sonst z.B. 123456
#define SCAN_TIME_MS   8000
#define MAX_SCAN       40
#define ALERT_LED_PIN  2                   // Onboard-LED (Security-Alarm)
#define LIST_MAX       60                  // max. Einträge in Node-/Tele-Liste (RAM schonen)
// =============================================================

// ═══════════════════════════════════════════════════════════════════════════
// DATENTYPEN (bewusst ganz oben wegen Arduino-Prototypen)
// ═══════════════════════════════════════════════════════════════════════════
#define PRESET_COUNT 9
char presetLabels[PRESET_COUNT][24];
char presetTexts[PRESET_COUNT][64];
bool presetCh1[PRESET_COUNT];
bool presetCh2[PRESET_COUNT];

#define TRACE_MAX_HOPS       8
#define TRACE_COOLDOWN_MS    30000

struct TraceState {
  bool          pending;
  bool          hasResult;
  uint32_t      targetNode;
  unsigned long sentMs;
  unsigned long resultMs;
  uint32_t      hops[TRACE_MAX_HOPS];
  int           hopCount;
  uint32_t      hopsBack[TRACE_MAX_HOPS];
  int           hopBackCount;
  bool          timedOut;
};
TraceState traceState = {false, false, 0, 0, 0, {}, 0, {}, 0, false};

#define MAX_MESSAGES   60
#define MAX_MSG_LEN    160
#define MAX_SEND_LEN   200

struct ChatMessage {
  uint32_t senderId;
  uint32_t targetId;
  char     senderName[24];
  char     text[MAX_MSG_LEN];
  char     channelName[24];
  uint8_t  channelIdx;
  uint32_t requestId;
  bool     acked;
};
ChatMessage messages[MAX_MESSAGES];
int messageCount = 0;

char channelPrimary[24]   = "Primary";
char channelSecondary[24] = "Secondary";

#define MAX_CACHED_NODES 100
struct NodeCache {
  uint32_t      num;
  char          longName[24];
  char          shortName[8];
  float         snr;
  bool          hasSnr;
  unsigned long lastSeen;     // millis() des letzten Live-Pakets (0 = nie live gehört)
  uint32_t      lastHeard;    // last_heard aus der Node-Liste des Geräts (Sekunden)
  float         battery;
  float         temperature;
  float         humidity;
  bool          hasTelemetry;
  bool          isFavorite;
};
NodeCache nodeDB[MAX_CACHED_NODES];
int nodeCacheCount = 0;
uint32_t gMaxHeard = 0;
unsigned long gRefMs = 0;

#define SEC_MAX_WHITELIST   8
#define SEC_ALERT_BLINKS    4
#define SEC_HISTORY_SIZE    5

struct SecEntry {
  char mac[18];
  bool active;
};
struct SecHistoryEntry {
  char     mac[18];
  char     ip[16];
  bool     allowed;
  uint32_t timestamp;
  bool     valid;
};
SecEntry        secWhitelist[SEC_MAX_WHITELIST];
int             secWhitelistCount  = 0;
bool            secWhitelistMode   = false;
SecHistoryEntry secHistory[SEC_HISTORY_SIZE];
int             secHistoryHead     = 0;
char      secCurrentMac[18]  = "";
char      secCurrentIp[16]   = "";
bool      secClientKnown     = false;

struct ScanDev {
  char    addr[18];
  uint8_t type;
  char    name[32];
  int16_t rssi;
  bool    mesh;
};

struct PbReader { const uint8_t* p; size_t n; size_t i; };
struct PbField  { uint32_t num; uint8_t wt; uint64_t v; const uint8_t* d; size_t len; };

struct SendJob {
  uint8_t  kind;      // 0 = Text, 1 = Traceroute
  uint32_t to;
  uint8_t  ch;
  uint32_t id;
  char     text[MAX_SEND_LEN + 1];
};

enum ConnResult { CR_OK, CR_RETRY, CR_STOP };

// ═══════════════════════════════════════════════════════════════════════════
// GLOBALS
// ═══════════════════════════════════════════════════════════════════════════
WebServer   server(80);
DNSServer   dns;
Preferences prefs;
SemaphoreHandle_t dataMux;

char    wifiSSID[33] = AP_SSID;
bool    wifiHidden   = false;
uint8_t uiMode       = 0;      // 0=PSP 1=Netbook 2=Smartphone
String  activeView   = "ch1";

static uint32_t gPacketIdCounter = 1;

// BLE
static const char* SERVICE_UUID   = "6ba1b218-15a8-461f-9fa8-5dcae273eafd";
static const char* TORADIO_UUID   = "f75c76d2-129e-4dad-a1dd-7866124401e7";
static const char* FROMRADIO_UUID = "2c55e69e-4993-11ed-b878-0242ac120002";
static const char* FROMNUM_UUID   = "ed9da18c-a800-4f66-a670-aa7547e34453";

static NimBLEClient* client = nullptr;
static NimBLERemoteCharacteristic* toRadioChr   = nullptr;
static NimBLERemoteCharacteristic* fromRadioChr = nullptr;
static NimBLERemoteCharacteristic* fromNumChr   = nullptr;

static char     bleState[96] = "Starte ...";
static uint32_t myNum = 0;

static ScanDev scanList[MAX_SCAN];
static int scanCount = 0;
static volatile bool scanning = false;

static volatile bool scanReq = false;
static volatile bool connectReq = false;
static volatile bool disconnectReq = false;
static volatile bool refreshReq = false;
static volatile bool sleepReq = false;
static volatile bool bleIdle = false;
static volatile bool shutdownPending = false;
static char    reqAddr[18];
static uint8_t reqType = 0;

static char    tgtAddr[18] = "";
static uint8_t tgtType = 0;
static bool    tgtValid = false;
static bool    autoRetry = false;

#define SENDQ 8
static SendJob sendQ[SENDQ];
static int sendHead = 0, sendCount = 0;

static volatile bool fromNumFlag = false;
static volatile bool bleDisconnected = false;

static NimBLEConnInfo* pinConn = nullptr;
static volatile bool pinWaiting = false;

// ═══════════════════════════════════════════════════════════════════════════
// LOCK (rekursiv) — Web-Task und BLE-Task teilen sich die Daten
// ═══════════════════════════════════════════════════════════════════════════
static void lock()   { xSemaphoreTakeRecursive(dataMux, portMAX_DELAY); }
static void unlock() { xSemaphoreGiveRecursive(dataMux); }
struct Lk { Lk() { lock(); } ~Lk() { unlock(); } };

static void setState(const char* s) {
  lock();
  strncpy(bleState, s, sizeof(bleState) - 1);
  bleState[sizeof(bleState) - 1] = 0;
  unlock();
  Serial.printf("[BLE] %s\n", s);
}

static void copyStr(char* dst, size_t size, const uint8_t* src, size_t len) {
  size_t n = len < size - 1 ? len : size - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}

static bool isConn() {
  return client && client->isConnected() && !bleDisconnected;
}

uint32_t nextPacketId() {
  uint32_t id = gPacketIdCounter++;
  if (id == 0) id = gPacketIdCounter++;
  return id;
}

// ═══════════════════════════════════════════════════════════════════════════
// NVS: UI-MODE / WIFI / BT-ZIEL
// ═══════════════════════════════════════════════════════════════════════════
void loadUiMode() {
  uiMode = (uint8_t)prefs.getUChar("ui_mode", 0);
  if (uiMode > 2) uiMode = 0;
}
void saveUiMode() { prefs.putUChar("ui_mode", uiMode); }

void loadWifiSettings() {
  String s = prefs.getString("wifi_ssid", AP_SSID);
  strncpy(wifiSSID, s.c_str(), 32); wifiSSID[32] = '\0';
  wifiHidden = prefs.getBool("wifi_hidden", false);
}
void saveWifiSettings() {
  prefs.putString("wifi_ssid", wifiSSID);
  prefs.putBool("wifi_hidden", wifiHidden);
}
void applyWifiSettings() {
  WiFi.softAPdisconnect(true);
  delay(200);
  IPAddress local_IP(192,168,4,1), gateway(192,168,4,1), subnet(255,255,255,0);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP(wifiSSID, nullptr, 1, wifiHidden ? 1 : 0, 1);
  esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B);
  Serial.printf("[WIFI] AP neu: SSID=%s Hidden=%d\n", wifiSSID, wifiHidden);
}

static void saveTarget(const char* addr, uint8_t type) {
  prefs.putString("bt_addr", addr);
  prefs.putUChar("bt_type", type);
}
static void clearSavedTarget() {
  prefs.remove("bt_addr");
  prefs.remove("bt_type");
}

// ═══════════════════════════════════════════════════════════════════════════
// VIEWPORT
// ═══════════════════════════════════════════════════════════════════════════
String viewportMeta() {
  switch (uiMode) {
    case 1: return "<meta name='viewport' content='width=800,initial-scale=1'>";
    case 2: return "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    default: return "";
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// FAVORITEN / PRESETS
// ═══════════════════════════════════════════════════════════════════════════
bool isSavedFavorite(uint32_t num) {
  String favStr = prefs.getString("favs", "");
  if (favStr.length() == 0) return false;
  String numStr = String(num); int pos = 0;
  while (pos < (int)favStr.length()) {
    int comma = favStr.indexOf(',', pos);
    String token = (comma == -1) ? favStr.substring(pos) : favStr.substring(pos, comma);
    if (token == numStr) return true;
    if (comma == -1) break;
    pos = comma + 1;
  }
  return false;
}
void saveFavorites() {
  String s = "";
  for (int i = 0; i < nodeCacheCount; i++) {
    if (nodeDB[i].isFavorite) { if (s.length() > 0) s += ","; s += String(nodeDB[i].num); }
  }
  prefs.putString("favs", s);
}
void savePresets() {
  for (int i = 0; i < PRESET_COUNT; i++) {
    char key[12]; snprintf(key, sizeof(key), "prs%d", i);
    prefs.putString(key, presetTexts[i]);
    char key2[12]; snprintf(key2, sizeof(key2), "prc%d", i);
    uint8_t flags = (presetCh1[i] ? 1 : 0) | (presetCh2[i] ? 2 : 0);
    prefs.putUChar(key2, flags);
  }
}
void loadPresets() {
  const char* defaults[PRESET_COUNT] = {"go","stp","wg2","arm","drm","","","",""};
  for (int i = 0; i < PRESET_COUNT; i++) {
    char key[12]; snprintf(key, sizeof(key), "prs%d", i);
    String val = prefs.getString(key, defaults[i]);
    strncpy(presetTexts[i], val.c_str(), 63); presetTexts[i][63] = '\0';
    strncpy(presetLabels[i], presetTexts[i], 23); presetLabels[i][23] = '\0';
    char key2[12]; snprintf(key2, sizeof(key2), "prc%d", i);
    uint8_t flags = prefs.getUChar(key2, 3);
    presetCh1[i] = (flags & 1) != 0; presetCh2[i] = (flags & 2) != 0;
    if (!presetCh1[i] && !presetCh2[i]) { presetCh1[i] = true; presetCh2[i] = true; }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// SECURITY (MAC-Whitelist am WLAN)
// ═══════════════════════════════════════════════════════════════════════════
void secHistorySave() {
  prefs.putInt("sec_h_head", secHistoryHead);
  for (int i = 0; i < SEC_HISTORY_SIZE; i++) {
    char km[12], ki[12], ko[12], kt[12], kv[12];
    snprintf(km, 12, "sh_mac%d", i); snprintf(ki, 12, "sh_ip%d", i);
    snprintf(ko, 12, "sh_ok%d", i);  snprintf(kt, 12, "sh_ts%d", i);
    snprintf(kv, 12, "sh_v%d", i);
    prefs.putString(km, secHistory[i].mac); prefs.putString(ki, secHistory[i].ip);
    prefs.putBool(ko, secHistory[i].allowed); prefs.putUInt(kt, secHistory[i].timestamp);
    prefs.putBool(kv, secHistory[i].valid);
  }
}
void secHistoryLoad() {
  secHistoryHead = prefs.getInt("sec_h_head", 0);
  if (secHistoryHead < 0 || secHistoryHead >= SEC_HISTORY_SIZE) secHistoryHead = 0;
  for (int i = 0; i < SEC_HISTORY_SIZE; i++) {
    char km[12], ki[12], ko[12], kt[12], kv[12];
    snprintf(km, 12, "sh_mac%d", i); snprintf(ki, 12, "sh_ip%d", i);
    snprintf(ko, 12, "sh_ok%d", i);  snprintf(kt, 12, "sh_ts%d", i);
    snprintf(kv, 12, "sh_v%d", i);
    String m  = prefs.getString(km, ""); strncpy(secHistory[i].mac, m.c_str(), 17);  secHistory[i].mac[17] = '\0';
    String ip = prefs.getString(ki, ""); strncpy(secHistory[i].ip, ip.c_str(), 15); secHistory[i].ip[15] = '\0';
    secHistory[i].allowed   = prefs.getBool(ko, false);
    secHistory[i].timestamp = prefs.getUInt(kt, 0);
    secHistory[i].valid     = prefs.getBool(kv, false);
  }
}
void secHistoryPush(const char* mac, const char* ip, bool allowed) {
  SecHistoryEntry* e = &secHistory[secHistoryHead];
  strncpy(e->mac, mac, 17); e->mac[17] = '\0';
  strncpy(e->ip, ip, 15);   e->ip[15] = '\0';
  e->allowed = allowed;
  e->timestamp = (uint32_t)(millis() / 1000);
  e->valid = true;
  secHistoryHead = (secHistoryHead + 1) % SEC_HISTORY_SIZE;
  secHistorySave();
}
void secInit() {
  secWhitelistMode  = prefs.getBool("sec_mode", false);
  secWhitelistCount = (int)prefs.getInt("sec_wl_cnt", 0);
  if (secWhitelistCount < 0 || secWhitelistCount > SEC_MAX_WHITELIST) secWhitelistCount = 0;
  for (int i = 0; i < secWhitelistCount; i++) {
    char key[12]; snprintf(key, sizeof(key), "sec_wl%d", i);
    String val = prefs.getString(key, "");
    strncpy(secWhitelist[i].mac, val.c_str(), 17); secWhitelist[i].mac[17] = '\0';
    secWhitelist[i].active = true;
  }
  memset(secHistory, 0, sizeof(secHistory));
  secHistoryLoad();
  Serial.printf("[SEC] Geladen: %d MACs, Modus=%s\n", secWhitelistCount, secWhitelistMode ? "GESPERRT" : "LERNMODUS");
}
void secSave() {
  prefs.putBool("sec_mode", secWhitelistMode);
  prefs.putInt("sec_wl_cnt", secWhitelistCount);
  for (int i = 0; i < secWhitelistCount; i++) {
    char key[12]; snprintf(key, sizeof(key), "sec_wl%d", i);
    prefs.putString(key, secWhitelist[i].mac);
  }
}
bool secMACinWhitelist(const char* mac) {
  for (int i = 0; i < secWhitelistCount; i++)
    if (secWhitelist[i].active && strcasecmp(secWhitelist[i].mac, mac) == 0) return true;
  return false;
}
bool secCheckMAC(const char* mac) {
  if (!secWhitelistMode) {
    if (!secMACinWhitelist(mac) && secWhitelistCount < SEC_MAX_WHITELIST) {
      strncpy(secWhitelist[secWhitelistCount].mac, mac, 17);
      secWhitelist[secWhitelistCount].mac[17] = '\0';
      secWhitelist[secWhitelistCount].active = true;
      secWhitelistCount++; secSave();
      Serial.printf("[SEC] MAC gelernt: %s\n", mac);
    }
    return true;
  }
  return secMACinWhitelist(mac);
}
void secAlertBlink(int n) {
  for (int i = 0; i < n; i++) {
    digitalWrite(ALERT_LED_PIN, HIGH); delay(80);
    digitalWrite(ALERT_LED_PIN, LOW);  delay(80);
  }
}
void secOnConnect(const String& mac, const String& ip) {
  strncpy(secCurrentMac, mac.c_str(), 17); secCurrentMac[17] = '\0';
  strncpy(secCurrentIp, ip.c_str(), 15);   secCurrentIp[15] = '\0';
  bool allowed = secCheckMAC(mac.c_str());
  secHistoryPush(mac.c_str(), ip.c_str(), allowed);
  if (allowed) { secClientKnown = true; Serial.printf("[SEC] Client OK: %s / %s\n", mac.c_str(), ip.c_str()); }
  else { secClientKnown = false; secAlertBlink(SEC_ALERT_BLINKS);
         Serial.printf("[SEC] Client GEBLOCKT: %s / %s\n", mac.c_str(), ip.c_str()); }
}
void secOnDisconnect(const String& mac) {
  secClientKnown = false; memset(secCurrentMac, 0, sizeof(secCurrentMac));
  Serial.printf("[SEC] Client getrennt: %s\n", mac.c_str());
}
void wifiEventCallback(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    uint8_t* m = info.wifi_ap_staconnected.mac;
    char macBuf[18];
    snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X:%02X:%02X:%02X", m[0],m[1],m[2],m[3],m[4],m[5]);
    delay(300);
    String ip = "192.168.4.2";
    wifi_sta_list_t sl;
    if (esp_wifi_ap_get_sta_list(&sl) == ESP_OK) {
      for (int i = 0; i < sl.num; i++) {
        if (memcmp(sl.sta[i].mac, m, 6) == 0) { ip = "192.168.4." + String(2 + i); break; }
      }
    }
    secOnConnect(String(macBuf), ip);
  } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
    uint8_t* m = info.wifi_ap_stadisconnected.mac;
    char macBuf[18];
    snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X:%02X:%02X:%02X", m[0],m[1],m[2],m[3],m[4],m[5]);
    secOnDisconnect(String(macBuf));
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// NODE CACHE
// ═══════════════════════════════════════════════════════════════════════════
NodeCache* findNode(uint32_t num) {
  for (int i = 0; i < nodeCacheCount; i++) if (nodeDB[i].num == num) return &nodeDB[i];
  return nullptr;
}
NodeCache* ensureNode(uint32_t num) {
  NodeCache* n = findNode(num);
  if (n) return n;
  if (nodeCacheCount >= MAX_CACHED_NODES) return nullptr;
  n = &nodeDB[nodeCacheCount++];
  memset(n, 0, sizeof(NodeCache));
  n->num = num;
  n->isFavorite = isSavedFavorite(num);
  return n;
}
const char* resolveNodeName(uint32_t nodeNum) {
  static char hb[16];
  NodeCache* n = findNode(nodeNum);
  if (n && strlen(n->longName) > 0) return n->longName;
  snprintf(hb, sizeof(hb), "0x%08X", (unsigned)nodeNum);
  return hb;
}
uint32_t nodeAgeSec(const NodeCache* n) {
  if (n->lastSeen > 0) return (millis() - n->lastSeen) / 1000;
  if (n->lastHeard > 0 && gMaxHeard >= n->lastHeard)
    return (gMaxHeard - n->lastHeard) + (uint32_t)((millis() - gRefMs) / 1000);
  return 0xFFFFFFFF;
}
String ageShort(uint32_t s) {
  if (s == 0xFFFFFFFF) return String("-");
  if (s < 60)   return String(s) + "s";
  if (s < 3600) return String(s / 60) + "m";
  return String(s / 3600) + "h";
}
void sortNodes() {
  for (int i = 0; i < nodeCacheCount - 1; i++)
    for (int j = i + 1; j < nodeCacheCount; j++) {
      bool sw;
      if (nodeDB[j].isFavorite != nodeDB[i].isFavorite) sw = nodeDB[j].isFavorite;
      else sw = nodeAgeSec(&nodeDB[j]) < nodeAgeSec(&nodeDB[i]);
      if (sw) { NodeCache t = nodeDB[i]; nodeDB[i] = nodeDB[j]; nodeDB[j] = t; }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// NACHRICHTEN-HELFER
// ═══════════════════════════════════════════════════════════════════════════
ChatMessage* newMsg() {
  int idx = messageCount % MAX_MESSAGES;
  memset(&messages[idx], 0, sizeof(ChatMessage));
  messageCount++;
  return &messages[idx];
}
void addIncomingText(uint32_t from, uint32_t to, uint32_t ch, const char* text) {
  ChatMessage* m = newMsg();
  strncpy(m->text, text, MAX_MSG_LEN - 1);
  m->senderId = from; m->targetId = to;
  strncpy(m->senderName, resolveNodeName(from), sizeof(m->senderName) - 1);
  if (to != 0xFFFFFFFF) {
    m->channelIdx = 2; strcpy(m->channelName, "PN");
  } else {
    m->channelIdx = (ch == 1) ? 1 : 0;
    strncpy(m->channelName, (ch == 1) ? channelSecondary : channelPrimary, sizeof(m->channelName) - 1);
  }
}
void addSysMessage(uint8_t channelIdx, uint32_t target, const char* text) {
  ChatMessage* m = newMsg();
  strncpy(m->text, text, MAX_MSG_LEN - 1);
  strcpy(m->senderName, "SYS");
  m->senderId = 0; m->targetId = target; m->channelIdx = channelIdx;
  if (channelIdx == 2) snprintf(m->channelName, sizeof(m->channelName), "PN>%s", resolveNodeName(target));
  else strncpy(m->channelName, (channelIdx == 1) ? channelSecondary : channelPrimary, sizeof(m->channelName) - 1);
}
void markAcked(uint32_t requestId) {
  if (requestId == 0) return;
  int total = min(messageCount, MAX_MESSAGES);
  for (int i = 0; i < total; i++) {
    if (messages[i].requestId == requestId && !messages[i].acked) {
      messages[i].acked = true;
      Serial.printf("[ACK] requestId=0x%08X zugestellt\n", (unsigned)requestId);
    }
  }
}

// Sendeauftrag in die Queue (Web-Seite). Der BLE-Task schickt ihn raus.
void sendToRadio(const char* text, uint32_t dst, uint8_t channelIdx) {
  uint8_t sysCh = (dst == 0xFFFFFFFF) ? channelIdx : 2;
  if (!isConn()) { addSysMessage(sysCh, dst, "[BT] nicht verbunden - nicht gesendet"); return; }
  if (sendCount >= SENDQ) { addSysMessage(sysCh, dst, "[BT] Sendeliste voll - bitte kurz warten"); return; }
  uint32_t reqId = nextPacketId();

  ChatMessage* m = newMsg();
  strncpy(m->text, text, MAX_MSG_LEN - 1);
  strcpy(m->senderName, "ICH");
  m->senderId = 0; m->targetId = dst;
  m->requestId = reqId; m->acked = false;
  if (dst != 0xFFFFFFFF) {
    m->channelIdx = 2;
    snprintf(m->channelName, sizeof(m->channelName), "PN>%s", resolveNodeName(dst));
  } else {
    m->channelIdx = channelIdx;
    strncpy(m->channelName, (channelIdx == 1) ? channelSecondary : channelPrimary, sizeof(m->channelName) - 1);
  }

  SendJob& j = sendQ[(sendHead + sendCount) % SENDQ];
  j.kind = 0; j.to = dst; j.ch = (dst == 0xFFFFFFFF) ? channelIdx : 0; j.id = reqId;
  strncpy(j.text, text, MAX_SEND_LEN); j.text[MAX_SEND_LEN] = 0;
  sendCount++;
  Serial.printf("[ACK] Queue: pkt.id=0x%08X dst=0x%08X\n", (unsigned)reqId, (unsigned)dst);
}

bool sendTraceroute(uint32_t targetNode) {
  if (!isConn() || sendCount >= SENDQ) return false;
  SendJob& j = sendQ[(sendHead + sendCount) % SENDQ];
  j.kind = 1; j.to = targetNode; j.ch = 0; j.id = nextPacketId(); j.text[0] = 0;
  sendCount++;
  Serial.printf("[TRACE] Queue -> 0x%08X\n", (unsigned)targetNode);
  return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// MINI-PROTOBUF
// ═══════════════════════════════════════════════════════════════════════════
static bool pbVarint(PbReader& r, uint64_t& v) {
  v = 0; int s = 0;
  while (r.i < r.n && s < 64) {
    uint8_t b = r.p[r.i++];
    v |= (uint64_t)(b & 0x7F) << s;
    if (!(b & 0x80)) return true;
    s += 7;
  }
  return false;
}
static bool pbNext(PbReader& r, PbField& f) {
  if (r.i >= r.n) return false;
  uint64_t key;
  if (!pbVarint(r, key)) return false;
  f.num = (uint32_t)(key >> 3);
  f.wt = key & 7;
  f.v = 0; f.d = nullptr; f.len = 0;
  switch (f.wt) {
    case 0: return pbVarint(r, f.v);
    case 1:
      if (r.i + 8 > r.n) return false;
      memcpy(&f.v, r.p + r.i, 8); r.i += 8; return true;
    case 2: {
      uint64_t l;
      if (!pbVarint(r, l) || r.i + l > r.n) return false;
      f.d = r.p + r.i; f.len = (size_t)l; r.i += (size_t)l; return true;
    }
    case 5: {
      if (r.i + 4 > r.n) return false;
      uint32_t x; memcpy(&x, r.p + r.i, 4); r.i += 4; f.v = x; return true;
    }
    default: return false;
  }
}
static float asFloat(uint64_t v) {
  uint32_t x = (uint32_t)v; float f; memcpy(&f, &x, 4); return f;
}
static size_t putVarint(uint8_t* b, uint32_t v) {
  size_t i = 0;
  while (v >= 0x80) { b[i++] = (v & 0x7F) | 0x80; v >>= 7; }
  b[i++] = (uint8_t)v;
  return i;
}

// RouteDiscovery (Traceroute-Antwort)
bool decodeRouteDiscovery(const uint8_t* payload, size_t payloadLen,
                          uint32_t* route, int* routeCount,
                          uint32_t* routeBack, int* routeBackCount) {
  *routeCount = 0; *routeBackCount = 0;
  if (!payload || payloadLen == 0) return true;
  size_t pos = 0;
  while (pos < payloadLen) {
    uint32_t tag = 0; int shift = 0;
    while (pos < payloadLen) {
      uint8_t b = payload[pos++];
      tag |= (uint32_t)(b & 0x7F) << shift; shift += 7;
      if (!(b & 0x80)) break;
    }
    uint8_t fieldNum = (uint8_t)(tag >> 3);
    uint8_t wireType = (uint8_t)(tag & 0x07);
    if (wireType == 5) {
      if (pos + 4 <= payloadLen) {
        uint32_t val = (uint32_t)payload[pos] | ((uint32_t)payload[pos+1] << 8) |
                       ((uint32_t)payload[pos+2] << 16) | ((uint32_t)payload[pos+3] << 24);
        pos += 4;
        if (fieldNum == 1 && *routeCount < TRACE_MAX_HOPS)           route[(*routeCount)++] = val;
        else if (fieldNum == 3 && *routeBackCount < TRACE_MAX_HOPS)  routeBack[(*routeBackCount)++] = val;
      } else pos = payloadLen;
    } else if (wireType == 0) {
      uint32_t val = 0; shift = 0;
      while (pos < payloadLen) { uint8_t b = payload[pos++]; val |= (uint32_t)(b & 0x7F) << shift; shift += 7; if (!(b & 0x80)) break; }
      if (fieldNum == 1 && *routeCount < TRACE_MAX_HOPS)           route[(*routeCount)++] = val;
      else if (fieldNum == 3 && *routeBackCount < TRACE_MAX_HOPS)  routeBack[(*routeBackCount)++] = val;
    } else if (wireType == 2) {
      uint32_t len = 0; shift = 0;
      while (pos < payloadLen) { uint8_t b = payload[pos++]; len |= (uint32_t)(b & 0x7F) << shift; shift += 7; if (!(b & 0x80)) break; }
      size_t end = pos + len; if (end > payloadLen) end = payloadLen;
      if (fieldNum == 1 || fieldNum == 3) {
        while (pos + 4 <= end) {
          uint32_t val = (uint32_t)payload[pos] | ((uint32_t)payload[pos+1] << 8) |
                         ((uint32_t)payload[pos+2] << 16) | ((uint32_t)payload[pos+3] << 24);
          pos += 4;
          if (fieldNum == 1 && *routeCount < TRACE_MAX_HOPS)           route[(*routeCount)++] = val;
          else if (fieldNum == 3 && *routeBackCount < TRACE_MAX_HOPS)  routeBack[(*routeBackCount)++] = val;
        }
        pos = end;
      } else pos = end;
    } else break;
  }
  return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// FROMRADIO PARSEN
// ═══════════════════════════════════════════════════════════════════════════
static void parseUserInto(NodeCache* nd, const uint8_t* d, size_t n) {
  PbReader r = {d, n, 0}; PbField f;
  while (pbNext(r, f)) {
    if (f.wt != 2) continue;
    if (f.num == 2) copyStr(nd->longName, sizeof(nd->longName), f.d, f.len);
    else if (f.num == 3) copyStr(nd->shortName, sizeof(nd->shortName), f.d, f.len);
  }
}

static void parseNodeInfo(const uint8_t* d, size_t n) {
  uint32_t num = 0;
  { PbReader r2 = {d, n, 0}; PbField g;
    while (pbNext(r2, g)) if (g.num == 1 && g.wt == 0) num = (uint32_t)g.v; }
  if (!num) return;
  NodeCache* nd = ensureNode(num);
  if (!nd) return;
  PbReader r = {d, n, 0}; PbField f;
  while (pbNext(r, f)) {
    if (f.num == 2 && f.wt == 2) parseUserInto(nd, f.d, f.len);
    else if (f.num == 4 && f.wt == 5) { float s = asFloat(f.v); if (s != 0) { nd->snr = s; nd->hasSnr = true; } }
    else if (f.num == 5 && f.wt == 5) {
      nd->lastHeard = (uint32_t)f.v;
      if (nd->lastHeard > gMaxHeard) { gMaxHeard = nd->lastHeard; gRefMs = millis(); }
    }
    else if (f.num == 6 && f.wt == 2) {
      PbReader m = {f.d, f.len, 0}; PbField g;
      while (pbNext(m, g)) if (g.num == 1 && g.wt == 0) { nd->battery = (float)g.v; nd->hasTelemetry = true; }
    }
  }
}

static void parsePacket(const uint8_t* d, size_t n) {
  PbReader r = {d, n, 0}; PbField f;
  uint32_t from = 0, to = 0, ch = 0;
  const uint8_t* dec = nullptr; size_t decLen = 0;
  bool hasSnr = false; float snr = 0;
  while (pbNext(r, f)) {
    if (f.num == 1 && f.wt == 5) from = (uint32_t)f.v;
    else if (f.num == 2 && f.wt == 5) to = (uint32_t)f.v;
    else if (f.num == 3 && f.wt == 0) ch = (uint32_t)f.v;
    else if (f.num == 4 && f.wt == 2) { dec = f.d; decLen = f.len; }
    else if (f.num == 8 && f.wt == 5) { snr = asFloat(f.v); hasSnr = true; }
  }

  if (from != 0 && from != myNum) {
    NodeCache* sn = ensureNode(from);
    if (sn) {
      sn->lastSeen = millis();
      if (hasSnr && snr != 0) { sn->snr = snr; sn->hasSnr = true; }
    }
  }
  if (!dec) return;

  uint32_t port = 0, reqId = 0;
  const uint8_t* pl = nullptr; size_t plLen = 0;
  PbReader dr = {dec, decLen, 0}; PbField g;
  while (pbNext(dr, g)) {
    if (g.num == 1 && g.wt == 0) port = (uint32_t)g.v;
    else if (g.num == 2 && g.wt == 2) { pl = g.d; plLen = g.len; }
    else if (g.num == 6 && g.wt == 5) reqId = (uint32_t)g.v;
  }

  // ---- TEXT ----
  if (port == 1 && pl) {
    char tb[MAX_MSG_LEN];
    size_t tl = plLen < (size_t)(MAX_MSG_LEN - 1) ? plLen : (size_t)(MAX_MSG_LEN - 1);
    memcpy(tb, pl, tl); tb[tl] = '\0';
    // Foto-Protokoll anderer Geräte ignorieren (hier nicht unterstützt)
    if (strncmp(tb, "IMG:", 4) == 0 || strncmp(tb, "IMGDONE:", 8) == 0 || strncmp(tb, "IMGREQ:", 7) == 0) return;
    if (tl > 0) {
      addIncomingText(from, to, ch, tb);
      Serial.printf("[MSG] von 0x%08X: %s\n", (unsigned)from, tb);
    }
    return;
  }

  // ---- NODEINFO (User) ----
  if (port == 4 && pl && from != 0) {
    NodeCache* nd = ensureNode(from);
    if (nd) parseUserInto(nd, pl, plLen);
    return;
  }

  // ---- ROUTING (ACK) ----
  if (port == 5) {
    if (reqId != 0) {
      uint32_t err = 0; bool isRoute = false;
      if (pl) {
        PbReader rr = {pl, plLen, 0}; PbField h;
        while (pbNext(rr, h)) {
          if (h.num == 3 && h.wt == 0) err = (uint32_t)h.v;
          else if (h.num == 1 || h.num == 2) isRoute = true;
        }
      }
      Serial.printf("[ACK] ROUTING von 0x%08X req=0x%08X err=%u\n", (unsigned)from, (unsigned)reqId, (unsigned)err);
      if (!isRoute && err == 0) markAcked(reqId);
    }
    return;
  }

  // ---- TELEMETRIE ----
  if (port == 67 && pl && from != 0) {
    NodeCache* nd = ensureNode(from);
    if (!nd) return;
    PbReader tr = {pl, plLen, 0}; PbField t;
    while (pbNext(tr, t)) {
      if (t.num == 2 && t.wt == 2) {
        PbReader m = {t.d, t.len, 0}; PbField q;
        while (pbNext(m, q)) if (q.num == 1 && q.wt == 0) { nd->battery = (float)q.v; nd->hasTelemetry = true; }
      } else if (t.num == 3 && t.wt == 2) {
        PbReader m = {t.d, t.len, 0}; PbField q;
        while (pbNext(m, q)) {
          if (q.num == 1 && q.wt == 5) { nd->temperature = asFloat(q.v); nd->hasTelemetry = true; }
          else if (q.num == 2 && q.wt == 5) { nd->humidity = asFloat(q.v); nd->hasTelemetry = true; }
        }
      }
    }
    return;
  }

  // ---- TRACEROUTE ----
  if (port == 70) {
    Serial.printf("[TRACE] Antwort von 0x%08X, %d Bytes\n", (unsigned)from, (int)plLen);
    if (traceState.pending && from == traceState.targetNode) {
      uint32_t route[TRACE_MAX_HOPS], routeBack[TRACE_MAX_HOPS];
      int rc = 0, rbc = 0;
      decodeRouteDiscovery(pl, plLen, route, &rc, routeBack, &rbc);
      traceState.pending = false; traceState.hasResult = true; traceState.timedOut = false;
      traceState.resultMs = millis(); traceState.hopCount = rc; traceState.hopBackCount = rbc;
      for (int i = 0; i < rc; i++)  traceState.hops[i] = route[i];
      for (int i = 0; i < rbc; i++) traceState.hopsBack[i] = routeBack[i];
      Serial.printf("[TRACE] Route: %d hin, %d zurueck\n", rc, rbc);
    }
    return;
  }
}

static void handleFromRadio(const uint8_t* d, size_t n) {
  PbReader r = {d, n, 0}; PbField f;
  while (pbNext(r, f)) {
    if (f.wt == 2) {
      switch (f.num) {
        case 2: parsePacket(f.d, f.len); break;
        case 3: {  // MyNodeInfo
          PbReader m = {f.d, f.len, 0}; PbField g;
          while (pbNext(m, g)) if (g.num == 1 && g.wt == 0) myNum = (uint32_t)g.v;
          break;
        }
        case 4: parseNodeInfo(f.d, f.len); break;
        case 10: { // Channel
          PbReader c = {f.d, f.len, 0}; PbField g;
          uint32_t idx = 0; char name[24] = ""; bool hasName = false;
          while (pbNext(c, g)) {
            if (g.num == 1 && g.wt == 0) idx = (uint32_t)g.v;
            else if (g.num == 2 && g.wt == 2) {
              PbReader s = {g.d, g.len, 0}; PbField h;
              while (pbNext(s, h)) if (h.num == 3 && h.wt == 2) { copyStr(name, sizeof(name), h.d, h.len); hasName = (name[0] != 0); }
            }
          }
          if (hasName) {
            if (idx == 0) { strncpy(channelPrimary, name, sizeof(channelPrimary) - 1);   channelPrimary[sizeof(channelPrimary) - 1] = 0; }
            if (idx == 1) { strncpy(channelSecondary, name, sizeof(channelSecondary) - 1); channelSecondary[sizeof(channelSecondary) - 1] = 0; }
          }
          break;
        }
        default: break;
      }
    } else if (f.num == 7 && f.wt == 0) {   // config_complete_id
      setState("Verbunden");
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// BLE
// ═══════════════════════════════════════════════════════════════════════════
class ClientCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient* c, int reason) override {
    if (c == client) bleDisconnected = true;
    pinWaiting = false;
  }
  void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
#if FIXED_PIN > 0
    NimBLEDevice::injectPassKey(connInfo, FIXED_PIN);
#else
    lock();
    if (pinConn) { delete pinConn; pinConn = nullptr; }
    pinConn = new NimBLEConnInfo(connInfo);
    pinWaiting = true;
    unlock();
    setState("PIN erforderlich - bitte im BT-Tab eingeben");
#endif
  }
  void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pin) override {
    NimBLEDevice::injectConfirmPasskey(connInfo, true);
  }
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    pinWaiting = false;
    if (!connInfo.isEncrypted()) setState("Pairing fehlgeschlagen (falsche PIN?)");
  }
};
static ClientCB clientCB;

static void notifyCB(NimBLERemoteCharacteristic*, uint8_t*, size_t, bool) {
  fromNumFlag = true;
}

static bool requestConfig() {
  uint8_t buf[16]; size_t n = 0;
  buf[n++] = 0x18;                                   // ToRadio.want_config_id
  n += putVarint(buf + n, 69420 + (esp_random() & 0xFF));
  return toRadioChr->writeValue(buf, n, true);
}

// Generisches MeshPacket senden (portnum, payload, want_ack, want_response, hop_limit)
static bool sendMeshPacket(uint32_t to, uint8_t ch, uint32_t id, uint8_t port,
                           const uint8_t* pl, size_t plen,
                           bool wantAck, bool wantResp, uint8_t hop) {
  if (plen > MAX_SEND_LEN) return false;
  uint8_t data[260]; size_t dl = 0;
  data[dl++] = 0x08; dl += putVarint(data + dl, port);
  if (plen > 0) { data[dl++] = 0x12; dl += putVarint(data + dl, plen); memcpy(data + dl, pl, plen); dl += plen; }
  if (wantResp) { data[dl++] = 0x18; data[dl++] = 0x01; }

  uint8_t pkt[300]; size_t p = 0;
  pkt[p++] = 0x15; memcpy(pkt + p, &to, 4); p += 4;
  if (ch > 0) { pkt[p++] = 0x18; pkt[p++] = ch; }
  pkt[p++] = 0x22; p += putVarint(pkt + p, dl); memcpy(pkt + p, data, dl); p += dl;
  pkt[p++] = 0x35; memcpy(pkt + p, &id, 4); p += 4;
  if (hop) { pkt[p++] = 0x48; pkt[p++] = hop; }
  if (wantAck) { pkt[p++] = 0x50; pkt[p++] = 0x01; }

  uint8_t out[330]; size_t ol = 0;
  out[ol++] = 0x0A; ol += putVarint(out + ol, p);
  memcpy(out + ol, pkt, p); ol += p;
  return toRadioChr->writeValue(out, ol, true);
}

static void drainFromRadio() {
  for (int i = 0; i < 300; i++) {
    if (!client || !client->isConnected()) break;
    NimBLEAttValue v = fromRadioChr->readValue();
    if (v.size() == 0) break;
    lock();
    handleFromRadio(v.data(), v.size());
    unlock();
  }
}

static void cleanupClient() {
  toRadioChr = nullptr; fromRadioChr = nullptr; fromNumChr = nullptr;
  pinWaiting = false;
  if (client) {
    if (client->isConnected()) client->disconnect();
    vTaskDelay(pdMS_TO_TICKS(250));
    NimBLEDevice::deleteClient(client);
    client = nullptr;
  }
  bleDisconnected = false;
}

static void doScan() {
  scanning = true;
  setState("Scanne nach Bluetooth-Geraeten ...");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(160);
  scan->setWindow(80);
  scan->clearResults();
  NimBLEScanResults res = scan->getResults(SCAN_TIME_MS, false);

  int total = res.getCount();
  Serial.printf("[SCAN] %d Geraete im Ergebnis\n", total);

  lock();
  scanCount = 0;
  for (int i = 0; i < total && scanCount < MAX_SCAN; i++) {
    const NimBLEAdvertisedDevice* dev = res.getDevice(i);
    if (!dev) continue;
    ScanDev& s = scanList[scanCount++];
    NimBLEAddress a = dev->getAddress();
    strncpy(s.addr, a.toString().c_str(), sizeof(s.addr) - 1);
    s.addr[sizeof(s.addr) - 1] = 0;
    s.type = a.getType();
    std::string nm = dev->getName();
    strncpy(s.name, nm.c_str(), sizeof(s.name) - 1);
    s.name[sizeof(s.name) - 1] = 0;
    s.rssi = dev->getRSSI();
    s.mesh = dev->isAdvertisingService(NimBLEUUID(SERVICE_UUID)) || nm.rfind("Meshtastic", 0) == 0;
  }
  int found = scanCount;
  unlock();
  scan->clearResults();
  scanning = false;

  char buf[80];
  if (found > 0) snprintf(buf, sizeof(buf), "Scan fertig: %d Geraete - Geraet auswaehlen", found);
  else snprintf(buf, sizeof(buf), "Scan fertig: keine Geraete gefunden - erneut scannen");
  setState(buf);
}

static ConnResult connectTo(const char* addrStr, uint8_t type) {
  NimBLEAddress addr(std::string(addrStr), type);

  setState("Verbinde ...");
  client = NimBLEDevice::createClient();
  client->setClientCallbacks(&clientCB, false);
  client->setConnectTimeout(10000);
  if (!client->connect(addr)) {
    setState("Verbindung fehlgeschlagen - Geraet in Reichweite? Neuer Versuch ...");
    cleanupClient();
    return CR_RETRY;
  }

  setState("Pairing ...");
  if (!client->secureConnection()) {
    setState("Pairing fehlgeschlagen (PIN falsch oder zu spaet) - bitte erneut verbinden");
    cleanupClient();
    NimBLEDevice::deleteBond(addr);
    return CR_STOP;
  }

  NimBLERemoteService* svc = client->getService(SERVICE_UUID);
  if (!svc) {
    setState("Kein Meshtastic-Geraet (Service nicht gefunden) - anderes Geraet waehlen");
    cleanupClient();
    return CR_STOP;
  }
  toRadioChr   = svc->getCharacteristic(TORADIO_UUID);
  fromRadioChr = svc->getCharacteristic(FROMRADIO_UUID);
  fromNumChr   = svc->getCharacteristic(FROMNUM_UUID);
  if (!toRadioChr || !fromRadioChr || !fromNumChr) {
    setState("Meshtastic-Characteristics fehlen");
    cleanupClient();
    return CR_STOP;
  }
  if (fromNumChr->canNotify()) fromNumChr->subscribe(true, notifyCB);

  setState("Verbunden - lade Nodes ...");
  if (!requestConfig()) {
    setState("Config-Anfrage fehlgeschlagen");
    cleanupClient();
    return CR_RETRY;
  }
  saveTarget(addrStr, type);
  fromNumFlag = true;
  return CR_OK;
}

static void bleTask(void*) {
  uint32_t lastPoll = 0;
  uint32_t lastTry = 0;

  for (;;) {
    if (sleepReq) {
      if (client) cleanupClient();
      bleIdle = true;
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    bool connected = client && client->isConnected() && !bleDisconnected;

    if (connected && (scanReq || connectReq || disconnectReq)) {
      cleanupClient();
      connected = false;
    }

    if (connected) {
      if (refreshReq) {
        refreshReq = false;
        requestConfig();
        fromNumFlag = true;
      }

      // ein Sendeauftrag pro Durchlauf
      SendJob j; bool have = false;
      lock();
      if (sendCount > 0) { j = sendQ[sendHead]; sendHead = (sendHead + 1) % SENDQ; sendCount--; have = true; }
      unlock();
      if (have) {
        bool ok;
        if (j.kind == 1) ok = sendMeshPacket(j.to, 0, j.id, 70, nullptr, 0, false, true, 7);
        else ok = sendMeshPacket(j.to, j.ch, j.id, 1, (const uint8_t*)j.text, strlen(j.text), true, false, 0);
        if (!ok) setState("Senden fehlgeschlagen");
        else Serial.printf("[TX] kind=%d id=0x%08X\n", j.kind, (unsigned)j.id);
      }

      if (fromNumFlag || millis() - lastPoll > 1500) {
        fromNumFlag = false;
        lastPoll = millis();
        drainFromRadio();
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // ---- nicht verbunden ----
    if (client) {
      cleanupClient();
      setState(autoRetry ? "Verbindung verloren - verbinde neu ..." : "Getrennt");
    } else if (bleDisconnected) {
      bleDisconnected = false;
    }
    lock(); sendCount = 0; unlock();
    refreshReq = false;

    if (disconnectReq) {
      disconnectReq = false;
      lock();
      tgtValid = false; tgtAddr[0] = 0; autoRetry = false;
      unlock();
      clearSavedTarget();
      setState("Getrennt - Scan starten, um ein Geraet auszuwaehlen");
    }

    if (scanReq) {
      scanReq = false;
      autoRetry = false;
      doScan();
      continue;
    }

    if (connectReq) {
      lock();
      strncpy(tgtAddr, reqAddr, sizeof(tgtAddr) - 1);
      tgtAddr[sizeof(tgtAddr) - 1] = 0;
      tgtType = reqType;
      tgtValid = true;
      autoRetry = true;
      connectReq = false;
      unlock();
      lastTry = 0;
    }

    bool due = tgtValid && autoRetry && (lastTry == 0 || millis() - lastTry > 3000);
    if (due) {
      ConnResult r = connectTo(tgtAddr, tgtType);
      lastTry = (r == CR_OK) ? 0 : millis();
      if (r == CR_STOP) autoRetry = false;
      continue;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// HTML HELFER
// ═══════════════════════════════════════════════════════════════════════════
String escHtml(const char* s) {
  String out = "";
  while (s && *s) {
    if (*s == '<') out += "&lt;"; else if (*s == '>') out += "&gt;";
    else if (*s == '&') out += "&amp;"; else out += (char)*s;
    s++;
  }
  return out;
}

String refreshBar(bool refreshActive, String selfUrlBase) {
  bool c = isConn();
  String bar = "<div class='rbar'>";
  bar += "<a href='" + selfUrlBase + "refresh=" + String(refreshActive ? "0" : "1") + "'>";
  bar += String(refreshActive ? "[X] Auto 5s" : "[ ] Manuell");
  bar += "</a> &nbsp;<span style='color:" + String(c ? "#00ff66" : "#ff4444") + "'>BT:" + String(c ? "OK" : "--") + "</span></div>";
  return bar;
}

void redir(const String& path) {
  server.sendHeader("Location", path);
  server.send(303);
}

// ═══════════════════════════════════════════════════════════════════════════
// CSS je Modus
// ═══════════════════════════════════════════════════════════════════════════
String frameCssMode(uint8_t mode) {
  const char* bodyFs = "11px"; const char* bodyPad = "2px"; const char* inputFs = "12px";
  const char* btnPad = "4px 8px"; const char* msgPad = "3px 3px 3px 6px";
  const char* nodeRowPad = "5px 0"; const char* rbarPad = "4px 3px"; const char* rbarFs = "10px";
  const char* metaFs = "9px"; const char* sndrFs = "9px";

  if (mode == 1) {
    bodyFs = "13px"; bodyPad = "4px"; inputFs = "14px";
    btnPad = "6px 12px"; msgPad = "5px 5px 5px 8px"; nodeRowPad = "7px 0";
    rbarPad = "5px 4px"; rbarFs = "12px"; metaFs = "11px"; sndrFs = "11px";
  } else if (mode == 2) {
    bodyFs = "14px"; bodyPad = "4px"; inputFs = "16px";
    btnPad = "10px 16px"; msgPad = "8px 8px 8px 10px"; nodeRowPad = "10px 0";
    rbarPad = "8px 6px"; rbarFs = "13px"; metaFs = "12px"; sndrFs = "12px";
  }

  String css = "<style>";
  css += String("html,body{background:#000;color:#ccc;font-family:monospace;margin:0;padding:0;")
       + "font-size:" + bodyFs + ";height:auto;overflow-y:auto}";
  css += String("body{padding:") + bodyPad + "}";
  css += "a{color:#00cccc;text-decoration:none}";
  css += "hr{border:0;border-top:1px solid #1a1a1a;margin:4px 0}";
  css += String(".rbar{position:sticky;top:0;background:#0a0a0a;border-bottom:1px solid #1a1a1a;")
       + "padding:" + rbarPad + ";font-size:" + rbarFs + ";color:#888;z-index:99}";
  css += ".rbar a{color:#00aa88}";

  css += ".preset-box{background:#111;border-bottom:1px solid #2a2a2a;padding:2px 2px;margin:0 0 3px 0;display:flex;gap:2px;align-items:center}";
  css += ".preset-toggle{background:#0a0a14;color:#336699;border:1px solid #1a2a44;padding:3px 4px;font-family:monospace;font-size:9px;font-weight:bold;text-align:center;text-decoration:none;min-width:18px;white-space:nowrap}";
  css += ".preset-toggle.on{background:#001a00;color:#00ff66;border-color:#006622}";
  css += ".preset-btn{flex:1;background:#0d1a00;color:#aaff44;border:1px solid #2a4400;padding:3px 1px;font-family:monospace;font-size:9px;font-weight:bold;text-align:center;text-decoration:none;overflow:hidden;white-space:nowrap;text-overflow:ellipsis;min-width:0}";
  css += ".preset-btn:active{background:#1a3300;border-color:#88cc00}";

  css += String(".msg{margin-bottom:4px;padding:") + msgPad + ";border-left:2px solid #00ff66}";
  css += String(".msg-pn{margin-bottom:4px;padding:") + msgPad + ";border-left:2px solid #ff44ff;background:#100010}";
  css += String(".msg-me{margin-bottom:4px;padding:") + msgPad + ";border-left:2px solid #ffaa00;background:#0a0800}";
  css += String(".msg-sys{margin-bottom:4px;padding:") + msgPad + ";border-left:2px solid #555555;background:#0a0a0a}";
  css += String(".sndr{font-size:") + sndrFs + ";color:#00cccc}";
  css += String(".sndr-pn{font-size:") + sndrFs + ";color:#ff88ff}";
  css += String(".sndr-me{font-size:") + sndrFs + ";color:#ffcc44}";
  css += String(".sndr-sys{font-size:") + sndrFs + ";color:#555555}";
  css += String(".meta{color:#444;font-size:") + metaFs + "}";
  css += ".txt{color:#ddd;margin-top:1px}";
  css += ".ack-ok{color:#00ff66;font-size:9px;margin-left:4px;font-weight:bold}";
  css += ".ack-pend{color:#555555;font-size:9px;margin-left:4px}";
  css += ".empty{color:#333;font-size:10px;padding:6px}";

  css += String("input[type=text]{background:#000;color:#fff;border:1px solid #444;padding:4px;font-family:monospace;font-size:") + inputFs + "}";
  css += String("input[type=submit]{background:#001800;color:#00ff00;border:1px solid #00ff00;padding:") + btnPad + ";font-family:monospace;font-size:11px}";
  css += String(".btn-red{background:#180000;color:#ff4444;border:1px solid #ff4444;padding:") + btnPad + ";font-family:monospace;font-size:11px}";
  css += ".btn-pn{background:#180018;color:#ff88ff;border:1px solid #ff44ff;padding:5px 8px;font-family:monospace;font-size:11px;display:block;text-align:center;margin-top:5px;width:100%;box-sizing:border-box}";
  css += ".btn-trace{background:#001818;color:#00ffcc;border:1px solid #00aa88;padding:5px 8px;font-family:monospace;font-size:11px;display:block;text-align:center;margin-top:5px;width:100%;box-sizing:border-box;text-decoration:none}";

  css += String(".node-row{border-bottom:1px solid #111;padding:") + nodeRowPad + "}";
  css += ".node-name{color:#00ff66}";
  css += String(".node-id{color:#555;font-size:") + metaFs + "}";
  css += ".node-snr{color:#ffaa00;font-size:9px}";
  css += ".fav-btn{font-size:12px;padding:2px 5px;background:#222;border:1px solid #444;color:#ffff00;text-decoration:none;float:right}";

  css += ".tele-box{background:#050505;border:1px solid #1a1a1a;padding:5px;margin-bottom:5px}";
  css += String(".tele-label{color:#555;font-size:") + metaFs + "}";
  css += ".tele-val{color:#00ff66}";

  css += ".pn-tile{display:block;background:#0c060c;border:1px solid #441144;padding:8px;margin-bottom:5px;text-decoration:none;box-sizing:border-box}";
  css += ".tile-name{color:#ff88ff;font-size:12px;font-weight:bold}";
  css += String(".tile-preview{color:#775577;font-size:") + metaFs + ";display:block;margin-top:2px}";

  css += ".send-row{margin-top:6px;padding-top:4px;border-top:1px solid #1a1a1a}";

  css += ".set-section{margin-bottom:10px}";
  css += ".set-label{color:#888;font-size:9px;margin-bottom:2px}";
  css += ".set-row{display:flex;align-items:center;gap:4px;margin-bottom:4px}";
  css += ".set-idx{color:#445544;font-size:9px;min-width:14px;text-align:right}";
  css += ".set-inp{flex:1;background:#050a05;color:#aaff44;border:1px solid #2a3a1a;padding:4px;font-family:monospace;font-size:11px}";
  css += ".set-inp-wifi{flex:1;background:#050505;color:#aaccff;border:1px solid #1a2a3a;padding:4px;font-family:monospace;font-size:11px}";
  css += ".set-save{background:#001800;color:#00ff00;border:1px solid #00aa44;padding:3px 7px;font-family:monospace;font-size:10px}";
  css += ".set-save-wifi{background:#000a18;color:#44aaff;border:1px solid #224488;padding:3px 7px;font-family:monospace;font-size:10px}";
  css += "input[type=checkbox]{width:12px;height:12px;margin:0;accent-color:#00ff66}";

  css += ".sec-box{background:#0a0500;border:1px solid #2a1a00;padding:5px;margin-bottom:6px}";
  css += ".sec-label{color:#886644;font-size:9px}";
  css += ".sec-mac{font-size:10px;font-family:monospace;color:#ffcc88}";
  css += ".sec-ok{color:#00ff66;font-size:9px}";
  css += ".sec-blocked{color:#ff4444;font-size:9px}";
  css += ".sec-btn{background:#1a0a00;color:#ffaa44;border:1px solid #664422;padding:3px 7px;font-family:monospace;font-size:10px;text-decoration:none}";
  css += ".sec-btn-red{background:#1a0000;color:#ff4444;border:1px solid #882222;padding:3px 7px;font-family:monospace;font-size:10px;text-decoration:none}";
  css += ".sec-mode-learn{background:#001a1a;color:#00ccff;border:1px solid #006688;padding:5px 8px;font-family:monospace;font-size:10px;display:block;text-align:center;margin-bottom:4px;text-decoration:none}";
  css += ".sec-mode-lock{background:#1a0500;color:#ff6600;border:1px solid #883300;padding:5px 8px;font-family:monospace;font-size:10px;display:block;text-align:center;margin-bottom:4px;text-decoration:none}";
  css += ".sec-hist-ok{color:#00ff66;font-size:9px}";
  css += ".sec-hist-bad{color:#ff4444;font-size:9px}";

  css += ".trace-box{background:#00100a;border:1px solid #004433;padding:6px;margin-bottom:6px}";
  css += ".trace-hop{padding:3px 0;border-bottom:1px solid #002a1a;display:flex;align-items:center}";
  css += ".trace-hop:last-child{border-bottom:none}";
  css += ".trace-idx{color:#006644;font-size:9px;min-width:22px;font-family:monospace}";
  css += ".trace-arrow{color:#004433;font-size:10px;margin:0 4px}";
  css += ".trace-name{color:#00ffcc;font-size:10px}";
  css += ".trace-id{color:#004433;font-size:9px;margin-left:4px;font-family:monospace}";
  css += ".trace-dest{color:#00ff66;font-size:10px;font-weight:bold}";
  css += ".trace-src{color:#ffcc00;font-size:9px}";
  css += ".trace-pending{color:#ffaa00;font-size:10px;padding:6px 0}";
  css += ".trace-timeout{color:#ff4444;font-size:10px;padding:6px 0}";
  css += ".trace-direct{color:#00ff66;font-size:10px;padding:4px 0}";
  css += ".trace-cooldown{color:#555;font-size:9px;margin-top:4px}";

  css += ".ui-mode-btn{display:inline-block;background:#111;color:#445544;border:1px solid #2a3a2a;padding:6px 10px;font-family:monospace;font-size:11px;text-decoration:none;margin-right:4px;margin-bottom:4px}";
  css += ".ui-mode-btn.on{background:#001800;color:#00ff66;border-color:#00aa44}";

  // Bluetooth-Tab
  css += ".bt-box{background:#00080f;border:1px solid #0a2a44;padding:6px;margin-bottom:6px}";
  css += ".bt-btn{display:block;background:#001520;color:#00ccff;border:1px solid #0077aa;padding:7px 8px;font-family:monospace;font-size:11px;text-align:center;text-decoration:none;margin-top:5px}";
  css += ".bt-btn-red{display:block;background:#1a0000;color:#ff4444;border:1px solid #882222;padding:7px 8px;font-family:monospace;font-size:11px;text-align:center;text-decoration:none;margin-top:5px}";
  css += ".bt-dev{display:block;background:#050a10;border:1px solid #1a2a3a;padding:6px;margin-bottom:4px;text-decoration:none}";
  css += ".bt-dev-mesh{border-color:#00aa44;background:#001a08}";
  css += ".bt-name{color:#00ff66;font-size:12px}";
  css += ".bt-name-other{color:#88aacc;font-size:12px}";
  css += String(".bt-info{color:#557799;font-size:") + metaFs + "}";

  css += "</style>";
  return css;
}
String frameCss() { return frameCssMode(uiMode); }

String pageStart(int refreshSec, const String& url) {
  String h = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  h += viewportMeta();
  if (refreshSec > 0) h += "<meta http-equiv='refresh' content='" + String(refreshSec) + ";url=" + url + "'>";
  h += frameCss();
  h += "</head><body>";
  return h;
}

// ═══════════════════════════════════════════════════════════════════════════
// HANDLER: UI-MODE / SHELL / TABS
// ═══════════════════════════════════════════════════════════════════════════
void handleUiMode() {
  if (server.hasArg("v")) {
    uint8_t m = (uint8_t)server.arg("v").toInt();
    if (m <= 2) { uiMode = m; saveUiMode(); }
  }
  redir("/settings?subtab=display&saved=1");
}

void handleShell() {
  String frameUrl = "/chat?tab=0";
  activeView = "ch1";
  if (server.hasArg("view")) {
    String v = server.arg("view"); activeView = v;
    if (v == "ch1")        frameUrl = "/chat?tab=0";
    else if (v == "ch2")   frameUrl = "/chat?tab=1";
    else if (v == "pn")    frameUrl = "/pnlist";
    else if (v == "nodes") frameUrl = "/nodes";
    else if (v == "tele")  frameUrl = "/tele";
    else if (v == "bt")    frameUrl = "/bt";
    else if (v == "set")   frameUrl = "/settings";
  }
  int tabH = 20;
  if (uiMode == 1) tabH = 28; else if (uiMode == 2) tabH = 38;

  String html = "<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01 Frameset//EN\">";
  html += "<html><head><meta charset='UTF-8'>" + viewportMeta() + "<title>Mesh PSP BT</title></head>";
  html += "<frameset rows='" + String(tabH) + ",*' border='0' frameborder='0' framespacing='0'>";
  html += "<frame src='/tabs?active=" + activeView + "' name='tabs' scrolling='no' noresize>";
  html += "<frame src='" + frameUrl + "' name='content' scrolling='auto'>";
  html += "</frameset></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

void handleTabs() {
  String active = server.hasArg("active") ? server.arg("active") : "ch1";
  bool secAlert = secWhitelistMode && strlen(secCurrentMac) > 0 && !secClientKnown;
  bool btOk = isConn();

  int barH = 20; String tabFs = "10px";
  if (uiMode == 1) { barH = 28; tabFs = "12px"; }
  else if (uiMode == 2) { barH = 38; tabFs = "14px"; }

  String h = "<!DOCTYPE html><html><head><meta charset='UTF-8'>" + viewportMeta();
  h += "<style>"
    "html,body{margin:0;padding:0 2px;background:#111;overflow:hidden;"
      "display:flex;gap:2px;height:" + String(barH) + "px;box-sizing:border-box;font-family:monospace}"
    ".tab{flex:1;display:flex;align-items:center;justify-content:center;"
      "background:#1a1a1a;color:#556655;"
      "text-decoration:none;border:1px solid #2a2a2a;"
      "font-size:" + tabFs + ";font-weight:bold;text-align:center;"
      "box-sizing:border-box;overflow:hidden;text-overflow:ellipsis;"
      "letter-spacing:0px;line-height:1}"
    ".tab.on{background:#003300;color:#00ff00;border:1px solid #00aa00;border-bottom:2px solid #00ff00}"
    ".tab:hover{color:#00ff00;background:#002200}"
    ".tab-cam{color:#006688;border-color:#004455}"
    ".tab-cam.on{background:#001520;color:#00ccff;border-color:#0077aa;border-bottom:2px solid #00ccff}"
    ".tab-set{color:#666644;border-color:#333322}"
    ".tab-set.on{background:#151500;color:#cccc00;border-color:#888800;border-bottom:2px solid #cccc00}"
    ".tab-set-alert{color:#ff6600;border-color:#662200}"
    ".tab-set-alert.on{background:#1a0500;color:#ff6600;border-color:#882200;border-bottom:2px solid #ff6600}"
    "</style>";

  struct { const char* id; const char* lbl; const char* url; int style; } tabs[] = {
    {"ch1",   "CH1",  "/?view=ch1",   0},
    {"ch2",   "CH2",  "/?view=ch2",   0},
    {"pn",    "PN",   "/?view=pn",    0},
    {"nodes", "Node", "/?view=nodes", 0},
    {"tele",  "Tele", "/?view=tele",  0},
    {"bt",    btOk ? "BT" : "BT!", "/?view=bt", btOk ? 1 : 3},
    {"set",   secAlert ? "SET!" : "SET", "/?view=set", secAlert ? 3 : 2},
  };
  for (auto& t : tabs) {
    bool on = (String(t.id) == active);
    String cls = "tab";
    if (t.style == 1) cls += " tab-cam";
    else if (t.style == 3) cls += " tab-set-alert";
    else if (t.style == 2) cls += " tab-set";
    if (on) cls += " on";
    h += "<a class='" + cls + "' href='" + String(t.url) + "' target='_top'>" + String(t.lbl) + "</a>";
  }
  h += "</body></html>";
  server.send(200, "text/html; charset=utf-8", h);
}

// ═══════════════════════════════════════════════════════════════════════════
// HANDLER: BLUETOOTH
// ═══════════════════════════════════════════════════════════════════════════
void handleBt() {
  bool ra = !(server.hasArg("refresh") && server.arg("refresh") == "0");
  bool pin = pinWaiting; bool conn = isConn(); bool scn = scanning;
  String base = "/bt?";
  String self = base + "refresh=" + String(ra ? "1" : "0");
  int rs = 0;
  if (!pin) { if (scn) rs = 2; else if (ra) rs = 4; }
  String html = pageStart(rs, self);
  html += refreshBar(ra, base);
  html += "<div style='color:#00ccff;font-size:12px;margin-bottom:4px'>Bluetooth</div>";

  html += "<div class='bt-box'>";
  html += "<div><span class='tele-label'>Status: </span><span class='tele-val'>" + escHtml(bleState) + "</span></div>";
  html += "<div><span class='tele-label'>Verbunden: </span><span class='tele-val'>" + String(conn ? "JA" : "NEIN") + "</span></div>";
  html += "<div><span class='tele-label'>Scan: </span><span class='tele-val'>" + String(scn ? "laeuft ..." : "-") + "</span></div>";
  html += "<div><span class='tele-label'>Ziel: </span><span class='tele-val'>" + String((tgtValid && tgtAddr[0]) ? tgtAddr : "-") + "</span></div>";
  html += "<div><span class='tele-label'>Eigene Node: </span><span class='tele-val'>";
  if (myNum) html += "0x" + String(myNum, HEX); else html += "-";
  html += "</span></div>";
  html += "<div><span class='tele-label'>Nodes / Msgs: </span><span class='tele-val'>" + String(nodeCacheCount) + " / " + String(min(messageCount, MAX_MESSAGES)) + "</span></div>";
  html += "</div>";

  if (pin) {
    html += "<div class='bt-box' style='border-color:#ffaa00'>";
    html += "<div style='color:#ffaa00;font-size:11px;margin-bottom:4px'>PIN eingeben (steht am Meshtastic-Display):</div>";
    html += "<form method='POST' action='/btpin'>";
    html += "<input type='text' name='pin' size='8' maxlength='8' autocomplete='off'> ";
    html += "<input type='submit' value='PIN senden'></form></div>";
  }

  html += "<a class='bt-btn' href='/btscan'>Scan starten</a>";
  if (conn) html += "<a class='bt-btn' href='/refreshnodes' target='content'>Nodes neu laden</a>";
  if (conn || tgtValid) html += "<a class='bt-btn-red' href='/btdisc'>Trennen + vergessen</a>";

  if (!scn && scanCount > 0) {
    html += "<hr><div style='color:#557799;font-size:9px;margin:4px 0'>Geraet antippen zum Verbinden (gruen = Meshtastic erkannt):</div>";
    for (int pass = 0; pass < 2; pass++) {
      for (int i = 0; i < scanCount; i++) {
        if ((pass == 0) != scanList[i].mesh) continue;
        html += "<a class='bt-dev" + String(scanList[i].mesh ? " bt-dev-mesh" : "") + "' href='/btconnect?addr=" + String(scanList[i].addr) + "&type=" + String((int)scanList[i].type) + "'>";
        html += "<span class='" + String(scanList[i].mesh ? "bt-name" : "bt-name-other") + "'>";
        if (scanList[i].name[0]) html += escHtml(scanList[i].name); else html += "(ohne Name)";
        html += "</span><br><span class='bt-info'>" + String(scanList[i].addr) + " | " + String((int)scanList[i].rssi) + " dBm";
        if (scanList[i].mesh) html += " | MESH";
        html += "</span></a>";
      }
    }
  } else if (!scn) {
    html += "<div class='empty'>Keine Geraete in der Liste. Scan starten.</div>";
  }
  html += "</body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

void handleBtScan() {
  if (!scanning) { scanning = true; scanReq = true; }
  redir("/bt");
}

void handleBtConnect() {
  if (scanning || !server.hasArg("addr")) { redir("/bt"); return; }
  String a = server.arg("addr");
  a.toLowerCase();
  bool valid = (a.length() == 17);
  for (int i = 0; valid && i < 17; i++) {
    char c = a[i];
    bool ok = (i % 3 == 2) ? (c == ':') : ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    if (!ok) valid = false;
  }
  if (!valid) { redir("/bt"); return; }
  uint8_t type = server.hasArg("type") ? (uint8_t)server.arg("type").toInt() : 0;
  strncpy(reqAddr, a.c_str(), sizeof(reqAddr) - 1);
  reqAddr[sizeof(reqAddr) - 1] = 0;
  reqType = type;
  connectReq = true;
  redir("/bt");
}

void handleBtDisc() {
  disconnectReq = true;
  redir("/bt");
}

void handleBtPin() {
  if (!pinWaiting || !server.hasArg("pin")) { redir("/bt"); return; }
  uint32_t pin = (uint32_t)strtoul(server.arg("pin").c_str(), NULL, 10);
  NimBLEConnInfo* c = pinConn;
  pinConn = nullptr;
  pinWaiting = false;
  if (!c) { redir("/bt"); return; }
  setState("PIN gesendet - pruefe ...");
  NimBLEDevice::injectPassKey(*c, pin);
  delete c;
  redir("/bt");
}

void handleRefreshNodes() {
  refreshReq = true;
  redir("/nodes");
}

// ═══════════════════════════════════════════════════════════════════════════
// HANDLER: TRACEROUTE
// ═══════════════════════════════════════════════════════════════════════════
void handleTrace() {
  if (!server.hasArg("node")) { server.send(400, "text/plain", "Kein Node"); return; }
  uint32_t nodeId = strtoul(server.arg("node").c_str(), NULL, 10);
  if (nodeId == 0) { server.send(400, "text/plain", "Ungültige Node-ID"); return; }
  const char* name = resolveNodeName(nodeId);
  unsigned long now = millis();
  unsigned long cooldownLeft = 0; bool inCooldown = false;
  if (traceState.sentMs > 0 && (now - traceState.sentMs) < TRACE_COOLDOWN_MS) {
    cooldownLeft = (TRACE_COOLDOWN_MS - (now - traceState.sentMs)) / 1000; inCooldown = true;
  }
  bool startNew = !inCooldown && !traceState.pending
                  && (traceState.targetNode != nodeId || !traceState.hasResult);
  bool notConn = false;
  if (startNew) {
    if (!isConn()) { notConn = true; startNew = false; }
    else {
      memset(&traceState, 0, sizeof(traceState));
      traceState.targetNode = nodeId; traceState.pending = true;
      traceState.hasResult = false; traceState.timedOut = false; traceState.sentMs = now;
      if (!sendTraceroute(nodeId)) { traceState.pending = false; traceState.timedOut = true; }
      Serial.printf("[TRACE] Starte Trace zu 0x%08X (%s)\n", (unsigned)nodeId, name);
    }
  }
  bool needRefresh = traceState.pending && !traceState.hasResult && !traceState.timedOut;
  String html = pageStart(needRefresh ? 3 : 0, "/trace?node=" + String(nodeId));
  html += "<div style='font-size:9px;margin-bottom:4px'>";
  html += "<a href='/tele?node=" + String(nodeId) + "' target='content'>&lt; Tele</a></div>";
  html += "<div style='color:#00ffcc;font-size:12px;margin-bottom:1px'>Traceroute</div>";
  html += "<div style='color:#004433;font-size:9px;margin-bottom:6px'>" + escHtml(name)
        + " &nbsp;<span style='color:#003322'>0x" + String(nodeId, HEX) + "</span></div>";

  if (notConn) {
    html += "<div class='trace-timeout'>&#9888; Nicht mit Meshtastic verbunden (BT-Tab).</div>";
  } else if (inCooldown && traceState.targetNode == nodeId && !traceState.hasResult && !traceState.timedOut) {
    html += "<div class='trace-pending'>&#9201; Warte auf Antwort...</div>";
    html += "<div class='trace-cooldown'>Nächste Traceroute in " + String(cooldownLeft) + "s möglich</div>";
  } else if (traceState.pending && traceState.targetNode == nodeId) {
    html += "<div class='trace-pending'>&#9201; Warte auf Antwort... (max. 30s)</div>";
    unsigned long elapsed = (now - traceState.sentMs) / 1000;
    html += "<div class='trace-cooldown'>" + String(elapsed) + "s vergangen</div>";
  } else if (traceState.timedOut && traceState.targetNode == nodeId) {
    html += "<div class='trace-timeout'>&#9888; Timeout — keine Antwort erhalten.</div>";
    if (!inCooldown) html += "<div style='margin-top:6px'><a class='btn-trace' href='/trace?node=" + String(nodeId) + "'>Erneut versuchen</a></div>";
    else html += "<div class='trace-cooldown'>Erneut in " + String(cooldownLeft) + "s möglich</div>";
  } else if (traceState.hasResult && traceState.targetNode == nodeId) {
    html += "<div class='trace-box'>";
    html += "<div style='color:#006644;font-size:9px;margin-bottom:4px'>ROUTE HIN</div>";
    html += "<div class='trace-hop'><span class='trace-idx'>HOP 0</span><span class='trace-arrow'>&#9654;</span><span class='trace-src'>ICH (lokal)</span></div>";
    if (traceState.hopCount == 0) {
      html += "<div class='trace-hop'><span class='trace-idx'>HOP 1</span><span class='trace-arrow'>&#9654;</span><span class='trace-dest'>" + escHtml(name) + " [ZIEL]</span><span class='trace-id'>&nbsp;0x" + String(nodeId, HEX) + "</span></div>";
      html += "<div class='trace-direct' style='margin-top:4px'>&#10003; Direkte Verbindung</div>";
    } else {
      for (int i = 0; i < traceState.hopCount; i++) {
        uint32_t hopId = traceState.hops[i];
        html += "<div class='trace-hop'><span class='trace-idx'>HOP " + String(i + 1) + "</span><span class='trace-arrow'>&#9654;</span><span class='trace-name'>" + escHtml(resolveNodeName(hopId)) + "</span><span class='trace-id'>&nbsp;0x" + String(hopId, HEX) + "</span></div>";
      }
      html += "<div class='trace-hop'><span class='trace-idx'>HOP " + String(traceState.hopCount + 1) + "</span><span class='trace-arrow'>&#9654;</span><span class='trace-dest'>" + escHtml(name) + " [ZIEL]</span><span class='trace-id'>&nbsp;0x" + String(nodeId, HEX) + "</span></div>";
    }
    html += "</div>";
    html += "<div class='trace-box' style='border-color:#004466;background:#000f18'>";
    html += "<div style='color:#004466;font-size:9px;margin-bottom:4px'>ROUTE ZURÜCK</div>";
    html += "<div class='trace-hop'><span class='trace-idx'>HOP 0</span><span class='trace-arrow'>&#9664;</span><span class='trace-dest'>" + escHtml(name) + "</span></div>";
    for (int i = 0; i < traceState.hopBackCount; i++) {
      uint32_t hopId = traceState.hopsBack[i];
      html += "<div class='trace-hop'><span class='trace-idx'>HOP " + String(i + 1) + "</span><span class='trace-arrow'>&#9664;</span><span class='trace-name'>" + escHtml(resolveNodeName(hopId)) + "</span><span class='trace-id'>&nbsp;0x" + String(hopId, HEX) + "</span></div>";
    }
    html += "<div class='trace-hop'><span class='trace-idx'>HOP " + String(traceState.hopBackCount + 1) + "</span><span class='trace-arrow'>&#9664;</span><span class='trace-src'>ICH (lokal)</span></div>";
    html += "</div>";
    html += "<div style='color:#444;font-size:9px;margin-top:4px'>";
    if (traceState.hopCount == 0) html += "Direkt erreichbar (0 Zwischenhops)";
    else html += String(traceState.hopCount) + " Zwischenhop(s)";
    unsigned long rtt = (traceState.resultMs - traceState.sentMs);
    html += " &nbsp;|&nbsp; RTT ~" + String(rtt) + "ms</div>";
    html += "<div style='margin-top:8px'>";
    if (!inCooldown) html += "<a class='btn-trace' href='/trace?node=" + String(nodeId) + "'>Erneut messen</a>";
    else html += "<div class='trace-cooldown'>Erneut in " + String(cooldownLeft) + "s möglich</div>";
    html += "</div>";
  } else {
    html += "<div class='trace-pending'>Starte Traceroute...</div>";
    html += "<meta http-equiv='refresh' content='1;url=/trace?node=" + String(nodeId) + "'>";
  }
  html += "</body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

// ═══════════════════════════════════════════════════════════════════════════
// HANDLER: SECURITY / WIFI SAVE
// ═══════════════════════════════════════════════════════════════════════════
void handleSecMode() {
  if (server.hasArg("v")) { secWhitelistMode = (server.arg("v").toInt() == 1); secSave(); }
  redir("/settings?subtab=sec");
}
void handleSecWlRemove() {
  if (server.hasArg("i")) {
    int idx = server.arg("i").toInt();
    if (idx >= 0 && idx < secWhitelistCount) {
      for (int i = idx; i < secWhitelistCount - 1; i++) secWhitelist[i] = secWhitelist[i + 1];
      secWhitelistCount--; secSave();
    }
  }
  redir("/settings?subtab=sec");
}
void handleWifiSave() {
  if (server.hasArg("wifi_ssid")) {
    String s = server.arg("wifi_ssid"); s.trim();
    if (s.length() > 0 && s.length() <= 32) { strncpy(wifiSSID, s.c_str(), 32); wifiSSID[32] = '\0'; }
  }
  wifiHidden = server.hasArg("wifi_hidden");
  saveWifiSettings(); applyWifiSettings();
  redir("/settings?subtab=wifi&saved=1");
}

// ═══════════════════════════════════════════════════════════════════════════
// HANDLER: CHANNEL CHAT
// ═══════════════════════════════════════════════════════════════════════════
void handleChat() {
  uint8_t tab = 0;
  if (server.hasArg("tab")) tab = (uint8_t)server.arg("tab").toInt();
  bool ra = !(server.hasArg("refresh") && server.arg("refresh") == "0");
  bool psOn = !(server.hasArg("ps") && server.arg("ps") == "0");
  String base = "/chat?tab=" + String(tab) + "&";
  String self = base + "refresh=" + String(ra ? "1" : "0") + "&ps=" + String(psOn ? "1" : "0");
  String html = pageStart(ra ? 5 : 0, self);

  int presetCount = 0;
  for (int i = 0; i < PRESET_COUNT; i++) {
    if (strlen(presetTexts[i]) > 0 && ((tab == 0 && presetCh1[i]) || (tab == 1 && presetCh2[i]))) presetCount++;
  }
  if (presetCount > 0) {
    String toggleUrl = base + "refresh=" + String(ra ? "1" : "0") + "&ps=" + String(psOn ? "0" : "1");
    html += "<div class='preset-box'>";
    html += "<a class='preset-toggle" + String(psOn ? " on" : "") + "' href='" + toggleUrl + "'>" + String(psOn ? "[~]" : "[+]") + "</a>";
    if (psOn) {
      for (int i = 0; i < PRESET_COUNT; i++) {
        if (strlen(presetTexts[i]) == 0) continue;
        bool show = (tab == 0 && presetCh1[i]) || (tab == 1 && presetCh2[i]);
        if (!show) continue;
        char lbl[7]; strncpy(lbl, presetTexts[i], 5); lbl[5] = '\0';
        html += "<a class='preset-btn' href='/send?target=" +
                String(tab == 0 ? "BROADCAST_CH1" : "BROADCAST_CH2") +
                "&tab=" + String(tab) + "&msg=" + String(presetTexts[i])
                + "&ps=" + String(psOn ? "1" : "0") + "&refresh=1' target='_self'>" +
                escHtml(lbl) + "</a>";
      }
    }
    html += "</div>";
  }
  html += refreshBar(ra, base + "ps=" + String(psOn ? "1" : "0") + "&");
  int start = (messageCount > MAX_MESSAGES) ? messageCount % MAX_MESSAGES : 0;
  int count = min(messageCount, MAX_MESSAGES);
  int found = 0;
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % MAX_MESSAGES;
    if (messages[idx].channelIdx != tab) continue;
    found++;
    bool isMe = (messages[idx].senderId == 0);
    bool isSys = (strcmp(messages[idx].senderName, "SYS") == 0);
    String divCls = isSys ? "msg-sys" : (isMe ? "msg-me" : "msg");
    String sndrCls = isSys ? "sndr-sys" : (isMe ? "sndr-me" : "sndr");
    html += "<div class='" + divCls + "'><div class='" + sndrCls + "'>";
    if (!isMe && !isSys && messages[idx].senderId != 0)
      html += "<a href='/tele?node=" + String(messages[idx].senderId) + "' target='content'>" + escHtml(messages[idx].senderName) + "</a>";
    else html += escHtml(messages[idx].senderName);
    html += " <span class='meta'>[" + escHtml(messages[idx].channelName) + "]</span></div>";
    html += "<div class='txt'>" + escHtml(messages[idx].text);
    if (isMe && messages[idx].requestId != 0) {
      if (messages[idx].acked) html += "<span class='ack-ok'>(OK)</span>";
      else html += "<span class='ack-pend'>(...)</span>";
    }
    html += "</div></div>";
  }
  if (!found) html += "<div class='empty'>Keine Nachrichten.</div>";
  String uid = String(millis());
  html += "<div class='send-row'><form method='POST' action='/send'>";
  html += "<input type='hidden' name='target' value='" + String(tab == 0 ? "BROADCAST_CH1" : "BROADCAST_CH2") + "'>";
  html += "<input type='hidden' name='tab' value='" + String(tab) + "'>";
  html += "<input type='hidden' name='field_id' value='" + uid + "'>";
  html += "<input type='text' name='msg_" + uid + "' value='' autocomplete='off' placeholder='Nachricht...' maxlength='100' style='width:100%;box-sizing:border-box'><br><br>";
  html += "<input type='submit' value='Senden'> ";
  html += "<input type='submit' name='clear' value='Clear' class='btn-red'>";
  html += "</form></div></body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

// ═══════════════════════════════════════════════════════════════════════════
// HANDLER: SETTINGS
// ═══════════════════════════════════════════════════════════════════════════
void handleSettings() {
  String subtab = server.hasArg("subtab") ? server.arg("subtab") : "presets";
  bool saved = server.hasArg("saved");
  String html = pageStart(0, "");
  html += "<style>"
    ".set-subtab{flex:1;padding:7px 0;background:#111;color:#556655;"
      "text-decoration:none;border:1px solid #2a2a2a;font-size:11px;"
      "font-weight:bold;text-align:center;font-family:monospace;display:block}"
    ".set-subtab.on{background:#151500;color:#cccc00;border-color:#888800}"
    ".set-subtab-wifi.on{background:#00050f;color:#44aaff;border-color:#224488}"
    ".set-subtab-sec.on{background:#1a0500;color:#ff8800;border-color:#883300}"
    ".set-subtab-disp.on{background:#001818;color:#00ffcc;border-color:#00aa88}"
    ".stabs{display:flex;gap:2px;margin-bottom:6px}"
    ".ch-chk{display:flex;align-items:center;gap:3px;font-size:9px;color:#667766;white-space:nowrap}"
    "</style>";
  html += "<div class='stabs'>";

  String secTabLabel = (secWhitelistMode && !secClientKnown && strlen(secCurrentMac) > 0) ? "SEC!" : "SEC";

  auto stab = [&](String id, String lbl, String extra) {
    bool on = (subtab == id);
    String cls = "set-subtab"; if (extra.length() > 0) cls += " " + extra; if (on) cls += " on";
    html += "<a class='" + cls + "' href='/settings?subtab=" + id + "'>" + lbl + "</a>";
  };
  stab("presets", "PRESET", "");
  stab("wifi", "WIFI", "set-subtab-wifi");
  stab("sec", secTabLabel, "set-subtab-sec");
  stab("display", "ANZEIGE", "set-subtab-disp");
  html += "</div>";

  if (saved) {
    html += "<div style='color:#00ff66;font-size:10px;margin-bottom:8px;border:1px solid #005500;padding:4px;background:#001200'>&#10003; Gespeichert.</div>";
  }

  if (subtab == "presets") {
    html += "<div class='set-section'>";
    html += "<div class='set-label'>QUICK-SEND BUTTONS &mdash; 9 Slots</div>";
    html += "<div style='color:#334433;font-size:9px;margin-bottom:5px'>Haken CH1/CH2: auf welchem Kanal der Button erscheint</div>";
    html += "<form method='POST' action='/settingssave'>";
    for (int i = 0; i < PRESET_COUNT; i++) {
      html += "<div class='set-row'>";
      html += "<span class='set-idx'>" + String(i + 1) + "</span>";
      html += "<input class='set-inp' type='text' name='p" + String(i) + "' value='" + escHtml(presetTexts[i]) + "' maxlength='60' placeholder='leer = ausgeblendet' style='flex:1'>";
      html += "<span class='ch-chk'><input type='checkbox' name='c1_" + String(i) + "' id='c1_" + String(i) + "'" + String(presetCh1[i] ? " checked" : "") + ">";
      html += "<label for='c1_" + String(i) + "'>1</label></span>";
      html += "<span class='ch-chk'><input type='checkbox' name='c2_" + String(i) + "' id='c2_" + String(i) + "'" + String(presetCh2[i] ? " checked" : "") + ">";
      html += "<label for='c2_" + String(i) + "'>2</label></span>";
      html += "</div>";
    }
    html += "<div style='margin-top:8px'><input type='submit' value='Speichern' class='set-save'></div>";
    html += "</form></div>";
  }
  else if (subtab == "wifi") {
    html += "<div class='set-section'>";
    html += "<div class='set-label'>WLAN ACCESS POINT EINSTELLUNGEN</div>";
    html += "<form method='POST' action='/wifisave'>";
    html += "<div style='color:#334455;font-size:9px;margin-bottom:6px'>Aktuelle SSID: <span style='color:#44aaff'>" + escHtml(wifiSSID) + "</span>"
            " &nbsp;|&nbsp; Versteckt: <span style='color:#44aaff'>" + String(wifiHidden ? "JA" : "NEIN") + "</span>"
            " &nbsp;|&nbsp; <span style='color:#334444'>Offen (kein Passwort)</span></div>";
    html += "<div class='set-row'><span class='set-label' style='min-width:70px'>SSID&nbsp;</span>";
    html += "<input class='set-inp-wifi' type='text' name='wifi_ssid' value='" + escHtml(wifiSSID) + "' maxlength='32' placeholder='Netzwerkname' style='flex:1'></div>";
    html += "<div class='set-row' style='margin-top:4px'><input type='checkbox' name='wifi_hidden' id='wifi_hidden'" + String(wifiHidden ? " checked" : "") + " style='margin-right:4px'>";
    html += "<label for='wifi_hidden' style='color:#778899;font-size:10px'>Verstecktes WLAN (SSID nicht sichtbar)</label></div>";
    html += "<div style='margin-top:8px;border-top:1px solid #0a1520;padding-top:6px'><input type='submit' value='Speichern &amp; Neu verbinden' class='set-save-wifi'></div>";
    html += "<div style='color:#222;font-size:9px;margin-top:6px'>ESP startet WLAN sofort neu. Kurzer Verbindungsabbruch.</div>";
    html += "</form></div>";
  }
  else if (subtab == "display") {
    const char* modeNames[3] = {"PSP", "Netbook", "Smartphone"};
    const char* modeDesc[3] = {
      "Minimal: 11px Schrift, 20px Tabs, kein Viewport-Tag. Optimal für PSP-Browser.",
      "Mittel: 13px Schrift, 28px Tabs, Viewport width=800. Gut für Netbooks/Tablets.",
      "Touch: 14px Schrift, 38px Tabs, Viewport device-width. Für Smartphones."
    };
    html += "<div class='set-section'>";
    html += "<div class='set-label'>ANZEIGEMODUS</div>";
    html += "<div style='color:#334433;font-size:9px;margin-bottom:8px'>Aktuell: <span style='color:#00ffcc'>" + String(modeNames[uiMode]) + "</span></div>";
    for (uint8_t m = 0; m < 3; m++) {
      bool on = (m == uiMode);
      html += "<a href='/uimode?v=" + String(m) + "' class='ui-mode-btn" + String(on ? " on" : "") + "'>"
            + String(modeNames[m]) + "</a>";
    }
    html += "<div style='color:#334433;font-size:9px;margin-top:8px'>" + String(modeDesc[uiMode]) + "</div>";
    html += "<div style='color:#222;font-size:8px;margin-top:6px'>Einstellung wird sofort gespeichert und beim nächsten Seitenaufruf wirksam.</div>";
    html += "</div>";
  }
  else {
    html += "<div class='set-section'>";
    html += "<div class='sec-box'><div class='set-label'>AKTUELLER CLIENT</div>";
    if (strlen(secCurrentMac) > 0) {
      html += "<div class='sec-mac'>" + escHtml(secCurrentMac) + " / " + escHtml(secCurrentIp) + "</div>";
      if (secClientKnown) html += "<div class='sec-ok'>&#10003; Bekannt &mdash; verbunden</div>";
      else html += "<div class='sec-blocked'>&#9888; UNBEKANNTE MAC &mdash; geblockt</div>";
    } else html += "<div style='color:#333;font-size:9px'>Kein Client verbunden</div>";
    html += "</div>";
    html += "<div class='sec-box'><div class='set-label'>VERBINDUNGS-HISTORY (letzte " + String(SEC_HISTORY_SIZE) + ")</div>";
    bool anyHist = false;
    for (int k = 0; k < SEC_HISTORY_SIZE; k++) {
      int i = (secHistoryHead - 1 - k + SEC_HISTORY_SIZE) % SEC_HISTORY_SIZE;
      if (!secHistory[i].valid) continue; anyHist = true;
      html += "<div style='border-bottom:1px solid #1a0a00;padding:2px 0;margin-bottom:2px'>";
      html += (secHistory[i].allowed ? "<span class='sec-hist-ok'>&#10003;</span> " : "<span class='sec-hist-bad'>&#9888;</span> ");
      html += "<span class='sec-mac'>" + escHtml(secHistory[i].mac) + "</span> <span style='color:#556;font-size:9px'>" + escHtml(secHistory[i].ip) + "</span>";
      if (!secHistory[i].allowed) html += " <span class='sec-hist-bad' style='font-size:8px'>[GEBLOCKT]</span>";
      html += "</div>";
    }
    if (!anyHist) html += "<div style='color:#333;font-size:9px'>Noch keine Verbindungen.</div>";
    html += "</div>";
    html += "<div class='set-label'>MODUS</div>";
    if (!secWhitelistMode) {
      html += "<div style='color:#00ccff;font-size:9px;margin-bottom:3px'>&#9679; Lernmodus: Neue MACs werden automatisch gespeichert.</div>";
      html += "<a class='sec-mode-lock' href='/sec_mode?v=1'>Lernmodus beenden &rarr; SPERREN</a>";
    } else {
      html += "<div style='color:#ff6600;font-size:9px;margin-bottom:3px'>&#9679; Sperrmodus: Nur Whitelist-MACs erlaubt.</div>";
      html += "<a class='sec-mode-learn' href='/sec_mode?v=0'>Sperrmodus aufheben &rarr; LERNEN</a>";
    }
    html += "<hr><div class='set-label'>MAC-WHITELIST (" + String(secWhitelistCount) + "/" + String(SEC_MAX_WHITELIST) + ")</div>";
    if (secWhitelistCount == 0) {
      html += "<div style='color:#333;font-size:9px'>Leer. Verbinde im Lernmodus.</div>";
    } else {
      for (int i = 0; i < secWhitelistCount; i++) {
        bool isCur = (strcasecmp(secWhitelist[i].mac, secCurrentMac) == 0);
        html += "<div class='set-row' style='border-bottom:1px solid #1a1a0a;padding-bottom:3px'>";
        html += "<span class='sec-mac' style='flex:1;color:" + String(isCur ? "#00ff66" : "#ffcc88") + "'>" + escHtml(secWhitelist[i].mac);
        if (isCur) html += " <span style='color:#00ff66;font-size:8px'>[DU]</span>";
        html += "</span><a class='sec-btn-red' href='/sec_wl_rm?i=" + String(i) + "'>&#10005;</a></div>";
      }
    }
    html += "<hr><div style='color:#222;font-size:8px;margin-top:4px'>Schutzmassnahmen: Max. 1 Client | 802.11b | MAC-Whitelist | LED-Alarm</div>";
    html += "</div>";
  }

  html += "<div style='margin:10px 0 6px 0;border-top:1px solid #1a0000;padding-top:8px'>";
  html += "<div class='set-label'>SYSTEM</div>";
  html += "<a href='/shutdown' style='display:block;background:#1a0000;color:#ff4444;border:1px solid #882222;padding:6px 10px;font-family:monospace;font-size:11px;text-align:center;text-decoration:none;margin-top:4px'>&#9210; ESP32 herunterfahren</a>";
  html += "<div style='color:#2a1010;font-size:8px;margin-top:3px'>BT trennen &rarr; Deep Sleep &mdash; Neustart nur per Reset/Strom</div>";
  html += "</div>";
  html += "<hr><div style='color:#333;font-size:9px;margin-top:4px'>Mesh PSP BT (UI v24)</div>";
  html += "</body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

void handleSettingsSave() {
  for (int i = 0; i < PRESET_COUNT; i++) {
    String key = "p" + String(i);
    if (server.hasArg(key)) {
      String val = server.arg(key); val.trim();
      strncpy(presetTexts[i], val.c_str(), 63); presetTexts[i][63] = '\0';
      strncpy(presetLabels[i], presetTexts[i], 23); presetLabels[i][23] = '\0';
    }
    presetCh1[i] = server.hasArg("c1_" + String(i));
    presetCh2[i] = server.hasArg("c2_" + String(i));
    if (strlen(presetTexts[i]) > 0 && !presetCh1[i] && !presetCh2[i]) { presetCh1[i] = true; presetCh2[i] = true; }
  }
  savePresets();
  redir("/settings?subtab=presets&saved=1");
}

// ═══════════════════════════════════════════════════════════════════════════
// HANDLER: SEND / CLEAR
// ═══════════════════════════════════════════════════════════════════════════
void handleSend() {
  uint8_t tab = 0;
  if (server.hasArg("tab")) tab = (uint8_t)server.arg("tab").toInt();
  if (server.hasArg("clear")) {
    if (tab == 2 && server.hasArg("target")) redir("/clear?tab=2&node=" + server.arg("target"));
    else redir("/clear?tab=" + String(tab));
    return;
  }
  String msg = "";
  if (server.hasArg("field_id")) {
    String df = "msg_" + server.arg("field_id");
    if (server.hasArg(df)) msg = server.arg(df);
  }
  if (msg.length() == 0 && server.hasArg("msg")) msg = server.arg("msg");
  msg.trim();
  if (msg.length() > 0 && server.hasArg("target")) {
    String target = server.arg("target");
    if (target == "BROADCAST_CH1")      sendToRadio(msg.c_str(), 0xFFFFFFFF, 0);
    else if (target == "BROADCAST_CH2") sendToRadio(msg.c_str(), 0xFFFFFFFF, 1);
    else { uint32_t tid = strtoul(target.c_str(), NULL, 10); if (tid != 0) sendToRadio(msg.c_str(), tid, 0); }
  }
  if (server.hasArg("returnnode")) redir("/pnconv?node=" + server.arg("returnnode") + "&refresh=1");
  else if (server.hasArg("tab"))   redir("/chat?tab=" + server.arg("tab") + "&refresh=1");
  else redir("/pnlist?refresh=1");
}

void handleClear() {
  uint8_t tab = 0; uint32_t partnerId = 0; bool isPN = false;
  if (server.hasArg("tab")) {
    tab = (uint8_t)server.arg("tab").toInt();
    if (tab == 2 && server.hasArg("node")) { partnerId = strtoul(server.arg("node").c_str(), NULL, 10); isPN = true; }
  }
  for (int i = 0; i < MAX_MESSAGES; i++) {
    if (isPN) {
      uint32_t p = (messages[i].senderId == 0) ? messages[i].targetId : messages[i].senderId;
      if (messages[i].channelIdx == 2 && p == partnerId) { messages[i].channelIdx = 99; messages[i].text[0] = '\0'; }
    } else {
      if (messages[i].channelIdx == tab) { messages[i].channelIdx = 99; messages[i].text[0] = '\0'; }
    }
  }
  if (isPN) redir("/pnconv?node=" + String(partnerId));
  else redir("/chat?tab=" + String(tab));
}

// ═══════════════════════════════════════════════════════════════════════════
// HANDLER: PN LISTE / KONVERSATION
// ═══════════════════════════════════════════════════════════════════════════
void handlePNList() {
  bool ra = !(server.hasArg("refresh") && server.arg("refresh") == "0");
  String base = "/pnlist?"; String self = base + "refresh=" + String(ra ? "1" : "0");
  String html = pageStart(ra ? 5 : 0, self);
  html += refreshBar(ra, base);
  html += "<div style='color:#ff88ff;font-size:10px;margin-bottom:6px'>Private Nachrichten</div>";
  uint32_t pnPartners[MAX_CACHED_NODES]; int pnCount = 0;
  int start = (messageCount > MAX_MESSAGES) ? messageCount % MAX_MESSAGES : 0;
  int count = min(messageCount, MAX_MESSAGES);
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % MAX_MESSAGES;
    if (messages[idx].channelIdx != 2) continue;
    uint32_t p = (messages[idx].senderId == 0) ? messages[idx].targetId : messages[idx].senderId;
    if (p == 0 || p == 0xFFFFFFFF) continue;
    bool ex = false;
    for (int j = 0; j < pnCount; j++) if (pnPartners[j] == p) { ex = true; break; }
    if (!ex && pnCount < MAX_CACHED_NODES) pnPartners[pnCount++] = p;
  }
  if (pnCount == 0) {
    html += "<div class='empty'>Keine Privatnachrichten.</div>";
  } else {
    for (int i = 0; i < pnCount; i++) {
      uint32_t partner = pnPartners[i];
      String name = escHtml(resolveNodeName(partner));
      const char* preview = "";
      for (int j = count - 1; j >= 0; j--) {
        int idx = (start + j) % MAX_MESSAGES;
        if (messages[idx].channelIdx != 2) continue;
        uint32_t p2 = (messages[idx].senderId == 0) ? messages[idx].targetId : messages[idx].senderId;
        if (p2 == partner) { preview = messages[idx].text; break; }
      }
      html += "<a class='pn-tile' href='/pnconv?node=" + String(partner) + "' target='content'>";
      html += "<span class='tile-name'>" + name + "</span>";
      String prev = escHtml(preview); if (prev.length() > 45) prev = prev.substring(0, 45) + "...";
      html += "<span class='tile-preview'>" + prev + "</span>";
      html += "</a>";
    }
  }
  html += "</body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

void handlePNConv() {
  if (!server.hasArg("node")) { redir("/pnlist"); return; }
  uint32_t nodeId = strtoul(server.arg("node").c_str(), NULL, 10);
  String name = escHtml(resolveNodeName(nodeId));
  bool ra = !(server.hasArg("refresh") && server.arg("refresh") == "0");
  String base = "/pnconv?node=" + String(nodeId) + "&";
  String self = base + "refresh=" + String(ra ? "1" : "0");
  String html = pageStart(ra ? 5 : 0, self);
  html += refreshBar(ra, base);
  html += "<div style='font-size:10px;margin-bottom:5px'><a href='/pnlist' target='content'>&lt; PN</a> | <span style='color:#ff88ff;font-weight:bold'>" + name + "</span></div>";
  int start = (messageCount > MAX_MESSAGES) ? messageCount % MAX_MESSAGES : 0;
  int count = min(messageCount, MAX_MESSAGES);
  int found = 0;
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % MAX_MESSAGES;
    if (messages[idx].channelIdx != 2) continue;
    uint32_t partner = (messages[idx].senderId == 0) ? messages[idx].targetId : messages[idx].senderId;
    if (partner != nodeId) continue;
    found++;
    bool isMe = (messages[idx].senderId == 0);
    bool isSys = (strcmp(messages[idx].senderName, "SYS") == 0);
    if (isSys) { html += "<div class='msg-sys'><div class='sndr-sys'>SYS</div><div class='txt' style='color:#555'>" + escHtml(messages[idx].text) + "</div></div>"; continue; }
    html += "<div class='" + String(isMe ? "msg-me" : "msg-pn") + "'>";
    html += "<div class='" + String(isMe ? "sndr-me" : "sndr-pn") + "'>" + (isMe ? String("ICH") : name) + "</div>";
    html += "<div class='txt'>" + escHtml(messages[idx].text);
    if (isMe && messages[idx].requestId != 0) {
      if (messages[idx].acked) html += "<span class='ack-ok'>(OK)</span>";
      else html += "<span class='ack-pend'>(...)</span>";
    }
    html += "</div></div>";
  }
  if (!found) html += "<div class='empty'>Noch keine Nachrichten.</div>";
  String uid = String(millis() + 1);
  html += "<div class='send-row'><form method='POST' action='/send'>";
  html += "<input type='hidden' name='target' value='" + String(nodeId, DEC) + "'>";
  html += "<input type='hidden' name='returnnode' value='" + String(nodeId, DEC) + "'>";
  html += "<input type='hidden' name='tab' value='2'>";
  html += "<input type='hidden' name='field_id' value='" + uid + "'>";
  html += "<input type='text' name='msg_" + uid + "' value='' autocomplete='off' placeholder='Nachricht...' maxlength='100' style='width:100%;box-sizing:border-box'><br><br>";
  html += "<input type='submit' value='Senden'> </form>";
  html += "<form method='POST' action='/send' style='display:inline'>";
  html += "<input type='hidden' name='tab' value='2'>";
  html += "<input type='hidden' name='target' value='" + String(nodeId, DEC) + "'>";
  html += "<input type='submit' name='clear' value='Clear' class='btn-red'></form>";
  html += "</div></body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

// ═══════════════════════════════════════════════════════════════════════════
// HANDLER: NODES / TELEMETRIE / FAVORITEN
// ═══════════════════════════════════════════════════════════════════════════
void handleToggleFav() {
  if (server.hasArg("node")) {
    uint32_t id = strtoul(server.arg("node").c_str(), NULL, 10);
    NodeCache* n = findNode(id);
    if (n) { n->isFavorite = !n->isFavorite; saveFavorites(); }
  }
  String from = server.hasArg("from") ? server.arg("from") : "nodes";
  if (from == "tele")       redir("/tele");
  else if (from == "tnode") redir("/tele?node=" + server.arg("node"));
  else                      redir("/nodes");
}

void handleNodeList() {
  sortNodes();
  String html = pageStart(0, "");
  html += "<div style='color:#ffff00;font-size:9px;padding:4px 0'>" + String(nodeCacheCount) + " Nodes &nbsp;";
  html += "<a href='/refreshnodes' target='content'>[Refresh]</a></div>";
  if (nodeCacheCount == 0) html += "<div class='empty'>Keine Nodes. Erst im BT-Tab verbinden.</div>";
  int shown = min(nodeCacheCount, LIST_MAX);
  for (int i = 0; i < shown; i++) {
    NodeCache* n = &nodeDB[i];
    html += "<div class='node-row'>";
    html += "<a class='fav-btn' href='/fav?node=" + String(n->num) + "&from=nodes' target='content'>" + String(n->isFavorite ? "[*]" : "[ ]") + "</a>";
    html += "<a href='/tele?node=" + String(n->num) + "' target='content'><span class='node-name'>";
    if (n->isFavorite) html += "* ";
    if (strlen(n->shortName) > 0) html += "[" + escHtml(n->shortName) + "] ";
    html += escHtml(strlen(n->longName) > 0 ? n->longName : "Unbekannt") + "</span></a>";
    html += "<br><span class='node-id'>0x" + String(n->num, HEX) + "</span>";
    html += " <span style='color:#444;font-size:9px'>(" + ageShort(nodeAgeSec(n)) + " ago)</span>";
    if (n->hasSnr) { char buf[10]; dtostrf(n->snr, 4, 1, buf); html += " <span class='node-snr'>SNR " + String(buf) + "dB</span>"; }
    html += "</div>";
  }
  if (nodeCacheCount > shown) html += "<div class='empty'>... und " + String(nodeCacheCount - shown) + " weitere</div>";
  html += "</body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

void handleTelemetry() {
  bool ra = !(server.hasArg("refresh") && server.arg("refresh") == "0");
  if (!server.hasArg("node")) {
    sortNodes();
    String base = "/tele?"; String self = base + "refresh=" + String(ra ? "1" : "0");
    String html = pageStart(ra ? 10 : 0, self);
    html += refreshBar(ra, base);
    html += "<div style='color:#ffff00;font-size:9px;margin-bottom:3px'>Telemetrie</div>";
    int tc = 0;
    for (int i = 0; i < nodeCacheCount && tc < LIST_MAX; i++) {
      NodeCache* n = &nodeDB[i];
      if (!n->hasTelemetry && !n->hasSnr) continue; tc++;
      html += "<div class='node-row'>";
      html += "<a class='fav-btn' href='/fav?node=" + String(n->num) + "&from=tele' target='content'>" + String(n->isFavorite ? "[*]" : "[ ]") + "</a>";
      html += "<a href='/tele?node=" + String(n->num) + "' target='content'><span class='node-name'>";
      if (n->isFavorite) html += "* ";
      if (strlen(n->shortName) > 0) html += "[" + escHtml(n->shortName) + "] ";
      html += escHtml(strlen(n->longName) > 0 ? n->longName : "???") + "</span></a><br>";
      if (n->battery > 0) {
        if (n->battery > 100) html += "<span class='node-snr'>BAT:USB </span>";
        else { char buf[8]; dtostrf(n->battery, 4, 0, buf); html += "<span class='node-snr'>BAT:" + String(buf) + "% </span>"; }
      }
      if (n->temperature != 0) { char buf[8]; dtostrf(n->temperature, 4, 1, buf); html += "<span class='node-snr'>TEMP:" + String(buf) + "C </span>"; }
      if (n->hasSnr) { char buf[8]; dtostrf(n->snr, 4, 1, buf); html += "<span class='node-snr'>SNR:" + String(buf) + "dB</span>"; }
      html += "</div>";
    }
    if (tc == 0) html += "<div class='empty'>Keine Telemetriedaten.</div>";
    html += "</body></html>"; server.send(200, "text/html; charset=utf-8", html); return;
  }

  uint32_t nodeId = strtoul(server.arg("node").c_str(), NULL, 10);
  NodeCache* n = findNode(nodeId); String name = escHtml(resolveNodeName(nodeId));
  String base = "/tele?node=" + String(nodeId) + "&";
  String self = base + "refresh=" + String(ra ? "1" : "0");
  String html = pageStart(ra ? 10 : 0, self);
  html += refreshBar(ra, base);
  html += "<div style='font-size:9px;margin-bottom:3px'><a href='/tele' target='content'>&lt; Tele</a> | <a href='/nodes' target='content'>Nodes</a> | <a href='/fav?node=" + String(nodeId) + "&from=tnode' target='content'>" + String((n && n->isFavorite) ? "[* Fav-]" : "[ Fav+]") + "</a></div>";
  html += "<div style='color:#00ff66;font-size:12px;margin-bottom:2px'>";
  if (n && n->isFavorite) html += "* ";
  if (n && strlen(n->shortName) > 0) html += "[" + escHtml(n->shortName) + "] ";
  html += name + "</div>";
  html += "<div style='color:#555;font-size:9px;margin-bottom:5px'>0x" + String(nodeId, HEX) + "</div>";
  html += "<div class='tele-box'>";
  if (n && n->hasSnr) { char buf[10]; dtostrf(n->snr, 4, 1, buf); html += "<div><span class='tele-label'>SNR: </span><span class='tele-val'>" + String(buf) + " dB</span></div>"; }
  if (n && n->hasTelemetry) {
    if (n->battery > 0) {
      if (n->battery > 100) html += "<div><span class='tele-label'>Batterie: </span><span class='tele-val'>USB / extern</span></div>";
      else { char buf[8]; dtostrf(n->battery, 4, 0, buf); html += "<div><span class='tele-label'>Batterie: </span><span class='tele-val'>" + String(buf) + "%</span></div>"; }
    }
    if (n->temperature != 0) { char buf[8]; dtostrf(n->temperature, 5, 1, buf); html += "<div><span class='tele-label'>Temperatur: </span><span class='tele-val'>" + String(buf) + " C</span></div>"; }
    if (n->humidity != 0) { char buf[8]; dtostrf(n->humidity, 5, 1, buf); html += "<div><span class='tele-label'>Luftfeuchte: </span><span class='tele-val'>" + String(buf) + "%</span></div>"; }
  } else html += "<div class='empty'>Keine Telemetrie-Daten.</div>";
  if (n) {
    uint32_t ago = nodeAgeSec(n);
    html += "<div><span class='tele-label'>Zuletzt: </span><span class='tele-val'>";
    if (ago == 0xFFFFFFFF) html += "unbekannt"; else html += "vor " + ageShort(ago);
    html += "</span></div>";
  }
  html += "</div>";
  html += "<div style='display:flex;gap:4px;margin-top:5px'>";
  html += "<a href='/pnconv?node=" + String(nodeId, DEC) + "' target='content' style='flex:1;background:#180018;color:#ff88ff;border:1px solid #ff44ff;padding:5px 8px;font-family:monospace;font-size:11px;display:block;text-align:center;text-decoration:none'>[ PN senden ]</a>";
  html += "<a href='/trace?node=" + String(nodeId, DEC) + "' target='content' style='flex:1;background:#001818;color:#00ffcc;border:1px solid #00aa88;padding:5px 8px;font-family:monospace;font-size:11px;display:block;text-align:center;text-decoration:none'>[ Traceroute ]</a>";
  html += "</div></body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

// ═══════════════════════════════════════════════════════════════════════════
// SHUTDOWN
// ═══════════════════════════════════════════════════════════════════════════
void handleShutdown() {
  server.send(200, "text/html; charset=utf-8",
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<style>html,body{background:#000;color:#ccc;font-family:monospace;padding:8px;font-size:11px}</style></head><body>"
    "<div style='color:#ff4444;margin-bottom:6px'>ESP32 wird heruntergefahren...</div>"
    "<div style='color:#555;font-size:9px'>Trenne Bluetooth...</div>"
    "<div style='color:#333;font-size:9px;margin-top:4px'>Stromversorgung kann getrennt werden.</div>"
    "</body></html>");
  shutdownPending = true;
}

void doShutdown() {
  server.client().flush();
  delay(500);
  sleepReq = true;
  unsigned long t = millis();
  while (!bleIdle && millis() - t < 3000) delay(20);
  digitalWrite(ALERT_LED_PIN, LOW);
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  esp_deep_sleep_start();
}

// ═══════════════════════════════════════════════════════════════════════════
// SETUP / LOOP
// ═══════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("--- Meshtastic PSP BT (UI v24) ---");

  dataMux = xSemaphoreCreateRecursiveMutex();

  randomSeed(esp_random());
  gPacketIdCounter = (esp_random() % 0x3FFFFFFFUL) + 0x10000UL;

  pinMode(ALERT_LED_PIN, OUTPUT);
  digitalWrite(ALERT_LED_PIN, LOW);

  memset(messages, 0, sizeof(messages));
  memset(nodeDB, 0, sizeof(nodeDB));
  memset(&traceState, 0, sizeof(traceState));
  gRefMs = millis();

  prefs.begin("mesh_bt", false);
  if (!prefs.isKey("nvs_init")) {
    prefs.putBool("nvs_init", true);
    prefs.putString("wifi_ssid", AP_SSID);
    prefs.putBool("wifi_hidden", false);
    Serial.println("[NVS] Erstinitialisierung");
  }
  loadPresets();
  loadWifiSettings();
  loadUiMode();
  secInit();

  // gespeichertes BT-Geraet
  {
    String a = prefs.getString("bt_addr", "");
    uint8_t t = prefs.getUChar("bt_type", 0);
    if (a.length() == 17) {
      strncpy(tgtAddr, a.c_str(), sizeof(tgtAddr) - 1);
      tgtType = t; tgtValid = true; autoRetry = true;
      setState("Verbinde mit gespeichertem Geraet ...");
    } else {
      setState("Bereit - im BT-Tab scannen und Geraet auswaehlen");
    }
  }

  // Offenes WLAN
  WiFi.onEvent(wifiEventCallback);
  WiFi.mode(WIFI_AP);
  IPAddress local_IP(192,168,4,1), gateway(192,168,4,1), subnet(255,255,255,0);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP(wifiSSID, nullptr, 1, wifiHidden ? 1 : 0, 1);
  esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B);
  Serial.printf("AP '%s' gestartet, IP: %s\n", wifiSSID, WiFi.softAPIP().toString().c_str());
  dns.start(53, "*", WiFi.softAPIP());

  server.on("/",             handleShell);
  server.on("/tabs",         handleTabs);
  server.on("/chat",         handleChat);
  server.on("/pnlist",       handlePNList);
  server.on("/pnconv",       handlePNConv);
  server.on("/nodes",        handleNodeList);
  server.on("/refreshnodes", handleRefreshNodes);
  server.on("/tele",         handleTelemetry);
  server.on("/fav",          handleToggleFav);
  server.on("/clear",        handleClear);
  server.on("/send",         HTTP_POST, handleSend);
  server.on("/send",         HTTP_GET,  handleSend);
  server.on("/settings",     handleSettings);
  server.on("/settingssave", HTTP_POST, handleSettingsSave);
  server.on("/wifisave",     HTTP_POST, handleWifiSave);
  server.on("/uimode",       handleUiMode);
  server.on("/sec_mode",     handleSecMode);
  server.on("/sec_wl_rm",    handleSecWlRemove);
  server.on("/trace",        handleTrace);
  server.on("/bt",           handleBt);
  server.on("/btscan",       handleBtScan);
  server.on("/btconnect",    handleBtConnect);
  server.on("/btdisc",       handleBtDisc);
  server.on("/btpin",        HTTP_POST, handleBtPin);
  server.on("/shutdown",     handleShutdown);
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();

  // BLE
  NimBLEDevice::init("ESP32-Gateway");
  NimBLEDevice::setMTU(512);
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);

  xTaskCreatePinnedToCore(bleTask, "ble", 10240, nullptr, 1, nullptr, 0);
}

void loop() {
  dns.processNextRequest();
  {
    Lk l;
    server.handleClient();

    if (traceState.pending && (millis() - traceState.sentMs > TRACE_COOLDOWN_MS)) {
      traceState.pending = false; traceState.timedOut = true;
      traceState.hasResult = false; Serial.println("[TRACE] Timeout");
    }

    static unsigned long lastSecCheck = 0;
    if (millis() - lastSecCheck > 8000) {
      lastSecCheck = millis();
      if (secWhitelistMode && strlen(secCurrentMac) > 0 && !secClientKnown)
        secAlertBlink(2);
    }
  }
  if (shutdownPending) doShutdown();
  delay(2);
}
