#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <functional>
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp32-hal-cpu.h>
#include <Esp.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>

// Define the pins for the rc522 RFID reader
#define SS_PIN  5   // ESP32 DevKit V1 pin GPIO5
#define RST_PIN 27  // ESP32 DevKit V1 pin GPIO27

// Define the pins for the OLED display
#define SDA_PIN 21  // ESP32 DevKit V1 pin GPIO21
#define SCL_PIN 22  // ESP32 DevKit V1 pin GPIO22

#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels

#define OLED_RESET     -1 // Reset pin # (or -1 if sharing Arduino reset pin)
#define SCREEN_ADDRESS 0x3C ///< See datasheet for Address; 0x3D for 128x64, 0x3C for 128x32

// Initialize Peripherals
MFRC522 rfid(SS_PIN, RST_PIN);
TwoWire oledWire(0);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &oledWire, OLED_RESET);
websockets::WebsocketsClient wsc;
Preferences prefs;
DNSServer dnsServer;
AsyncWebServer wsr(80);

// simple captive portal page for wifi configuration and websocket server configuration
const char* captivePortalPage = R"(
<!DOCTYPE html>
<html>
<head>
    <title>Configuration Page</title>
</head>
<body>
    <h3>WiFi Configuration</h3>
    <form action="/save-config" method="get">
        <label for="ssid">SSID:</label><br>
        <input type="text" id="ssid" name="ssid" required><br>
        <label for="password">Password:</label><br>
        <input type="password" id="password" name="password" required><br><br>

        <h3>WebSocket Configuration</h3>
        <label for="server">Server:</label><br>
        <input type="text" id="server" name="server" required><br>
        <label for="port">Port:</label><br>
        <input type="text" id="port" name="port" required><br>
        <label for="path">Path:</label><br>
        <input type="text" id="path" name="path" required><br><br>

        <h3>Configuration Key</h3>
        <label for="path">Key:</label><br>
        <input type="text" id="key" name="key" required><br><br>

        <input type="submit" value="Save">
    </form>
</body>
</html>
)";

JsonDocument makeResponse(String topic,String data,String to,String from = String(ESP.getEfuseMac()))
{
  JsonDocument res;
  res["topic"] = topic;
  res["from"] = from;
  res["data"] = data;
  res["to"] = to;
  return res;
}

JsonDocument makeResponse(String topic,JsonDocument data,String to,String from = String(ESP.getEfuseMac()))
{
  JsonDocument res;
  res["topic"] = topic;
  res["from"] = from;
  res["data"] = data;
  res["to"] = to;
  return res;
}

bool checkPeripherals()
{
  bool oledConnected = false;
  bool rfidConnected = false;

  // Check if the OLED display is connected
  oledConnected = display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
  if (!oledConnected) {
    Serial.println(F("SSD1306 allocation failed"));
  }

  // Check if the RFID reader is connected
  byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  rfidConnected = (v != 0x00 && v != 0xFF);
  if (!rfidConnected) {
    Serial.println(F("RFID reader not detected"));
  }

  return (oledConnected && rfidConnected);
}

void printToDisplay(String message)
{
  display.clearDisplay();
  display.setCursor(0, 0);
  display.print(message);
  display.display();
}

void turnlamp1s(void *parameter)
{
  digitalWrite(BUILTIN_LED, HIGH);
  vTaskDelay(200 / portTICK_PERIOD_MS);
  digitalWrite(BUILTIN_LED, LOW);
  vTaskDelete(NULL);
}

void handleCardRead(MFRC522::Uid uid, MFRC522::PICC_Type piccType){
  xTaskCreatePinnedToCore(turnlamp1s, "turnlamp1s", 2048, NULL, 1, NULL, 0);
  // const String cardType = rfid.PICC_GetTypeName(piccType);
  // const String cardUid = String(uid.uidByte[0], HEX) + " " + String(uid.uidByte[1], HEX) + " " + String(uid.uidByte[2], HEX) + " " + String(uid.uidByte[3], HEX);
  JsonDocument data_doc;
  data_doc["cardType"] = rfid.PICC_GetTypeName(piccType);
  data_doc["cardUid"] = String(uid.uidByte[0], HEX) + " " + String(uid.uidByte[1], HEX) + " " + String(uid.uidByte[2], HEX) + " " + String(uid.uidByte[3], HEX);
  wsc.send(makeResponse("rfid_data",data_doc, "server").as<String>());

  // print card information to display
  printToDisplay("Type: " + data_doc["cardType"].as<String>() + "\n" + "UID: " + data_doc["cardUid"].as<String>());
}

