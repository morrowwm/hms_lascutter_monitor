/***************************************************
Monitor for HMS laser cutter
 ****************************************************/
#include <ESP8266WiFi.h>
#include <DallasTemperature.h>
#include <FS.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <WebSocketsClient.h>
#include <WebSocketsServer.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>

#include "secrets.h"

#define ONE_HOUR 3600000UL

/************************* Temperature and other Sensors *********************************/

#define TEMP_SENSOR_PIN D4
#define ONE_WIRE_MAX_DEV 15 //The maximum number of devices
OneWire oneWire(TEMP_SENSOR_PIN);        // Set up a OneWire instance to communicate with OneWire devices

DallasTemperature tempSensors(&oneWire); // Create an instance of the temperature sensor class
int numberOfDevices = 0;

const int GATE_OPEN = D3;   // 
const int GATE_CLOSED = D7; // 
const int READY_TO_CUT = D6;
const int NOT_READY_TO_CUT = D5;

/************************* WiFi Access Point *********************************/
// in secrets.h

WiFiClientSecure client;

/************************* Web Server, NTP , Telegram*********************************/
ESP8266WebServer server(80);       // create a web server on port 80                                  // a File variable to temporarily store the received file

const int NTP_PACKET_SIZE = 48;          // NTP time stamp is in the first 48 bytes of the message
byte packetBuffer[NTP_PACKET_SIZE];      // A buffer to hold incoming and outgoing packets
IPAddress timeServerIP;        // The time.nist.gov NTP server's IP address
const char* ntpServerName = "time.nist.gov";

WebSocketsClient webSocket;

// Telegram config
UniversalTelegramBot bot(BOTtoken, client);

/*
 * mean time between scan messages 1000ms = 1 sec
 * keep this a few seconds longer than the data read interval, to keep timing right
 * there's a blocking delay in the UniversalTelegramBot which make getUpdates take a few seconds
 */

unsigned long botInterval = 10000; 
unsigned long lastBotTime;

int bulk_messages_mtbs = 1500; // mean time between send messages, 1.5 seconds
int messages_limit_per_second = 25; // Telegram API have limit for bulk messages ~30 messages per second

String subscribed_users_filename = "subscribed_users.json";

DynamicJsonBuffer jsonBuffer;

WiFiUDP UDP; 

float temperature[ONE_WIRE_MAX_DEV];
float lastTemperature = -100;
float ventTemperature = 0.0;
float lastAlarmVal = 0.0;

void setup() {
  Serial.begin(115200);
  delay(10);

  Serial.println(F("Monitor HMS laser cutter"));

  startWiFi();
  startSensors();
  startUDP();                  // Start listening for UDP messages to port 123

  UniversalTelegramBot bot(BOTtoken, client);
  
  WiFi.hostByName(ntpServerName, timeServerIP); // Get the IP address of the NTP server
  Serial.print("Time server IP:\t");
  Serial.println(timeServerIP);

  sendNTPpacket(timeServerIP);
  delay(500);
  
  String message = "The HMS laser cutter monitor is starting.";
  Serial.println(message);
  sendMessageToAllSubscribedUsers(message);

}

const unsigned long intervalNTP = ONE_HOUR; // Update the time every hour
unsigned long prevNTP = 0;
unsigned long lastNTPResponse = millis();
uint32_t timeUNIX = 0;                      // The most recent timestamp received from the time server

const unsigned long dataInterval = 2000;   // Do a temperature measurement 2 seconds
const unsigned long publishInterval = 30000;   // Publish every 5 minutes - 300000 ms
long lastReadTime = -600000;
long lastPublishTime = -600000;
long lastAlarmTime = 10000; // set this to a bit after start to avoid startup spurious alarms
const unsigned long alarmInterval = 30000;
float lowAlarmLimit = 10.0, hiAlarmLimit = 35.0;
bool inAlarm = false;

const unsigned long DS_delay = 750;         // Reading the temperature from the DS18x20 can take up to 750ms
const unsigned long switchBounce = 750;     // give switches some time to bounce

