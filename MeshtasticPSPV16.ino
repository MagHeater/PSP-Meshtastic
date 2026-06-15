// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  Meshtastic PSP  —  v16                                                  ║
// ║  Erstellt:  2026-06-14                                                   ║
// ║  Timestamp: 2026-06-14T14:00:00Z                                         ║
// ║                                                                          ║
// ║  Änderungen gegenüber v15:                                               ║
// ║                                                                          ║
// ║  [TABS] Grauer Streifen komplett entfernt —                              ║
// ║         Frameset rows='62,*', Tab-Body height=56px                       ║
// ║                                                                          ║
// ║  [PRESETS] Ein globaler Toggle-Button für ALLE Preset-Buttons            ║
// ║  (statt CH1/CH2-Checkbox pro Slot)                                       ║
// ║                                                                          ║
// ║  [SECURITY] History der letzten 5 Verbindungen/Versuche                 ║
// ║  (inkl. geblockte), kein einzelner "letzter Client" mehr                 ║
// ║                                                                          ║
// ║  Board:  AI-Thinker ESP32-CAM                                            ║
// ║  Serial: Serial2 RX=13 TX=12  @38400                                     ║
// ╚══════════════════════════════════════════════════════════════════════════╝

#include <WiFi.h>
#include <WebServer.h>
#include <esp_wifi.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <meshtastic/mesh.pb.h>
#include <meshtastic/portnums.pb.h>
#include <Preferences.h>
#include "esp_camera.h"

WebServer   server(80);
Preferences prefs;

// SSID/Hidden aus NVS geladen (kein Passwort)
char wifiSSID[33]     = "XIAO_MONITOR";
bool wifiHidden       = false;

// ═══════════════════════════════════════════════════════════════════════════
// KAMERA PINS  (AI-Thinker ESP32-CAM)
// ═══════════════════════════════════════════════════════════════════════════
#define CAM_PIN_PWDN    32
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK     0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27
#define CAM_PIN_D7      35
#define CAM_PIN_D6      34
#define CAM_PIN_D5      39
#define CAM_PIN_D4      36
#define CAM_PIN_D3      21
#define CAM_PIN_D2      19
#define CAM_PIN_D1      18
#define CAM_PIN_D0       5
#define CAM_PIN_VSYNC   25
#define CAM_PIN_HREF    23
#define CAM_PIN_PCLK    22

// Flash-LED
#define FLASH_LED_PIN    4

// ═══════════════════════════════════════════════════════════════════════════
// FOTO-PROTOKOLL
// ═══════════════════════════════════════════════════════════════════════════
#define PHOTO_CHUNK_SIZE     64
#define PHOTO_MAX_CHUNKS     80
#define PHOTO_SEND_DELAY_MS  5000

// ═══════════════════════════════════════════════════════════════════════════
// IMGREQ — Retry fehlender Chunks
// ═══════════════════════════════════════════════════════════════════════════
#define IMGREQ_DELAY_MS   3000
#define IMGREQ_MAX_ROUNDS    2

// ═══════════════════════════════════════════════════════════════════════════
// PRESET BUTTONS  (9 Stück)
// presetCh1/presetCh2 bleiben für Settings-Kompatibilität,
// aber im Chat gibt es nur einen globalen Toggle (alle an/aus)
// ═══════════════════════════════════════════════════════════════════════════
#define PRESET_COUNT 9
char presetLabels[PRESET_COUNT][24];
char presetTexts[PRESET_COUNT][64];
bool presetCh1[PRESET_COUNT];
bool presetCh2[PRESET_COUNT];

// ═══════════════════════════════════════════════════════════════════════════
// CHAT
// ═══════════════════════════════════════════════════════════════════════════
#define MAX_MESSAGES   60
#define MAX_MSG_LEN    160

struct ChatMessage {
  uint32_t senderId;
  uint32_t targetId;
  char     senderName[20];
  char     text[MAX_MSG_LEN];
  char     channelName[20];
  uint8_t  channelIdx;   // 0=CH1  1=CH2  2=PN  99=gelöscht
};
ChatMessage messages[MAX_MESSAGES];
int messageCount = 0;

char channelPrimary[20]   = "Primary";
char channelSecondary[20] = "Secondary";

// ═══════════════════════════════════════════════════════════════════════════
// NODE CACHE
// ═══════════════════════════════════════════════════════════════════════════
#define MAX_CACHED_NODES 120
struct NodeCache {
  uint32_t      num;
  char          longName[20];
  char          shortName[8];
  float         snr;
  bool          hasSnr;
  unsigned long lastSeen;
  float         battery;
  float         temperature;
  float         humidity;
  bool          hasTelemetry;
  bool          isFavorite;
};
NodeCache nodeDB[MAX_CACHED_NODES];
int nodeCacheCount = 0;

// ═══════════════════════════════════════════════════════════════════════════
// FOTO — SENDEN (normal)
// ═══════════════════════════════════════════════════════════════════════════
struct PhotoSendJob {
  bool          active;
  uint32_t      targetNode;
  uint16_t      sessionId;
  uint8_t*      jpegData;
  size_t        jpegLen;
  int           totalChunks;
  int           nextChunk;
  unsigned long lastSentMs;
  uint16_t      imgW;
  uint16_t      imgH;
};
PhotoSendJob photoJob = {false,0,0,nullptr,0,0,0,0,0,0};

// ═══════════════════════════════════════════════════════════════════════════
// IMGREQ — Retry-Job
// ═══════════════════════════════════════════════════════════════════════════
#define IMGREQ_MAX_MISSING  PHOTO_MAX_CHUNKS

struct RetryJob {
  bool          active;
  uint32_t      targetNode;
  uint16_t      sessionId;
  uint8_t*      jpegData;
  size_t        jpegLen;
  int           totalChunks;
  uint16_t      imgW;
  uint16_t      imgH;
  int           missingList[IMGREQ_MAX_MISSING];
  int           missingCount;
  int           nextIdx;
  unsigned long lastSentMs;
  int           roundsDone;
};
RetryJob retryJob = {false,0,0,nullptr,0,0,0,0,{},0,0,0,0};

// ═══════════════════════════════════════════════════════════════════════════
// FOTO — EMPFANGEN
// ═══════════════════════════════════════════════════════════════════════════
#define MAX_RECV_SESSIONS 4

struct PhotoRecvSession {
  bool      active;
  uint32_t  senderNode;
  uint16_t  sessionId;
  int       totalChunks;
  uint16_t  imgW, imgH;
  uint8_t*  chunkData[PHOTO_MAX_CHUNKS];
  bool      chunkReceived[PHOTO_MAX_CHUNKS];
  int       receivedCount;
  bool      complete;
  bool      reqSent;
  int       reqRounds;
  uint8_t*  assembledJpeg;
  size_t    assembledLen;
};
PhotoRecvSession recvSessions[MAX_RECV_SESSIONS];

// ═══════════════════════════════════════════════════════════════════════════
// SECURITY — MAC-Whitelist + History der letzten 5 Clients/Versuche
// ═══════════════════════════════════════════════════════════════════════════
#define SEC_MAX_WHITELIST   8
#define SEC_ALERT_BLINKS    4
#define SEC_HISTORY_SIZE    5   // letzte 5 Verbindungen/Versuche

struct SecEntry {
  char mac[18];
  bool active;
};

struct SecHistoryEntry {
  char     mac[18];
  char     ip[16];
  bool     allowed;
  uint32_t timestamp;   // millis()/1000
  bool     valid;
};

SecEntry        secWhitelist[SEC_MAX_WHITELIST];
int             secWhitelistCount  = 0;
bool            secWhitelistMode   = false;

// Ring-History der letzten 5 Verbindungen (inkl. geblockte Versuche)
SecHistoryEntry secHistory[SEC_HISTORY_SIZE];
int             secHistoryHead     = 0;   // nächster Schreibindex

char      secCurrentMac[18]  = "";
char      secCurrentIp[16]   = "";
bool      secClientKnown     = false;

// ═══════════════════════════════════════════════════════════════════════════
// KAMERA STATE
// ═══════════════════════════════════════════════════════════════════════════
uint8_t* capturedJpeg    = nullptr;
size_t   capturedJpegLen = 0;
uint16_t capturedW       = 0;
uint16_t capturedH       = 0;
bool     cameraReady     = false;
uint32_t activePnPartner = 0;

uint8_t camQuality = 0;

// ═══════════════════════════════════════════════════════════════════════════
// SERIAL / HEARTBEAT
// ═══════════════════════════════════════════════════════════════════════════
#define MAX_PACKET_LEN 512
uint8_t       packetBuf[MAX_PACKET_LEN];
unsigned long lastHeartbeat            = 0;
const unsigned long HEARTBEAT_INTERVAL = 5000;
String activeView = "ch1";

// ═══════════════════════════════════════════════════════════════════════════
// VORWÄRTSDEKLARATIONEN
// ═══════════════════════════════════════════════════════════════════════════
void handleShell();       void handleTabs();        void handleChat();
void handlePNList();      void handlePNConv();      void handleNodeList();
void handleTelemetry();   void handleClear();       void handleSend();
void handleToggleFav();   void handleRefreshNodes();
void handleCamera();      void handleCamSnap();     void handleCamPreview();
void handleCamJpeg();     void handleCamSend();     void handlePhotoView();
void handleRecvJpeg();    void handleSettings();    void handleSettingsSave();
void handleSecMode();     void handleSecWlRemove();
void handleWifiSave();
void processFromRadio(uint8_t* buf, uint16_t len);
void processPhotoChunk(uint32_t senderNode, const char* msg);
void processPhotoDone(uint32_t senderNode, const char* msg);
void processImgReq(uint32_t senderNode, const char* msg);
void sendImgReqIfNeeded(PhotoRecvSession* s);
void sendToRadio(const char* text, uint32_t targetDst, uint8_t channelIdx);
void tickPhotoSend();
void tickRetryJob();
void initSerialConnection(); void sendWantConfig(); void sendHeartbeat();
const char* resolveNodeName(uint32_t nodeNum);
void cacheNode(uint32_t num, const char* longName, const char* shortName);
NodeCache* findNode(uint32_t num);
String escHtml(const char* s);   String frameCss();   void sortNodes();
bool initCamera(uint8_t quality);
void saveFavorites(); void loadFavorites(); bool isSavedFavorite(uint32_t num);
void savePresets();   void loadPresets();
void secInit();       void secSave();
bool secCheckMAC(const char* mac);
bool secMACinWhitelist(const char* mac);
void secAlertBlink(int n);
void secOnConnect(const String& mac, const String& ip);
void secOnDisconnect(const String& mac);
void secHistoryPush(const char* mac, const char* ip, bool allowed);
void secHistorySave(); void secHistoryLoad();
void loadWifiSettings(); void saveWifiSettings(); void applyWifiSettings();
PhotoRecvSession* findOrCreateRecvSession(uint32_t sn, uint16_t sid, int tot, uint16_t w, uint16_t h);
PhotoRecvSession* findRecvSessionByIdStr(const char* hex);
String bytesToHex(const uint8_t* data, size_t len);
bool   hexToBytes(const char* hex, uint8_t* out, size_t outMax, size_t* outLen);
int getOpenPnPartners(uint32_t* out, int maxOut);
String refreshBar(bool refreshActive, String selfUrlBase);

// ═══════════════════════════════════════════════════════════════════════════
// WIFI SETTINGS
// ═══════════════════════════════════════════════════════════════════════════
void loadWifiSettings() {
  String s = prefs.getString("wifi_ssid", "XIAO_MONITOR");
  strncpy(wifiSSID, s.c_str(), 32); wifiSSID[32] = '\0';
  wifiHidden = prefs.getBool("wifi_hidden", false);
}

