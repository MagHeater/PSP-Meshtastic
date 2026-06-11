// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  Meshtastic PSP  —  v11                                                  ║
// ║  Erstellt: 2026-06-10                                                    ║
// ║                                                                          ║
// ║  Änderungen gegenüber v10:                                               ║
// ║  - Nachrichten scrollen nach unten (overflow-y:auto), kein Abschneiden  ║
// ║  - Refresh-Toggle Button OBEN in jeder Ansicht                           ║
// ║  - Kamera: 3 Qualitäten (Hoch/Mittel/Preview), AWB-Fix (Frame wegwerf.) ║
// ║  - Sensor-Register: AWB/AEC/AGC auto, Sättigung/Helligkeit optimiert    ║
// ║  - CAM-Tab: nur Snap + Vorschau + "Bild in PN senden" (nur wenn PN offen)║
// ║  - PN-Fenster: [Senden] [Foto senden] nebeneinander, kein Snap-Button   ║
// ║  - Foto senden nur an bereits offene PN-Konversationen                  ║
// ║  Board:  AI-Thinker ESP32-CAM                                            ║
// ║  Serial: Serial2 RX=13 TX=12  @38400                                     ║
// ╚══════════════════════════════════════════════════════════════════════════╝

#include <WiFi.h>
#include <WebServer.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <meshtastic/mesh.pb.h>
#include <meshtastic/portnums.pb.h>
#include <Preferences.h>
#include "esp_camera.h"

WebServer   server(80);
Preferences prefs;

const char* ssid     = "XIAO_MONITOR";
const char* password = "";

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

// ═══════════════════════════════════════════════════════════════════════════
// FOTO-PROTOKOLL
// ═══════════════════════════════════════════════════════════════════════════
#define PHOTO_CHUNK_SIZE    64    // Bytes pro Chunk → 128 Hex-Zeichen
#define PHOTO_MAX_CHUNKS    80    // reicht für QVGA ~4-5KB
#define PHOTO_SEND_DELAY_MS 2000  // ms zwischen Chunks

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
// FOTO — SENDEN
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
  uint8_t*  assembledJpeg;
  size_t    assembledLen;
};
PhotoRecvSession recvSessions[MAX_RECV_SESSIONS];

// ═══════════════════════════════════════════════════════════════════════════
// KAMERA STATE
// ═══════════════════════════════════════════════════════════════════════════
uint8_t* capturedJpeg    = nullptr;
size_t   capturedJpegLen = 0;
uint16_t capturedW       = 0;
uint16_t capturedH       = 0;
bool     cameraReady     = false;
uint32_t activePnPartner = 0;

// Qualität: 0=Hoch(320x240 Q12)  1=Mittel(320x240 Q20)  2=Preview(160x120 Q25)
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
void handleRecvJpeg();
void processFromRadio(uint8_t* buf, uint16_t len);
void processPhotoChunk(uint32_t senderNode, const char* msg);
void processPhotoDone(uint32_t senderNode, const char* msg);
void sendToRadio(const char* text, uint32_t targetDst, uint8_t channelIdx);
void tickPhotoSend();
void initSerialConnection(); void sendWantConfig(); void sendHeartbeat();
const char* resolveNodeName(uint32_t nodeNum);
void cacheNode(uint32_t num, const char* longName, const char* shortName);
NodeCache* findNode(uint32_t num);
String escHtml(const char* s);   String frameCss();   void sortNodes();
bool initCamera(uint8_t quality);
void saveFavorites(); void loadFavorites(); bool isSavedFavorite(uint32_t num);
PhotoRecvSession* findOrCreateRecvSession(uint32_t sn, uint16_t sid, int tot, uint16_t w, uint16_t h);
PhotoRecvSession* findRecvSessionByIdStr(const char* hex);
String bytesToHex(const uint8_t* data, size_t len);
bool   hexToBytes(const char* hex, uint8_t* out, size_t outMax, size_t* outLen);
// Hilfsfunktion: liefert offene PN-Partner-IDs
int getOpenPnPartners(uint32_t* out, int maxOut);