bool tmpRequested = false;
unsigned long lastSwitchRead = -600000;

int gateOpen = 0, gateClosed = 0, lastGateOpen = 0, lastGateClosed = 0;
unsigned long lastMovedFromClosed = 0;

void loop() {
  unsigned long currentMillis = millis();
  // if just started, don't do anything until we get a good reading
  if(lastReadTime <= 0) {
      lastReadTime = currentMillis + DS_delay;
      ventTemperature = 0.5 * (hiAlarmLimit - lowAlarmLimit);
  }
    
  gateOpen = (digitalRead(GATE_OPEN) == 0); // switches are reverse logic
  gateClosed = (digitalRead(GATE_CLOSED) == 0);
  if (currentMillis - lastSwitchRead > switchBounce) { // Request the time from the time server every hour
    lastSwitchRead = currentMillis;     
    if(gateOpen != lastGateOpen){
      lastGateOpen = gateOpen;
      Serial.print("Open switch now: "); Serial.println(gateOpen);
    }
    if(gateClosed != lastGateClosed){
      lastGateClosed = gateClosed;
      Serial.print("Closed switch now: "); Serial.println(gateClosed);
      if(gateClosed == 0){
        lastMovedFromClosed = currentMillis;;
      }
    }
  }  

  // Light LED according to gate position
  // TODO: and air feed, and blower state, and water, ...
  // a 1 means the switch is open. The blast gate is open if:
  if( gateOpen == 0 && gateClosed == 1){
    digitalWrite(READY_TO_CUT, HIGH);
    digitalWrite(NOT_READY_TO_CUT, LOW);
  }
  else{
    digitalWrite(NOT_READY_TO_CUT, HIGH);  
    digitalWrite(READY_TO_CUT, LOW);
  }
  
  if (currentMillis - prevNTP > intervalNTP) { // Request the time from the time server every hour
    prevNTP = currentMillis;
    Serial.print("NTP request...");
    sendNTPpacket(timeServerIP);
  }

  uint32_t time = getTime();                   // Check if the time server has responded, if so, get the UNIX time
  if (time != 0) {
    timeUNIX = time;
    Serial.print("Current time:\t");
    Serial.println(timeUNIX);
    lastNTPResponse = millis();
  } else if ((millis() - lastNTPResponse) > 24UL * ONE_HOUR) {
    Serial.println("More than 24 hours since last NTP response. Rebooting.");
    Serial.flush();
    ESP.reset();
  }

  if (currentMillis - lastReadTime > dataInterval) {  // Every minute, request the temperature
    lastReadTime = currentMillis;
    tempSensors.requestTemperatures(); // Request the temperature from the sensor (it takes some time to read it)
    tmpRequested = true;
  }

  if (currentMillis - lastReadTime > DS_delay && tmpRequested) { // 750 ms after requesting the temperature
    tmpRequested = false;          
    for(int i=0; i< numberOfDevices; i++){
      temperature[i] = tempSensors.getTempCByIndex(i); // Get the temperature from the sensor
      temperature[i] = round(temperature[i] * 100.0) / 100.0; // round temperature to 2 digits after decimal   
    }
    
    if(temperature[0] < -40.0 && temperature[0] > 40.0){
          // maybe report on this?
    }
    else{
      ventTemperature = 0.9 * ventTemperature + 0.1 * temperature[0];
    }
    
    Serial.print("runtime: "); Serial.print(currentMillis/1000);
    Serial.print(" pub: "); Serial.print(lastPublishTime/1000);
    Serial.print(" read: "); Serial.print(lastReadTime/1000);
    Serial.print(" last val: "); Serial.print(lastTemperature);
    Serial.print(" new val: "); Serial.print(ventTemperature);
    Serial.print(" last alarm: "); Serial.print(lastAlarmTime/1000); 
    Serial.print(" w value: "); Serial.println(lastAlarmVal);

    if(fabs(lastTemperature - ventTemperature) > 1.0){
      Serial.println("Big change, forcing pub");
      lastPublishTime = -publishInterval; // force publication
    }
    lastTemperature = ventTemperature;
  }

  if(inAlarm && ventTemperature > lowAlarmLimit && ventTemperature < hiAlarmLimit){
      inAlarm = false;

      char alarmValStr[8];
      dtostrf(ventTemperature, 5, 1, alarmValStr);
      
      String alarmMessage = "Normal: The HMS laser cutter vent temperature is now normal at ";
      alarmMessage += String(alarmValStr) + "C.\nThe blast gate is ";  
      alarmMessage += (gateClosed == 1) ? "closed." : "open."; 
      Serial.println(alarmMessage);
      sendMessageToAllSubscribedUsers(alarmMessage);
   }
   
   if((ventTemperature <= lowAlarmLimit || ventTemperature >= hiAlarmLimit)
    && currentMillis - lastAlarmTime > alarmInterval){
      inAlarm = true;
      lastAlarmVal = ventTemperature;
      lastAlarmTime = currentMillis;
      
      char alarmValStr[8];
      dtostrf(ventTemperature, 5, 1, alarmValStr);
      String alarmMessage = "Alarm: The HMS laser cutter vent temperature is currently ";
      alarmMessage += String(alarmValStr) + "C.\nThe blast gate is ";
      alarmMessage += (gateClosed == 1) ? "closed." : "open.";

      Serial.println(alarmMessage);
      sendMessageToAllSubscribedUsers(alarmMessage);
  }

  if (currentMillis - lastBotTime > botInterval)  {
      lastBotTime = currentMillis;
      int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
      while(numNewMessages) {
          Serial.print("got response ");
          handleNewMessages(numNewMessages);
          numNewMessages = bot.getUpdates(bot.last_message_received + 1);
      }     
  }
  
  server.handleClient();                      // run the server
  webSocket.loop();
}