void rcReader(std::function<void(MFRC522::Uid, MFRC522::PICC_Type)> callback)
{
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) { // new tag is available && NUID has been read
    if (callback) {
      callback(rfid.uid, rfid.PICC_GetType(rfid.uid.sak)); 
    }
    rfid.PICC_HaltA(); // halt PICC
    rfid.PCD_StopCrypto1(); // stop encryption on PCD
  }
}

void resetDevice() {
  printToDisplay("Resetting device");
  prefs.begin("cred", false);
  prefs.clear();
  prefs.end();
  prefs.begin("esp32", false);
  prefs.clear();
  prefs.end();
  delay(1000);
  printToDisplay("Restarting device");
  delay(1000);
  ESP.restart();
}

using namespace websockets;

void onMessageCallback(WebsocketsMessage message) {
  JsonDocument data;
  DeserializationError error = deserializeJson(data, message.data());
  if (error) {
    Serial.print("deserializeJson() failed: ");
    Serial.println(error.c_str());
    return;
  }


  if (data.containsKey("command")) {
    const String command = data["command"];
    const String from = data["from"];
    const String topic = data["topic"];
    const String to = data["to"];

    prefs.begin("esp32", true);
    if (to != prefs.getString("device_id")) {
      prefs.end();
      return;
    }
    prefs.end();
    
    if (command == "getDeviceInfo") {
      prefs.begin("esp32", true);
      JsonDocument device_info;
      device_info["device_id"] = prefs.getString("device_id");
      device_info["device_type"] = prefs.getString("device_type");
      device_info["device_name"] = prefs.getString("device_name");
      wsc.send(makeResponse(
        topic,
        device_info, 
        from
      ).as<String>());
      prefs.end();
      return;
    }

    if (command == "upgrade") {
      prefs.begin("cred", false);
      prefs.putString("path", data["data"].as<String>());
      prefs.end();
      wsc.send(makeResponse(
        topic,
        "upgrade to " + data["data"].as<String>(),
        from
      ).as<String>());
      return;
    }

    if (command == "verify") {
      prefs.begin("esp32", true);
      wsc.send(makeResponse(
        topic,
        prefs.getString("device_id"),
        from
      ).as<String>());
      prefs.end();
    }
    
    if (command == "getEvents") {
      prefs.begin("esp32", true);
      JsonArray events;
      wsc.send(makeResponse(
        topic,
        prefs.getString("events"),
        from
      ).as<String>());
      prefs.end();
      return;
    }
    
    if (command == "restart") {
      wsc.send(makeResponse(
        topic,
        "Restarting device in 3 seconds", 
        from
      ).as<String>());

      delay(3000);

      wsc.close();
      ESP.restart();
      return;
    }

    if (command == "printToDisplay") {
      wsc.send(makeResponse(
        topic,
        "Message printed to display : " + data["data"].as<String>(),
        from
      ).as<String>());
      return;
    }
    return;
  }
}

void onWebSocketEventsCallback(WebsocketsEvent event, String data) {
    if(event == WebsocketsEvent::ConnectionOpened) {
      printToDisplay("Connected to WebSocket");
    } else if(event == WebsocketsEvent::ConnectionClosed) {
      printToDisplay("Disconnected from WebSocket");
    } else if(event == WebsocketsEvent::GotPing) {
      wsc.pong();
    } else if(event == WebsocketsEvent::GotPong) {
      wsc.ping();
    }
}

void onWifiEventsCallback(WiFiEvent_t event) {
  int attempt_remaining = 10;
  switch (event) {
    case WIFI_EVENT_STA_START:
      printToDisplay("Connecting to WiFi ...");
      break;
    case WIFI_EVENT_STA_CONNECTED:
      printToDisplay("Connected to WiFi");
      break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
      printToDisplay("Disconnected from WiFi");
      break;
    default:
      break;
  }
}

void data_init_once() {
  prefs.begin("esp32", false);
  if (!prefs.isKey("device_id")) {
    Serial.println("Generate Metadata");
    prefs.putString("device_id", String(ESP.getEfuseMac()));
    prefs.putString("device_type", "ESP32");
    prefs.putString("device_name", "RFID-ESP32");
  }
  prefs.putString("events", R"(
  [
    "restart-: args: none; description: restart the device",
    "getDeviceInfo-: args: none; description: get device information",
    "getEvents-: args: none; description: get available events",
    "printToDisplay-: args: message(str); description: print message to display"
  ]
  )");
  prefs.end();

  prefs.begin("cred",false);
  if (!prefs.isKey("ssid") && !prefs.isKey("server")) {
    prefs.putBool("captive_portal", false);
    Serial.println("ssid" + prefs.getString("ssid"));
    Serial.println("server" + prefs.getString("server"));
    prefs.end();
    Serial.println("Credentials not found");
  }
}

