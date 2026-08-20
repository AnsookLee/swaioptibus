/* ESP32 신호등 WebSocket 수신기 (ESP32 core 3.3.2, ArduinoWebsockets 0.5.4)
   - WiFi STA로 접속 후 WebSocket 서버(포트 82)
   - 브라우저 또는 버스 ESP32에서 {"type":"light","state":"RED|YELLOW|GREEN"} 수신
   - 각 색상에 맞게 핀 점등
*/

#include <WiFi.h>
#include <ArduinoWebsockets.h>
using namespace websockets;

// ===== WiFi 설정 =====
const char* WIFI_SSID = "GalaxyS22+";
const char* WIFI_PW   = "12345678";

// ===== WebSocket 서버 =====
WebsocketsServer wsServer;
WebsocketsClient wsClient;
bool clientConnected = false;

// ===== 신호등 핀 =====
const int PIN_RED    = 16;
const int PIN_GREEN  = 17;
const int PIN_YELLOW = 5;

// ===== 현재 상태 =====
String currentState = "OFF";

// ===== 함수 선언 =====
void applyLight(const String& color);
bool parseLightMsg(const String& msg);

// ===== 함수 정의 =====
void applyLight(const String& color) {
  digitalWrite(PIN_RED, LOW);
  digitalWrite(PIN_GREEN, LOW);
  digitalWrite(PIN_YELLOW, LOW);

  if (color == "RED") digitalWrite(PIN_RED, HIGH);
  else if (color == "GREEN") digitalWrite(PIN_GREEN, HIGH);
  else if (color == "YELLOW") digitalWrite(PIN_YELLOW, HIGH);

  currentState = color;
  Serial.print("Light changed to: ");
  Serial.println(color);
}

// {"type":"light","state":"RED"} 형식 파서
bool parseLightMsg(const String& msg) {
  if (msg.indexOf("\"type\":\"light\"") < 0) return false;

  int sIdx = msg.indexOf("\"state\":\"");
  if (sIdx < 0) return false;

  int start = sIdx + 9;
  int end = msg.indexOf("\"", start);
  if (end < 0) return false;

  String state = msg.substring(start, end);
  applyLight(state);
  return true;
}

// 메시지 콜백
void onMessage(WebsocketsMessage msg) {
  String m = msg.data();
  if (parseLightMsg(m)) return;
  if (m.indexOf("\"type\":\"ping\"") >= 0) {
    wsClient.send("{\"type\":\"pong\"}");
  }
}

// WebSocket 연결 상태 콜백
void onClientEvent(WebsocketsEvent event, String data) {
  if (event == WebsocketsEvent::ConnectionClosed) {
    Serial.println("Client disconnected");
    clientConnected = false;
  }
}

// ===== Setup / Loop =====
void setup() {
  Serial.begin(115200);

  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_GREEN, OUTPUT);
  pinMode(PIN_YELLOW, OUTPUT);

  digitalWrite(PIN_RED, LOW);
  digitalWrite(PIN_GREEN, LOW);
  digitalWrite(PIN_YELLOW, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PW);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  wsServer.listen(82);
  Serial.println("Signal WebSocket server started on port 82");
}

void loop() {
  // 클라이언트 연결 수락
  if (!clientConnected) {
    auto client = wsServer.accept();
    if (client.available()) {
      wsClient = client;
      clientConnected = true;
      wsClient.onMessage(onMessage);
      wsClient.onEvent(onClientEvent);
      Serial.println("Client connected to signal controller");
    }
  } else {
    if (wsClient.available()) {
      wsClient.poll();
    } else {
      clientConnected = false;
    }
  }
}