void startSensors() {
  DeviceAddress ds18b20Address;

  tempSensors.setWaitForConversion(false); // Don't block the program while the temperature sensor is reading
  tempSensors.begin();                     // Start the temperature sensor

  pinMode(GATE_OPEN, INPUT_PULLUP);
  pinMode(GATE_CLOSED, INPUT_PULLUP);
  pinMode(READY_TO_CUT, OUTPUT);
  pinMode(NOT_READY_TO_CUT, OUTPUT);

  numberOfDevices = tempSensors.getDeviceCount();
  if (numberOfDevices > 0) {
    Serial.printf("Found %d sensors\r\n", numberOfDevices);
  }
  else{
    Serial.printf("No DS18x20 temperature sensor found on pin %d. Rebooting.\r\n", TEMP_SENSOR_PIN);
    Serial.flush();
    delay(2000);
    //ESP.reset();
  }
  for(int i = 0; i < numberOfDevices; i++)  {
    if(tempSensors.getAddress(ds18b20Address, i))
    {
      tempSensors.setResolution(ds18b20Address, 12);
    }
  }
}

void startWiFi() { // Try to connect to some given access points. Then wait for a connection
  WiFi.mode(WIFI_STA);
  // static IP 
  //WiFi.config(ip, dns, gateway, subnet);
  WiFi.begin(WLAN_SSID, WLAN_PASS); 
  
    // Connect to WiFi access point.
  Serial.println(); 
  Serial.print("Connecting to "); Serial.print(WLAN_SSID);

  while (WiFi.status() != WL_CONNECTED) {  // Wait for the Wi-Fi to connect
    delay(250);
    Serial.print('.');
  }
  Serial.println("\r\n");
  Serial.print("Connected to ");
  Serial.println(WiFi.SSID());             // Tell us what network we're connected to
  Serial.print("IP address:\t");
  Serial.print(WiFi.localIP());            // Send the IP address of the ESP8266 to the computer
  Serial.println("\r\n");
}

void startUDP() {
  Serial.println("Starting UDP");
  UDP.begin(123);                          // Start listening for UDP messages to port 123
  Serial.print("Local port:\t");
  Serial.println(UDP.localPort());
}

