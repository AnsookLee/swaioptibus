// === ESP32 BUS (WebSockets client) ===
// WiFi + ArduinoWebsockets + simple route follower (grid cells step with timing)
#include <WiFi.h>
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>

using namespace websockets;

// ---- USER CONFIG (change SSID/PASS and HUB_HOST) ----
const char* WIFI_SSID = "KT_GiGA_3977";  // TODO: change if needed
const char* WIFI_PASS = "cd73fd8418";    // TODO: change if needed
const char* HUB_HOST  = "ws://172.30.1.254:81"; // TODO: set to PC's IP where hub runs (e.g., ws://172.30.1.33:81)

// ---- Motor pins (from your validated mapping) ----
const int ENA = 27;  // PWM left
const int IN1 = 26;
const int IN2 = 25;
const int ENB = 18;  // PWM right
const int IN3 = 21;
const int IN4 = 19;

// PWM setup
const uint32_t PWM_FREQ = 15000;
const uint8_t  PWM_RES  = 10;
const uint16_t DUTY_MAX = (1 << PWM_RES) - 1;

const int SPEED_FWD = 700;
const int SPEED_TURN = 600;
const int SPEED_BACK = 600;
const int RAMP_STEP  = 40;
const int RAMP_DELAY = 8;
int targetA=0, targetB=0, currentA=0, currentB=0;

WebsocketsClient ws;

String BUS_ID;

static inline uint16_t clampDuty(int v){ if(v<0)v=-v; if(v>(int)DUTY_MAX)v=DUTY_MAX; return (uint16_t)v; }

void setDirA(int s){ if(s>0){digitalWrite(IN1,HIGH);digitalWrite(IN2,LOW);} else if(s<0){digitalWrite(IN1,LOW);digitalWrite(IN2,HIGH);} else {digitalWrite(IN1,LOW);digitalWrite(IN2,LOW);} }
void setDirB(int s){ if(s>0){digitalWrite(IN3,HIGH);digitalWrite(IN4,LOW);} else if(s<0){digitalWrite(IN3,LOW);digitalWrite(IN4,HIGH);} else {digitalWrite(IN3,LOW);digitalWrite(IN4,LOW);} }
void writeDutyA(int s){
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(ENA, clampDuty(s));
  #else
    ledcWrite(0, clampDuty(s));
  #endif
}
void writeDutyB(int s){
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(ENB, clampDuty(s));
  #else
    ledcWrite(1, clampDuty(s));
  #endif
}
void setMotors(int a, int b){ targetA=a; targetB=b; }
void stopCoast(){ targetA=0; targetB=0; }
void rampToTargets(){
  if(currentA<targetA) currentA=min(currentA+RAMP_STEP,targetA);
  else if(currentA>targetA) currentA=max(currentA-RAMP_STEP,targetA);
  if(currentB<targetB) currentB=min(currentB+RAMP_STEP,targetB);
  else if(currentB>targetB) currentB=max(currentB-RAMP_STEP,targetB);
  setDirA(currentA); setDirB(currentB);
  writeDutyA(currentA); writeDutyB(currentB);
}

// --- Route simulation ---
struct Cell { int x; int y; };
Cell routeBuf[64];
int routeLen=0;
int routeIdx=0;
unsigned long cellMS=800;
unsigned long lastStep=0;
bool moving=false;

// current "grid position" estimate
int curX=0, curY=0;

void sendHello(){
  StaticJsonDocument<256> doc;
  doc["type"]="hello";
  doc["role"]="bus";
  doc["id"]=BUS_ID;
  String out; serializeJson(doc,out);
  ws.send(out);
}

void sendPos(){
  StaticJsonDocument<256> doc;
  doc["type"]="pos";
  doc["bus"]=BUS_ID;
  doc["i"]=curX;
  doc["j"]=curY;
  String out; serializeJson(doc,out);
  ws.send(out);
}

void onMessage(WebsocketsMessage msg){
  String s = msg.data();
  StaticJsonDocument<512> doc;
  auto err = deserializeJson(doc, s);
  if(err){ Serial.println("[WS] JSON parse error"); return; }
  const char* type = doc["type"] | "";
  if(strcmp(type,"route")==0){
    // address check
    const char* to = doc["to"] | "";
    if(strlen(to)>0 && String(to) != BUS_ID){
      // not for me
      return;
    }
    JsonArray arr = doc["cells"].as<JsonArray>();
    int n=0;
    for(JsonVariant v: arr){
      if(n>=64) break;
      routeBuf[n].x = v["x"] | 0;
      routeBuf[n].y = v["y"] | 0;
      n++;
    }
    routeLen = n;
    routeIdx = 0;
    cellMS = doc["cell_ms"] | 800;
    if(routeLen>0){
      // start from current to first cell (simple straight step simulation)
      moving=true;
      lastStep=millis();
      Serial.printf("[ROUTE] %d cells received, cell_ms=%lu\n", routeLen, cellMS);
    }
  }else if(strcmp(type,"stop")==0){
    moving=false; stopCoast();
  }
}

void setup(){
  Serial.begin(115200);
  BUS_ID = "bus-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  pinMode(IN1,OUTPUT); pinMode(IN2,OUTPUT);
  pinMode(IN3,OUTPUT); pinMode(IN4,OUTPUT);

  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(ENA, PWM_FREQ, PWM_RES);
    ledcAttach(ENB, PWM_FREQ, PWM_RES);
  #else
    ledcSetup(0, PWM_FREQ, PWM_RES); ledcSetup(1, PWM_FREQ, PWM_RES);
    ledcAttachPin(ENA, 0); ledcAttachPin(ENB, 1);
  #endif
  stopCoast(); setDirA(0); setDirB(0); writeDutyA(0); writeDutyB(0);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WIFI] connecting to %s ...\n", WIFI_SSID);
  while(WiFi.status()!=WL_CONNECTED){ delay(500); Serial.print("."); }
  Serial.printf("\n[WIFI] connected, IP=%s\n", WiFi.localIP().toString().c_str());

  ws.onMessage(onMessage);
  Serial.printf("[WS] connecting to %s ...\n", HUB_HOST);
  if(!ws.connect(HUB_HOST)){
    Serial.println("[WS] connect fail"); 
  }else{
    Serial.println("[WS] connected");
    sendHello();
  }
}

void loop(){
  // keep websockets alive
  ws.poll();

  // simple stepping logic
  if(moving && millis()-lastStep >= cellMS){
    lastStep = millis();
    if(routeIdx < routeLen){
      // move one manhattan step from current towards target cell
      int tx = routeBuf[routeIdx].x;
      int ty = routeBuf[routeIdx].y;
      if(curX != tx){
        if(curX < tx){ curX++; setMotors(SPEED_FWD, SPEED_FWD); } 
        else { curX--; setMotors(-SPEED_BACK, -SPEED_BACK); }
      } else if(curY != ty){
        if(curY < ty){ curY++; setMotors(SPEED_FWD, SPEED_FWD); }
        else { curY--; setMotors(-SPEED_BACK, -SPEED_BACK); }
      } else {
        // reached this cell, advance to next target
        routeIdx++;
        if(routeIdx >= routeLen){
          moving = false; stopCoast();
        }
      }
      sendPos();
    }else{
      moving=false; stopCoast();
    }
  }

  rampToTargets();
  delay(RAMP_DELAY);
}
