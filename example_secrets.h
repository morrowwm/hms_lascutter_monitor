#define WLAN_SSID       "XXX"
#define WLAN_PASS       "XXX"

IPAddress ip(192, 168, 0, 10);
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(8, 8, 8, 8);

const char *OTAName = "ESP8266mon";         // A name and a password for the OTA service
const char *OTAPassword = "XXX";
const char* mdnsName = "esp8266mon";    // Domain name for the mDNS responder

#define AIO_SERVER      "io.adafruit.com"
#define AIO_SERVERPORT  8883                   // use 8883 for SSL
#define AIO_USERNAME    "XXX"
#define AIO_KEY         "XXX"

// Initialize Telegram BOT
#define BOTtoken "XXX:YYY"  // your Bot Token (Get from Botfather)