// ═══════════════════════════════════════════════════════════════════════════
// SETUP / LOOP
// ═══════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial2.begin(38400, SERIAL_8N1, 13, 12);
  delay(1500);
  Serial.println("--- Meshtastic PSP v11 (2026-06-10) ---");

  memset(recvSessions, 0, sizeof(recvSessions));
  prefs.begin("mesh_favs", false);
  loadFavorites();

  cameraReady = initCamera(camQuality);
  Serial.println(cameraReady ? "Kamera OK" : "Kamera FEHLER");

  WiFi.mode(WIFI_AP);
  IPAddress local_IP(192,168,4,1), gateway(192,168,4,1), subnet(255,255,255,0);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP(ssid, password);

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
  server.on("/cam",          handleCamera);
  server.on("/camsnap",      handleCamSnap);
  server.on("/campreview",   handleCamPreview);
  server.on("/camjpeg",      handleCamJpeg);
  server.on("/camsend",      handleCamSend);
  server.on("/photoview",    handlePhotoView);
  server.on("/recvjpeg",     handleRecvJpeg);
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
// KAMERA INIT + AWB/AEC FIX
// quality: 0=Hoch(320x240 Q12)  1=Mittel(320x240 Q20)  2=Preview(160x120 Q25)
// ═══════════════════════════════════════════════════════════════════════════
bool initCamera(uint8_t quality) {
  esp_camera_deinit();
  delay(150);

  framesize_t fs; int jpegQ;
  switch (quality) {
    case 1:  fs=FRAMESIZE_QVGA;  jpegQ=20; break;  // 320x240 Mittel
    case 2:  fs=FRAMESIZE_QQVGA; jpegQ=25; break;  // 160x120 Preview
    default: fs=FRAMESIZE_QVGA;  jpegQ=12; break;  // 320x240 Hoch
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
  cfg.frame_size=fs;
  cfg.jpeg_quality=jpegQ;
  cfg.fb_count=1;

  if (esp_camera_init(&cfg) != ESP_OK) return false;

  // Sensor-Register: AWB/AEC/AGC auf Auto, Helligkeit +1, Sättigung +1
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_whitebal(s, 1);        // AWB ein
    s->set_awb_gain(s, 1);        // AWB-Gain ein
    s->set_wb_mode(s, 0);         // Auto-WB-Modus
    s->set_exposure_ctrl(s, 1);   // AEC (Auto-Belichtung) ein
    s->set_aec2(s, 1);            // AEC2 ein
    s->set_ae_level(s, 1);        // Belichtungs-Bias +1 (heller)
    s->set_gain_ctrl(s, 1);       // AGC (Auto-Gain) ein
    s->set_agc_gain(s, 0);        // AGC-Gain auf Minimum
    s->set_gainceiling(s, (gainceiling_t)2); // max 8x Gain
    s->set_bpc(s, 1);             // Bad-Pixel-Korrektur
    s->set_wpc(s, 1);             // White-Pixel-Korrektur
    s->set_raw_gma(s, 1);         // Gamma-Korrektur
    s->set_lenc(s, 1);            // Linsen-Shading-Korrektur
    s->set_brightness(s, 1);      // Helligkeit +1
    s->set_saturation(s, 1);      // Sättigung +1
    s->set_contrast(s, 0);        // Kontrast normal
    s->set_sharpness(s, 1);       // leichte Schärfung
    s->set_denoise(s, 1);         // Rauschunterdrückung
  }

  // Ersten Frame wegwerfen — Sensor braucht Zeit zum Einpegeln
  delay(300);
  camera_fb_t* warmup = esp_camera_fb_get();
  if (warmup) { esp_camera_fb_return(warmup); delay(100); }
  camera_fb_t* warmup2 = esp_camera_fb_get();
  if (warmup2) { esp_camera_fb_return(warmup2); delay(100); }

  return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// FAVORITEN — NVS
// ═══════════════════════════════════════════════════════════════════════════
void saveFavorites() {
  String s="";
  for(int i=0;i<nodeCacheCount;i++){
    if(nodeDB[i].isFavorite){if(s.length()>0)s+=","; s+=String(nodeDB[i].num);}
  }
  prefs.putString("favs",s);
}

void loadFavorites() {
  Serial.printf("[Favs] Geladen: %s\n", prefs.getString("favs","").c_str());
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
// FOTO EMPFANG — CHUNKS
// ═══════════════════════════════════════════════════════════════════════════
void processPhotoChunk(uint32_t senderNode,const char* msg){
  char buf[MAX_MSG_LEN];
  strncpy(buf,msg,MAX_MSG_LEN-1); buf[MAX_MSG_LEN-1]='\0';
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
  if(p){
    char db[16]; strncpy(db,p,15); char* dp=db;
    char* ws=strsep(&dp,"x");
    if(ws&&dp){w=atoi(ws);h=atoi(dp);}
  }
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
    free(photoJob.jpegData); photoJob.jpegData=nullptr; photoJob.active=false;
    return;
  }
  if(millis()-photoJob.lastSentMs<PHOTO_SEND_DELAY_MS) return;
  photoJob.lastSentMs=millis();
  int idx=photoJob.nextChunk;
  size_t offset=(size_t)idx*PHOTO_CHUNK_SIZE;
  size_t rem=photoJob.jpegLen-offset;
  size_t cl=(rem>PHOTO_CHUNK_SIZE)?PHOTO_CHUNK_SIZE:rem;
  String hc=bytesToHex(photoJob.jpegData+offset,cl);
  char cm[MAX_MSG_LEN];
  snprintf(cm,sizeof(cm),"IMG:%04X:%d:%02d:%s",
           photoJob.sessionId,photoJob.totalChunks,idx,hc.c_str());
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
        if(tl>0){
          int idx=messageCount%MAX_MESSAGES;
          strncpy(messages[idx].text,tb,MAX_MSG_LEN-1); messages[idx].text[MAX_MSG_LEN-1]='\0';
          messages[idx].senderId=pkt.from; messages[idx].targetId=pkt.to;
          strncpy(messages[idx].senderName,resolveNodeName(pkt.from),19); messages[idx].senderName[19]='\0';
          if(pkt.to!=0xFFFFFFFF){
            messages[idx].channelIdx=2; strcpy(messages[idx].channelName,"PN");
          }else{
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

// Refresh-Bar oben — gibt HTML-String zurück
// selfUrl = URL dieser Seite ohne refresh-Parameter
String refreshBar(bool refreshActive, String selfUrlBase){
  String bar = "<div class='rbar'>";
  bar += "<a href='" + selfUrlBase + "refresh=" + String(refreshActive?"0":"1") + "'>";
  bar += String(refreshActive ? "[X] Auto 5s" : "[ ] Manuell");
  bar += "</a></div>";
  return bar;
}

String frameCss(){
  return "<style>"
    // Basis — body scrollt, kein overflow-hidden
    "html,body{background:#000;color:#ccc;font-family:monospace;margin:0;padding:0;"
      "font-size:11px;height:auto;overflow-y:auto}"
    "body{padding:3px}"
    "a{color:#00cccc;text-decoration:none}"
    "hr{border:0;border-top:1px solid #1a1a1a;margin:4px 0}"
    // Refresh-Bar oben — sticky
    ".rbar{position:sticky;top:0;background:#0a0a0a;border-bottom:1px solid #1a1a1a;"
      "padding:4px 3px;font-size:10px;color:#888;z-index:99}"
    ".rbar a{color:#00aa88}"
    // Nachrichten
    ".msg{margin-bottom:4px;padding:3px 3px 3px 6px;border-left:2px solid #00ff66}"
    ".msg-pn{margin-bottom:4px;padding:3px 3px 3px 6px;border-left:2px solid #ff44ff;background:#100010}"
    ".msg-me{margin-bottom:4px;padding:3px 3px 3px 6px;border-left:2px solid #ffaa00;background:#0a0800}"
    ".msg-foto{margin-bottom:4px;padding:5px;border-left:2px solid #00ccff;background:#001520}"
    ".sndr{font-size:9px;color:#00cccc}"
    ".sndr-pn{font-size:9px;color:#ff88ff}"
    ".sndr-me{font-size:9px;color:#ffcc44}"
    ".meta{color:#444;font-size:9px}"
    ".txt{color:#ddd;margin-top:1px}"
    ".empty{color:#333;font-size:10px;padding:6px}"
    // Eingabe
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
    // Nodes
    ".node-row{border-bottom:1px solid #111;padding:5px 0}"
    ".node-name{color:#00ff66}"
    ".node-id{color:#555;font-size:9px}"
    ".node-snr{color:#ffaa00;font-size:9px}"
    ".fav-btn{font-size:12px;padding:2px 5px;background:#222;border:1px solid #444;"
      "color:#ffff00;text-decoration:none;float:right}"
    // Tele
    ".tele-box{background:#050505;border:1px solid #1a1a1a;padding:5px;margin-bottom:5px}"
    ".tele-label{color:#555;font-size:9px}"
    ".tele-val{color:#00ff66}"
    // PN-Tiles
    ".pn-tile{display:block;background:#0c060c;border:1px solid #441144;"
      "padding:8px;margin-bottom:5px;text-decoration:none;box-sizing:border-box}"
    ".tile-name{color:#ff88ff;font-size:12px;font-weight:bold}"
    ".tile-preview{color:#775577;font-size:10px;display:block;margin-top:2px}"
    // Kamera
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
    ".qual-row{margin-bottom:8px}"
    ".qual-btn{display:inline-block;background:#111;color:#666;border:1px solid #2a2a2a;"
      "padding:5px 8px;font-family:monospace;font-size:10px;text-align:center;"
      "text-decoration:none;margin-right:3px}"
    ".qual-btn.on{background:#002200;color:#00ff66;border-color:#00aa44}"
    ".chunk-bar{font-size:9px;color:#ffaa00;margin-top:3px}"
    ".foto-link{color:#00ccff;font-size:11px;text-decoration:underline}"
    // Eingabe-Zeile unten
    ".send-row{margin-top:6px;padding-top:4px;border-top:1px solid #1a1a1a}"
    "</style>";
}

// ═══════════════════════════════════════════════════════════════════════════
// FRAMESET SHELL
// ═══════════════════════════════════════════════════════════════════════════
void handleShell(){
  String frameUrl="/chat?tab=0";
  activeView="ch1";
  if(server.hasArg("view")){
    String v=server.arg("view"); activeView=v;
    if(v=="ch1")   frameUrl="/chat?tab=0";
    else if(v=="ch2")   frameUrl="/chat?tab=1";
    else if(v=="pn")    frameUrl="/pnlist";
    else if(v=="nodes") frameUrl="/nodes";
    else if(v=="tele")  frameUrl="/tele";
    else if(v=="cam")   frameUrl="/cam";
  }
  String html="<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01 Frameset//EN\">";
  html+="<html><head><meta charset='UTF-8'><title>Mesh PSP v11</title></head>";
  html+="<frameset rows='52,*' border='0' frameborder='0' framespacing='0'>";
  html+="<frame src='/tabs?active="+activeView+"' name='tabs' scrolling='no' noresize>";
  html+="<frame src='"+frameUrl+"' name='content' scrolling='auto'>";
  html+="</frameset></html>";
  server.send(200,"text/html; charset=utf-8",html);
}

// ═══════════════════════════════════════════════════════════════════════════
// TABS
// ═══════════════════════════════════════════════════════════════════════════
void handleTabs(){
  String active=server.hasArg("active")?server.arg("active"):"ch1";
  String h="<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  h+="<style>"
    "body{background:#111;margin:0;padding:3px 2px;font-family:monospace;"
      "white-space:nowrap;overflow:hidden}"
    ".tab{display:inline-block;padding:10px 0;background:#1a1a1a;color:#556655;"
      "text-decoration:none;border:1px solid #2a2a2a;border-bottom:none;"
      "font-size:12px;font-weight:bold;text-align:center;"
      "width:15%;box-sizing:border-box;margin-right:1px}"
    ".tab.on{background:#003300;color:#00ff00;border:1px solid #00aa00;"
      "border-bottom:2px solid #00ff00}"
    ".tab:hover{color:#00ff00;background:#002200}"
    ".tab-cam{color:#006688;border-color:#004455}"
    ".tab-cam.on{background:#001520;color:#00ccff;border-color:#0077aa;"
      "border-bottom:2px solid #00ccff}"
    "</style></head><body>";

  struct { const char* id; const char* lbl; const char* url; bool isCam; } tabs[]={
    {"ch1","CH1","/?view=ch1",false},
    {"ch2","CH2","/?view=ch2",false},
    {"pn","PN","/?view=pn",false},
    {"nodes","Nodes","/?view=nodes",false},
    {"tele","Tele","/?view=tele",false},
    {"cam","CAM","/?view=cam",true},
  };
  for(auto& t:tabs){
    bool on=(String(t.id)==active);
    h+="<a class='tab"+String(t.isCam?" tab-cam":"")+String(on?" on":"")
      +"' href='"+t.url+"' target='_top'>"+t.lbl+"</a>";
  }
  h+="</body></html>";
  server.send(200,"text/html; charset=utf-8",h);
}

// ═══════════════════════════════════════════════════════════════════════════
// CHANNEL CHAT  — Refresh-Toggle oben, Nachrichten scrollen nach unten
// ═══════════════════════════════════════════════════════════════════════════
void handleChat(){
  uint8_t tab=0;
  if(server.hasArg("tab")) tab=(uint8_t)server.arg("tab").toInt();
  bool ra=!(server.hasArg("refresh")&&server.arg("refresh")=="0");
  String base="/chat?tab="+String(tab)+"&";
  String self=base+"refresh="+String(ra?"1":"0");

  String html="<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  if(ra) html+="<meta http-equiv='refresh' content='5;url="+self+"'>";
  html+=frameCss();
  html+="</head><body>";

  // Refresh-Bar ganz oben
  html+=refreshBar(ra, base);

  int start=(messageCount>MAX_MESSAGES)?messageCount%MAX_MESSAGES:0;
  int count=min(messageCount,MAX_MESSAGES);
  int found=0;

  for(int i=0;i<count;i++){
    int idx=(start+i)%MAX_MESSAGES;
    if(messages[idx].channelIdx!=tab) continue;
    found++;
    bool isMe=(messages[idx].senderId==0);
    html+="<div class='"+String(isMe?"msg-me":"msg")+"'>";
    html+="<div class='"+String(isMe?"sndr-me":"sndr")+"'>";
    if(!isMe&&messages[idx].senderId!=0)
      html+="<a href='/tele?node="+String(messages[idx].senderId)+"' target='content'>"
           +escHtml(messages[idx].senderName)+"</a>";
    else html+=escHtml(messages[idx].senderName);
    html+=" <span class='meta'>["+escHtml(messages[idx].channelName)+"]</span></div>";
    html+="<div class='txt'>"+escHtml(messages[idx].text)+"</div></div>";
  }
  if(!found) html+="<div class='empty'>Keine Nachrichten.</div>";

  // Eingabe unten
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
// PN LISTE  — Refresh oben
// ═══════════════════════════════════════════════════════════════════════════
void handlePNList(){
  bool ra=!(server.hasArg("refresh")&&server.arg("refresh")=="0");
  String base="/pnlist?";
  String self=base+"refresh="+String(ra?"1":"0");

  String html="<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  if(ra) html+="<meta http-equiv='refresh' content='5;url="+self+"'>";
  html+=frameCss();
  html+="</head><body>";
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
    html+="<div style='color:#555;font-size:9px;margin-top:6px'>"
          "PN starten: Node in Nodes- oder Tele-Tab anklicken.</div>";
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
// PN KONVERSATION  — Refresh oben, [Senden] [Foto senden] unten
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
  html+=frameCss();
  html+="</head><body>";

  // Refresh-Bar oben
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

    if(strncmp(messages[idx].text,"FOTO:",5)==0){
      const char* sx=messages[idx].text+5;
      if(!isMe){
        html+="<div class='msg-foto'>";
        html+="<div class='sndr-pn'>"+escHtml(name)+" [Foto]</div>";
        html+="<div class='txt'>";
        PhotoRecvSession* s=findRecvSessionByIdStr(sx);
        if(s&&s->complete)
          html+="<a class='foto-link' href='/photoview?session="+String(sx)
               +"' target='content'>Foto ansehen ("+String(s->imgW)+"x"+String(s->imgH)+")</a>";
        else if(s)
          html+="<span style='color:#888'>Empfange... "+String(s->receivedCount)
               +"/"+String(s->totalChunks)+"</span>";
        else html+="<span style='color:#555'>Foto nicht verfuegbar</span>";
        html+="</div></div>";
      } else {
        html+="<div class='msg-me'><div class='sndr-me'>ICH [Foto]</div>";
        html+="<div class='txt'><span style='color:#888'>Foto gesendet ("+String(sx)+")</span></div></div>";
      }
      continue;
    }

    html+="<div class='"+String(isMe?"msg-me":"msg-pn")+"'>";
    html+="<div class='"+String(isMe?"sndr-me":"sndr-pn")+"'>"+(isMe?String("ICH"):escHtml(name))+"</div>";
    html+="<div class='txt'>"+escHtml(messages[idx].text)+"</div></div>";
  }
  if(!found) html+="<div class='empty'>Noch keine Nachrichten.</div>";

  activePnPartner=nodeId;

  // Eingabe + Foto-Senden Button
  String uid=String(millis()+1);
  html+="<div class='send-row'>";
  html+="<form method='POST' action='/send'>";
  html+="<input type='hidden' name='target' value='"+String(nodeId,DEC)+"'>";
  html+="<input type='hidden' name='returnnode' value='"+String(nodeId,DEC)+"'>";
  html+="<input type='hidden' name='tab' value='2'>";
  html+="<input type='hidden' name='field_id' value='"+uid+"'>";
  html+="<input type='text' name='msg_"+uid+"' value='' autocomplete='off' "
        "placeholder='Nachricht...' maxlength='100' style='width:100%;box-sizing:border-box'><br><br>";
  // [Senden] [Foto senden] [Clear] nebeneinander
  html+="<input type='submit' value='Senden'> ";
  html+="</form>";
  // Foto-senden als Link (kein POST nötig)
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
// SEND POST
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
// KAMERA HAUPTSEITE
// - Qualitätswahl oben
// - Großer Snap-Button
// - Vorschau 100%/50%/25%
// - "Bild in PN senden" NUR wenn offene PN-Konversationen existieren
// ═══════════════════════════════════════════════════════════════════════════
void handleCamera(){
  // Qualität wechseln
  if(server.hasArg("qual")){
    uint8_t q=(uint8_t)server.arg("qual").toInt();
    if(q<=2&&q!=camQuality){
      camQuality=q; cameraReady=initCamera(camQuality);
      if(capturedJpeg){free(capturedJpeg);capturedJpeg=nullptr;capturedJpegLen=0;}
    }
  }
  if(server.hasArg("pnnode"))
    activePnPartner=strtoul(server.arg("pnnode").c_str(),NULL,10);

  const char* qNames[3]={"Hoch (320x240)","Mittel (320x240)","Preview (160x120)"};
  const char* qInfo[3] ={"Q12 ~3-6KB","Q20 ~2-4KB","Q25 ~1-2KB"};

  String html="<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html+=frameCss(); html+="</head><body>";

  // Status-Zeile
  html+="<div class='cam-status'>";
  html+=cameraReady?"<span style='color:#00aa44'>Kamera OK</span>":"<span style='color:#ff4444'>Kamera FEHLER</span>";
  if(photoJob.active)
    html+=" &nbsp;<span class='chunk-bar'>Sendet "+String(photoJob.nextChunk)+"/"+String(photoJob.totalChunks)+"...</span>";
  html+="</div>";

  // Qualitätswahl
  html+="<div class='qual-row'>";
  for(uint8_t q=0;q<=2;q++){
    String href="/cam?qual="+String(q)+(activePnPartner?"&pnnode="+String(activePnPartner):"");
    html+="<a class='qual-btn"+String(q==camQuality?" on":"")+"' href='"+href+"'>"
         +String(qNames[q])+"<br><span style='font-size:8px;color:#445544'>"+String(qInfo[q])+"</span></a>";
  }
  html+="</div>";

  // Snap-Button
  if(cameraReady){
    String snapUrl="/camsnap"+(activePnPartner?String("?pnnode=")+String(activePnPartner):String(""));
    html+="<a class='cam-btn-snap' href='"+snapUrl+"'>FOTO AUFNEHMEN</a>";
  }

  // Vorschau wenn Foto vorhanden
  if(capturedJpeg!=nullptr&&capturedJpegLen>0){
    int chunks=min((int)((capturedJpegLen+PHOTO_CHUNK_SIZE-1)/PHOTO_CHUNK_SIZE),PHOTO_MAX_CHUNKS);
    html+="<div class='cam-box'>";
    html+="<div class='cam-status'>"+String(capturedW)+"x"+String(capturedH)
         +" | "+String(capturedJpegLen)+" Bytes | "+String(chunks)+" Chunks</div>";
    html+="<a class='cam-btn' href='/campreview?scale=1' target='content'>100%</a>";
    html+="<a class='cam-btn' href='/campreview?scale=2' target='content'>50%</a>";
    html+="<a class='cam-btn' href='/campreview?scale=4' target='content'>25%</a>";

    // "Bild in PN senden" — nur wenn offene PNs existieren
    html+="<hr><div style='color:#888;font-size:9px;margin:4px 0'>[ Nur per PN sendbar ]</div>";
    uint32_t pnPartners[MAX_CACHED_NODES]; int pnCount=getOpenPnPartners(pnPartners,MAX_CACHED_NODES);
    if(!photoJob.active){
      if(pnCount==0){
        html+="<div style='color:#444;font-size:9px;margin-top:4px'>"
              "Keine offenen PN-Konversationen.<br>"
              "Zuerst im PN-Tab eine Konversation öffnen.</div>";
      } else {
        if(pnCount==1&&activePnPartner==pnPartners[0]){
          html+="<a class='cam-btn-send' href='/camsend?node="+String(activePnPartner)
               +"'>Bild senden an: "+escHtml(resolveNodeName(activePnPartner))+"</a>";
        } else {
          html+="<div style='color:#aaa;font-size:9px;margin-top:4px'>Bild senden an:</div>";
          for(int i=0;i<pnCount;i++){
            html+="<a class='cam-btn-send' style='margin-top:3px' href='/camsend?node="+String(pnPartners[i])+"'>"
                 +escHtml(resolveNodeName(pnPartners[i]))+"</a>";
          }
        }
      }
    } else {
      html+="<div style='color:#ffaa00;font-size:10px;margin-top:4px'>Übertragung läuft... "
           +String(photoJob.nextChunk)+"/"+String(photoJob.totalChunks)+"</div>";
    }
    html+="</div>";
  } else {
    html+="<div class='empty'>Noch kein Foto aufgenommen.</div>";
  }

  // Empfangene Fotos
  bool anyRcv=false;
  for(int i=0;i<MAX_RECV_SESSIONS;i++) if(recvSessions[i].active){anyRcv=true;break;}
  if(anyRcv){
    html+="<hr><div style='color:#00ccff;font-size:9px;margin-bottom:4px'>Empfangene Fotos:</div>";
    for(int i=0;i<MAX_RECV_SESSIONS;i++){
      PhotoRecvSession* s=&recvSessions[i]; if(!s->active) continue;
      char sx[8]; snprintf(sx,8,"%04X",s->sessionId);
      html+="<div style='border-left:2px solid #00ccff;padding:3px 5px;margin-bottom:3px'>";
      html+="<span style='color:#00ccff;font-size:9px'>Von: "+escHtml(resolveNodeName(s->senderNode))+"</span><br>";
      if(s->complete)
        html+="<a class='foto-link' href='/photoview?session="+String(sx)
             +"' target='content'>Ansehen ("+String(s->imgW)+"x"+String(s->imgH)+")</a>";
      else
        html+="<span style='color:#888'>Empfange... "+String(s->receivedCount)+"/"+String(s->totalChunks)+"</span>";
      html+="</div>";
    }
  }
  html+="</body></html>";
  server.send(200,"text/html; charset=utf-8",html);
}

// ═══════════════════════════════════════════════════════════════════════════
// KAMERA SNAP  — 2 Warmup-Frames schon in initCamera, hier noch 1 extra
// ═══════════════════════════════════════════════════════════════════════════
void handleCamSnap(){
  if(server.hasArg("pnnode"))
    activePnPartner=strtoul(server.arg("pnnode").c_str(),NULL,10);

  String snapStatus="";
  if(!cameraReady){
    snapStatus="<div style='color:#ff4444'>Kamera nicht bereit.</div>";
  } else {
    if(capturedJpeg!=nullptr){free(capturedJpeg);capturedJpeg=nullptr;capturedJpegLen=0;}
    // Noch einen Extra-Frame verwerfen für frische Belichtung
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
        snapStatus="<div style='color:#00ff66'>Foto OK: "+String(capturedW)+"x"+String(capturedH)
                  +" ("+String(capturedJpegLen)+" Bytes)</div>";
      } else {
        snapStatus="<div style='color:#ff4444'>Kein RAM.</div>";
      }
      esp_camera_fb_return(fb);
    }
  }

  // Kurzer Status, dann redirect
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
  int scale=1;
  if(server.hasArg("scale")) scale=server.arg("scale").toInt();
  if(scale<1) scale=1; if(scale>4) scale=4;
  int dw=capturedW/scale, dh=capturedH/scale;

  String html="<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html+=frameCss(); html+="</head><body>";
  html+="<div style='font-size:9px;color:#555;padding:3px 0'>"
       +String(100/scale)+"% | "+String(dw)+"x"+String(dh)
       +" &nbsp;<a href='/cam'>zurück</a></div>";
  html+="<img src='/camjpeg' width='"+String(dw)+"' height='"+String(dh)
       +"' style='display:block;border:1px solid #333;margin-bottom:6px'>";
  html+="<a class='cam-btn' href='/campreview?scale=1'>100%</a>";
  html+="<a class='cam-btn' href='/campreview?scale=2'>50%</a>";
  html+="<a class='cam-btn' href='/campreview?scale=4'>25%</a>";
  if(activePnPartner!=0&&!photoJob.active)
    html+="<a class='cam-btn-send' href='/camsend?node="+String(activePnPartner)
         +"'>Senden an "+escHtml(resolveNodeName(activePnPartner))+"</a>";
  html+="</body></html>";
  server.send(200,"text/html; charset=utf-8",html);
}

// ═══════════════════════════════════════════════════════════════════════════
// FOTO VERSENDEN  — NUR PN, Broadcast blockiert
// ═══════════════════════════════════════════════════════════════════════════
void handleCamSend(){
  if(!server.hasArg("node")){server.sendHeader("Location","/cam");server.send(303);return;}
  uint32_t targetNode=strtoul(server.arg("node").c_str(),NULL,10);
  if(targetNode==0||targetNode==0xFFFFFFFF||capturedJpeg==nullptr||capturedJpegLen==0||photoJob.active){
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
    if(s) html+="<div style='color:#888;font-size:9px'>"+String(s->receivedCount)+"/"+String(s->totalChunks)+" Chunks</div>";
  } else {
    html+="<div style='font-size:9px;color:#555;margin-bottom:4px'>"
         +String(s->imgW)+"x"+String(s->imgH)+" | "+String(s->assembledLen)+" Bytes</div>";
    html+="<img src='/recvjpeg?session="+sx+"' width='"+String(s->imgW)+"' height='"+String(s->imgH)
         +"' style='display:block;border:1px solid #333'>";
  }
  html+="</body></html>";
  server.send(200,"text/html; charset=utf-8",html);
}
