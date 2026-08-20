/* ESP32 WebSocket 버스 (ESP32 core 3.3.2, ArduinoWebsockets 0.5.4)
   - WiFi STA로 접속 → WebSocket 서버(포트 81) 오픈
   - 브라우저로부터 {"type":"route","cell_ms":900,"path":[[i,j],...]} 수신
   - 셀 단위 전진 & 회전, 각 셀 도착 시 {"type":"pos","i":..,"j":..} 전송
   - 모터 제어: L298N, 가감속(램핑) 유지
*/

#include <WiFi.h>
#include <ArduinoWebsockets.h>
#include <ctype.h>
using namespace websockets;

// ====== WiFi ======
const char* WIFI_SSID = "GalaxyS22+";
const char* WIFI_PW   = "12345678";

// ====== WebSocket ======
WebsocketsServer wsServer;
WebsocketsClient wsClient;
bool clientConnected = false;

// ====== L298N 핀 ======
const int ENA = 27;  // PWM (왼쪽)
const int IN1 = 26;
const int IN2 = 25;

const int ENB = 18;  // PWM (오른쪽)
const int IN3 = 21;
const int IN4 = 19;

// ===== PWM 설정 =====
const uint32_t PWM_FREQ = 15000;
const uint8_t  PWM_RES  = 10;
const uint16_t DUTY_MAX = (1 << PWM_RES) - 1;

// ===== 속도/램핑 =====
const int SPEED_FWD  = 1023;
const int SPEED_TURN = 1023;
const int SPEED_BACK = 1023;

const int   RAMP_STEP  = 40;
const int   RAMP_DELAY = 8;

int targetA = 0, targetB = 0;  // -1023 ~ 1023
int currentA = 0, currentB = 0;

// ===== 타입/프로토타입을 먼저 선언 =====
struct Cell { int i; int j; };
enum Heading { NORTH=0, EAST=1, SOUTH=2, WEST=3 };

// 주행 관련 함수 프로토타입
void rotateTo(Heading target);
void driveForwardOneCell();
void goToNextCell(const Cell& next);
void startDriving();
void stepDriving();

// ===== 경로 주행 상태 =====
static const int MAX_PATH = 128;
Cell routePath[MAX_PATH];
int  routeLen = 0;
int  routeIdx = 0;
uint32_t cell_ms   = 900;   // 셀 하나 전진 시간
uint32_t turn90_ms = 250;   // 90도 회전 시간
uint32_t turn180_ms= 450;   // 180도 회전 시간
bool driving = false;

// 현재 그리드 좌표(브라우저 기준과 동기)
int curI = 0, curJ = 0;
Heading heading = NORTH;

// ---------------------
// Core 3.x / 2.x 호환 래퍼
// ---------------------
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void pwmInit() { ledcAttach(ENA, PWM_FREQ, PWM_RES); ledcAttach(ENB, PWM_FREQ, PWM_RES); }
inline void pwmWriteA(uint16_t d){ ledcWrite(ENA, d); }
inline void pwmWriteB(uint16_t d){ ledcWrite(ENB, d); }
#else
const int CH_A = 0; const int CH_B = 1;
void pwmInit(){
  ledcSetup(CH_A, PWM_FREQ, PWM_RES);
  ledcSetup(CH_B, PWM_FREQ, PWM_RES);
  ledcAttachPin(ENA, CH_A);
  ledcAttachPin(ENB, CH_B);
}
inline void pwmWriteA(uint16_t d){ ledcWrite(CH_A, d); }
inline void pwmWriteB(uint16_t d){ ledcWrite(CH_B, d); }
#endif

// ===== 유틸 =====
static inline uint16_t clampDuty(int v){ if(v<0) v=-v; if(v>(int)DUTY_MAX) v=DUTY_MAX; return (uint16_t)v; }

void setDirA(int speed){
  if (speed > 0)      { digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);  }
  else if (speed < 0) { digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH); }
  else                { digitalWrite(IN1, LOW);  digitalWrite(IN2, LOW);  }
}
void setDirB(int speed){
  if (speed > 0)      { digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);  }
  else if (speed < 0) { digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH); }
  else                { digitalWrite(IN3, LOW);  digitalWrite(IN4, LOW);  }
}
void writeDutyA(int speed){ pwmWriteA(clampDuty(speed)); }
void writeDutyB(int speed){ pwmWriteB(clampDuty(speed)); }

void setMotors(int a, int b){ targetA = a; targetB = b; }
void stopCoast(){ targetA = 0; targetB = 0; }
void brakeNow(){
  digitalWrite(IN1, HIGH); digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, HIGH);
  pwmWriteA(0); pwmWriteB(0);
  currentA = currentB = targetA = targetB = 0;
}

void rampToTargets(){
  if (currentA < targetA)      currentA = min(currentA + RAMP_STEP, targetA);
  else if (currentA > targetA) currentA = max(currentA - RAMP_STEP, targetA);
  if (currentB < targetB)      currentB = min(currentB + RAMP_STEP, targetB);
  else if (currentB > targetB) currentB = max(currentB - RAMP_STEP, targetB);
  setDirA(currentA); setDirB(currentB);
  writeDutyA(currentA); writeDutyB(currentB);
}

