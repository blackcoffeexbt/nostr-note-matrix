#include <vector>
#include <utility>
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "time.h"
#include <NostrEvent.h>
#include <NostrRelayManager.h>
#include <NostrRequestOptions.h>
#include <Wire.h>
#include "Bitcoin.h"
#include "Hash.h"
#include <esp_random.h>
#include "QRCode.h"
#include <math.h>
#include <SPIFFS.h>
#include <vector>
#include <ESP32Ping.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>
#include "wManager.h"

#include <ArduinoJson.h>

// freertos
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PARAM_FILE "/elements.json"

TaskHandle_t DisplayTaskHandle = NULL;

#define PIN            32  // Define the pin where the data line is connected to
#define MATRIX_WIDTH   32
#define MATRIX_HEIGHT  8

Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(MATRIX_WIDTH, MATRIX_HEIGHT, PIN,
  NEO_MATRIX_TOP     + NEO_MATRIX_LEFT +
  NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG,
  NEO_GRB            + NEO_KHZ800);

const uint16_t colors[] = {
  matrix.Color(255, 0, 0),
  matrix.Color(0, 255, 0),
  matrix.Color(0, 0, 255),
  matrix.Color(255, 0, 255), // purple
};  // Add more colors if you wish!

// declare a pubkey > username vector to use when creating the authors sub request
std::vector<std::pair<String, String>> pubkeyUsernameVector;

// Global message buffers shared by Serial and Scrolling functions
#define BUF_SIZE 250
char curMessage[BUF_SIZE] = {""};
char newMessage[BUF_SIZE] = {""};
bool newMessageAvailable = true;

int triggerAp = false;

bool lastInternetConnectionState = true;

int socketDisconnectedCount = 0;

NostrEvent nostr;
NostrRelayManager nostrRelayManager;
NostrQueueProcessor nostrQueue;

String serialisedEventRequest;

extern bool hasInternetConnection;

NostrRequestOptions* eventRequestOptions;

bool hasSentEvent = false;

extern char npubHexString[80];
extern char relayString[80];

fs::SPIFFSFS &FlashFS = SPIFFS;
#define FORMAT_ON_FAIL true

// define funcs
void configureAccessPoint();
void initWiFi();
bool whileCP(void);
unsigned long getUnixTimestamp();
void noteEvent(const std::string& key, const char* payload);
void okEvent(const std::string& key, const char* payload);
void nip01Event(const std::string& key, const char* payload);
void relayConnectedEvent(const std::string& key, const std::string& message);
void relayDisonnectedEvent(const std::string& key, const std::string& message);
uint16_t getRandomNum(uint16_t min, uint16_t max);
void loadSettings();
void createNoteEventRequest();
void connectToNostrRelays();

#define BUTTON_PIN 0 // change this to the pin your button is connected to
#define DOUBLE_TAP_DELAY 250 // delay for double tap in milliseconds

volatile unsigned long lastButtonPress = 0;
volatile bool doubleTapDetected = false;

void IRAM_ATTR handleButtonInterrupt() {
  unsigned long now = millis();
  if (now - lastButtonPress < DOUBLE_TAP_DELAY) {
    doubleTapDetected = true;
  }
  lastButtonPress = now;
}

// Define the WiFi event callback function
void WiFiEvent(WiFiEvent_t event) {
  switch(event) {
    case SYSTEM_EVENT_STA_GOT_IP:
      Serial.println("Connected to WiFi and got an IP");
      connectToNostrRelays();      
      break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
      Serial.println("Disconnected from WiFi");
      // WiFi.begin(ssid, password); // Try to reconnect after getting disconnected
      break;
  }
}


/**
 * @brief Create a Zap Event Request object
 * 
 */
void createNoteEventRequest() {
  // Create the REQ
  eventRequestOptions = new NostrRequestOptions();
  // Populate kinds
  int kinds[] = {1};
  eventRequestOptions->kinds = kinds;
  eventRequestOptions->kinds_count = sizeof(kinds) / sizeof(kinds[0]);

  // // Populate #p
  // Serial.println("npubHexString is |" + String(npubHexString) + "|");
  // if(String(npubHexString) != "") {
  //   Serial.println("npub is specified");
    String* authors = new String[pubkeyUsernameVector.size()];
    // add each pubkey to the authors array
    for (int i = 0; i < pubkeyUsernameVector.size(); i++) {
      authors[i] = pubkeyUsernameVector[i].first;
    }
    eventRequestOptions->authors = authors;
    eventRequestOptions->authors_count = pubkeyUsernameVector.size();
  // }

  eventRequestOptions->limit = 20;


  // We store this here for sending this request again if a socket reconnects
  serialisedEventRequest = "[\"REQ\", \"" + nostrRelayManager.getNewSubscriptionId() + "\"," + eventRequestOptions->toJson() + "]";

  delete eventRequestOptions;
}

