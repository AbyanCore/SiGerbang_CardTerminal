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

// Define Wifi credentials
const char WIFI_SSID[] PROGMEM  = "dev.insidertech.id"; // Wifi SSID
const char WIFI_PASS[] PROGMEM  = "letmein1234";      // Wifi Password

// Define WebSocket server
#define WS_SERVER "192.168.88.10" // WebSocket server address
#define WS_PORT   8080    // WebSocket server port
#define WS_PATH   "/"     // WebSocket server path

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
  const String cardType = rfid.PICC_GetTypeName(piccType);
  const String cardUid = String(uid.uidByte[0], HEX) + " " + String(uid.uidByte[1], HEX) + " " + String(uid.uidByte[2], HEX) + " " + String(uid.uidByte[3], HEX);
  JsonDocument data_doc;
  data_doc["cardType"] = cardType;
  data_doc["cardUid"] = cardUid;
  JsonDocument res;
  res["from"] = ESP.getEfuseMac();
  res["data"] = data_doc.as<String>();
  res["to"] = "all";

  wsc.send(res.as<String>());

  // print card information to display
  printToDisplay("Type: " + cardType + "\n" + "UID: " + cardUid);
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
    JsonDocument res;
    
    if (command == "getDeviceInfo") {
      prefs.begin("esp32", true);
      res["data"] = prefs.getString("device_info");
      res["to"] = from;
      res["from"] = ESP.getEfuseMac();
      wsc.send(res.as<String>());
      prefs.end();
      res.clear();
      return;
    } 
    
    if (command == "getEvents") {
      prefs.begin("events", true);
      res["data"] = prefs.getString("events");
      res["to"] = from;
      res["from"] = ESP.getEfuseMac();
      wsc.send(res.as<String>());
      prefs.end();
      res.clear();
      return;
    }
    
    if (command == "restart") {
      res["data"] = "Restarting device in 3 seconds";
      res["to"] = from;
      res["from"] = ESP.getEfuseMac();
      wsc.send(res.as<String>());
      res.clear();

      delay(3000);

      wsc.close();
      ESP.restart();
      return;
    }

    if (command == "printToDisplay") {
      printToDisplay(data["message"].as<String>());
      res["data"] = "Message printed to display : " + data["message"].as<String>();
      res["to"] = from;
      res["from"] = ESP.getEfuseMac();
      wsc.send(res.as<String>());
      res.clear();
      return;
    }
    return;
  }
}

void onWebSocketEventsCallback(WebsocketsEvent event, String data) {
    if(event == WebsocketsEvent::ConnectionOpened) {
      prefs.begin("esp32", true);
      wsc.send(prefs.getString("device_info"));
      prefs.end();
      printToDisplay("Connected to WebSocket");
    } else if(event == WebsocketsEvent::ConnectionClosed) {
      printToDisplay("Disconnected from WebSocket");
    } else if(event == WebsocketsEvent::GotPing) {
      Serial.println("Got a Ping!");
    } else if(event == WebsocketsEvent::GotPong) {
      Serial.println("Got a Pong!");
    }
}

void onWifiEventsCallback(WiFiEvent_t event) {
  switch (event) {
    case WIFI_EVENT_STA_CONNECTED:
      printToDisplay("Connected to WiFi");
      break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
      printToDisplay("Disconnected from WiFi \n Reconnecting ...");
      break;
    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  SPI.begin();
  oledWire.begin(SDA_PIN, SCL_PIN);

  prefs.begin("esp32", false);
  if (prefs.getString("device_info").length() == 0) {
    JsonDocument device_doc;
    device_doc["device_id"] = ESP.getEfuseMac();
    device_doc["device_type"] = "ESP32";
    device_doc["device_name"] = "RFID-ESP32";
    prefs.putString("device_info", device_doc.as<String>());
  }
  prefs.end();

  // init functions can be called from websockets
  prefs.begin("events", false);
  JsonDocument events_doc;
  events_doc["restart"] = "args: none; description: restart the device";
  events_doc["getDeviceInfo"] = "args: none; description: get device information";
  events_doc["getEvents"] = "args: none; description: get available events";
  events_doc["printToDisplay"] = "args: message(str); description: print message to display";
  prefs.putString("events", events_doc.as<String>());
  prefs.end();

  // init Peripherals
  rfid.PCD_Init();
  display.setTextColor(SSD1306_WHITE);
  pinMode(BUILTIN_LED, OUTPUT);

  // make loop until both peripherals are connected
  while (!checkPeripherals()) {
    Serial.println("Check peripherals ... ");
    delay(1000);
  }

  // Connect to WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWifiEventsCallback);
  while (WiFi.status() != WL_CONNECTED) {
    printToDisplay("Connecting to WiFi...");
    delay(1000);
  }

  // register WebSocket event handlers
  wsc.onMessage(onMessageCallback);
  wsc.onEvent(onWebSocketEventsCallback);

  // connect to WebSocket server 
  wsc.connect(WS_SERVER, WS_PORT, WS_PATH);

  wsc.ping();

  printToDisplay("Ready");
}

void loop() {
  rcReader(handleCardRead);
  
  // check websocket connection and reconnect if necessary
  if (!wsc.available() && !wsc.connect(WS_SERVER, WS_PORT, WS_PATH))
    printToDisplay("websocket reconnecting");
  else 
    wsc.poll();
}