class CaptivePortalHandler : public AsyncWebHandler {
public:
  CaptivePortalHandler() {}
  virtual ~CaptivePortalHandler() {}

  bool canHandle(AsyncWebServerRequest *request){
    return request->url() == "/";
  }

  void handleRequest(AsyncWebServerRequest *request) {
    if (request->method() == HTTP_GET && request->url() == "/") {
      request->send(200, "text/html", captivePortalPage);
    } else {
      request->send(200, "text/html", captivePortalPage);
    }
  }
};

class FormHandler : public AsyncWebHandler {
public:
  FormHandler() {}
  virtual ~FormHandler() {}

  bool canHandle(AsyncWebServerRequest *request){
    return request->url() == "/save-config";
  }

  void handleRequest(AsyncWebServerRequest *request) {
    if (request->url() == "/save-config") {

      if (request->arg("key") != "DDN") {
        request->send(200, "text/html", "Invalid key");
        return;
      };

      prefs.begin("cred", false);
      prefs.putBool("captive_portal", true);
      prefs.putString("ssid", request->arg("ssid"));
      prefs.putString("pass", request->arg("password"));
      prefs.putString("server", request->arg("server"));
      prefs.putInt("port", request->arg("port").toInt());
      prefs.putString("path", request->arg("path"));
      prefs.end();

      request->send(200, "text/html", "Configuration saved. Restarting device ...");
      ESP.restart();
    } else {
      request->send(200, "text/html", captivePortalPage);
    }
  }
};

void checkTouch(int pin) {
    static uint32_t touchStart = 0;
    if (touchRead(pin) < 40) { 
        if (touchStart == 0) {
            touchStart = millis();
        } else if (millis() - touchStart >= 5000) {
            resetDevice();
            touchStart = 0;
        }
    } else {
        touchStart = 0;
    }
}

void setup() {
  Serial.begin(115200);
  SPI.begin();
  oledWire.begin(SDA_PIN, SCL_PIN);

  // init metadata device
  data_init_once();

  // init Peripherals
  rfid.PCD_Init();
  display.setTextColor(SSD1306_WHITE);
  pinMode(BUILTIN_LED, OUTPUT);

  // make loop until both peripherals are connected
  while (!checkPeripherals()) delay(1000);

  prefs.begin("cred", true);
  if (!prefs.getBool("captive_portal")) {
    prefs.end();
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Card-Terminal");
    printToDisplay("Setup Mode ON");
  
    wsr.addHandler(new CaptivePortalHandler()).setFilter(ON_AP_FILTER);
    wsr.addHandler(new FormHandler()).setFilter(ON_AP_FILTER);
    wsr.onNotFound([&](AsyncWebServerRequest *request){
      request->send(200, "text/html", captivePortalPage); 
    });

    dnsServer.start(53, "*", WiFi.softAPIP());

    wsr.begin();

    while (true) {
      dnsServer.processNextRequest();
    }
  }

  // Connect to WiFi
  WiFi.onEvent(onWifiEventsCallback);
  WiFi.setAutoReconnect(true);
  prefs.begin("cred", true);
  WiFi.begin(prefs.getString("ssid"), prefs.getString("pass"));
  prefs.end();
  while (WiFi.status() != WL_CONNECTED) checkTouch(T0);

  // register WebSocket event handlers
  wsc.onMessage(onMessageCallback);
  wsc.onEvent(onWebSocketEventsCallback);

  // connect to WebSocket server
  prefs.begin("cred", false); 
  wsc.connect(
    prefs.getString("server"),
    prefs.getInt("port"),
    prefs.getString("path")
  );
  prefs.end();

}

void loop() {
  checkTouch(T0);

  // check websocket connection and reconnect if necessary
  prefs.begin("websocket", false);
  if (!wsc.available() && !wsc.connect(prefs.getString("server"),
    prefs.getInt("port"),
    prefs.getString("path"))) {
    prefs.end();
    delay(5000);
    return;
  }

  rcReader(handleCardRead);
  wsc.poll();
}