/**
 * @brief Connect to the Nostr relays
 * 
 */
void connectToNostrRelays() {
  // first disconnect from all relays
  nostrRelayManager.disconnect();
  Serial.println("Requesting events");

  // split relayString by comma into vector
  std::vector<String> relays;
  String relayStringCopy = String(relayString);
  int commaIndex = relayStringCopy.indexOf(",");
  // while (commaIndex != -1) {
  //   relays.push_back(relayStringCopy.substring(0, commaIndex));
  //   relayStringCopy = relayStringCopy.substring(commaIndex + 1);
  //   commaIndex = relayStringCopy.indexOf(",");
  // }
  // // add last item after last comma
  // if (relayStringCopy.length() > 0) {
  //   relays.push_back(relayStringCopy);
  // }
  relays.push_back("nostr.mom");

  // no need to convert to char* anymore
  nostr.setLogging(true);
  nostrRelayManager.setRelays(relays);
  nostrRelayManager.setMinRelaysAndTimeout(1,10000);

  // Set some event specific callbacks here
  Serial.println("Setting callbacks");
  nostrRelayManager.setEventCallback("ok", okEvent);
  nostrRelayManager.setEventCallback("connected", relayConnectedEvent);
  nostrRelayManager.setEventCallback("disconnected", relayDisonnectedEvent);
  nostrRelayManager.setEventCallback(1, noteEvent);

  Serial.println("connecting");
  nostrRelayManager.connect();
}

unsigned long getUnixTimestamp() {
  time_t now;
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return 0;
  } else {
    Serial.println("Got timestamp of " + String(now));
  }
  time(&now);
  return now;
}

String lastPayload = "";

void relayConnectedEvent(const std::string& key, const std::string& message) {
  socketDisconnectedCount = 0;
  Serial.println("Relay connected: ");
  
  Serial.print(F("Requesting events:"));
  Serial.println(serialisedEventRequest);

  nostrRelayManager.broadcastEvent(serialisedEventRequest);
}

void relayDisonnectedEvent(const std::string& key, const std::string& message) {
  Serial.println("Relay disconnected: ");
  socketDisconnectedCount++;
  // reboot after 3 socketDisconnectedCount subsequenet messages
  if(socketDisconnectedCount >= 3) {
    Serial.println("Too many socket disconnections. Restarting");
    // restart device
    ESP.restart();
  }
}

void okEvent(const std::string& key, const char* payload) {
    if(lastPayload != payload) { // Prevent duplicate events from multiple relays triggering the same logic
      lastPayload = payload;
      Serial.println("payload is: ");
      Serial.println(payload);
    }
}

void nip01Event(const std::string& key, const char* payload) {
    if(lastPayload != payload) { // Prevent duplicate events from multiple relays triggering the same logic
      lastPayload = payload;
      // We can convert the payload to a StaticJsonDocument here and get the content
      StaticJsonDocument<1024> eventJson;
      deserializeJson(eventJson, payload);
      String pubkey = eventJson[2]["pubkey"].as<String>();
      String content = eventJson[2]["content"].as<String>();
      Serial.println(pubkey + ": " + content);
    }
}

void setNewMessage(const char *message) {
  int j = 0; // Index for newMessage
  for (int i = 0; message[i] && j < BUF_SIZE - 1; i++) {
    if (message[i] != '\n' && message[i] != '\r') { // Skip line breaks
      newMessage[j++] = message[i];
    }
  }
  newMessage[j] = '\0'; // Null-terminate the string

  Serial.println("New message: " + String(newMessage));
  newMessageAvailable = true;
}

uint16_t getRandomNum(uint16_t min, uint16_t max) {
  uint16_t rand  = (esp_random() % (max - min + 1)) + min;
  Serial.println("Random number: " + String(rand));
  return rand;
}

String getUsernameByPubkey(const String& pubkey) {
    for (const auto& pair : pubkeyUsernameVector) {
        if (pair.first == pubkey) {
            return pair.second;
        }
    }
    return ""; // Return an empty string if pubkey not found
}

