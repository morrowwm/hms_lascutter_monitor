/***************************************************
  Adafruit MQTT Library ESP8266 Adafruit IO SSL/TLS example

  Must use the latest version of ESP8266 Arduino from:
    https://github.com/esp8266/Arduino

  Works great with Adafruit's Huzzah ESP board & Feather
  ----> https://www.adafruit.com/product/2471
  ----> https://www.adafruit.com/products/2821

  Adafruit invests time and resources providing this open source code,
  please support Adafruit and open-source hardware by purchasing
  products from Adafruit!

  Written by Tony DiCola for Adafruit Industries.
  SSL/TLS additions by Todd Treece for Adafruit Industries.
  MIT license, all text above must be included in any redistribution
 ****************************************************/
#include <ESP8266WiFi.h>
#include <DallasTemperature.h>
#include <ArduinoOTA.h>
#include <FS.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_Client.h"
#include <WebSocketsClient.h>
#include <WebSocketsServer.h>
#include "secrets.h"

#define ONE_HOUR 3600000UL

/************************* Temperature Sensors *********************************/

#define TEMP_SENSOR_PIN D4
#define ONE_WIRE_MAX_DEV 15 //The maximum number of devices
OneWire oneWire(TEMP_SENSOR_PIN);        // Set up a OneWire instance to communicate with OneWire devices

DallasTemperature tempSensors(&oneWire); // Create an instance of the temperature sensor class
int numberOfDevices = 0;

const char* dataFile = "/smellyroom.csv";
/************************* WiFi Access Point *********************************/
// in secrets.h

/************************* Web Server, NTP *********************************/
ESP8266WebServer server(80);       // create a web server on port 80
File fsUploadFile;                                    // a File variable to temporarily store the received file

const int NTP_PACKET_SIZE = 48;          // NTP time stamp is in the first 48 bytes of the message
byte packetBuffer[NTP_PACKET_SIZE];      // A buffer to hold incoming and outgoing packets
IPAddress timeServerIP;        // The time.nist.gov NTP server's IP address
const char* ntpServerName = "time.nist.gov";

WebSocketsClient webSocket;

/************************* Over The Air updates *********************************/

WiFiUDP UDP;                   // Create an instance of the WiFiUDP class to send and receive UDP messages

/************************* Adafruit.io Setup *********************************/

#define AIO_SERVER      "io.adafruit.com"
#define AIO_SERVERPORT  8883                   // 8883 for MQTTS


/************ Global State (you don't need to change this!) ******************/

// WiFiFlientSecure for SSL/TLS support
WiFiClientSecure client;

// Setup the MQTT client class by passing in the WiFi client and MQTT server and login details.
Adafruit_MQTT_Client mqtt(&client, AIO_SERVER, AIO_SERVERPORT, AIO_USERNAME, AIO_KEY);

// io.adafruit.com SHA1 fingerprint
const char* fingerprint = "AD 4B 64 B3 67 40 B5 FC 0E 51 9B BD 25 E9 7F 88 B6 2A A3 5B";

/****************************** Feeds ***************************************/

// Setup a feed to publish to.
// Notice MQTT paths for AIO follow the form: <username>/feeds/<feedname>
Adafruit_MQTT_Publish temperatureIO = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/smelly_room_temperature");

/*************************** Sketch Code ************************************/

// Bug workaround for Arduino 1.6.6, it seems to need a function declaration
// for some reason (only affects ESP8266, likely an arduino-builder bug).
void MQTT_connect();
void verifyFingerprint();

void setup() {
  Serial.begin(115200);
  delay(10);

  tempSensors.setWaitForConversion(false); // Don't block the program while the temperature sensor is reading
  tempSensors.begin();                     // Start the temperature sensor

  numberOfDevices = tempSensors.getDeviceCount();
  if (numberOfDevices == 0) {
    Serial.printf("No DS18x20 temperature sensor found on pin %d. Rebooting.\r\n", TEMP_SENSOR_PIN);
    Serial.flush();
    ESP.reset();
  }

  Serial.println(F("Adafruit IO MQTTS (SSL/TLS) of HMS temperatures"));

  // Connect to WiFi access point.
  Serial.println(); Serial.println();
  Serial.print("Connecting to ");
  Serial.println(WLAN_SSID);

  startWiFi();
  startOTA();                  // Start the OTA service
  startSPIFFS();               // Start the SPIFFS and list all contents
  startServer();               // Start a HTTP server with a file read handler and an upload handler
  startUDP();                  // Start listening for UDP messages to port 123

  WiFi.hostByName(ntpServerName, timeServerIP); // Get the IP address of the NTP server
  Serial.print("Time server IP:\t");
  Serial.println(timeServerIP);

  sendNTPpacket(timeServerIP);
  delay(500);
  
  // check the fingerprint of io.adafruit.com's SSL cert
  verifyFingerprint();
}