// ===== JSON 전송 =====
void sendPos(){
  if(!clientConnected) return;
  String payload = String("{\"type\":\"pos\",\"i\":") + String(curI) +
                   String(",\"j\":") + String(curJ) + String("}");
  wsClient.send(payload);
}
void sendHello(){
  if(!clientConnected) return;
  wsClient.send(String("{\"type\":\"hello\",\"role\":\"bus\"}"));
}

// ===== route 파서 =====
// 기대 형식: {"type":"route","cell_ms":900,"path":[[i,j],[i,j],...]}
bool parseRoute(const String& msg){
  if (msg.indexOf("\"type\":\"route\"") < 0) return false;

  // cell_ms
  int cmsIdx = msg.indexOf("\"cell_ms\":");
  if (cmsIdx >= 0){
    int start = cmsIdx + 10;
    int end = start;
    while (end < (int)msg.length() && isdigit((unsigned char)msg[end])) end++;
    cell_ms = msg.substring(start, end).toInt();
    if (cell_ms < 100) cell_ms = 100;
  }

  // path
  int p0 = msg.indexOf("\"path\":[");
  if (p0 < 0) return false;
  int p1 = msg.indexOf(']', p0);
  if (p1 < 0) return false;
  String arr = msg.substring(p0, p1+1); // "path":[[i,j],...]

  routeLen = 0;
  int idx = arr.indexOf('[');
  while (idx >= 0 && routeLen < MAX_PATH){
    int a = arr.indexOf('[', idx+1);
    if (a < 0) break;
    int comma = arr.indexOf(',', a+1);
    int close = arr.indexOf(']', comma+1);
    if (comma < 0 || close < 0) break;
    int vi = arr.substring(a+1, comma).toInt();
    int vj = arr.substring(comma+1, close).toInt();
    routePath[routeLen++] = {vi, vj};
    idx = close+1;
  }
  return (routeLen > 0);
}

// ===== 주행 로직 =====
void rotateTo(Heading target){
  int diff = ((int)target - (int)heading + 4) % 4;
  if (diff == 0) return;
  if (diff == 2){
    setMotors(+SPEED_TURN, -SPEED_TURN); delay(turn180_ms); stopCoast();
  } else if (diff == 1){
    setMotors(+SPEED_TURN, -SPEED_TURN); delay(turn90_ms);  stopCoast();
  } else { // diff == 3
    setMotors(-SPEED_TURN, +SPEED_TURN); delay(turn90_ms);  stopCoast();
  }
  heading = target;
}

void driveForwardOneCell(){
  setMotors(+SPEED_FWD, +SPEED_FWD);
  delay(cell_ms);
  stopCoast();
}

void goToNextCell(const Cell& next){
  int di = next.i - curI;
  int dj = next.j - curJ;

  if (di == -1 && dj == 0)      rotateTo(NORTH);
  else if (di == +1 && dj == 0) rotateTo(SOUTH);
  else if (di == 0 && dj == +1) rotateTo(EAST);
  else if (di == 0 && dj == -1) rotateTo(WEST);
  else { brakeNow(); return; } // 비연속/대각 보호

  driveForwardOneCell();
  curI = next.i; curJ = next.j;
  sendPos();
}

void startDriving(){
  if (routeLen <= 0) return;
  // 시작 지점 동기화
  curI = routePath[0].i;
  curJ = routePath[0].j;
  sendPos();
  routeIdx = 1;
  driving = true;
}

void stepDriving(){
  if (!driving) return;
  if (routeIdx >= routeLen){
    driving = false;
    stopCoast();
    return;
  }
  goToNextCell(routePath[routeIdx]);
  routeIdx++;
}

// ===== WebSocket 콜백 =====
void onMessage(WebsocketsMessage msg){
  String m = msg.data();
  if (parseRoute(m)){ startDriving(); return; }
  if (m.indexOf("\"type\":\"stop\"") >= 0){
    driving = false; brakeNow(); return;
  }
}
void onWSClientEvent(WebsocketsEvent event, String data){
  // 필요 시 로깅 (생략)
}

// ===== SETUP / LOOP =====
void setup(){
  Serial.begin(115200);

  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  pwmInit();
  stopCoast(); setDirA(0); setDirB(0);
  pwmWriteA(0); pwmWriteB(0);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PW);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED){ delay(300); Serial.print("."); }
  Serial.println();
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  wsServer.listen(81);
  Serial.println("WebSocket server listening on :81");
}

void loop(){
  // 신규 클라이언트 수락
  if (!clientConnected){
    auto client = wsServer.accept();
    if (client.available()){
      wsClient = client;
      clientConnected = true;
      wsClient.onMessage(onMessage);
      wsClient.onEvent(onWSClientEvent);
      sendHello();
    }
  } else {
    // 클라이언트 유지/폴링
    if (wsClient.available()){
      wsClient.poll();
    } else {
      clientConnected = false;
    }
  }

  // 경로 주행
  stepDriving();

  // 램핑 적용
  rampToTargets();
  delay(RAMP_DELAY);
}