void noteEvent(const std::string& key, const char* payload) {
    if(lastPayload != payload && !strstr(payload, "reply")) { // Prevent duplicate events from multiple relays triggering the same logic, as we are using multiple relays, this is likely to happen
      Serial.println("note payload is: ");
      Serial.println(payload);
      // get content from note
      DynamicJsonDocument eventJson(20485);
      // get err
      DeserializationError error = deserializeJson(eventJson, payload);
      if(error) {
        Serial.println("Error parsing json");
        Serial.println(error.c_str());
        return;
      }
      String content = eventJson[2]["content"].as<String>();
      Serial.println("content is: " + content);
      // get the author pubkey from the pubkey key
      String authorPubkey = eventJson[2]["pubkey"].as<String>();
      Serial.println("author pubkey is: " + authorPubkey);
      // lookup the username from the pubkey in pubkeyUsernameVector
      String username = getUsernameByPubkey(authorPubkey);
      
      // set the message
      if(username != "") {
        setNewMessage((username + " - " + content).c_str());
      } else {
      setNewMessage(content.c_str());
      }
    }
}

void setup() {
  Serial.begin(115200);
  Serial.println("boot");

  matrix.begin();
  matrix.setTextWrap(false);
  matrix.setBrightness(20);
  matrix.setTextColor(colors[3]);

  // now add some data to pubkeyUsernameVector
pubkeyUsernameVector.push_back(std::make_pair("50d94fc2d8580c682b071a542f8b1e31a200b0508bab95a33bef0855df281d63", "callebtc"));
pubkeyUsernameVector.push_back(std::make_pair("04c915daefee38317fa734444acee390a8269fe5810b2241e5e6dd343dfbecc9", "ODELL"));
pubkeyUsernameVector.push_back(std::make_pair("e9e4276490374a0daf7759fd5f475deff6ffb9b0fc5fa98c902b5f4b2fe3bba2", "benarc"));
pubkeyUsernameVector.push_back(std::make_pair("6e468422dfb74a5738702a8823b9b28168abab8655faacb6853cd0ee15deee93", "dergigi"));
pubkeyUsernameVector.push_back(std::make_pair("683211bd155c7b764e4b99ba263a151d81209be7a566a2bb1971dc1bbd3b715e", "BlackCoffee"));

  FlashFS.begin(FORMAT_ON_FAIL);
  // init spiffs
  if(!SPIFFS.begin(true)){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  xTaskCreatePinnedToCore(
    DisplayTask,               /* Task function */
    "DisplayTask",             /* Task name */
    10000,                     /* Stack depth (increase if you face any stack overflow issues) */
    NULL,                      /* Parameters for the task function */
    1,                         /* Priority */
    &DisplayTaskHandle,        /* Task handle */
    1                          /* Core where the task should run */
  );

  if (DisplayTaskHandle == NULL){
    Serial.println("Failed to create DisplayTask!");
  }

    setNewMessage("Nostr");

  randomSeed(analogRead(0)); // Seed the random number generator

  // delay(500);
  // signalWithLightning(2,250);

  WiFi.onEvent(WiFiEvent);
  init_WifiManager();

  createNoteEventRequest();

   if(hasInternetConnection) {
    Serial.println("Has internet connection. Connectring to relays");
    connectToNostrRelays();
   }

}

int x = matrix.width();

bool lastInternetConnectionCheckTime = 0;

void loop() {
  if (millis() - lastInternetConnectionCheckTime > 10000) {
    if(WiFi.status() == WL_CONNECTED) {
      IPAddress ip(9,9,9,9);  // Quad9 DNS
      bool ret = Ping.ping(ip);
      if(ret) {
        if(!lastInternetConnectionState) {
          Serial.println("Internet connection has come back! :D");
          // reboot
          ESP.restart();
        }
        lastInternetConnectionState = true;
      } else {
        lastInternetConnectionState = false;
      }
    }
  }

  nostrRelayManager.loop();
  nostrRelayManager.broadcastEvents();

  // reboot every hour
  if (millis() > 3600000) {
    Serial.println("Rebooting");
    ESP.restart();
  }

}

bool hasFinishedAnimating = false;

// The task function
void DisplayTask(void * parameter) {
  while(1) {
    if (newMessageAvailable) {
      strcpy(curMessage, newMessage);
      newMessageAvailable = false;
    }

    matrix.fillScreen(0);
  matrix.setCursor(x, 0);
  matrix.print(curMessage);
  int messageWidth = strlen(curMessage) * 6;

    if (--x < -messageWidth) {
      x = matrix.width();
      Serial.println("x is: " + String(x));
    } else {
      hasFinishedAnimating = true;
    }
    matrix.show();
    delay(40);  // Adjust speed
  }

  // while(1) { // Keep running this in a loop
  //   hasFinishedAnimating = P.displayAnimate();
    

  //   if (hasFinishedAnimating) {
  //     Serial.println("Finished animating");
      
  //     if (newMessageAvailable)
  //     {
  //       strcpy(curMessage, newMessage);
  //     }
  //     P.displayReset();
  //     newMessageAvailable = false;
  //   }
  //   // Optional delay to give other tasks time (if necessary)
  //   // vTaskDelay(pdMS_TO_TICKS(10));
  // }
}