float temperature[ONE_WIRE_MAX_DEV];
float lastTemperature = -100;

const unsigned long intervalNTP = ONE_HOUR; // Update the time every hour
unsigned long prevNTP = 0;
unsigned long lastNTPResponse = millis();
uint32_t timeUNIX = 0;                      // The most recent timestamp received from the time server

const unsigned long dataInterval = 2000;   // Do a temperature measurement 2 seconds
const unsigned long publishInterval = 60000;   // Publish every 5 minutes
unsigned long lastReadTime = -600000;
unsigned long lastPublishTime = -600000;

const unsigned long DS_delay = 750;         // Reading the temperature from the DS18x20 can take up to 750ms
bool tmpRequested = false;

void loop() {
  unsigned long currentMillis = millis();
  
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
  
  // Ensure the connection to the MQTT server is alive (this will make the first
  // connection and automatically reconnect when disconnected).  See the MQTT_connect
  // function definition further below.
  MQTT_connect();

  if (currentMillis - lastReadTime > dataInterval) {  // Every minute, request the temperature
    lastReadTime = currentMillis;
    tempSensors.requestTemperatures(); // Request the temperature from the sensor (it takes some time to read it)
    tmpRequested = true;
    Serial.print("Current time: "); Serial.print(currentMillis/1000);
    Serial.print(" Last publish: "); Serial.print(lastPublishTime/1000);
    Serial.print(" Last data: "); Serial.print(lastReadTime/1000);
    Serial.print(" Last temperature: "); Serial.print(lastTemperature);
    Serial.print(" Temperature is ...");
  }
  if (currentMillis - lastReadTime > DS_delay && tmpRequested) { // 750 ms after requesting the temperature
    tmpRequested = false;          
    for(int i=0; i< numberOfDevices; i++){
      temperature[i] = tempSensors.getTempCByIndex(i); // Get the temperature from the sensor
      temperature[i] = round(temperature[i] * 100.0) / 100.0; // round temperature to 2 digits after decimal
    }
    Serial.println(temperature[0]);
  }

  if(fabs(temperature[0] - lastTemperature) > 1.0){
    Serial.println("Big change, forcing pub");
    lastPublishTime = -publishInterval; // force publication
    lastTemperature = temperature[0];
  }
  
  if (currentMillis - lastPublishTime > publishInterval) { 
    lastPublishTime = currentMillis;
    Serial.print(F("\nSending temperature "));
    Serial.print(temperature[0]);
    Serial.print(F(" to feed..."));
    if (! temperatureIO.publish(temperature[0])) {
      Serial.println(F("Failed"));
    } else {
      Serial.println(F("OK!"));
    }

    if (timeUNIX != 0) {
      uint32_t actualTime = timeUNIX + (currentMillis - lastNTPResponse) / 1000;
      
      File tempLog = SPIFFS.open(dataFile, "a"); // Write the time and the temperatures to the csv file
      tempLog.print(actualTime);
            
      for(int i=0; i< numberOfDevices; i++){
        temperature[i] = tempSensors.getTempCByIndex(i); // Get the temperature from the sensor
        temperature[i] = round(temperature[i] * 100.0) / 100.0; // round temperature to 2 digits after decimal
        
        Serial.printf("Appending temperature %d to file: %lu,", i, actualTime);
        Serial.println(temperature[i]);
        
        tempLog.print(','); tempLog.print(temperature[i]);
      }
      tempLog.println();
      tempLog.close();
    }
    else {                                    // If we didn't receive an NTP response yet, send another request
      sendNTPpacket(timeServerIP);
      delay(500);
    }
  }
  server.handleClient();                      // run the server
  ArduinoOTA.handle();                        // listen for OTA events
  webSocket.loop();
  
  // give Espressif TCP/IP stack some time
  delay(0);

}


void verifyFingerprint() {

  const char* host = AIO_SERVER;

  Serial.print("Connecting to ");
  Serial.println(host);

  if (! client.connect(host, AIO_SERVERPORT)) {
    Serial.println("Connection failed. Halting execution.");
    while(1);
  }

  if (client.verify(fingerprint, host)) {
    Serial.println("Connection secure.");
  } else {
    Serial.println("Connection insecure! Halting execution.");
    while(1);
  }

}

// Function to connect and reconnect as necessary to the MQTT server.
// Should be called in the loop function and it will take care if connecting.
void MQTT_connect() {
  int8_t ret;

  // Stop if already connected.
  if (mqtt.connected()) {
    return;
  }

  Serial.print("Connecting to MQTT... ");

  uint8_t retries = 3;
  while ((ret = mqtt.connect()) != 0) { // connect will return 0 for connected
       Serial.println(mqtt.connectErrorString(ret));
       Serial.println("Retrying MQTT connection in 5 seconds...");
       mqtt.disconnect();
       delay(5000);  // wait 5 seconds
       retries--;
       if (retries == 0) {
         // basically die and wait for WDT to reset me
         while (1);
       }
  }

  Serial.println("MQTT Connected!");
}