void saveWifiSettings() {
  prefs.putString("wifi_ssid",   wifiSSID);
  prefs.putBool("wifi_hidden",   wifiHidden);
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

// ═══════════════════════════════════════════════════════════════════════════
// SECURITY — History-Hilfsfunktionen
// ═══════════════════════════════════════════════════════════════════════════
void secHistoryPush(const char* mac, const char* ip, bool allowed) {
  SecHistoryEntry* e = &secHistory[secHistoryHead];
  strncpy(e->mac, mac, 17); e->mac[17] = '\0';
  strncpy(e->ip,  ip,  15); e->ip[15]  = '\0';
  e->allowed   = allowed;
  e->timestamp = (uint32_t)(millis() / 1000);
  e->valid     = true;
  secHistoryHead = (secHistoryHead + 1) % SEC_HISTORY_SIZE;
  secHistorySave();
}

void secHistorySave() {
  // Speichere Head-Index
  prefs.putInt("sec_h_head", secHistoryHead);
  for (int i = 0; i < SEC_HISTORY_SIZE; i++) {
    char km[12], ki[12], ko[12], kt[12], kv[12];
    snprintf(km, 12, "sh_mac%d", i);
    snprintf(ki, 12, "sh_ip%d",  i);
    snprintf(ko, 12, "sh_ok%d",  i);
    snprintf(kt, 12, "sh_ts%d",  i);
    snprintf(kv, 12, "sh_v%d",   i);
    prefs.putString(km, secHistory[i].mac);
    prefs.putString(ki, secHistory[i].ip);
    prefs.putBool(ko,   secHistory[i].allowed);
    prefs.putUInt(kt,   secHistory[i].timestamp);
    prefs.putBool(kv,   secHistory[i].valid);
  }
}

void secHistoryLoad() {
  secHistoryHead = prefs.getInt("sec_h_head", 0);
  if (secHistoryHead < 0 || secHistoryHead >= SEC_HISTORY_SIZE) secHistoryHead = 0;
  for (int i = 0; i < SEC_HISTORY_SIZE; i++) {
    char km[12], ki[12], ko[12], kt[12], kv[12];
    snprintf(km, 12, "sh_mac%d", i);
    snprintf(ki, 12, "sh_ip%d",  i);
    snprintf(ko, 12, "sh_ok%d",  i);
    snprintf(kt, 12, "sh_ts%d",  i);
    snprintf(kv, 12, "sh_v%d",   i);
    String m = prefs.getString(km, ""); strncpy(secHistory[i].mac, m.c_str(), 17); secHistory[i].mac[17]='\0';
    String ip= prefs.getString(ki, ""); strncpy(secHistory[i].ip,  ip.c_str(),15); secHistory[i].ip[15] ='\0';
    secHistory[i].allowed   = prefs.getBool(ko,  false);
    secHistory[i].timestamp = prefs.getUInt(kt,  0);
    secHistory[i].valid     = prefs.getBool(kv,  false);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// SECURITY — Kern-Implementierung
// ═══════════════════════════════════════════════════════════════════════════
void secInit() {
  secWhitelistMode  = prefs.getBool("sec_mode", false);
  secWhitelistCount = (int)prefs.getInt("sec_wl_cnt", 0);
  if (secWhitelistCount < 0 || secWhitelistCount > SEC_MAX_WHITELIST)
    secWhitelistCount = 0;
  for (int i = 0; i < secWhitelistCount; i++) {
    char key[12]; snprintf(key, sizeof(key), "sec_wl%d", i);
    String val = prefs.getString(key, "");
    strncpy(secWhitelist[i].mac, val.c_str(), 17);
    secWhitelist[i].mac[17] = '\0';
    secWhitelist[i].active  = true;
  }
  memset(secHistory, 0, sizeof(secHistory));
  secHistoryLoad();
  Serial.printf("[SEC] Geladen: %d MACs, Modus=%s\n",
                secWhitelistCount, secWhitelistMode ? "GESPERRT" : "LERNMODUS");
}

void secSave() {
  prefs.putBool("sec_mode",   secWhitelistMode);
  prefs.putInt("sec_wl_cnt",  secWhitelistCount);
  for (int i = 0; i < secWhitelistCount; i++) {
    char key[12]; snprintf(key, sizeof(key), "sec_wl%d", i);
    prefs.putString(key, secWhitelist[i].mac);
  }
}

bool secMACinWhitelist(const char* mac) {
  for (int i = 0; i < secWhitelistCount; i++)
    if (secWhitelist[i].active && strcasecmp(secWhitelist[i].mac, mac) == 0)
      return true;
  return false;
}

bool secCheckMAC(const char* mac) {
  if (!secWhitelistMode) {
    if (!secMACinWhitelist(mac) && secWhitelistCount < SEC_MAX_WHITELIST) {
      strncpy(secWhitelist[secWhitelistCount].mac, mac, 17);
      secWhitelist[secWhitelistCount].mac[17] = '\0';
      secWhitelist[secWhitelistCount].active   = true;
      secWhitelistCount++;
      secSave();
      Serial.printf("[SEC] MAC gelernt: %s\n", mac);
    }
    return true;
  }
  return secMACinWhitelist(mac);
}

void secAlertBlink(int n) {
  for (int i = 0; i < n; i++) {
    digitalWrite(FLASH_LED_PIN, HIGH); delay(80);
    digitalWrite(FLASH_LED_PIN, LOW);  delay(80);
  }
}

void secOnConnect(const String& mac, const String& ip) {
  strncpy(secCurrentMac, mac.c_str(), 17); secCurrentMac[17] = '\0';
  strncpy(secCurrentIp,  ip.c_str(),  15); secCurrentIp[15]  = '\0';
  bool allowed = secCheckMAC(mac.c_str());

  // In History eintragen (inkl. geblockte Versuche)
  secHistoryPush(mac.c_str(), ip.c_str(), allowed);

  if (allowed) {
    secClientKnown = true;
    Serial.printf("[SEC] Client OK: %s / %s\n", mac.c_str(), ip.c_str());
  } else {
    secClientKnown = false;
    secAlertBlink(SEC_ALERT_BLINKS);
    Serial.printf("[SEC] Client GEBLOCKT: %s / %s\n", mac.c_str(), ip.c_str());
  }
}

void secOnDisconnect(const String& mac) {
  secClientKnown = false;
  memset(secCurrentMac, 0, sizeof(secCurrentMac));
  Serial.printf("[SEC] Client getrennt: %s\n", mac.c_str());
}

void wifiEventCallback(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    uint8_t* m = info.wifi_ap_staconnected.mac;
    char macBuf[18];
    snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);
    delay(300);
    String ip = "192.168.4.2";
    wifi_sta_list_t sl;
    if (esp_wifi_ap_get_sta_list(&sl) == ESP_OK) {
      for (int i = 0; i < sl.num; i++) {
        if (memcmp(sl.sta[i].mac, m, 6) == 0) {
          ip = "192.168.4." + String(2 + i);
          break;
        }
      }
    }
    secOnConnect(String(macBuf), ip);
  } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
    uint8_t* m = info.wifi_ap_stadisconnected.mac;
    char macBuf[18];
    snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);
    secOnDisconnect(String(macBuf));
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// SETUP / LOOP
// ═══════════════════════════════════════════════════════════════════════════
void setup() {
    if(esp_reset_reason() == ESP_RST_DEEPSLEEP) {
    delay(500);
    ESP.restart();
  }
  delay(2000);
  
  Serial.begin(115200);
  Serial2.begin(38400, SERIAL_8N1, 13, 12);
  delay(1500);
  Serial.println("--- Meshtastic PSP v16 (2026-06-14T14:00:00Z) ---");

  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  memset(recvSessions, 0, sizeof(recvSessions));
  prefs.begin("mesh_psp", false);
  loadFavorites();
  loadPresets();
  loadWifiSettings();
  secInit();

  cameraReady = initCamera(camQuality);
  Serial.println(cameraReady ? "Kamera OK" : "Kamera FEHLER");

  WiFi.onEvent(wifiEventCallback);
  WiFi.mode(WIFI_AP);
  IPAddress local_IP(192,168,4,1), gateway(192,168,4,1), subnet(255,255,255,0);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP(wifiSSID, nullptr, 1, wifiHidden ? 1 : 0, 1);
  esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B);

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
  server.on("/cam",          handleCamera);
  server.on("/camsnap",      handleCamSnap);
  server.on("/campreview",   handleCamPreview);
  server.on("/camjpeg",      handleCamJpeg);
  server.on("/camsend",      handleCamSend);
  server.on("/photoview",    handlePhotoView);
  server.on("/recvjpeg",     handleRecvJpeg);
  server.on("/settings",     handleSettings);
  server.on("/settingssave", HTTP_POST, handleSettingsSave);
  server.on("/wifisave",     HTTP_POST, handleWifiSave);
  server.on("/sec_mode",     [](){ handleSecMode(); });
  server.on("/sec_wl_rm",    [](){ handleSecWlRemove(); });
server.on("/shutdown", [](){
  server.send(200, "text/html; charset=utf-8",
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<style>html,body{background:#000;color:#ccc;font-family:monospace;"
    "padding:8px;font-size:11px}</style></head><body>"
    "<div style='color:#ff4444;margin-bottom:6px'>ESP32 wird heruntergefahren...</div>"
    "<div style='color:#333;font-size:9px'>Stromversorgung kann getrennt werden.</div>"
    "</body></html>");
  server.client().flush();
  delay(500);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_camera_deinit();
  digitalWrite(CAM_PIN_PWDN, HIGH);
  digitalWrite(FLASH_LED_PIN, LOW);
  btStop();
  esp_deep_sleep_start();
});
  server.begin();

  initSerialConnection();
}

void loop() {
  server.handleClient();
  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    sendHeartbeat();
    lastHeartbeat = millis();
  }
  tickPhotoSend();
  tickRetryJob();

  static unsigned long lastSecCheck = 0;
  if (millis() - lastSecCheck > 8000) {
    lastSecCheck = millis();
    if (secWhitelistMode && strlen(secCurrentMac) > 0 && !secClientKnown)
      secAlertBlink(2);
  }

  static uint8_t  state = 0;
  static uint16_t expectedLen = 0, bufPos = 0;
  while (Serial2.available()) {
    uint8_t b = Serial2.read();
    switch (state) {
      case 0: if (b==0x94) state=1; break;
      case 1: if (b==0xC3) state=2; else state=0; break;
      case 2: expectedLen=(uint16_t)b<<8; state=3; break;
      case 3:
        expectedLen|=b; bufPos=0;
        state=(expectedLen==0||expectedLen>MAX_PACKET_LEN)?0:4;
        break;
      case 4:
        packetBuf[bufPos++]=b;
        if (bufPos>=expectedLen){state=0; processFromRadio(packetBuf,expectedLen);}
        break;
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// KAMERA INIT
// ═══════════════════════════════════════════════════════════════════════════
bool initCamera(uint8_t quality) {
  esp_camera_deinit();
  delay(150);
  framesize_t fs; int jpegQ;
  switch (quality) {
    case 1:  fs=FRAMESIZE_QVGA;  jpegQ=25; break;
    case 2:  fs=FRAMESIZE_QQVGA; jpegQ=30; break;
    default: fs=FRAMESIZE_QVGA;  jpegQ=12; break;
  }
  camera_config_t cfg;
  cfg.ledc_channel  = LEDC_CHANNEL_0;  cfg.ledc_timer    = LEDC_TIMER_0;
  cfg.pin_d0=CAM_PIN_D0; cfg.pin_d1=CAM_PIN_D1; cfg.pin_d2=CAM_PIN_D2;
  cfg.pin_d3=CAM_PIN_D3; cfg.pin_d4=CAM_PIN_D4; cfg.pin_d5=CAM_PIN_D5;
  cfg.pin_d6=CAM_PIN_D6; cfg.pin_d7=CAM_PIN_D7;
  cfg.pin_xclk=CAM_PIN_XCLK;   cfg.pin_pclk=CAM_PIN_PCLK;
  cfg.pin_vsync=CAM_PIN_VSYNC;  cfg.pin_href=CAM_PIN_HREF;
  cfg.pin_sscb_sda=CAM_PIN_SIOD; cfg.pin_sscb_scl=CAM_PIN_SIOC;
  cfg.pin_pwdn=CAM_PIN_PWDN;    cfg.pin_reset=CAM_PIN_RESET;
  cfg.xclk_freq_hz=20000000;
  cfg.pixel_format=PIXFORMAT_JPEG;
  cfg.frame_size=fs; cfg.jpeg_quality=jpegQ; cfg.fb_count=1;
  if (esp_camera_init(&cfg) != ESP_OK) return false;
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_whitebal(s,1); s->set_awb_gain(s,1); s->set_wb_mode(s,0);
    s->set_exposure_ctrl(s,1); s->set_aec2(s,1); s->set_ae_level(s,1);
    s->set_gain_ctrl(s,1); s->set_agc_gain(s,0); s->set_gainceiling(s,(gainceiling_t)2);
    s->set_bpc(s,1); s->set_wpc(s,1); s->set_raw_gma(s,1); s->set_lenc(s,1);
    s->set_brightness(s,1); s->set_saturation(s,1); s->set_contrast(s,0);
    s->set_sharpness(s,1); s->set_denoise(s,1);
    s->set_special_effect(s,0);
  }
  delay(300);
  camera_fb_t* w1=esp_camera_fb_get(); if(w1){esp_camera_fb_return(w1);delay(100);}
  camera_fb_t* w2=esp_camera_fb_get(); if(w2){esp_camera_fb_return(w2);delay(100);}
  return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// FAVORITEN
// ═══════════════════════════════════════════════════════════════════════════
void saveFavorites() {
  String s="";
  for(int i=0;i<nodeCacheCount;i++){
    if(nodeDB[i].isFavorite){if(s.length()>0)s+=",";s+=String(nodeDB[i].num);}
  }
  prefs.putString("favs",s);
}

void loadFavorites() {
  Serial.printf("[Favs] %s\n", prefs.getString("favs","").c_str());
}

bool isSavedFavorite(uint32_t num) {
  String favStr=prefs.getString("favs","");
  if(favStr.length()==0) return false;
  String numStr=String(num); int pos=0;
  while(pos<(int)favStr.length()){
    int comma=favStr.indexOf(',',pos);
    String token=(comma==-1)?favStr.substring(pos):favStr.substring(pos,comma);
    if(token==numStr) return true;
    if(comma==-1) break;
    pos=comma+1;
  }
  return false;
}

// ═══════════════════════════════════════════════════════════════════════════
// PRESET BUTTONS — NVS
// ═══════════════════════════════════════════════════════════════════════════
void savePresets() {
  for(int i=0;i<PRESET_COUNT;i++){
    char key[12]; snprintf(key,sizeof(key),"prs%d",i);
    prefs.putString(key, presetTexts[i]);
    char key2[12]; snprintf(key2,sizeof(key2),"prc%d",i);
    uint8_t flags = (presetCh1[i]?1:0) | (presetCh2[i]?2:0);
    prefs.putUChar(key2, flags);
  }
}

void loadPresets() {
  const char* defaults[PRESET_COUNT]={"go","stp","wg2","arm","drm","","",""};
  for(int i=0;i<PRESET_COUNT;i++){
    char key[12]; snprintf(key,sizeof(key),"prs%d",i);
    String val=prefs.getString(key,defaults[i]);
    strncpy(presetTexts[i],val.c_str(),63);  presetTexts[i][63]='\0';
    strncpy(presetLabels[i],presetTexts[i],23); presetLabels[i][23]='\0';
    char key2[12]; snprintf(key2,sizeof(key2),"prc%d",i);
    uint8_t flags=prefs.getUChar(key2, 3);  // default: beide Kanäle
    presetCh1[i] = (flags & 1) != 0;
    presetCh2[i] = (flags & 2) != 0;
    if(!presetCh1[i]&&!presetCh2[i]){ presetCh1[i]=true; presetCh2[i]=true; }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// RADIO
// ═══════════════════════════════════════════════════════════════════════════
void initSerialConnection(){
  for(int i=0;i<32;i++) Serial2.write(0xC3);
  Serial2.flush(); delay(200); sendWantConfig();
}

void sendWantConfig(){
  meshtastic_ToRadio tr=meshtastic_ToRadio_init_zero;
  tr.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;
  tr.want_config_id=424242;
  uint8_t buf[64]; pb_ostream_t s=pb_ostream_from_buffer(buf,sizeof(buf));
  if(pb_encode(&s,meshtastic_ToRadio_fields,&tr)){
    Serial2.write(0x94); Serial2.write(0xC3);
    Serial2.write((s.bytes_written>>8)&0xFF); Serial2.write(s.bytes_written&0xFF);
    Serial2.write(buf,s.bytes_written); Serial2.flush();
  }
}

void sendHeartbeat(){
  meshtastic_ToRadio tr=meshtastic_ToRadio_init_zero;
  tr.which_payload_variant=meshtastic_ToRadio_heartbeat_tag;
  tr.heartbeat=meshtastic_Heartbeat_init_zero;
  uint8_t buf[16]; pb_ostream_t s=pb_ostream_from_buffer(buf,sizeof(buf));
  if(pb_encode(&s,meshtastic_ToRadio_fields,&tr)){
    Serial2.write(0x94); Serial2.write(0xC3);
    Serial2.write((s.bytes_written>>8)&0xFF); Serial2.write(s.bytes_written&0xFF);
    Serial2.write(buf,s.bytes_written); Serial2.flush();
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// NODE CACHE
// ═══════════════════════════════════════════════════════════════════════════
void cacheNode(uint32_t num,const char* longName,const char* shortName){
  for(int i=0;i<nodeCacheCount;i++){
    if(nodeDB[i].num==num){
      if(longName&&strlen(longName)>0){strncpy(nodeDB[i].longName,longName,19);nodeDB[i].longName[19]='\0';}
      if(shortName&&strlen(shortName)>0){strncpy(nodeDB[i].shortName,shortName,7);nodeDB[i].shortName[7]='\0';}
      nodeDB[i].lastSeen=millis(); return;
    }
  }
  if(nodeCacheCount<MAX_CACHED_NODES){
    NodeCache* n=&nodeDB[nodeCacheCount++];
    n->num=num;
    strncpy(n->longName,longName?longName:"",19);   n->longName[19]='\0';
    strncpy(n->shortName,shortName?shortName:"",7); n->shortName[7]='\0';
    n->hasSnr=false; n->snr=0; n->hasTelemetry=false;
    n->battery=0; n->temperature=0; n->humidity=0;
    n->lastSeen=millis(); n->isFavorite=isSavedFavorite(num);
  }
}

NodeCache* findNode(uint32_t num){
  for(int i=0;i<nodeCacheCount;i++) if(nodeDB[i].num==num) return &nodeDB[i];
  return nullptr;
}

const char* resolveNodeName(uint32_t nodeNum){
  static char hb[12]; NodeCache* n=findNode(nodeNum);
  if(n&&strlen(n->longName)>0) return n->longName;
  snprintf(hb,sizeof(hb),"0x%08X",nodeNum); return hb;
}

void sortNodes(){
  for(int i=0;i<nodeCacheCount-1;i++)
    for(int j=i+1;j<nodeCacheCount;j++){
      bool sw=(nodeDB[j].isFavorite&&!nodeDB[i].isFavorite)||
              (nodeDB[j].isFavorite==nodeDB[i].isFavorite&&nodeDB[j].lastSeen>nodeDB[i].lastSeen);
      if(sw){NodeCache t=nodeDB[i];nodeDB[i]=nodeDB[j];nodeDB[j]=t;}
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// OFFENE PN-PARTNER
// ═══════════════════════════════════════════════════════════════════════════
int getOpenPnPartners(uint32_t* out, int maxOut){
  int count=0;
  int start=(messageCount>MAX_MESSAGES)?messageCount%MAX_MESSAGES:0;
  int total=min(messageCount,MAX_MESSAGES);
  for(int i=0;i<total&&count<maxOut;i++){
    int idx=(start+i)%MAX_MESSAGES;
    if(messages[idx].channelIdx!=2) continue;
    uint32_t p=(messages[idx].senderId==0)?messages[idx].targetId:messages[idx].senderId;
    if(p==0||p==0xFFFFFFFF) continue;
    bool exists=false;
    for(int j=0;j<count;j++) if(out[j]==p){exists=true;break;}
    if(!exists) out[count++]=p;
  }
  return count;
}

// ═══════════════════════════════════════════════════════════════════════════
// FAVORITEN TOGGLE
// ═══════════════════════════════════════════════════════════════════════════
void handleToggleFav(){
  if(server.hasArg("node")){
    uint32_t id=strtoul(server.arg("node").c_str(),NULL,10);
    NodeCache* n=findNode(id);
    if(n){n->isFavorite=!n->isFavorite; saveFavorites();}
  }
  String from=server.hasArg("from")?server.arg("from"):"nodes";
  if(from=="tele")       server.sendHeader("Location","/tele");
  else if(from=="tnode") server.sendHeader("Location","/tele?node="+server.arg("node"));
  else                   server.sendHeader("Location","/nodes");
  server.send(303);
}

// ═══════════════════════════════════════════════════════════════════════════
// HEX
// ═══════════════════════════════════════════════════════════════════════════
String bytesToHex(const uint8_t* data,size_t len){
  String out; out.reserve(len*2);
  const char h[]="0123456789ABCDEF";
  for(size_t i=0;i<len;i++){out+=h[(data[i]>>4)&0xF];out+=h[data[i]&0xF];}
  return out;
}

bool hexToBytes(const char* hex,uint8_t* out,size_t outMax,size_t* outLen){
  size_t hl=strlen(hex); if(hl%2!=0) return false;
  size_t bytes=hl/2; if(bytes>outMax) return false;
  for(size_t i=0;i<bytes;i++){
    auto hv=[](char c)->int{
      if(c>='0'&&c<='9') return c-'0';
      if(c>='A'&&c<='F') return c-'A'+10;
      if(c>='a'&&c<='f') return c-'a'+10;
      return -1;
    };
    int hi=hv(hex[i*2]),lo=hv(hex[i*2+1]);
    if(hi<0||lo<0) return false;
    out[i]=(uint8_t)((hi<<4)|lo);
  }
  *outLen=bytes; return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// FOTO EMPFANG — SESSION MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════════
PhotoRecvSession* findOrCreateRecvSession(uint32_t sn,uint16_t sid,int tot,uint16_t w,uint16_t h){
  for(int i=0;i<MAX_RECV_SESSIONS;i++)
    if(recvSessions[i].active&&recvSessions[i].senderNode==sn&&recvSessions[i].sessionId==sid)
      return &recvSessions[i];
  for(int i=0;i<MAX_RECV_SESSIONS;i++){
    if(!recvSessions[i].active){
      PhotoRecvSession* s=&recvSessions[i];
      memset(s,0,sizeof(PhotoRecvSession));
      s->active=true; s->senderNode=sn; s->sessionId=sid;
      s->totalChunks=tot; s->imgW=w; s->imgH=h;
      s->reqSent=false; s->reqRounds=0;
      return s;
    }
  }
  return nullptr;
}

PhotoRecvSession* findRecvSessionByIdStr(const char* sx){
  uint16_t id=(uint16_t)strtoul(sx,nullptr,16);
  for(int i=0;i<MAX_RECV_SESSIONS;i++)
    if(recvSessions[i].active&&recvSessions[i].sessionId==id) return &recvSessions[i];
  return nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════
// IMGREQ
// ═══════════════════════════════════════════════════════════════════════════
void sendImgReqIfNeeded(PhotoRecvSession* s) {
  if (!s || s->complete || s->reqRounds >= IMGREQ_MAX_ROUNDS) return;
  if (s->receivedCount >= s->totalChunks) return;
  char req[MAX_MSG_LEN];
  char sessionStr[8]; snprintf(sessionStr, sizeof(sessionStr), "%04X", s->sessionId);
  int written = snprintf(req, sizeof(req), "IMGREQ:%s:", sessionStr);
  bool first = true; int missingCount = 0;
  for (int i = 0; i < s->totalChunks && written < (int)sizeof(req)-6; i++) {
    if (!s->chunkReceived[i]) {
      if (!first) req[written++] = ',';
      written += snprintf(req + written, sizeof(req) - written, "%d", i);
      first = false; missingCount++;
    }
  }
  req[written] = '\0';
  if (missingCount == 0) return;
  s->reqSent = true; s->reqRounds++;
  Serial.printf("[IMGREQ] %d fehlende Chunks, Runde %d\n", missingCount, s->reqRounds);
  sendToRadio(req, s->senderNode, 0);
  int idx = messageCount % MAX_MESSAGES;
  char note[MAX_MSG_LEN];
  snprintf(note, sizeof(note), "[IMGREQ] %d fehlende Chunks angefragt (Runde %d/%d)",
           missingCount, s->reqRounds, IMGREQ_MAX_ROUNDS);
  strncpy(messages[idx].text, note, MAX_MSG_LEN-1);
  messages[idx].text[MAX_MSG_LEN-1] = '\0';
  strcpy(messages[idx].senderName, "SYS");
  messages[idx].senderId=0; messages[idx].targetId=s->senderNode;
  messages[idx].channelIdx=2;
  snprintf(messages[idx].channelName, 19, "PN>%s", resolveNodeName(s->senderNode));
  messageCount++;
}

void processImgReq(uint32_t senderNode, const char* msg) {
  if (retryJob.active) { Serial.println("[IMGREQ] Retry aktiv, ignoriere."); return; }
  if (capturedJpeg == nullptr) { Serial.println("[IMGREQ] Keine Daten."); return; }
  char buf[MAX_MSG_LEN];
  strncpy(buf, msg, MAX_MSG_LEN-1); buf[MAX_MSG_LEN-1] = '\0';
  char* p = buf + 7;
  char* tok = strsep(&p, ":"); if (!tok || !p) return;
  uint16_t sid = (uint16_t)strtoul(tok, nullptr, 16);
  retryJob.active=true; retryJob.targetNode=senderNode; retryJob.sessionId=sid;
  retryJob.jpegData=capturedJpeg; retryJob.jpegLen=capturedJpegLen;
  retryJob.totalChunks=min((int)((capturedJpegLen+PHOTO_CHUNK_SIZE-1)/PHOTO_CHUNK_SIZE),PHOTO_MAX_CHUNKS);
  retryJob.imgW=capturedW; retryJob.imgH=capturedH;
  retryJob.missingCount=0; retryJob.nextIdx=0; retryJob.lastSentMs=0; retryJob.roundsDone=0;
  char* idxStr = p; char* comma;
  while (idxStr && *idxStr && retryJob.missingCount < IMGREQ_MAX_MISSING) {
    comma = strchr(idxStr, ','); if (comma) *comma = '\0';
    int ci = atoi(idxStr);
    if (ci >= 0 && ci < retryJob.totalChunks) retryJob.missingList[retryJob.missingCount++]=ci;
    if (comma) idxStr = comma+1; else break;
  }
  Serial.printf("[IMGREQ] Retry: %d Chunks\n", retryJob.missingCount);
}

void tickRetryJob() {
  if (!retryJob.active) return;
  if (photoJob.active)  return;
  if (retryJob.nextIdx >= retryJob.missingCount) {
    char dm[64];
    snprintf(dm, sizeof(dm), "IMGDONE:%04X:%d:%dx%d",
             retryJob.sessionId, retryJob.totalChunks, retryJob.imgW, retryJob.imgH);
    sendToRadio(dm, retryJob.targetNode, 0);
    retryJob.active = false; return;
  }
  if (millis() - retryJob.lastSentMs < IMGREQ_DELAY_MS) return;
  retryJob.lastSentMs = millis();
  int idx=retryJob.missingList[retryJob.nextIdx];
  size_t offset=(size_t)idx*PHOTO_CHUNK_SIZE;
  size_t rem=retryJob.jpegLen-offset;
  size_t cl=(rem>PHOTO_CHUNK_SIZE)?PHOTO_CHUNK_SIZE:rem;
  String hc=bytesToHex(retryJob.jpegData+offset,cl);
  char cm[MAX_MSG_LEN];
  snprintf(cm,sizeof(cm),"IMG:%04X:%d:%02d:%s",retryJob.sessionId,retryJob.totalChunks,idx,hc.c_str());
  sendToRadio(cm,retryJob.targetNode,0);
  retryJob.nextIdx++;
}

// ═══════════════════════════════════════════════════════════════════════════
// FOTO EMPFANG
// ═══════════════════════════════════════════════════════════════════════════
void processPhotoChunk(uint32_t senderNode,const char* msg){
  char buf[MAX_MSG_LEN]; strncpy(buf,msg,MAX_MSG_LEN-1); buf[MAX_MSG_LEN-1]='\0';
  char* p=buf+4;
  char sessionStr[8]={0}; int total=0,idx=0;
  char hexdata[PHOTO_CHUNK_SIZE*2+4]={0};
  char* tok;
  tok=strsep(&p,":"); if(!tok) return; strncpy(sessionStr,tok,7);
  tok=strsep(&p,":"); if(!tok) return; total=atoi(tok);
  tok=strsep(&p,":"); if(!tok) return; idx=atoi(tok);
  if(!p) return; strncpy(hexdata,p,sizeof(hexdata)-1);
  if(total<=0||total>PHOTO_MAX_CHUNKS||idx<0||idx>=total) return;
  uint16_t sid=(uint16_t)strtoul(sessionStr,nullptr,16);
  PhotoRecvSession* s=findOrCreateRecvSession(senderNode,sid,total,0,0);
  if(!s||s->chunkReceived[idx]) return;
  uint8_t cb[PHOTO_CHUNK_SIZE+4]; size_t cl=0;
  if(!hexToBytes(hexdata,cb,sizeof(cb),&cl)) return;
  s->chunkData[idx]=(uint8_t*)malloc(cl);
  if(!s->chunkData[idx]) return;
  memcpy(s->chunkData[idx],cb,cl);
  s->chunkReceived[idx]=true; s->receivedCount++;
}

void processPhotoDone(uint32_t senderNode,const char* msg){
  char buf[64]; strncpy(buf,msg,63); buf[63]='\0';
  char* p=buf+8;
  char sessionStr[8]={0}; int total=0; uint16_t w=0,h=0;
  char* tok=strsep(&p,":"); if(!tok) return; strncpy(sessionStr,tok,7);
  tok=strsep(&p,":"); if(!tok) return; total=atoi(tok);
  if(p){char db[16];strncpy(db,p,15);char* dp=db;char* ws=strsep(&dp,"x");if(ws&&dp){w=atoi(ws);h=atoi(dp);}}
  uint16_t sid=(uint16_t)strtoul(sessionStr,nullptr,16);
  PhotoRecvSession* s=findOrCreateRecvSession(senderNode,sid,total,w,h);
  if(!s) return;
  s->imgW=w; s->imgH=h; s->totalChunks=total;
  if(s->receivedCount==s->totalChunks){
    size_t tb=(size_t)s->totalChunks*PHOTO_CHUNK_SIZE;
    uint8_t* asmb=(uint8_t*)malloc(tb+16);
    if(asmb){
      size_t pos=0;
      for(int i=0;i<s->totalChunks;i++)
        if(s->chunkData[i]){memcpy(asmb+pos,s->chunkData[i],PHOTO_CHUNK_SIZE);pos+=PHOTO_CHUNK_SIZE;}
      s->assembledJpeg=asmb; s->assembledLen=pos; s->complete=true;
    }
  } else {
    sendImgReqIfNeeded(s);
  }
  int idx=messageCount%MAX_MESSAGES;
  char lm[48]; snprintf(lm,sizeof(lm),"FOTO:%s",sessionStr);
  strncpy(messages[idx].text,lm,MAX_MSG_LEN-1); messages[idx].text[MAX_MSG_LEN-1]='\0';
  messages[idx].senderId=senderNode; messages[idx].targetId=0;
  strncpy(messages[idx].senderName,resolveNodeName(senderNode),19); messages[idx].senderName[19]='\0';
  messages[idx].channelIdx=2; strcpy(messages[idx].channelName,"PN");
  messageCount++;
}

// ═══════════════════════════════════════════════════════════════════════════
// FOTO SENDE-TICKER
// ═══════════════════════════════════════════════════════════════════════════
void tickPhotoSend(){
  if(!photoJob.active) return;
  if(photoJob.nextChunk>=photoJob.totalChunks){
    char dm[64];
    snprintf(dm,sizeof(dm),"IMGDONE:%04X:%d:%dx%d",
             photoJob.sessionId,photoJob.totalChunks,photoJob.imgW,photoJob.imgH);
    sendToRadio(dm,photoJob.targetNode,0);
    free(photoJob.jpegData); photoJob.jpegData=nullptr; photoJob.active=false; return;
  }
  if(millis()-photoJob.lastSentMs<PHOTO_SEND_DELAY_MS) return;
  photoJob.lastSentMs=millis();
  int idx=photoJob.nextChunk;
  size_t offset=(size_t)idx*PHOTO_CHUNK_SIZE;
  size_t rem=photoJob.jpegLen-offset;
  size_t cl=(rem>PHOTO_CHUNK_SIZE)?PHOTO_CHUNK_SIZE:rem;
  String hc=bytesToHex(photoJob.jpegData+offset,cl);
  char cm[MAX_MSG_LEN];
  snprintf(cm,sizeof(cm),"IMG:%04X:%d:%02d:%s",photoJob.sessionId,photoJob.totalChunks,idx,hc.c_str());
  sendToRadio(cm,photoJob.targetNode,0);
  photoJob.nextChunk++;
}

// ═══════════════════════════════════════════════════════════════════════════
// PROTOBUF EMPFANG
// ═══════════════════════════════════════════════════════════════════════════
void processFromRadio(uint8_t* buf,uint16_t len){
  meshtastic_FromRadio fr=meshtastic_FromRadio_init_zero;
  pb_istream_t stream=pb_istream_from_buffer(buf,len);
  if(!pb_decode(&stream,meshtastic_FromRadio_fields,&fr)) return;
  if(fr.which_payload_variant==meshtastic_FromRadio_node_info_tag){
    meshtastic_NodeInfo ni=fr.node_info;
    if(ni.has_user) cacheNode(ni.num,ni.user.long_name,ni.user.short_name);
  }
  if(fr.which_payload_variant==meshtastic_FromRadio_channel_tag){
    meshtastic_Channel ch=fr.channel;
    if(ch.has_settings&&strlen(ch.settings.name)>0){
      if(ch.index==0){strncpy(channelPrimary,ch.settings.name,19);channelPrimary[19]='\0';}
      if(ch.index==1){strncpy(channelSecondary,ch.settings.name,19);channelSecondary[19]='\0';}
    }
  }
  if(fr.which_payload_variant==meshtastic_FromRadio_packet_tag){
    meshtastic_MeshPacket pkt=fr.packet;
    NodeCache* sn=findNode(pkt.from);
    if(!sn){cacheNode(pkt.from,"","");sn=findNode(pkt.from);}
    if(sn){sn->snr=pkt.rx_snr;sn->hasSnr=true;sn->lastSeen=millis();}
    if(pkt.which_payload_variant==meshtastic_MeshPacket_decoded_tag){
      meshtastic_Data data=pkt.decoded;
      if(data.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP){
        char tb[MAX_MSG_LEN];
        int tl=min((int)data.payload.size,MAX_MSG_LEN-1);
        memcpy(tb,data.payload.bytes,tl); tb[tl]='\0';
        if(strncmp(tb,"IMG:",4)==0){processPhotoChunk(pkt.from,tb);return;}
        if(strncmp(tb,"IMGDONE:",8)==0){processPhotoDone(pkt.from,tb);return;}
        if(strncmp(tb,"IMGREQ:",7)==0){processImgReq(pkt.from,tb);return;}
        if(tl>0){
          int idx=messageCount%MAX_MESSAGES;
          strncpy(messages[idx].text,tb,MAX_MSG_LEN-1); messages[idx].text[MAX_MSG_LEN-1]='\0';
          messages[idx].senderId=pkt.from; messages[idx].targetId=pkt.to;
          strncpy(messages[idx].senderName,resolveNodeName(pkt.from),19); messages[idx].senderName[19]='\0';
          if(pkt.to!=0xFFFFFFFF){messages[idx].channelIdx=2;strcpy(messages[idx].channelName,"PN");}
          else{
            messages[idx].channelIdx=(pkt.channel==1)?1:0;
            strncpy(messages[idx].channelName,(pkt.channel==1)?channelSecondary:channelPrimary,19);
            messages[idx].channelName[19]='\0';
          }
          messageCount++;
        }
      }
      if(data.portnum==meshtastic_PortNum_TELEMETRY_APP){
        meshtastic_Telemetry tele=meshtastic_Telemetry_init_zero;
        pb_istream_t ts=pb_istream_from_buffer(data.payload.bytes,data.payload.size);
        if(pb_decode(&ts,meshtastic_Telemetry_fields,&tele)){
          NodeCache* n=findNode(pkt.from);
          if(!n){cacheNode(pkt.from,"","");n=findNode(pkt.from);}
          if(n){
            n->lastSeen=millis();
            if(tele.which_variant==meshtastic_Telemetry_device_metrics_tag)
              {n->battery=tele.variant.device_metrics.battery_level;n->hasTelemetry=true;}
            if(tele.which_variant==meshtastic_Telemetry_environment_metrics_tag)
              {n->temperature=tele.variant.environment_metrics.temperature;
               n->humidity=tele.variant.environment_metrics.relative_humidity;n->hasTelemetry=true;}
          }
        }
      }
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// SENDEN
// ═══════════════════════════════════════════════════════════════════════════
void sendToRadio(const char* text,uint32_t targetDst,uint8_t channelIdx){
  meshtastic_ToRadio    tr=meshtastic_ToRadio_init_zero;
  meshtastic_MeshPacket pkt=meshtastic_MeshPacket_init_zero;
  meshtastic_Data       data=meshtastic_Data_init_zero;
  data.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
  data.payload.size=min(strlen(text),sizeof(data.payload.bytes));
  memcpy(data.payload.bytes,text,data.payload.size);
  pkt.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  pkt.decoded=data; pkt.to=targetDst;
  pkt.channel=(targetDst==0xFFFFFFFF)?channelIdx:0; pkt.want_ack=false;
  tr.which_payload_variant=meshtastic_ToRadio_packet_tag; tr.packet=pkt;
  uint8_t outBuf[MAX_PACKET_LEN];
  pb_ostream_t s=pb_ostream_from_buffer(outBuf,sizeof(outBuf));
  if(!pb_encode(&s,meshtastic_ToRadio_fields,&tr)) return;
  uint16_t outLen=s.bytes_written;
  Serial2.write(0x94); Serial2.write(0xC3);
  Serial2.write((outLen>>8)&0xFF); Serial2.write(outLen&0xFF);
  Serial2.write(outBuf,outLen); Serial2.flush();
  if(strncmp(text,"IMG:",4)!=0 && strncmp(text,"IMGDONE:",8)!=0 && strncmp(text,"IMGREQ:",7)!=0){
    int idx=messageCount%MAX_MESSAGES;
    strncpy(messages[idx].text,text,MAX_MSG_LEN-1); messages[idx].text[MAX_MSG_LEN-1]='\0';
    strcpy(messages[idx].senderName,"ICH");
    messages[idx].senderId=0; messages[idx].targetId=targetDst;
    if(targetDst!=0xFFFFFFFF){
      messages[idx].channelIdx=2;
      snprintf(messages[idx].channelName,19,"PN>%s",resolveNodeName(targetDst));
    }else{
      messages[idx].channelIdx=channelIdx;
      strncpy(messages[idx].channelName,(channelIdx==1)?channelSecondary:channelPrimary,19);
      messages[idx].channelName[19]='\0';
    }
    messageCount++;
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// HTML HELFER
// ═══════════════════════════════════════════════════════════════════════════
String escHtml(const char* s){
  String out="";
  while(s&&*s){
    if(*s=='<') out+="&lt;";
    else if(*s=='>') out+="&gt;";
    else if(*s=='&') out+="&amp;";
    else out+=(char)*s;
    s++;
  }
  return out;
}

String refreshBar(bool refreshActive, String selfUrlBase){
  String bar = "<div class='rbar'>";
  bar += "<a href='" + selfUrlBase + "refresh=" + String(refreshActive?"0":"1") + "'>";
  bar += String(refreshActive ? "[X] Auto 5s" : "[ ] Manuell");
  bar += "</a></div>";
  return bar;
}

String frameCss(){
  return "<style>"
    "html,body{background:#000;color:#ccc;font-family:monospace;margin:0;padding:0;"
      "font-size:11px;height:auto;overflow-y:auto}"
    "body{padding:2px}"
    "a{color:#00cccc;text-decoration:none}"
    "hr{border:0;border-top:1px solid #1a1a1a;margin:4px 0}"
    ".rbar{position:sticky;top:0;background:#0a0a0a;border-bottom:1px solid #1a1a1a;"
      "padding:4px 3px;font-size:10px;color:#888;z-index:99}"
    ".rbar a{color:#00aa88}"
    // Preset-Box mit globalem Toggle-Button
    ".preset-box{background:#111;border-bottom:1px solid #2a2a2a;padding:2px 2px;"
      "margin:0 0 3px 0;display:flex;gap:2px;align-items:center}"
    ".preset-toggle{background:#0a0a14;color:#336699;border:1px solid #1a2a44;"
      "padding:3px 4px;font-family:monospace;font-size:9px;font-weight:bold;"
      "text-align:center;text-decoration:none;min-width:18px;white-space:nowrap}"
    ".preset-toggle.on{background:#001a00;color:#00ff66;border-color:#006622}"
    ".preset-btn{flex:1;background:#0d1a00;color:#aaff44;border:1px solid #2a4400;"
      "padding:3px 1px;font-family:monospace;font-size:9px;font-weight:bold;"
      "text-align:center;text-decoration:none;overflow:hidden;"
      "white-space:nowrap;text-overflow:ellipsis;min-width:0}"
    ".preset-btn:active{background:#1a3300;border-color:#88cc00}"
    ".preset-btn-empty{flex:1;background:#0a0a0a;color:#2a2a2a;border:1px solid #181818;"
      "padding:3px 1px;font-family:monospace;font-size:9px;"
      "text-align:center;text-decoration:none;overflow:hidden;white-space:nowrap;min-width:0}"
    ".msg{margin-bottom:4px;padding:3px 3px 3px 6px;border-left:2px solid #00ff66}"
    ".msg-pn{margin-bottom:4px;padding:3px 3px 3px 6px;border-left:2px solid #ff44ff;background:#100010}"
    ".msg-me{margin-bottom:4px;padding:3px 3px 3px 6px;border-left:2px solid #ffaa00;background:#0a0800}"
    ".msg-foto{margin-bottom:4px;padding:5px;border-left:2px solid #00ccff;background:#001520}"
    ".msg-sys{margin-bottom:4px;padding:3px 3px 3px 6px;border-left:2px solid #555555;background:#0a0a0a}"
    ".sndr{font-size:9px;color:#00cccc}"
    ".sndr-pn{font-size:9px;color:#ff88ff}"
    ".sndr-me{font-size:9px;color:#ffcc44}"
    ".sndr-sys{font-size:9px;color:#555555}"
    ".meta{color:#444;font-size:9px}"
    ".txt{color:#ddd;margin-top:1px}"
    ".empty{color:#333;font-size:10px;padding:6px}"
    "input[type=text]{background:#000;color:#fff;border:1px solid #444;"
      "padding:4px;font-family:monospace;font-size:12px}"
    "input[type=submit]{background:#001800;color:#00ff00;border:1px solid #00ff00;"
      "padding:4px 8px;font-family:monospace;font-size:11px}"
    ".btn-red{background:#180000;color:#ff4444;border:1px solid #ff4444;"
      "padding:4px 8px;font-family:monospace;font-size:11px}"
    ".btn-foto{background:#100018;color:#cc88ff;border:1px solid #884488;"
      "padding:4px 8px;font-family:monospace;font-size:11px;text-decoration:none}"
    ".btn-pn{background:#180018;color:#ff88ff;border:1px solid #ff44ff;"
      "padding:5px 8px;font-family:monospace;font-size:11px;display:block;"
      "text-align:center;margin-top:5px;width:100%;box-sizing:border-box}"
    ".node-row{border-bottom:1px solid #111;padding:5px 0}"
    ".node-name{color:#00ff66}"
    ".node-id{color:#555;font-size:9px}"
    ".node-snr{color:#ffaa00;font-size:9px}"
    ".fav-btn{font-size:12px;padding:2px 5px;background:#222;border:1px solid #444;"
      "color:#ffff00;text-decoration:none;float:right}"
    ".tele-box{background:#050505;border:1px solid #1a1a1a;padding:5px;margin-bottom:5px}"
    ".tele-label{color:#555;font-size:9px}"
    ".tele-val{color:#00ff66}"
    ".pn-tile{display:block;background:#0c060c;border:1px solid #441144;"
      "padding:8px;margin-bottom:5px;text-decoration:none;box-sizing:border-box}"
    ".tile-name{color:#ff88ff;font-size:12px;font-weight:bold}"
    ".tile-preview{color:#775577;font-size:10px;display:block;margin-top:2px}"
    ".cam-box{background:#050a05;border:1px solid #0a2a0a;padding:6px;margin-bottom:6px}"
    ".cam-status{font-size:9px;color:#666;margin-bottom:4px}"
    ".cam-btn{display:inline-block;background:#001a00;color:#00ff66;border:1px solid #00aa44;"
      "padding:5px 10px;font-family:monospace;font-size:11px;text-align:center;"
      "text-decoration:none;margin-right:4px}"
    ".cam-btn-snap{display:block;background:#0a1400;color:#ccff00;border:1px solid #88cc00;"
      "padding:11px;font-family:monospace;font-size:13px;font-weight:bold;"
      "text-align:center;text-decoration:none;margin-bottom:8px}"
    ".cam-btn-send{display:block;background:#001520;color:#00ccff;border:1px solid #0077aa;"
      "padding:8px;font-family:monospace;font-size:11px;text-align:center;"
      "text-decoration:none;margin-top:6px}"
    ".qual-row{margin-bottom:8px;display:flex;gap:3px}"
    ".qual-btn{flex:1;display:block;background:#111;color:#666;border:1px solid #2a2a2a;"
      "padding:6px 4px;font-family:monospace;font-size:10px;text-align:center;"
      "text-decoration:none}"
    ".qual-btn.on{background:#002200;color:#00ff66;border-color:#00aa44}"
    ".chunk-bar{font-size:9px;color:#ffaa00;margin-top:3px}"
    ".foto-link{color:#00ccff;font-size:11px;text-decoration:underline}"
    ".send-row{margin-top:6px;padding-top:4px;border-top:1px solid #1a1a1a}"
    // SETTINGS
    ".set-section{margin-bottom:10px}"
    ".set-label{color:#888;font-size:9px;margin-bottom:2px}"
    ".set-row{display:flex;align-items:center;gap:4px;margin-bottom:4px}"
    ".set-idx{color:#445544;font-size:9px;min-width:14px;text-align:right}"
    ".set-inp{flex:1;background:#050a05;color:#aaff44;border:1px solid #2a3a1a;"
      "padding:4px;font-family:monospace;font-size:11px}"
    ".set-inp-wifi{flex:1;background:#050505;color:#aaccff;border:1px solid #1a2a3a;"
      "padding:4px;font-family:monospace;font-size:11px}"
    ".set-save{background:#001800;color:#00ff00;border:1px solid #00aa44;"
      "padding:3px 7px;font-family:monospace;font-size:10px}"
    ".set-save-wifi{background:#000a18;color:#44aaff;border:1px solid #224488;"
      "padding:3px 7px;font-family:monospace;font-size:10px}"
    "input[type=checkbox]{width:12px;height:12px;margin:0;accent-color:#00ff66}"
    // SECURITY
    ".sec-box{background:#0a0500;border:1px solid #2a1a00;padding:5px;margin-bottom:6px}"
    ".sec-label{color:#886644;font-size:9px}"
    ".sec-mac{font-size:10px;font-family:monospace;color:#ffcc88}"
    ".sec-ok{color:#00ff66;font-size:9px}"
    ".sec-blocked{color:#ff4444;font-size:9px}"
    ".sec-btn{background:#1a0a00;color:#ffaa44;border:1px solid #664422;"
      "padding:3px 7px;font-family:monospace;font-size:10px;text-decoration:none}"
    ".sec-btn-red{background:#1a0000;color:#ff4444;border:1px solid #882222;"
      "padding:3px 7px;font-family:monospace;font-size:10px;text-decoration:none}"
    ".sec-mode-learn{background:#001a1a;color:#00ccff;border:1px solid #006688;"
      "padding:5px 8px;font-family:monospace;font-size:10px;display:block;"
      "text-align:center;margin-bottom:4px;text-decoration:none}"
    ".sec-mode-lock{background:#1a0500;color:#ff6600;border:1px solid #883300;"
      "padding:5px 8px;font-family:monospace;font-size:10px;display:block;"
      "text-align:center;margin-bottom:4px;text-decoration:none}"
    // History
    ".sec-hist-ok{color:#00ff66;font-size:9px}"
    ".sec-hist-bad{color:#ff4444;font-size:9px}"
    "</style>";
}

// ═══════════════════════════════════════════════════════════════════════════
// SECURITY WEB-HANDLER
// ═══════════════════════════════════════════════════════════════════════════
void handleSecMode() {
  if (server.hasArg("v")) { secWhitelistMode=(server.arg("v").toInt()==1); secSave(); }
  server.sendHeader("Location", "/settings?subtab=sec"); server.send(303);
}
void handleSecWlRemove() {
  if (server.hasArg("i")) {
    int idx=server.arg("i").toInt();
    if(idx>=0&&idx<secWhitelistCount){
      for(int i=idx;i<secWhitelistCount-1;i++) secWhitelist[i]=secWhitelist[i+1];
      secWhitelistCount--; secSave();
    }
  }
  server.sendHeader("Location", "/settings?subtab=sec"); server.send(303);
}

// ═══════════════════════════════════════════════════════════════════════════
// WIFI SETTINGS SAVE
// ═══════════════════════════════════════════════════════════════════════════
void handleWifiSave() {
  if(server.hasArg("wifi_ssid")){
    String s=server.arg("wifi_ssid"); s.trim();
    if(s.length()>0&&s.length()<=32)
      strncpy(wifiSSID,s.c_str(),32);
  }
  wifiHidden = server.hasArg("wifi_hidden");
  saveWifiSettings();
  applyWifiSettings();
  server.sendHeader("Location","/settings?subtab=wifi&saved=1"); server.send(303);
}

// ═══════════════════════════════════════════════════════════════════════════
// FRAMESET SHELL
// ─────────────────────────────────────────────────────────────────────────
// FIX grauer Streifen: rows='62,*' statt '68,*'
// Tab-Body: height=56px (62px frame - 3px padding top - 3px padding bottom)
// ═══════════════════════════════════════════════════════════════════════════
void handleShell(){
  String frameUrl="/chat?tab=0";
  activeView="ch1";
  if(server.hasArg("view")){
    String v=server.arg("view"); activeView=v;
    if(v=="ch1")    frameUrl="/chat?tab=0";
    else if(v=="ch2") frameUrl="/chat?tab=1";
    else if(v=="pn")  frameUrl="/pnlist";
    else if(v=="nodes") frameUrl="/nodes";
    else if(v=="tele")  frameUrl="/tele";
    else if(v=="cam")   frameUrl="/cam";
    else if(v=="set")   frameUrl="/settings";
  }
  String html="<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01 Frameset//EN\">";
  html+="<html><head><meta charset='UTF-8'><title>Mesh PSP v16</title></head>";
  // 62px: Tab-Höhe exakt ohne Leerstreifen
  html+="<frameset rows='20,*' border='0' frameborder='0' framespacing='0'>";
  html+="<frame src='/tabs?active="+activeView+"' name='tabs' scrolling='no' noresize>";
  html+="<frame src='"+frameUrl+"' name='content' scrolling='auto'>";
  html+="</frameset></html>";
  server.send(200,"text/html; charset=utf-8",html);
}

// ═══════════════════════════════════════════════════════════════════════════
// TABS
// body: margin=0, padding=3px 2px, height=56px (= frame 62 - 6px padding)
// ═══════════════════════════════════════════════════════════════════════════
void handleTabs(){
  String active=server.hasArg("active")?server.arg("active"):"ch1";
  bool secAlert = secWhitelistMode && strlen(secCurrentMac) > 0 && !secClientKnown;
  String h="<!DOCTYPE html><html><head><meta charset='UTF-8'>";
h+="<style>"
  "html,body{margin:0;padding:0 2px;background:#111;overflow:hidden;"
    "display:flex;gap:2px;height:20px;box-sizing:border-box;font-family:monospace}"
  ".tab{flex:1;display:flex;align-items:center;justify-content:center;"
    "background:#1a1a1a;color:#556655;"
    "text-decoration:none;border:1px solid #2a2a2a;"
    "font-size:10px;font-weight:bold;text-align:center;"
    "box-sizing:border-box;overflow:hidden;text-overflow:ellipsis;"
    "letter-spacing:0px;line-height:1}"
  ".tab.on{background:#003300;color:#00ff00;border:1px solid #00aa00;"
    "border-bottom:2px solid #00ff00}"
  ".tab:hover{color:#00ff00;background:#002200}"
  ".tab-cam{color:#006688;border-color:#004455}"
  ".tab-cam.on{background:#001520;color:#00ccff;border-color:#0077aa;"
    "border-bottom:2px solid #00ccff}"
  ".tab-set{color:#666644;border-color:#333322}"
  ".tab-set.on{background:#151500;color:#cccc00;border-color:#888800;"
    "border-bottom:2px solid #cccc00}"
  ".tab-set-alert{color:#ff6600;border-color:#662200}"
  ".tab-set-alert.on{background:#1a0500;color:#ff6600;border-color:#882200;"
    "border-bottom:2px solid #ff6600}"
  "</style>";

  struct { const char* id; const char* lbl; const char* url; int style; } tabs[]={
    {"ch1","CH1","/?view=ch1",0},
    {"ch2","CH2","/?view=ch2",0},
    {"pn","PN","/?view=pn",0},
    {"nodes","Node","/?view=nodes",0},
    {"tele","Tele","/?view=tele",0},
    {"cam","CAM","/?view=cam",1},
    {"set",secAlert?"SET!":"SET","/?view=set",secAlert?3:2},
  };
  for(auto& t:tabs){
    bool on=(String(t.id)==active);
    String cls="tab";
    if(t.style==1) cls+=" tab-cam";
    else if(t.style==3) cls+=" tab-set-alert";
    else if(t.style==2) cls+=" tab-set";
    if(on) cls+=" on";
    h+="<a class='"+cls+"' href='"+String(t.url)+"' target='_top'>"+String(t.lbl)+"</a>";
  }
  h+="</body></html>";
  server.send(200,"text/html; charset=utf-8",h);
}

// ═══════════════════════════════════════════════════════════════════════════
// CHANNEL CHAT
// Presets: globaler Toggle-Button — ein Klick blendet alle ein/aus
// ═══════════════════════════════════════════════════════════════════════════
void handleChat(){
  uint8_t tab=0;
  if(server.hasArg("tab")) tab=(uint8_t)server.arg("tab").toInt();
  bool ra=!(server.hasArg("refresh")&&server.arg("refresh")=="0");
  // preset-Sichtbarkeit: URL-Param "ps=0" blendet aus, Default = 1 (sichtbar)
  bool psOn=!(server.hasArg("ps")&&server.arg("ps")=="0");
  String base="/chat?tab="+String(tab)+"&";
  String self=base+"refresh="+String(ra?"1":"0")+"&ps="+String(psOn?"1":"0");

  String html="<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  if(ra) html+="<meta http-equiv='refresh' content='5;url="+self+"'>";
  html+=frameCss(); html+="</head><body>";

  // Zähle Presets für diesen Tab
  int presetCount=0;
  for(int i=0;i<PRESET_COUNT;i++){
    if(strlen(presetTexts[i])>0 && ((tab==0&&presetCh1[i])||(tab==1&&presetCh2[i])))
      presetCount++;
  }

  if(presetCount>0){
    // Toggle-Link: schaltet ps um
    String toggleUrl=base+"refresh="+String(ra?"1":"0")+"&ps="+String(psOn?"0":"1");
    html+="<div class='preset-box'>";
    // Globaler Toggle-Button
    html+="<a class='preset-toggle"+String(psOn?" on":"")+"' href='"+toggleUrl+"'>"
         +String(psOn?"[~]":"[+]")+"</a>";
    if(psOn){
      for(int i=0;i<PRESET_COUNT;i++){
        if(strlen(presetTexts[i])==0) continue;
        bool show=(tab==0&&presetCh1[i])||(tab==1&&presetCh2[i]);
        if(!show) continue;
        char lbl[7]; strncpy(lbl,presetTexts[i],5); lbl[5]='\0';
        html+="<a class='preset-btn' href='/send?target="+
              String(tab==0?"BROADCAST_CH1":"BROADCAST_CH2")+
              "&tab="+String(tab)+"&msg="+String(presetTexts[i])
              +"&ps="+String(psOn?"1":"0")
              +"&refresh=1' target='_self'>"+
              escHtml(lbl)+"</a>";
      }
    }
    html+="</div>";
  }

  html+=refreshBar(ra,base+"ps="+String(psOn?"1":"0")+"&");

  int start=(messageCount>MAX_MESSAGES)?messageCount%MAX_MESSAGES:0;
  int count=min(messageCount,MAX_MESSAGES);
  int found=0;

  for(int i=0;i<count;i++){
    int idx=(start+i)%MAX_MESSAGES;
    if(messages[idx].channelIdx!=tab) continue;
    found++;
    bool isMe=(messages[idx].senderId==0);
    bool isSys=(strcmp(messages[idx].senderName,"SYS")==0);
    String divCls = isSys ? "msg-sys" : (isMe ? "msg-me" : "msg");
    String sndrCls= isSys ? "sndr-sys" : (isMe ? "sndr-me" : "sndr");
    html+="<div class='"+divCls+"'>";
    html+="<div class='"+sndrCls+"'>";
    if(!isMe&&!isSys&&messages[idx].senderId!=0)
      html+="<a href='/tele?node="+String(messages[idx].senderId)+"' target='content'>"
           +escHtml(messages[idx].senderName)+"</a>";
    else html+=escHtml(messages[idx].senderName);
    html+=" <span class='meta'>["+escHtml(messages[idx].channelName)+"]</span></div>";
    html+="<div class='txt'>"+escHtml(messages[idx].text)+"</div></div>";
  }
  if(!found) html+="<div class='empty'>Keine Nachrichten.</div>";

  String uid=String(millis());
  html+="<div class='send-row'>";
  html+="<form method='POST' action='/send'>";
  html+="<input type='hidden' name='target' value='"+String(tab==0?"BROADCAST_CH1":"BROADCAST_CH2")+"'>";
  html+="<input type='hidden' name='tab' value='"+String(tab)+"'>";
  html+="<input type='hidden' name='field_id' value='"+uid+"'>";
  html+="<input type='text' name='msg_"+uid+"' value='' autocomplete='off' "
        "placeholder='Nachricht...' maxlength='100' style='width:100%;box-sizing:border-box'><br><br>";
  html+="<input type='submit' value='Senden'> ";
  html+="<input type='submit' name='clear' value='Clear' class='btn-red'>";
  html+="</form></div></body></html>";
  server.send(200,"text/html; charset=utf-8",html);
}

// ═══════════════════════════════════════════════════════════════════════════
// SETTINGS — Presets + Security + WiFi
// ═══════════════════════════════════════════════════════════════════════════
void handleSettings(){
  String subtab = server.hasArg("subtab") ? server.arg("subtab") : "presets";
  bool saved    = server.hasArg("saved");

  String html="<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html+=frameCss(); html+="</head><body>";

  html+="<style>"
    ".set-subtab{flex:1;padding:7px 0;background:#111;color:#556655;"
      "text-decoration:none;border:1px solid #2a2a2a;font-size:11px;"
      "font-weight:bold;text-align:center;font-family:monospace;display:block}"
    ".set-subtab.on{background:#151500;color:#cccc00;border-color:#888800}"
    ".set-subtab-wifi.on{background:#00050f;color:#44aaff;border-color:#224488}"
    ".set-subtab-sec.on{background:#1a0500;color:#ff8800;border-color:#883300}"
    ".stabs{display:flex;gap:2px;margin-bottom:6px}"
    // CH-Checkboxen in Settings
    ".ch-chk{display:flex;align-items:center;gap:3px;font-size:9px;color:#667766;"
      "white-space:nowrap}"
    "</style>";
  html+="<div class='stabs'>";

  String secTabLabel = "SEC";
  if(secWhitelistMode && !secClientKnown && strlen(secCurrentMac)>0) secTabLabel="SEC!";

  auto stab = [&](String id, String lbl, String extra){
    bool on = (subtab==id);
    String cls = "set-subtab";
    if(!extra.isEmpty()) cls += " "+extra;
    if(on) cls += " on";
    html += "<a class='"+cls+"' href='/settings?subtab="+id+"'>"+lbl+"</a>";
  };
  stab("presets","PRESETS","");
  stab("wifi","WIFI","set-subtab-wifi");
  stab("sec",secTabLabel,"set-subtab-sec");
  html+="</div>";

  if(saved){
    html+="<div style='color:#00ff66;font-size:10px;margin-bottom:8px;"
          "border:1px solid #005500;padding:4px;background:#001200'>"
          "&#10003; Gespeichert.</div>";
  }

  // ── PRESETS ──────────────────────────────────────────────────────────────
  if(subtab=="presets"){
    html+="<div class='set-section'>";
    html+="<div class='set-label'>QUICK-SEND BUTTONS &mdash; 9 Slots</div>";
    html+="<div style='color:#334433;font-size:9px;margin-bottom:5px'>"
          "Haken CH1/CH2: auf welchem Kanal der Button erscheint</div>";
    html+="<form method='POST' action='/settingssave'>";
    for(int i=0;i<PRESET_COUNT;i++){
      html+="<div class='set-row'>";
      html+="<span class='set-idx'>"+String(i+1)+"</span>";
      html+="<input class='set-inp' type='text' name='p"+String(i)+"' "
            "value='"+escHtml(presetTexts[i])+"' maxlength='60' "
            "placeholder='leer = ausgeblendet' style='flex:1'>";
      html+="<span class='ch-chk'>";
      html+="<input type='checkbox' name='c1_"+String(i)+"' id='c1_"+String(i)+"'"
           +String(presetCh1[i]?" checked":"")+">";
      html+="<label for='c1_"+String(i)+"'>1</label>";
      html+="</span>";
      html+="<span class='ch-chk'>";
      html+="<input type='checkbox' name='c2_"+String(i)+"' id='c2_"+String(i)+"'"
           +String(presetCh2[i]?" checked":"")+">";
      html+="<label for='c2_"+String(i)+"'>2</label>";
      html+="</span>";
      html+="</div>";
    }
    html+="<div style='margin-top:8px'>";
    html+="<input type='submit' value='Speichern' class='set-save'>";
    html+="</div></form></div>";
  }

  // ── WIFI ─────────────────────────────────────────────────────────────────
  else if(subtab=="wifi"){
    html+="<div class='set-section'>";
    html+="<div class='set-label'>WLAN ACCESS POINT EINSTELLUNGEN</div>";
    html+="<form method='POST' action='/wifisave'>";
    html+="<div style='color:#334455;font-size:9px;margin-bottom:6px'>"
          "Aktuelle SSID: <span style='color:#44aaff'>"+escHtml(wifiSSID)+"</span>"
          +" &nbsp;|&nbsp; Versteckt: <span style='color:#44aaff'>"
          +String(wifiHidden?"JA":"NEIN")+"</span>"
          +" &nbsp;|&nbsp; <span style='color:#334444'>Offen (kein Passwort)</span></div>";
    html+="<div class='set-row'>";
    html+="<span class='set-label' style='min-width:70px'>SSID&nbsp;</span>";
    html+="<input class='set-inp-wifi' type='text' name='wifi_ssid' "
          "value='"+escHtml(wifiSSID)+"' maxlength='32' "
          "placeholder='Netzwerkname' style='flex:1'>";
    html+="</div>";
    html+="<div class='set-row' style='margin-top:4px'>";
    html+="<input type='checkbox' name='wifi_hidden' id='wifi_hidden'"
         +String(wifiHidden?" checked":"")+" style='margin-right:4px'>";
    html+="<label for='wifi_hidden' style='color:#778899;font-size:10px'>"
          "Verstecktes WLAN (SSID nicht sichtbar)</label>";
    html+="</div>";
    html+="<div style='margin-top:8px;border-top:1px solid #0a1520;padding-top:6px'>";
    html+="<input type='submit' value='Speichern &amp; Neu verbinden' class='set-save-wifi'>";
    html+="</div>";
    html+="<div style='color:#222;font-size:9px;margin-top:6px'>"
          "ESP startet WLAN sofort neu. Kurzer Verbindungsabbruch.</div>";
    html+="</form></div>";
  }

  // ── SECURITY ─────────────────────────────────────────────────────────────
  else {
    html+="<div class='set-section'>";

    // Aktueller Client
    html+="<div class='sec-box'>";
    html+="<div class='set-label'>AKTUELLER CLIENT</div>";
    if(strlen(secCurrentMac)>0){
      html+="<div class='sec-mac'>"+escHtml(secCurrentMac)+" / "+escHtml(secCurrentIp)+"</div>";
      if(secClientKnown) html+="<div class='sec-ok'>&#10003; Bekannt &mdash; verbunden</div>";
      else               html+="<div class='sec-blocked'>&#9888; UNBEKANNTE MAC &mdash; geblockt</div>";
    } else {
      html+="<div style='color:#333;font-size:9px'>Kein Client verbunden</div>";
    }
    html+="</div>";

    // Verbindungshistory (letzte 5, neueste zuerst)
    html+="<div class='sec-box'>";
    html+="<div class='set-label'>VERBINDUNGS-HISTORY (letzte "+String(SEC_HISTORY_SIZE)+")</div>";
    bool anyHist=false;
    // Ring-Buffer rückwärts durchlaufen: neueste zuerst
    for(int k=0;k<SEC_HISTORY_SIZE;k++){
      int i=(secHistoryHead - 1 - k + SEC_HISTORY_SIZE) % SEC_HISTORY_SIZE;
      if(!secHistory[i].valid) continue;
      anyHist=true;
      html+="<div style='border-bottom:1px solid #1a0a00;padding:2px 0;margin-bottom:2px'>";
      if(secHistory[i].allowed)
        html+="<span class='sec-hist-ok'>&#10003;</span> ";
      else
        html+="<span class='sec-hist-bad'>&#9888;</span> ";
      html+="<span class='sec-mac'>"+escHtml(secHistory[i].mac)+"</span>"
           +" <span style='color:#556;font-size:9px'>"+escHtml(secHistory[i].ip)+"</span>";
      if(!secHistory[i].allowed)
        html+=" <span class='sec-hist-bad' style='font-size:8px'>[GEBLOCKT]</span>";
      html+="</div>";
    }
    if(!anyHist)
      html+="<div style='color:#333;font-size:9px'>Noch keine Verbindungen.</div>";
    html+="</div>";

    // Modus
    html+="<div class='set-label'>MODUS</div>";
    if(!secWhitelistMode){
      html+="<div style='color:#00ccff;font-size:9px;margin-bottom:3px'>"
            "&#9679; Lernmodus: Neue MACs werden automatisch gespeichert.</div>";
      html+="<a class='sec-mode-lock' href='/sec_mode?v=1'>Lernmodus beenden &rarr; SPERREN</a>";
    } else {
      html+="<div style='color:#ff6600;font-size:9px;margin-bottom:3px'>"
            "&#9679; Sperrmodus: Nur Whitelist-MACs erlaubt.</div>";
      html+="<a class='sec-mode-learn' href='/sec_mode?v=0'>Sperrmodus aufheben &rarr; LERNEN</a>";
    }

    html+="<hr><div class='set-label'>MAC-WHITELIST ("+String(secWhitelistCount)+"/"+String(SEC_MAX_WHITELIST)+")</div>";
    if(secWhitelistCount==0){
      html+="<div style='color:#333;font-size:9px'>Leer. Verbinde im Lernmodus.</div>";
    } else {
      for(int i=0;i<secWhitelistCount;i++){
        bool isCur=(strcasecmp(secWhitelist[i].mac,secCurrentMac)==0);
        html+="<div class='set-row' style='border-bottom:1px solid #1a1a0a;padding-bottom:3px'>";
        html+="<span class='sec-mac' style='flex:1;color:"
             +String(isCur?"#00ff66":"#ffcc88")+"'>"+escHtml(secWhitelist[i].mac);
        if(isCur) html+=" <span style='color:#00ff66;font-size:8px'>[DU]</span>";
        html+="</span>";
        html+="<a class='sec-btn-red' href='/sec_wl_rm?i="+String(i)+"'>&#10005;</a>";
        html+="</div>";
      }
    }

    html+="<hr><div style='color:#222;font-size:8px;margin-top:4px'>"
          "Schutzmassnahmen: Max. 1 Client | 802.11b | MAC-Whitelist | LED-Alarm</div>";
    html+="</div>";
  }
html+="<div style='margin:10px 0 6px 0;border-top:1px solid #1a0000;padding-top:8px'>";
html+="<div class='set-label'>SYSTEM</div>";
html+="<a href='/shutdown' style='display:block;background:#1a0000;color:#ff4444;"
      "border:1px solid #882222;padding:6px 10px;font-family:monospace;font-size:11px;"
      "text-align:center;text-decoration:none;margin-top:4px'>"
      "&#9210; ESP32 herunterfahren</a>";
html+="<div style='color:#2a1010;font-size:8px;margin-top:3px'>"
      "Deep Sleep &mdash; Neustart nur per Reset/Strom</div>";
html+="</div>";
  html+="<hr><div style='color:#333;font-size:9px;margin-top:4px'>"
        "Mesh PSP v16 &mdash; 2026-06-14T14:00:00Z</div>";
  html+="</body></html>";
  server.send(200,"text/html; charset=utf-8",html);
}

// ═══════════════════════════════════════════════════════════════════════════
// SETTINGS SAVE (Presets + CH-Flags)
// ═══════════════════════════════════════════════════════════════════════════
void handleSettingsSave(){
  for(int i=0;i<PRESET_COUNT;i++){
    String key="p"+String(i);
    if(server.hasArg(key)){
      String val=server.arg(key); val.trim();
      strncpy(presetTexts[i],val.c_str(),63); presetTexts[i][63]='\0';
      strncpy(presetLabels[i],presetTexts[i],23); presetLabels[i][23]='\0';
    }
    presetCh1[i] = server.hasArg("c1_"+String(i));
    presetCh2[i] = server.hasArg("c2_"+String(i));
    // Wenn kein Kanal gewählt: beide aktivieren
    if(strlen(presetTexts[i])>0 && !presetCh1[i] && !presetCh2[i]){
      presetCh1[i]=true; presetCh2[i]=true;
    }
  }
  savePresets();
  server.sendHeader("Location","/settings?subtab=presets&saved=1");
  server.send(303);
}

// ═══════════════════════════════════════════════════════════════════════════
// SEND POST / GET
// ═══════════════════════════════════════════════════════════════════════════
void handleSend(){
  uint8_t tab=0;
  if(server.hasArg("tab")) tab=(uint8_t)server.arg("tab").toInt();
  if(server.hasArg("clear")){
    if(tab==2&&server.hasArg("target"))
      server.sendHeader("Location","/clear?tab=2&node="+server.arg("target"));
    else
      server.sendHeader("Location","/clear?tab="+String(tab));
    server.send(303); return;
  }
  String msg="";
  if(server.hasArg("field_id")){
    String df="msg_"+server.arg("field_id");
    if(server.hasArg(df)) msg=server.arg(df);
  }
  if(msg.length()==0&&server.hasArg("msg")) msg=server.arg("msg");
  msg.trim();
  if(msg.length()>0&&server.hasArg("target")){
    String target=server.arg("target");
    if(target=="BROADCAST_CH1")      sendToRadio(msg.c_str(),0xFFFFFFFF,0);
    else if(target=="BROADCAST_CH2") sendToRadio(msg.c_str(),0xFFFFFFFF,1);
    else{
      uint32_t tid=strtoul(target.c_str(),NULL,10);
      if(tid!=0) sendToRadio(msg.c_str(),tid,0);
    }
  }
  if(server.hasArg("returnnode"))
    server.sendHeader("Location","/pnconv?node="+server.arg("returnnode")+"&refresh=1");
  else if(server.hasArg("tab"))
    server.sendHeader("Location","/chat?tab="+server.arg("tab")+"&refresh=1");
  else
    server.sendHeader("Location","/pnlist?refresh=1");
  server.send(303);
}

// ═══════════════════════════════════════════════════════════════════════════
// PN LISTE
// ═══════════════════════════════════════════════════════════════════════════
void handlePNList(){
  bool ra=!(server.hasArg("refresh")&&server.arg("refresh")=="0");
  String base="/pnlist?"; String self=base+"refresh="+String(ra?"1":"0");
  String html="<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  if(ra) html+="<meta http-equiv='refresh' content='5;url="+self+"'>";
  html+=frameCss(); html+="</head><body>";
  html+=refreshBar(ra,base);
  html+="<div style='color:#ff88ff;font-size:10px;margin-bottom:6px'>Private Nachrichten</div>";
  uint32_t pnPartners[MAX_CACHED_NODES]; int pnCount=0;
  int start=(messageCount>MAX_MESSAGES)?messageCount%MAX_MESSAGES:0;
  int count=min(messageCount,MAX_MESSAGES);
  for(int i=0;i<count;i++){
    int idx=(start+i)%MAX_MESSAGES;
    if(messages[idx].channelIdx!=2) continue;
    uint32_t p=(messages[idx].senderId==0)?messages[idx].targetId:messages[idx].senderId;
    if(p==0||p==0xFFFFFFFF) continue;
    bool ex=false;
    for(int j=0;j<pnCount;j++) if(pnPartners[j]==p){ex=true;break;}
    if(!ex&&pnCount<MAX_CACHED_NODES) pnPartners[pnCount++]=p;
  }
  if(pnCount==0){
    html+="<div class='empty'>Keine Privatnachrichten.</div>";
  } else {
    for(int i=0;i<pnCount;i++){
      uint32_t partner=pnPartners[i];
      const char* name=resolveNodeName(partner);
      const char* preview="";
      for(int j=count-1;j>=0;j--){
        int idx=(start+j)%MAX_MESSAGES;
        if(messages[idx].channelIdx!=2) continue;
        uint32_t p2=(messages[idx].senderId==0)?messages[idx].targetId:messages[idx].senderId;
        if(p2==partner){preview=messages[idx].text;break;}
      }
      html+="<a class='pn-tile' href='/pnconv?node="+String(partner)+"' target='content'>";
      html+="<span class='tile-name'>"+escHtml(name)+"</span>";
      if(strncmp(preview,"FOTO:",5)==0)
        html+="<span class='tile-preview'>[Foto]</span>";
      else if(strncmp(preview,"[IMGREQ]",8)==0)
        html+="<span class='tile-preview' style='color:#ffaa00'>[Retry läuft...]</span>";
      else{
        String prev=escHtml(preview);
        if(prev.length()>45) prev=prev.substring(0,45)+"...";
        html+="<span class='tile-preview'>"+prev+"</span>";
      }
      html+="</a>";
    }
  }
  html+="</body></html>";
  server.send(200,"text/html; charset=utf-8",html);
}

// ═══════════════════════════════════════════════════════════════════════════
// PN KONVERSATION
// ═══════════════════════════════════════════════════════════════════════════
void handlePNConv(){
  if(!server.hasArg("node")){server.sendHeader("Location","/pnlist");server.send(303);return;}
  uint32_t nodeId=strtoul(server.arg("node").c_str(),NULL,10);
  const char* name=resolveNodeName(nodeId);
  bool ra=!(server.hasArg("refresh")&&server.arg("refresh")=="0");
  String base="/pnconv?node="+String(nodeId)+"&";
  String self=base+"refresh="+String(ra?"1":"0");
  String html="<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  if(ra) html+="<meta http-equiv='refresh' content='5;url="+self+"'>";
  html+=frameCss(); html+="</head><body>";
  html+=refreshBar(ra,base);
  html+="<div style='font-size:10px;margin-bottom:5px'>";
  html+="<a href='/pnlist' target='content'>&lt; PN</a> | ";
  html+="<span style='color:#ff88ff;font-weight:bold'>"+escHtml(name)+"</span></div>";
  int start=(messageCount>MAX_MESSAGES)?messageCount%MAX_MESSAGES:0;
  int count=min(messageCount,MAX_MESSAGES);
  int found=0;
  for(int i=0;i<count;i++){
    int idx=(start+i)%MAX_MESSAGES;
    if(messages[idx].channelIdx!=2) continue;
    uint32_t partner=(messages[idx].senderId==0)?messages[idx].targetId:messages[idx].senderId;
    if(partner!=nodeId) continue;
    found++;
    bool isMe=(messages[idx].senderId==0);
    bool isSys=(strcmp(messages[idx].senderName,"SYS")==0);
    if(strncmp(messages[idx].text,"FOTO:",5)==0){
      const char* sx=messages[idx].text+5;
      if(!isMe){
        html+="<div class='msg-foto'>";
        html+="<div class='sndr-pn'>"+escHtml(name)+" [Foto]</div>";
        html+="<div class='txt'>";
        PhotoRecvSession* s=findRecvSessionByIdStr(sx);
        if(s&&s->complete){
          html+="<a class='foto-link' href='/photoview?session="+String(sx)
               +"' target='content'>Foto ansehen ("+String(s->imgW)+"x"+String(s->imgH)+")</a>";
        } else if(s){
          int missing=s->totalChunks-s->receivedCount;
          html+="<span style='color:#888'>Empfange... "+String(s->receivedCount)
               +"/"+String(s->totalChunks)+"</span>";
          if(missing>0){
            html+=" <span style='color:#ffaa00;font-size:9px'>("+String(missing)+" fehlend";
            if(s->reqSent) html+=", Retry "+String(s->reqRounds)+"/"+String(IMGREQ_MAX_ROUNDS);
            html+=")</span>";
          }
        } else html+="<span style='color:#555'>Foto nicht verfuegbar</span>";
        html+="</div></div>";
      } else {
        html+="<div class='msg-me'><div class='sndr-me'>ICH [Foto]</div>";
        html+="<div class='txt'><span style='color:#888'>Foto gesendet ("+String(sx)+")</span>";
        if(retryJob.active&&retryJob.sessionId==(uint16_t)strtoul(sx,nullptr,16))
          html+=" <span style='color:#ffaa00;font-size:9px'>[Retry: "
               +String(retryJob.nextIdx)+"/"+String(retryJob.missingCount)+"]</span>";
        html+="</div></div>";
      }
      continue;
    }
    if(isSys){
      html+="<div class='msg-sys'><div class='sndr-sys'>SYS</div>";
      html+="<div class='txt' style='color:#555'>"+escHtml(messages[idx].text)+"</div></div>";
      continue;
    }
    html+="<div class='"+String(isMe?"msg-me":"msg-pn")+"'>";
    html+="<div class='"+String(isMe?"sndr-me":"sndr-pn")+"'>"+(isMe?String("ICH"):escHtml(name))+"</div>";
    html+="<div class='txt'>"+escHtml(messages[idx].text)+"</div></div>";
  }
  if(!found) html+="<div class='empty'>Noch keine Nachrichten.</div>";
  activePnPartner=nodeId;
  String uid=String(millis()+1);
  html+="<div class='send-row'>";
  html+="<form method='POST' action='/send'>";
  html+="<input type='hidden' name='target' value='"+String(nodeId,DEC)+"'>";
  html+="<input type='hidden' name='returnnode' value='"+String(nodeId,DEC)+"'>";
  html+="<input type='hidden' name='tab' value='2'>";
  html+="<input type='hidden' name='field_id' value='"+uid+"'>";
  html+="<input type='text' name='msg_"+uid+"' value='' autocomplete='off' "
        "placeholder='Nachricht...' maxlength='100' style='width:100%;box-sizing:border-box'><br><br>";
  html+="<input type='submit' value='Senden'> ";
  html+="</form>";
  html+="<a class='btn-foto' href='/cam?pnnode="+String(nodeId)+"' target='content'>Foto senden</a> ";
  html+="<form method='POST' action='/send' style='display:inline'>";
  html+="<input type='hidden' name='tab' value='2'>";
  html+="<input type='hidden' name='target' value='"+String(nodeId,DEC)+"'>";
  html+="<input type='submit' name='clear' value='Clear' class='btn-red'>";
  html+="</form>";
  html+="</div></body></html>";
  server.send(200,"text/html; charset=utf-8",html);
}

// ═══════════════════════════════════════════════════════════════════════════
// NODES LISTE
// ═══════════════════════════════════════════════════════════════════════════
void handleNodeList(){
  sortNodes();
  String html="<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html+=frameCss(); html+="</head><body>";
  html+="<div style='color:#ffff00;font-size:9px;padding:4px 0'>";
  html+=String(nodeCacheCount)+" Nodes &nbsp;";
  html+="<a href='/refreshnodes' target='content'>[Refresh]</a></div>";
  if(nodeCacheCount==0) html+="<div class='empty'>Keine Nodes.</div>";
  for(int i=0;i<nodeCacheCount;i++){
    NodeCache* n=&nodeDB[i];
    html+="<div class='node-row'>";
    html+="<a class='fav-btn' href='/fav?node="+String(n->num)+"&from=nodes' target='content'>"
         +String(n->isFavorite?"[*]":"[ ]")+"</a>";
    html+="<a href='/tele?node="+String(n->num)+"' target='content'><span class='node-name'>";
    if(n->isFavorite) html+="* ";
    if(strlen(n->shortName)>0) html+="["+String(n->shortName)+"] ";
    html+=escHtml(strlen(n->longName)>0?n->longName:"Unbekannt")+"</span></a>";
    html+="<br><span class='node-id'>0x"+String(n->num,HEX)+"</span>";
    unsigned long ago=(millis()-n->lastSeen)/1000;
    html+=" <span style='color:#444;font-size:9px'>(";
    if(ago<60) html+=String(ago)+"s"; else if(ago<3600) html+=String(ago/60)+"m"; else html+=String(ago/3600)+"h";
    html+=" ago)</span>";
    if(n->hasSnr){char buf[10];dtostrf(n->snr,4,1,buf);html+=" <span class='node-snr'>SNR "+String(buf)+"dB</span>";}
    html+="</div>";
  }
  html+="</body></html>";
  server.send(200,"text/html; charset=utf-8",html);
}

void handleRefreshNodes(){
  sendWantConfig();
  unsigned long t=millis();
  while(millis()-t<300) server.handleClient();
  server.sendHeader("Location","/nodes"); server.send(303);
}

// ═══════════════════════════════════════════════════════════════════════════
// TELEMETRIE
// ═══════════════════════════════════════════════════════════════════════════
void handleTelemetry(){
  bool ra=!(server.hasArg("refresh")&&server.arg("refresh")=="0");
  if(!server.hasArg("node")){
    sortNodes();
    String base="/tele?"; String self=base+"refresh="+String(ra?"1":"0");
    String html="<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    if(ra) html+="<meta http-equiv='refresh' content='10;url="+self+"'>";
    html+=frameCss(); html+="</head><body>";
    html+=refreshBar(ra,base);
    html+="<div style='color:#ffff00;font-size:9px;margin-bottom:3px'>Telemetrie</div>";
    int tc=0;
    for(int i=0;i<nodeCacheCount;i++){
      NodeCache* n=&nodeDB[i];
      if(!n->hasTelemetry&&!n->hasSnr) continue; tc++;
      html+="<div class='node-row'>";
      html+="<a class='fav-btn' href='/fav?node="+String(n->num)+"&from=tele' target='content'>"
           +String(n->isFavorite?"[*]":"[ ]")+"</a>";
      html+="<a href='/tele?node="+String(n->num)+"' target='content'><span class='node-name'>";
      if(n->isFavorite) html+="* ";
      if(strlen(n->shortName)>0) html+="["+String(n->shortName)+"] ";
      html+=escHtml(strlen(n->longName)>0?n->longName:"???")+"</span></a><br>";
      if(n->battery>0){char buf[8];dtostrf(n->battery,4,0,buf);html+="<span class='node-snr'>BAT:"+String(buf)+"% </span>";}
      if(n->temperature!=0){char buf[8];dtostrf(n->temperature,4,1,buf);html+="<span class='node-snr'>TEMP:"+String(buf)+"C </span>";}
      if(n->hasSnr){char buf[8];dtostrf(n->snr,4,1,buf);html+="<span class='node-snr'>SNR:"+String(buf)+"dB</span>";}
      html+="</div>";
    }
    if(tc==0) html+="<div class='empty'>Keine Telemetriedaten.</div>";
    html+="</body></html>"; server.send(200,"text/html; charset=utf-8",html); return;
  }
  uint32_t nodeId=strtoul(server.arg("node").c_str(),NULL,10);
  NodeCache* n=findNode(nodeId); const char* name=resolveNodeName(nodeId);
  String base="/tele?node="+String(nodeId)+"&";
  String self=base+"refresh="+String(ra?"1":"0");
  String html="<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  if(ra) html+="<meta http-equiv='refresh' content='10;url="+self+"'>";
  html+=frameCss(); html+="</head><body>";
  html+=refreshBar(ra,base);
  html+="<div style='font-size:9px;margin-bottom:3px'>"
        "<a href='/tele' target='content'>&lt; Tele</a> | "
        "<a href='/nodes' target='content'>Nodes</a> | "
        "<a href='/fav?node="+String(nodeId)+"&from=tnode' target='content'>"
        +String((n&&n->isFavorite)?"[* Fav-]":"[ Fav+]")+"</a></div>";
  html+="<div style='color:#00ff66;font-size:12px;margin-bottom:2px'>";
  if(n&&n->isFavorite) html+="* ";
  if(n&&strlen(n->shortName)>0) html+="["+String(n->shortName)+"] ";
  html+=escHtml(name)+"</div>";
  html+="<div style='color:#555;font-size:9px;margin-bottom:5px'>0x"+String(nodeId,HEX)+"</div>";
  html+="<div class='tele-box'>";
  if(n&&n->hasSnr){char buf[10];dtostrf(n->snr,4,1,buf);html+="<div><span class='tele-label'>SNR: </span><span class='tele-val'>"+String(buf)+" dB</span></div>";}
  if(n&&n->hasTelemetry){
    if(n->battery>0){char buf[8];dtostrf(n->battery,4,0,buf);html+="<div><span class='tele-label'>Batterie: </span><span class='tele-val'>"+String(buf)+"%</span></div>";}
    if(n->temperature!=0){char buf[8];dtostrf(n->temperature,5,1,buf);html+="<div><span class='tele-label'>Temperatur: </span><span class='tele-val'>"+String(buf)+" C</span></div>";}
    if(n->humidity!=0){char buf[8];dtostrf(n->humidity,5,1,buf);html+="<div><span class='tele-label'>Luftfeuchte: </span><span class='tele-val'>"+String(buf)+"%</span></div>";}
  } else html+="<div class='empty'>Keine Telemetrie-Daten.</div>";
  if(n){
    unsigned long ago=(millis()-n->lastSeen)/1000;
    html+="<div><span class='tele-label'>Zuletzt: </span><span class='tele-val'>";
    if(ago<60) html+="vor "+String(ago)+"s"; else if(ago<3600) html+="vor "+String(ago/60)+"min"; else html+="vor "+String(ago/3600)+"h";
    html+="</span></div>";
  }
  html+="</div>";
  html+="<a href='/pnconv?node="+String(nodeId,DEC)+"' target='content' class='btn-pn'>[ PN senden ]</a>";
  html+="</body></html>"; server.send(200,"text/html; charset=utf-8",html);
}

// ═══════════════════════════════════════════════════════════════════════════
// CLEAR
// ═══════════════════════════════════════════════════════════════════════════
void handleClear(){
  uint8_t tab=0; uint32_t partnerId=0; bool isPN=false;
  if(server.hasArg("tab")){
    tab=(uint8_t)server.arg("tab").toInt();
    if(tab==2&&server.hasArg("node")){partnerId=strtoul(server.arg("node").c_str(),NULL,10);isPN=true;}
  }
  for(int i=0;i<MAX_MESSAGES;i++){
    if(isPN){
      uint32_t p=(messages[i].senderId==0)?messages[i].targetId:messages[i].senderId;
      if(messages[i].channelIdx==2&&p==partnerId){messages[i].channelIdx=99;messages[i].text[0]='\0';}
    } else {
      if(messages[i].channelIdx==tab){messages[i].channelIdx=99;messages[i].text[0]='\0';}
    }
  }
  if(isPN) server.sendHeader("Location","/pnconv?node="+String(partnerId));
  else     server.sendHeader("Location","/chat?tab="+String(tab));
  server.send(303);
}

// ═══════════════════════════════════════════════════════════════════════════
// KAMERA HAUPTSEITE
// ═══════════════════════════════════════════════════════════════════════════
void handleCamera(){
  if(server.hasArg("qual")){
    uint8_t q=(uint8_t)server.arg("qual").toInt();
    if(q<=2&&q!=camQuality){
      camQuality=q; cameraReady=initCamera(camQuality);
      if(capturedJpeg){free(capturedJpeg);capturedJpeg=nullptr;capturedJpegLen=0;}
    }
  }
  if(server.hasArg("pnnode"))
    activePnPartner=strtoul(server.arg("pnnode").c_str(),NULL,10);

  const char* qNames[3]={"320x240 Hoch","320x240 Low","160x120"};
  const char* qInfo[3] ={"Q12 ~2-4KB","Q25 ~1-3KB","Q30 ~1-2KB"};

  String html="<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html+=frameCss(); html+="</head><body>";
  html+="<div class='cam-status'>";
  html+=cameraReady?"<span style='color:#00aa44'>Kamera OK</span>":"<span style='color:#ff4444'>Kamera FEHLER</span>";
  if(photoJob.active)
    html+=" &nbsp;<span class='chunk-bar'>Sendet "+String(photoJob.nextChunk)+"/"+String(photoJob.totalChunks)
         +" (5s/Chunk)</span>";
  if(retryJob.active)
    html+=" &nbsp;<span style='color:#ffaa00;font-size:9px'>Retry "
         +String(retryJob.nextIdx)+"/"+String(retryJob.missingCount)+" Chunks</span>";
  html+="</div>";

  html+="<div class='qual-row'>";
  for(uint8_t q=0;q<=2;q++){
    String href="/cam?qual="+String(q)+(activePnPartner?"&pnnode="+String(activePnPartner):"");
    html+="<a class='qual-btn"+String(q==camQuality?" on":"")+"' href='"+href+"'>"
         +String(qNames[q])
         +"<br><span style='font-size:8px;color:#445544'>"+String(qInfo[q])+"</span></a>";
  }
  html+="</div>";

  if(cameraReady){
    String snapUrl="/camsnap"+(activePnPartner?String("?pnnode=")+String(activePnPartner):String(""));
    html+="<a class='cam-btn-snap' href='"+snapUrl+"'>FOTO AUFNEHMEN</a>";
  }

  if(capturedJpeg!=nullptr&&capturedJpegLen>0){
    int chunks=min((int)((capturedJpegLen+PHOTO_CHUNK_SIZE-1)/PHOTO_CHUNK_SIZE),PHOTO_MAX_CHUNKS);
    html+="<div class='cam-box'>";
    html+="<div class='cam-status'>"+String(capturedW)+"x"+String(capturedH)
         +" | "+String(capturedJpegLen)+" Bytes | "+String(chunks)+" Chunks"
         +" | ca. "+String((unsigned long)chunks*5/60)+"min "+String((unsigned long)chunks*5%60)+"s</div>";
    html+="<a class='cam-btn' href='/campreview' target='content'>Vorschau</a>";
    html+="<hr><div style='color:#888;font-size:9px;margin:4px 0'>[ Nur per PN sendbar ]</div>";
    uint32_t pnPartners[MAX_CACHED_NODES]; int pnCount=getOpenPnPartners(pnPartners,MAX_CACHED_NODES);
    if(!photoJob.active&&!retryJob.active){
      if(pnCount==0){
        html+="<div style='color:#444;font-size:9px;margin-top:4px'>"
              "Keine offenen PN-Konversationen.</div>";
      } else {
        if(pnCount==1&&activePnPartner==pnPartners[0]){
          html+="<a class='cam-btn-send' href='/camsend?node="+String(activePnPartner)
               +"'>Senden an: "+escHtml(resolveNodeName(activePnPartner))+"</a>";
        } else {
          html+="<div style='color:#aaa;font-size:9px;margin-top:4px'>Bild senden an:</div>";
          for(int i=0;i<pnCount;i++){
            html+="<a class='cam-btn-send' style='margin-top:3px' href='/camsend?node="+String(pnPartners[i])+"'>"
                 +escHtml(resolveNodeName(pnPartners[i]))+"</a>";
          }
        }
      }
    } else {
      String jobInfo = photoJob.active
        ? "Übertragung läuft... "+String(photoJob.nextChunk)+"/"+String(photoJob.totalChunks)
          +" ~"+String((photoJob.totalChunks-photoJob.nextChunk)*5)+"s"
        : "Retry läuft... "+String(retryJob.nextIdx)+"/"+String(retryJob.missingCount)+" Chunks";
      html+="<div style='color:#ffaa00;font-size:10px;margin-top:4px'>"+jobInfo+"</div>";
    }
    html+="</div>";
  } else {
    html+="<div class='empty'>Noch kein Foto aufgenommen.</div>";
  }

  bool anyRcv=false;
  for(int i=0;i<MAX_RECV_SESSIONS;i++) if(recvSessions[i].active){anyRcv=true;break;}
  if(anyRcv){
    html+="<hr><div style='color:#00ccff;font-size:9px;margin-bottom:4px'>Empfangene Fotos:</div>";
    for(int i=0;i<MAX_RECV_SESSIONS;i++){
      PhotoRecvSession* s=&recvSessions[i]; if(!s->active) continue;
      char sx[8]; snprintf(sx,8,"%04X",s->sessionId);
      html+="<div style='border-left:2px solid #00ccff;padding:3px 5px;margin-bottom:3px'>";
      html+="<span style='color:#00ccff;font-size:9px'>Von: "+escHtml(resolveNodeName(s->senderNode))+"</span><br>";
      if(s->complete){
        html+="<a class='foto-link' href='/photoview?session="+String(sx)
             +"' target='content'>Ansehen ("+String(s->imgW)+"x"+String(s->imgH)+")</a>";
      } else {
        int missing=s->totalChunks-s->receivedCount;
        html+="<span style='color:#888'>Empfange... "+String(s->receivedCount)
             +"/"+String(s->totalChunks)+"</span>";
        if(s->reqSent)
          html+=" <span style='color:#ffaa00;font-size:9px'>Retry "+String(s->reqRounds)
               +"/"+String(IMGREQ_MAX_ROUNDS)+"</span>";
        if(missing==0&&!s->complete)
          html+=" <span style='color:#ff4444;font-size:9px'>[Assemblierung fehlt]</span>";
      }
      html+="</div>";
    }
  }
  html+="</body></html>";
  server.send(200,"text/html; charset=utf-8",html);
}

// ═══════════════════════════════════════════════════════════════════════════
// KAMERA SNAP
// ═══════════════════════════════════════════════════════════════════════════
void handleCamSnap(){
  if(server.hasArg("pnnode"))
    activePnPartner=strtoul(server.arg("pnnode").c_str(),NULL,10);
  String snapStatus="";
  if(!cameraReady){
    snapStatus="<div style='color:#ff4444'>Kamera nicht bereit.</div>";
  } else {
    if(capturedJpeg!=nullptr){free(capturedJpeg);capturedJpeg=nullptr;capturedJpegLen=0;}
    camera_fb_t* ex=esp_camera_fb_get();
    if(ex){esp_camera_fb_return(ex); delay(80);}
    camera_fb_t* fb=esp_camera_fb_get();
    if(!fb){
      snapStatus="<div style='color:#ff4444'>Capture fehlgeschlagen.</div>";
    } else {
      capturedJpeg=(uint8_t*)malloc(fb->len);
      if(capturedJpeg){
        memcpy(capturedJpeg,fb->buf,fb->len);
        capturedJpegLen=fb->len; capturedW=fb->width; capturedH=fb->height;
        int chunks=min((int)((capturedJpegLen+PHOTO_CHUNK_SIZE-1)/PHOTO_CHUNK_SIZE),PHOTO_MAX_CHUNKS);
        snapStatus="<div style='color:#00ff66'>Foto OK: "+String(capturedW)+"x"+String(capturedH)
                  +" ("+String(capturedJpegLen)+" Bytes, "+String(chunks)+" Chunks, ~"
                  +String((unsigned long)chunks*5)+"s Sendezeit)</div>";
      } else {
        snapStatus="<div style='color:#ff4444'>Kein RAM.</div>";
      }
      esp_camera_fb_return(fb);
    }
  }
  String html="<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html+="<meta http-equiv='refresh' content='2;url=/cam"
       +(activePnPartner?String("?pnnode=")+String(activePnPartner):String(""))+"'>";
  html+=frameCss(); html+="</head><body>";
  html+=snapStatus;
  html+="<div style='color:#555;font-size:9px;margin-top:4px'>Zurück zur Kamera...</div>";
  html+="</body></html>";
  server.send(200,"text/html; charset=utf-8",html);
}

// ═══════════════════════════════════════════════════════════════════════════
// JPEG AUSLIEFERN
// ═══════════════════════════════════════════════════════════════════════════
void handleCamJpeg(){
  if(capturedJpeg==nullptr||capturedJpegLen==0){server.send(404,"text/plain","Kein Foto");return;}
  server.sendHeader("Content-Length",String(capturedJpegLen));
  server.sendHeader("Cache-Control","no-cache");
  server.send_P(200,"image/jpeg",(const char*)capturedJpeg,capturedJpegLen);
}

void handleRecvJpeg(){
  if(!server.hasArg("session")){server.send(404,"text/plain","Keine Session");return;}
  PhotoRecvSession* s=findRecvSessionByIdStr(server.arg("session").c_str());
  if(!s||!s->complete||!s->assembledJpeg){server.send(404,"text/plain","Foto nicht fertig");return;}
  server.sendHeader("Content-Length",String(s->assembledLen));
  server.sendHeader("Cache-Control","no-cache");
  server.send_P(200,"image/jpeg",(const char*)s->assembledJpeg,s->assembledLen);
}

// ═══════════════════════════════════════════════════════════════════════════
// VORSCHAU
// ═══════════════════════════════════════════════════════════════════════════
void handleCamPreview(){
  if(capturedJpeg==nullptr){server.send(404,"text/plain","Kein Foto");return;}
  String html="<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html+=frameCss(); html+="</head><body>";
  html+="<div style='font-size:9px;color:#555;padding:3px 0'>"
       +String(capturedW)+"x"+String(capturedH)
       +" &nbsp;<a href='/cam'>zurück</a></div>";
  html+="<img src='/camjpeg' width='"+String(capturedW)+"' height='"+String(capturedH)
       +"' style='display:block;border:1px solid #333;max-width:100%;margin-bottom:6px'>";
  if(activePnPartner!=0&&!photoJob.active&&!retryJob.active)
    html+="<a class='cam-btn-send' href='/camsend?node="+String(activePnPartner)
         +"'>Senden an "+escHtml(resolveNodeName(activePnPartner))+"</a>";
  html+="</body></html>";
  server.send(200,"text/html; charset=utf-8",html);
}

// ═══════════════════════════════════════════════════════════════════════════
// FOTO VERSENDEN
// ═══════════════════════════════════════════════════════════════════════════
void handleCamSend(){
  if(!server.hasArg("node")){server.sendHeader("Location","/cam");server.send(303);return;}
  uint32_t targetNode=strtoul(server.arg("node").c_str(),NULL,10);
  if(targetNode==0||targetNode==0xFFFFFFFF||capturedJpeg==nullptr||capturedJpegLen==0
     ||photoJob.active||retryJob.active){
    server.sendHeader("Location","/cam"); server.send(303); return;
  }
  int chunks=min((int)((capturedJpegLen+PHOTO_CHUNK_SIZE-1)/PHOTO_CHUNK_SIZE),PHOTO_MAX_CHUNKS);
  photoJob.active=true; photoJob.targetNode=targetNode;
  photoJob.sessionId=(uint16_t)(millis()&0xFFFF);
  photoJob.totalChunks=chunks; photoJob.nextChunk=0; photoJob.lastSentMs=0;
  photoJob.imgW=capturedW; photoJob.imgH=capturedH;
  photoJob.jpegData=(uint8_t*)malloc(capturedJpegLen+PHOTO_CHUNK_SIZE);
  if(!photoJob.jpegData){photoJob.active=false;server.sendHeader("Location","/cam");server.send(303);return;}
  memset(photoJob.jpegData,0,capturedJpegLen+PHOTO_CHUNK_SIZE);
  memcpy(photoJob.jpegData,capturedJpeg,capturedJpegLen);
  photoJob.jpegLen=capturedJpegLen;
  int idx=messageCount%MAX_MESSAGES;
  char sm[24]; snprintf(sm,24,"FOTO:%04X",photoJob.sessionId);
  strncpy(messages[idx].text,sm,MAX_MSG_LEN-1); messages[idx].text[MAX_MSG_LEN-1]='\0';
  strcpy(messages[idx].senderName,"ICH");
  messages[idx].senderId=0; messages[idx].targetId=targetNode;
  messages[idx].channelIdx=2;
  snprintf(messages[idx].channelName,19,"PN>%s",resolveNodeName(targetNode));
  messageCount++;
  server.sendHeader("Location","/cam?pnnode="+String(targetNode));
  server.send(303);
}

// ═══════════════════════════════════════════════════════════════════════════
// EMPFANGENES FOTO ANZEIGEN
// ═══════════════════════════════════════════════════════════════════════════
void handlePhotoView(){
  if(!server.hasArg("session")){server.send(404,"text/plain","Keine Session");return;}
  String sx=server.arg("session");
  PhotoRecvSession* s=findRecvSessionByIdStr(sx.c_str());
  String html="<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html+=frameCss(); html+="</head><body>";
  html+="<div style='font-size:9px;padding:3px 0'><a href='/cam'>CAM</a>";
  if(s&&s->senderNode)
    html+=" | Von: <span style='color:#ff88ff'>"+escHtml(resolveNodeName(s->senderNode))+"</span>"
         +" | <a href='/pnconv?node="+String(s->senderNode)+"' target='content'>PN</a>";
  html+="</div>";
  if(!s||!s->complete||!s->assembledJpeg){
    html+="<div style='color:#ff4444'>Foto nicht verfuegbar.</div>";
    if(s){
      html+="<div style='color:#888;font-size:9px'>"+String(s->receivedCount)+"/"+String(s->totalChunks)+" Chunks</div>";
      if(s->reqSent)
        html+="<div style='color:#ffaa00;font-size:9px'>Retry gesendet (Runde "
             +String(s->reqRounds)+"/"+String(IMGREQ_MAX_ROUNDS)+")</div>";
    }
  } else {
    html+="<div style='font-size:9px;color:#555;margin-bottom:4px'>"
         +String(s->imgW)+"x"+String(s->imgH)+" | "+String(s->assembledLen)+" Bytes</div>";
    html+="<img src='/recvjpeg?session="+sx+"' width='"+String(s->imgW)+"' height='"+String(s->imgH)
         +"' style='display:block;border:1px solid #333;max-width:100%'>";
  }
  html+="</body></html>";
  server.send(200,"text/html; charset=utf-8",html);
}
