/***** ESP32 Bus — WebSocket route follower (port 81)
 * Follows a grid path sent from the browser index.html via WebSocket.
 * Sends back {"type":"pos","i":..,"j":..} after each cell.
 *
 * Libraries:
 *   - WebSockets by Markus Sattler
 *   - ArduinoJson
 *   - WiFi (ESP32 core)
 *****/
#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include "esp32-hal-ledc.h"

// ====== Wi-Fi ======
const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASS";

// ====== L298N Pins (SAFE defaults for ESP32) ======
// NOTE: If you already wired different pins, adjust here.
// Avoid GPIOs 6-11 (flash), 34-39 (input only), 0 (boot strapping) when possible.
#define ENA 25   // Left motor speed (PWM)
#define IN1 26
#define IN2 27

#define ENB 14   // Right motor speed (PWM)
#define IN3 12
#define IN4 13

// ====== Motion Tunables ======
int pwmLeft  = 180;
int pwmRight = 180;
uint32_t oneCellMs = 900;   // Will be updated by "cell_ms" from browser
uint32_t turnLeftMs  = 250; // Tune to your robot
uint32_t turnRightMs = 250;
uint32_t uturnMs     = 450;

// ====== WebSocket ======
WebSocketsServer ws(81);

// ====== Grid / Route State ======
enum Heading {NORTH=0, EAST=1, SOUTH=2, WEST=3};
int curI = 0, curJ = 0;
Heading heading = EAST;

const int MAX_PATH = 512;
int pathI[MAX_PATH];
int pathJ[MAX_PATH];
int pathLen = 0;
int pathIdx = 0;

bool executing = false;
uint32_t stepDeadline = 0;
enum StepState {IDLE, TURNING, DRIVING, SETTLING};
StepState stepState = IDLE;

// LEDC channels (ESP32 core v3 API)
int chLeft = -1;
int chRight = -1;

// ====== Motor helpers ======
void motorsStop(){
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
  ledcWrite(chLeft, 0); ledcWrite(chRight, 0);
}
void motorsForward(){
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
  ledcWrite(chLeft, pwmLeft);
  ledcWrite(chRight, pwmRight);
}
void motorsBackward(){
  digitalWrite(IN1, LOW); digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW); digitalWrite(IN4, HIGH);
  ledcWrite(chLeft, pwmLeft);
  ledcWrite(chRight, pwmRight);
}
void motorsLeftTurn(){
  // In-place left: left wheel backward, right wheel forward
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
  ledcWrite(chLeft, pwmLeft);
  ledcWrite(chRight, pwmRight);
}
void motorsRightTurn(){
  // In-place right: left wheel forward, right wheel backward
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
  ledcWrite(chLeft, pwmLeft);
  ledcWrite(chRight, pwmRight);
}

// ====== WS send current pos ======
void sendPos(){
  StaticJsonDocument<128> doc;
  doc["type"] = "pos";
  doc["i"] = curI;
  doc["j"] = curJ;
  String out; serializeJson(doc, out);
  ws.broadcastTXT(out);
}

// ====== Heading math ======
int desiredTurnMs(Heading from, Heading to, bool &leftTurn, bool &rightTurn, bool &doUTurn){
  int diff = ((int)to - (int)from + 4) % 4;
  leftTurn = rightTurn = doUTurn = false;
  if(diff == 0) return 0;
  if(diff == 1){ rightTurn = true; return turnRightMs; }
  if(diff == 2){ doUTurn = true; return uturnMs; }
  if(diff == 3){ leftTurn  = true; return turnLeftMs; }
  return 0;
}
Heading dirToTarget(int fromI, int fromJ, int toI, int toJ){
  if(toI == fromI && toJ == fromJ-1) return NORTH;
  if(toI == fromI+1 && toJ == fromJ) return EAST;
  if(toI == fromI && toJ == fromJ+1) return SOUTH;
  if(toI == fromI-1 && toJ == fromJ) return WEST;
  return heading; // default: keep
}

// ====== Execute one segment (turn then drive one cell) ======
void beginNextSegment(){
  if(pathIdx >= pathLen-1){
    executing = false;
    motorsStop();
    return;
  }
  int toI = pathI[pathIdx+1];
  int toJ = pathJ[pathIdx+1];
  Heading need = dirToTarget(curI, curJ, toI, toJ);
  bool lt, rt, uu;
  int tms = desiredTurnMs(heading, need, lt, rt, uu);

  if(tms > 0){
    stepState = TURNING;
    if(lt) motorsLeftTurn();
    else if(rt) motorsRightTurn();
    else if(uu){ motorsLeftTurn(); } // treat U-turn as left turn timing
    stepDeadline = millis() + tms;
    heading = need; // will face this direction after turning
  }else{
    stepState = DRIVING;
    motorsForward();
    stepDeadline = millis() + oneCellMs;
  }
}

void onFinishTurning(){
  motorsStop();
  // Immediately start driving
  stepState = DRIVING;
  motorsForward();
  stepDeadline = millis() + oneCellMs;
}

void onFinishDriving(){
  motorsStop();
  // Arrived at next cell
  curI = pathI[pathIdx+1];
  curJ = pathJ[pathIdx+1];
  sendPos();
  pathIdx++;
  // Short settle before next segment
  stepState = SETTLING;
  stepDeadline = millis() + 80;
}

// ====== WS handlers ======
void handleRouteMessage(const JsonObject& obj){
  // Parse path
  JsonArray arr = obj["path"].as<JsonArray>();
  pathLen = min((int)arr.size(), MAX_PATH);
  for(int k=0; k<pathLen; ++k){
    JsonObject p = arr[k];
    pathI[k] = p["i"].as<int>();
    pathJ[k] = p["j"].as<int>();
  }
  if(obj.containsKey("cell_ms")) oneCellMs = obj["cell_ms"].as<uint32_t>();

  // Sync start position to first node (optional but recommended)
  if(pathLen > 0){
    curI = pathI[0];
    curJ = pathJ[0];
    sendPos();
  }
  // Prepare to execute
  pathIdx = 0;
  executing = (pathLen >= 2);
  stepState = IDLE;
  beginNextSegment(); // kick off
}

void wsEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t len){
  if(type == WStype_CONNECTED){
    sendPos(); // greet with current position
  }else if(type == WStype_TEXT){
    StaticJsonDocument<3072> doc;
    DeserializationError err = deserializeJson(doc, payload, len);
    if(err) return;
    const char* t = doc["type"] | "";
    if(strcmp(t, "route") == 0){
      handleRouteMessage(doc.as<JsonObject>());
    }
  }
}

// ====== setup/loop ======
void setup(){
  // PWM channels
  chLeft = ledcAttach(ENA, 20000, 8);
  chRight = ledcAttach(ENB, 20000, 8);

  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  motorsStop();

  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while(WiFi.status() != WL_CONNECTED){
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected. IP: "); Serial.println(WiFi.localIP());

  ws.begin();
  ws.onEvent(wsEvent);
}

void loop(){
  ws.loop();

  if(!executing) return;
  uint32_t now = millis();

  switch(stepState){
    case TURNING:
      if(now >= stepDeadline) onFinishTurning();
      break;
    case DRIVING:
      if(now >= stepDeadline) onFinishDriving();
      break;
    case SETTLING:
      if(now >= stepDeadline) beginNextSegment();
      break;
    case IDLE:
    default:
      beginNextSegment();
      break;
  }
}