void startWiFi() { // Try to connect to some given access points. Then wait for a connection
  WiFi.mode(WIFI_STA);
  WiFi.begin(WLAN_SSID, WLAN_PASS);
  Serial.println("Connecting");
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

void startSPIFFS() { // Start the SPIFFS and list all contents
  SPIFFS.begin();                             // Start the SPI Flash File System (SPIFFS)
  //SPIFFS.remove("/upstairs.csv");
  //SPIFFS.remove("/upstairs.csv.gz");
  Serial.println("SPIFFS started. Contents:");
  {
    Dir dir = SPIFFS.openDir("/");
    while (dir.next()) {                      // List the file system contents
      String fileName = dir.fileName();
      size_t fileSize = dir.fileSize();
      Serial.printf("\tFS File: %s, size: %s\r\n", fileName.c_str(), formatBytes(fileSize).c_str());
    }
    Serial.printf("\n");
  }
}

void startUDP() {
  Serial.println("Starting UDP");
  UDP.begin(123);                          // Start listening for UDP messages to port 123
  Serial.print("Local port:\t");
  Serial.println(UDP.localPort());
}

void startOTA() { // Start the OTA service
  ArduinoOTA.setHostname(OTAName);
  ArduinoOTA.setPassword(OTAPassword);

  ArduinoOTA.onStart([]() {
    Serial.println("Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\r\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("OTA ready\r\n");
}


void startServer() { // Start a HTTP server with a file read handler and an upload handler
  server.on("/edit.html",  HTTP_POST, []() {  // If a POST request is sent to the /edit.html address,
    server.send(200, "text/plain", "");
  }, handleFileUpload);                       // go to 'handleFileUpload'

  server.onNotFound(handleNotFound);          // if someone requests any other file or page, go to function 'handleNotFound'
  // and check if the file exists

  server.begin();                             // start the HTTP server
  Serial.println("HTTP server started.");
}

/*__________________________________________________________SERVER_HANDLERS__________________________________________________________*/

void handleNotFound() { // if the requested file or page doesn't exist, return a 404 not found error
  if (!handleFileRead(server.uri())) {        // check if the file exists in the flash memory (SPIFFS), if so, send it
    server.send(404, "text/plain", "404: File Not Found");
  }
}

bool handleFileRead(String path) { // send the right file to the client (if it exists)
  Serial.println("handleFileRead: " + path);
  if (path.endsWith("/")) path += "index.html";          // If a folder is requested, send the index file
  String contentType = getContentType(path);             // Get the MIME type
  String pathWithGz = path + ".gz";
  if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)) { // If the file exists, either as a compressed archive, or normal
    if (SPIFFS.exists(pathWithGz))                         // If there's a compressed version available
      path += ".gz";                                         // Use the compressed verion
    File file = SPIFFS.open(path, "r");                    // Open the file
    size_t sent = server.streamFile(file, contentType);    // Send it to the client
    file.close();                                          // Close the file again
    Serial.println(String("\tSent file: ") + path);
    return true;
  }
  Serial.println(String("\tFile Not Found: ") + path);   // If the file doesn't exist, return false
  return false;
}

void handleFileUpload() { // upload a new file to the SPIFFS
  HTTPUpload& upload = server.upload();
  String path;
  if (upload.status == UPLOAD_FILE_START) {
    path = upload.filename;
    if (!path.startsWith("/")) path = "/" + path;
    if (!path.endsWith(".gz")) {                         // The file server always prefers a compressed version of a file
      String pathWithGz = path + ".gz";                  // So if an uploaded file is not compressed, the existing compressed
      if (SPIFFS.exists(pathWithGz))                     // version of that file must be deleted (if it exists)
        SPIFFS.remove(pathWithGz);
    }
    Serial.print("handleFileUpload Name: "); Serial.println(path);
    fsUploadFile = SPIFFS.open(path, "w");               // Open the file for writing in SPIFFS (create if it doesn't exist)
    path = String();
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (fsUploadFile)
      fsUploadFile.write(upload.buf, upload.currentSize); // Write the received bytes to the file
  } else if (upload.status == UPLOAD_FILE_END) {
    if (fsUploadFile) {                                   // If the file was successfully created
      fsUploadFile.close();                               // Close the file again
      Serial.print("handleFileUpload Size: "); Serial.println(upload.totalSize);
      server.sendHeader("Location", "/success.html");     // Redirect the client to the success page
      server.send(303);
    } else {
      server.send(500, "text/plain", "500: couldn't create file");
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


