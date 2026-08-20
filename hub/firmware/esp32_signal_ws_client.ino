// === ESP32 SIGNAL (WebSockets client) ===
#include <WiFi.h>
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>

using namespace websockets;

// ---- USER CONFIG ----
const char* WIFI_SSID = "KT_GiGA_3977";
const char* WIFI_PASS = "cd73fd8418";
const char* HUB_HOST  = "ws://172.30.1.254:81"; // set to PC IP running hub

// LED pins
const int PIN_RED   = 16;
const int PIN_GREEN = 17;
const int PIN_YELLOW= 5;

WebsocketsClient ws;
String SIGNAL_ID;

void setLight(const char* state){
  if(strcmp(state,"green")==0){
    digitalWrite(PIN_RED,LOW); digitalWrite(PIN_YELLOW,LOW); digitalWrite(PIN_GREEN,HIGH);
  }else if(strcmp(state,"red")==0){
    digitalWrite(PIN_RED,HIGH); digitalWrite(PIN_YELLOW,LOW); digitalWrite(PIN_GREEN,LOW);
  }else if(strcmp(state,"yellow")==0){
    digitalWrite(PIN_RED,LOW); digitalWrite(PIN_YELLOW,HIGH); digitalWrite(PIN_GREEN,LOW);
  }else{
    digitalWrite(PIN_RED,LOW); digitalWrite(PIN_YELLOW,LOW); digitalWrite(PIN_GREEN,LOW);
  }
}

void sendHello(){
  StaticJsonDocument<200> doc;
  doc["type"]="hello";
  doc["role"]="signal";
  doc["id"]=SIGNAL_ID;
  String out; serializeJson(doc,out);
  ws.send(out);
}

void onMessage(WebsocketsMessage msg){
  StaticJsonDocument<256> doc;
  auto err = deserializeJson(doc, msg.data());
  if(err) return;
  const char* type = doc["type"] | "";
  if(strcmp(type,"signal")==0){
    // Optionally check "to" or "id"
    const char* id = doc["id"] | "";
    if(strlen(id)>0 && String(id)!=SIGNAL_ID) return;
    const char* st = doc["state"] | "off";
    setLight(st);
  }
}

void setup(){
  Serial.begin(115200);
  pinMode(PIN_RED,OUTPUT); pinMode(PIN_YELLOW,OUTPUT); pinMode(PIN_GREEN,OUTPUT);
  setLight("off");

  SIGNAL_ID = "sig-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WIFI] connecting to %s ...\n", WIFI_SSID);
  while(WiFi.status()!=WL_CONNECTED){ delay(500); Serial.print("."); }
  Serial.printf("\n[WIFI] connected, IP=%s\n", WiFi.localIP().toString().c_str());

  ws.onMessage(onMessage);
  Serial.printf("[WS] connecting to %s ...\n", HUB_HOST);
  if(ws.connect(HUB_HOST)){
    Serial.println("[WS] connected");
    sendHello();
  }else{
    Serial.println("[WS] connect fail");
  }
}

void loop(){
  ws.poll();
  delay(10);
}