/*__________________________________________________________SERVER_HANDLERS__________________________________________________________*/

void handleNewMessages(int numNewMessages) {
  Serial.println("handleNewMessages");
  Serial.println(String(numNewMessages));

  Serial.println( ESP.getFreeHeap(),DEC);

  for (int i=0; i<numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;

    String from_name = bot.messages[i].from_name;

    Serial.print("from: "); Serial.print(from_name);
    Serial.print(" chat_id: "); Serial.print(chat_id);
    Serial.print(" text: "); Serial.print(text);
    if (from_name == "") from_name = "Guest";

    if (text == "/start") {
      if (addSubscribedUser(chat_id, from_name)) {
        String welcome = "Welcome to the HMS monitor Telegram Bot, " + from_name + ".\n";
        welcome += "Try these commands.\n\n";
        welcome += "/current : current conditions being monitored\n";
        welcome += "/showallusers : show all subscribed users\n";
        welcome += "/removeallusers : remove all subscribed users\n";
        welcome += "/testbulkmessage : send test message to subscribed users\n";
        welcome += "/stop : unsubscribe from bot\n";
        welcome += "/start : resubscribe from bot\n\n";
        //welcome += "trend at: https://io.adafruit.com/morrowwm/dashboards/maker-space";
    
        bot.sendMessage(chat_id, welcome, "Markdown");
      } else {
        bot.sendMessage(chat_id, "Something wrong, please try again (later?)", "");
      }
    }
    if (text == "/current") { 
      // Summary, then detail
      String message = "VENT: " + String(int(ventTemperature));
      message += (gateClosed == 1) ? "C. CLOSED." : "C. OPEN.";
      
      message += "\n\nThe HMS laser cutter vent temperature is currently ";

      char valStr[8];
      dtostrf(ventTemperature, 5, 1, valStr);
      
      message += String(valStr) + "C.\nThe blast gate is ";
      message += (gateClosed == 1) ? "closed." : "open.";
      message += " It was last opened ";
      int openTime = (millis()-lastMovedFromClosed)/60000;
      if (openTime > 120) { 
        message += String(openTime/60) + " hours ago.\n";
      }
      else {
        message += String(openTime) + " minutes ago.\n";
      }
      message += "Local server is http://" + WiFi.localIP().toString();

      Serial.println(message);
      bot.sendMessage(chat_id, message);
    }

    if (text == "/stop") {
      if (removeSubscribedUser(chat_id)) {
        bot.sendMessage(chat_id, "Thank you, " + from_name + ", please return later", "");
      } else {
        bot.sendMessage(chat_id, "Something wrong, please try again (later?)", "");
      }
    }

    if (text == "/testbulkmessage") {
      sendMessageToAllSubscribedUsers("This is a test broadcast to all subscribed users!");
    }

    if (text == "/showallusers") {
      File subscribedUsersFile = SPIFFS.open("/"+subscribed_users_filename, "r");

      if (!subscribedUsersFile) {
        bot.sendMessage(chat_id, "No subscription file", "");
      }

      size_t size = subscribedUsersFile.size();

      if (size > 1024) {
        bot.sendMessage(chat_id, "Subscribed users file is too large", "");
      } else {
          String file_content = subscribedUsersFile.readString();
          Serial.println("Showing subscribers:");
          Serial.println(file_content);
          bot.sendMessage(chat_id, file_content, "");
      }
    }

    if (text == "/removeallusers") {
      if (SPIFFS.remove("/"+subscribed_users_filename)) {
        bot.sendMessage(chat_id, "All users removed", "");
      } else {
        bot.sendMessage(chat_id, "Something wrong, please try again (later?)", "");
      }
    }
  }
}


JsonObject& getSubscribedUsers() {
  if (!SPIFFS.exists("/"+subscribed_users_filename)) {
    Serial.println("Creating subscribed users file");

    // Create empty file (w+ not working as expected)
    File f = SPIFFS.open("/"+subscribed_users_filename, "w");
    f.close();

    JsonObject& users = jsonBuffer.createObject();

    return users;
  } else {
    File subscribedUsersFile = SPIFFS.open("/"+subscribed_users_filename, "r");

    size_t size = subscribedUsersFile.size();

    if (size > 1024) {
      Serial.println("Subscribed users file is too large");
      //return users;
    }

    String file_content = subscribedUsersFile.readString();

    JsonObject& users = jsonBuffer.parseObject(file_content);

    if (!users.success()) {
      Serial.println("Failed to parse subscribed users file");
    }
    subscribedUsersFile.close();
    return users;
  }
}

bool addSubscribedUser(String chat_id, String from_name) {
  JsonObject& users = getSubscribedUsers();

  File subscribedUsersFile = SPIFFS.open("/"+subscribed_users_filename, "w+");

  if (!subscribedUsersFile) {
    Serial.println("Failed to open subscribed users file for writing");
    //return false;
  }

  Serial.print("Subscribing: "); Serial.println(from_name);
  users.set(chat_id, from_name);
  users.printTo(subscribedUsersFile);

  subscribedUsersFile.close();

  return true;
}

bool removeSubscribedUser(String chat_id) {
  JsonObject& users = getSubscribedUsers();

  File subscribedUsersFile = SPIFFS.open("/"+subscribed_users_filename, "w");

  if (!subscribedUsersFile) {
    Serial.println("Failed to open subscribed users file for writing");
    return false;
  }

  users.remove(chat_id);
  users.printTo(subscribedUsersFile);

  subscribedUsersFile.close();

  return true;
}

void sendMessageToAllSubscribedUsers(String message) {
  int users_processed = 0;

  JsonObject& users = getSubscribedUsers();

  for (JsonObject::iterator it=users.begin(); it!=users.end(); ++it) {
    users_processed++;

    if (users_processed < messages_limit_per_second)  {
      const char* chat_id = it->key;
      if(strcmp(chat_id, "") != 0) {
          bot.sendMessage(chat_id, message, "");
      }
    } else {
      delay(bulk_messages_mtbs);
      users_processed = 0;
    }
  }
}

/*__________________________________________________________HELPER_FUNCTIONS__________________________________________________________*/

String formatBytes(size_t bytes) { // convert sizes in bytes to KB and MB
  if (bytes < 1024) {
    return String(bytes) + "B";
  } else if (bytes < (1024 * 1024)) {
    return String(bytes / 1024.0) + "KB";
  } else if (bytes < (1024 * 1024 * 1024)) {
    return String(bytes / 1024.0 / 1024.0) + "MB";
  }
  else{
    return String(bytes);
  }
}

String getContentType(String filename) { // determine the filetype of a given filename, based on the extension
  if (filename.endsWith(".html")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  else if (filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}

unsigned long getTime() { // Check if the time server has responded, if so, get the UNIX time, otherwise, return 0
  if (UDP.parsePacket() == 0) { // If there's no response (yet)
    return 0;
  }
  UDP.read(packetBuffer, NTP_PACKET_SIZE); // read the packet into the buffer
  // Combine the 4 timestamp bytes into one 32-bit number
  uint32_t NTPTime = (packetBuffer[40] << 24) | (packetBuffer[41] << 16) | (packetBuffer[42] << 8) | packetBuffer[43];
  // Convert NTP time to a UNIX timestamp:
  // Unix time starts on Jan 1 1970. That's 2208988800 seconds in NTP time:
  const uint32_t seventyYears = 2208988800UL;
  // subtract seventy years:
  uint32_t UNIXTime = NTPTime - seventyYears;
  return UNIXTime;
}


void sendNTPpacket(IPAddress& address) {
  Serial.println("Sending NTP request");
  memset(packetBuffer, 0, NTP_PACKET_SIZE);  // set all bytes in the buffer to 0
  // Initialize values needed to form NTP request
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode

  // send a packet requesting a timestamp:
  UDP.beginPacket(address, 123); // NTP requests are to port 123
  UDP.write(packetBuffer, NTP_PACKET_SIZE);
  UDP.endPacket();
}
