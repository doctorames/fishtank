#include <WiFi.h>
#include <rom/rtc.h>
#include <WiFiClientSecure.h>
#include <MQTTClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <WebServer.h>
#include <Update.h>
//#include <OneWire.h>  <-- OneWire.h is already included in DallasTemperature.h
#include <DallasTemperature.h>
#include <LiquidCrystal_I2C.h>
#include "time.h"

#include "certs.h"

#define VERSION  "1.4"
#define DEBUG 0
#define PRODUCTION_UNIT 1

#if PRODUCTION_UNIT
char ssid[] = "ATT3HQ3Bhl";
char pass[] = "9z=ba=684snb";
#else
char ssid[] = "CenturyLink9630";
char pass[] = "fdk3d84aap6cpe";
#endif

#define NUMBER_OF_AMBIENT_SENSORS 1
#define NUMBER_OF_BOILER_SENSORS 1
#define NUMBER_OF_WATER_SENSORS 3
#define NUMBER_OF_SENSORS  (NUMBER_OF_AMBIENT_SENSORS + NUMBER_OF_BOILER_SENSORS + NUMBER_OF_WATER_SENSORS)
#define MILLISECONDS_IN_ONE_DAY  86400000u

// GPIO pins
#define ONE_WIRE_BUS 16
#define PUMP_RELAY 19
#define PUSH_BUTTON 17
#define ALERT_LED LED_BUILTIN

// EEPROM bytes
#define EEPROM_RECOVERY_BYTE 0
#define EEPROM_RECOVERY_NUMBER_OF_BYTES_TO_ACCESS 2

#define EEPROM_RECOVERY_NO_SENSORS 1         // Rebooted because I could not detect any sensors at startup
#define EEPROM_RECOVERY_WHAT_TO_BELIEVE 2    // Rebooted because all sensors disagree and I don't know what to believe anymore

#define EEPROM_MUTE_NOTIFICATIONS_BYTE  1    // 0 = notifications NOT muted, 1 = notifications muted

// Temperature set points
#define MAX_ACCEPTABLE_TEMPERATURE_DELTA 4.0  // If a sensor's average drift from the others exceeds this amount, it will be blacklisted
#define TEMP_LOWER_TRIGGER 70.00              // Turn on the pump when temperature is less than this amount, if the boiler is hot enough
#define TEMP_UPPER_TRIGGER 75.00              // Turn off the pump when temperature is more than this amount, unconditionally
#define TEMP_ABSOLUTE_LOWER  50.00            // Send an alert if the temperature ever drops below this amount
#define TEMP_ABSOLUTE_UPPER  85.00            // Send an alert if the temperature ever rises above this amount
#define TEMP_SENSOR_DISCONNECTED -196.60      // The sensor library will return this specific reading for a sensor that becomes unresponsive
#define TEMP_BOILER_UPPER_TRIGGER  90.00      // If the boiler is at or above this temp, turn on the pump if needed
#define TEMP_BOILER_LOWER_TRIGGER  89.00      // If the boiler is at or below this temp, turn off the pump unconditionally

// Time-outs and retry thresholds
#define MAX_WIFI_WAIT       6                 // Wait this many seconds for a wifi connection attempt to complete. I've found it rarely takes more than 3 seconds.
                                              // You don't want to be too generous with this wait time. While it's waiting, the system isn't doing anything else.
                                              // In other words, if you set it to something silly like 10 minutes, and it can't reconnect (maybe the router died),
                                              // then it will spend 10 minutes ignoring your fishtank while it spins in a wait loop. You wanna keep this value as
                                              // low as you can. If it can't connect in the time allotted, don't worry, it will try again later.
#define LCD_TIMEOUT  5000                     // LCD screen will turn off after this amount of time
#define AWS_MAX_RECONNECT_TRIES 50            // How many times we should attempt to connect to AWS
#define HTTP_INCOMING_REQ_TIMEOUT 4000uL      // Catching incoming http requests is appallingly hit or miss. It can sit forever waiting for a missed GET. Never wait longer than this amount.
#define CORRECTION_TIMEOUT 7200000            // The max amount of time it should take for temperature to return to an acceptable range.
                                              //   acceptable range = anywhere between TEMP_LOWER_TRIGGER and TEMP_UPPER_TRIGGER

// AWS defines
#define DEVICE_NAME "fishtemp-controller"     // The name of the device. This MUST match up with the name defined in the AWS console
#define AWS_IOT_ENDPOINT "a3u6y1u8z9tj3s-ats.iot.us-east-1.amazonaws.com"  // The MQTTT endpoint for the device (unique for each AWS account but shared amongst devices within the account)
#define AWS_IOT_TOPIC "$aws/things/" DEVICE_NAME "/shadow/update"  // The MQTT topic that this device should publish to


// Globals
WiFiClientSecure net = WiFiClientSecure();
MQTTClient client = MQTTClient(512);

bool timeInitialized = false;
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = -21600;
const int   daylightOffset_sec = 3600;
struct tm timeinfo_boot;
struct tm timeinfo_pumpState;

bool otaHasBeenSetup = false;
bool lcdIsOn = false;
IPAddress localIP;
unsigned long lcdTimeoutCounter = 0;
float trustedTemp;
float boilerTemp;
char httpStr[256];
#define SYS_STATUS_PAGE_STR_LEN 2048
char systemStatusPageStr[SYS_STATUS_PAGE_STR_LEN];
char tempFloat[12];
short pushButtonSemaphore = 0;

typedef enum {
  showWifi,
  showWaterTemp,
  showAmbientTemp,
  showPumpStatus,
  showNotificationStatus
} WhatToDisplay;

WhatToDisplay displaySelector = showWifi;

unsigned long httpCurrentMillis = 0;
unsigned long httpLastMillis = 0;
unsigned long httpElapsedMillis = 0;

unsigned long millisSinceOneSensorTimerNagSent = 0;
unsigned long currentOneSensorTimerTick = 0;
unsigned long lastOneSensorTimerTick = 0;
bool oneSensorNagSent = false;

float deltaHi;
float deltaLo;
float delta;
float temp;
float tempA;
float tempB;
float outlyingTemp;

int goodSensors = 0;
int trustedSensor;
int outlyingSensor;

float highestAverageDelta;
float runningAverageDelta;
char charErrorMessage[512];

bool pendingAlert = false;
bool pumpIsOn = false;
bool temperatureIsGood = true;
bool tempOutOfRangeForTooLong = false;

unsigned long systemBootTime = 0;
int correctionTimer = 0;
int correctionTimerStart = 0;
unsigned long currentPumpStateStart = 0;
bool badTempAlertPending = false;
bool badTempAlertSent = false;
bool extremeLowTempAlertPending = false;
bool extremeLowTempAlertSent = false;
bool extremeHighTempAlertPending = false;
bool extremeHighTempAlertSent = false;

char* upgradeHtml = 
"<script src='https://ajax.googleapis.com/ajax/libs/jquery/3.2.1/jquery.min.js'></script>"
"<form method='POST' action='#' enctype='multipart/form-data' id='upload_form'>"
#if !PRODUCTION_UNIT
  "<span style=\"color:Red;font-size:60px\">---THIS IS THE DEBUG UNIT---</span></br>"
#endif
"  <input type='file' name='update'>"
"  <input type='submit' value='Update'>"
"</form>"
"<div id='prg'>progress: 0%</div>"
"<script>"
"  $('form').submit(function(e){"
"    e.preventDefault();"
"    var form = $('#upload_form')[0];"
"    var data = new FormData(form);"
"     $.ajax({"
"      url: '/update',"
"      type: 'POST',"
"      data: data,"
"      contentType: false,"
"      processData:false,"
"      xhr: function() {"
"        var xhr = new window.XMLHttpRequest();"
"        xhr.upload.addEventListener('progress', function(evt) {"
"          if (evt.lengthComputable) {"
"            var per = evt.loaded / evt.total;"
"            $('#prg').html('progress: ' + Math.round(per*100) + '%');"
"          }"
"        }, false);"
"        return xhr;"
"      },"
"      success:function(d, s) {"
"        console.log('success!')"
"      },"
"      error: function (a, b, c) {"
"      }"
"    });"
"  });"
"</script>";

WebServer server(80);
LiquidCrystal_I2C lcd(0x27, 16, 4);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

typedef enum {
  tank,
  ambient,
  boiler
} SensorLocation;

typedef struct _SensorInfo {
  int            busIndex;  // The index of the sensor as seen by DallasTemperature library
  byte           stickerId; // The number we assign to the sensor when we physically put a sticker on it
  bool           blacklisted;
  SensorLocation location;
  DeviceAddress  address;   // The factory-assigned ID of the sensor (we have no say in this)
} SensorInfo;

#if PRODUCTION_UNIT
SensorInfo sensorMap[NUMBER_OF_SENSORS] = {
  { 0xff, (byte)1, false, tank,    {0x28, 0xF8, 0x13, 0x07, 0xB6, 0x01, 0x3C, 0xCD}},
  { 0xff, (byte)2, false, tank,    {0x28, 0x4E, 0xE3, 0x07, 0xB6, 0x01, 0x3C, 0xDE}},
  { 0xff, (byte)3, false, tank,    {0x28, 0x69, 0xA8, 0x07, 0xB6, 0x01, 0x3C, 0x74}},
  { 0xff, (byte)4, false, ambient, {0x28, 0xC2, 0xDC, 0x07, 0xB6, 0x01, 0x3C, 0xA2}},
  { 0xff, (byte)5, false, boiler,  {0x28, 0x2E, 0x3E, 0x07, 0xD6, 0x01, 0x3C, 0x0A}}
};
#else
SensorInfo sensorMap[NUMBER_OF_SENSORS] = {
  { 0xff, (byte)1, false, tank,    {0x28, 0x6B, 0xB7, 0x07, 0xD6, 0x01, 0x3C, 0xAC}}, // purple
  { 0xff, (byte)2, false, tank,    {0x28, 0x90, 0x2D, 0x07, 0xD6, 0x01, 0x3C, 0x2C}}, // white
  { 0xff, (byte)3, false, tank,    {0x28, 0x16, 0x2C, 0x07, 0xD6, 0x01, 0x3C, 0xB9}}, // green
  { 0xff, (byte)4, false, ambient, {0x28, 0x4F, 0x41, 0x07, 0xB6, 0x01, 0x3C, 0x59}}, // orange
  { 0xff, (byte)5, false, boiler,  {0x28, 0xD2, 0xD0, 0x07, 0xD6, 0x01, 0x3C, 0x7D}}  // brown
};
#endif































bool compareAddresses(DeviceAddress a, DeviceAddress b) {
  bool result = true;
  for (int i = 0; i < 8; i++) {
    if(a[i] != b[i]){
      result = false;
      break;
    }
  }
  return result;
}

bool connectToAWS()
{
  if (WiFi.status() != WL_CONNECTED) {
     // We're down for some reason. Try to connect.
     connectToWifi();
     if (WiFi.status() != WL_CONNECTED) {
        // Reconnect failed. Not much we can do.
        return false;
     }
  }

  // Configure WiFiClientSecure to use the AWS certificates we generated
  net.setCACert(AWS_CERT_CA);
  net.setCertificate(AWS_CERT_CRT);
  net.setPrivateKey(AWS_CERT_PRIVATE);

  // Connect to the MQTT broker on the AWS endpoint we defined earlier
  client.begin(AWS_IOT_ENDPOINT, 8883, net);

  // Try to connect to AWS and count how many times we retried.
  int retries = 0;
  Serial.print("Connecting to AWS IOT");

  while (!client.connect(DEVICE_NAME) && retries < AWS_MAX_RECONNECT_TRIES) {
    Serial.print(".");
    delay(100);
    retries++;
  }

  // Make sure that we did indeed successfully connect to the MQTT broker
  // If not we just end the function and wait for the next loop.
  if(!client.connected()){
    Serial.println(" Timeout!");
    return false;
  }

  // If we land here, we have successfully connected to AWS!
  // And we can subscribe to topics and send messages.
  Serial.println(" ..Connected!");
  return true;
}

bool sendMessageToAWS(const char* message)
{
#if !PRODUCTION_UNIT
  Serial.println("sendMessageToAws() DISABLED!!");
  return true;
#endif
  char notificationsMuted = EEPROM.read(EEPROM_MUTE_NOTIFICATIONS_BYTE);
  if (notificationsMuted == 1) {
     Serial.println("sendMessageToAws() DISABLED. Notifications muted.");
     return true;
  }
  if (!connectToAWS()) {
      // this is no bueno
      return false;
  }

  StaticJsonDocument<512> jsonDoc;
  JsonObject stateObj = jsonDoc.createNestedObject("state");
  JsonObject reportedObj = stateObj.createNestedObject("reported");
  
  // "Message" is what will actually end up in the SMS message that gets sent.
  // The AWS SNS rule uses a SQL query to specifically pick this message out of the JSON.
  reportedObj["message"] = message;

  Serial.println("Publishing message to AWS...");
  char jsonBuffer[512];
  serializeJson(jsonDoc, jsonBuffer);
  bool ret = client.publish(AWS_IOT_TOPIC, jsonBuffer);
  client.disconnect();

  return ret;
}

// Returns a timestamp string in the format:
// hh:mm:ss AM|PM, mm/dd/yyyy
void getDateTimeMyWay(tm* time, char* ptr, int length) {
  memset(ptr, 0, length);
  snprintf(ptr, length, "%d:%02d:%02d %s, %d/%d/%d",
  time->tm_hour > 12 ? time->tm_hour - 12 : time->tm_hour,
  time->tm_min,
  time->tm_sec,
  time->tm_hour > 11 ? "PM" : "AM",
  time->tm_mon+1,
  time->tm_mday,
  time->tm_year+1900);
}

// Converts milliseconds into natural language
void millisToDaysHoursMinutes(unsigned long milliseconds, char* str, int length) {
  
  uint seconds = milliseconds / 1000;
  memset(str, 0, length);

  if (seconds <= 60) {
    // It's only been a few seconds
    // Longest string example, 11 chars: 59 seconds\0
    snprintf(str, 11, "%d second%s", seconds, seconds == 1 ? "" : "s");
    return;
  }
  uint minutes = seconds / 60;
  if (minutes <= 60) {
    // It's only been a few minutes
    // Longest string example, 11 chars: 59 minutes\0
    snprintf(str, 11, "%d minute%s", minutes, minutes == 1 ? "" : "s");
    return;
  }
  uint hours = minutes / 60;
  minutes -= hours * 60;
  if (hours <= 24) {
    // It's only been a few hours
    if (minutes == 0)
      // Longest string example, 9 chars: 23 hours\0
      snprintf(str, 9, "%d hour%s", hours, hours == 1 ? "" : "s");
    else
      // Longest string example, 24 chars: 23 hours and 59 minutes\0
      snprintf(str, 24, "%d hour%s and %d minute%s", hours, hours == 1 ? "" : "s", minutes, minutes == 1 ? "" : "s");
    return;
  }

  // It's been more than a day
  uint days = hours / 24;
  hours -= days * 24;
  if (minutes == 0)
    // Longest string example, 23 chars: 9999 days and 23 hours\0
    snprintf(str, 23, "%d day%s and %d hour%s", days, days == 1 ? "" : "s", hours, hours == 1 ? "" : "s");
  else
    // Longest string example, 35 chars: 9999 days, 23 hours and 59 minutes\0
    snprintf(str, 35, "%d day%s, %d hour%s and %d minute%s", days, days == 1 ? "" : "s", hours, hours == 1 ? "" : "s", minutes, minutes == 1 ? "" : "s");
}

char* getSystemStatus() {

  // Pardon the html mess. Gotta tell the browser to not make the text super tiny.
  String html = "<!DOCTYPE html><html><head><title>Temperatures</title></head><body><p style=\"font-size:36px\">";
#if !PRODUCTION_UNIT
  html += "<span style=\"color:Red;font-size:60px\">---THIS IS THE DEBUG UNIT---</span></br>";
#endif
  html += "<script>function toggle() {var xhttp = new XMLHttpRequest();xhttp.open('POST', 'toggle_mute', true);xhttp.onload = function(){console.log(this.responseText); document.getElementById('toggle_button').value = this.responseText; document.getElementById('notifications_span').innerHTML = this.responseText == 'Turn Off' ? 'ON' : 'OFF'; document.getElementById('notifications_span').style = this.responseText == 'Turn Off' ? 'color:Green;' : 'color:Red;'; }; xhttp.send('poop');}</script>";

  char dateTime[24];
  struct tm ti;
  getLocalTime(&ti);
  // Longest string example, 41 chars: Readings as of:  10:38:16 AM, 11/28/2021\0
  char currentTime[41];
  char notificationsMuted = EEPROM.read(EEPROM_MUTE_NOTIFICATIONS_BYTE);
  memset(currentTime, 0, 41);
  getDateTimeMyWay(&ti, dateTime, 24);
  snprintf(currentTime, 41, "Readings as of:  %s", dateTime);
  html += currentTime;
  html+= "</br></br>";

  // Show the water sensors
  for(int i = 0; i < NUMBER_OF_SENSORS; i++) {
    // snprintf() does not support %f on arduino, so we have to convert the temperature
    // to a string first, and pass the string into snprintf().
    if (sensorMap[i].location == tank) {
      dtostrf(sensors.getTempF(sensorMap[i].address), 4, 2, tempFloat);
      // Longest string example, 42 chars: Sensor 999:  -196.60   (blacklisted)</br>\0
      snprintf(httpStr, 42, "Sensor %d:  %s%s</br>", sensorMap[i].stickerId, tempFloat, sensorMap[i].blacklisted ? "   (blacklisted)" : "");
      html += httpStr;
    }
  }
  html += "</br>";
  
  // Show the ambient and boiler sensors
  for(int i = 0; i < NUMBER_OF_SENSORS; i++) {
    // snprintf() does not support %f on arduino, so we have to convert the temperature
    // to a string first, and pass the string into snprintf().
    if (sensorMap[i].location == ambient || sensorMap[i].location == boiler) {
      dtostrf(sensors.getTempF(sensorMap[i].address), 4, 2, tempFloat);
      // Longest string example, 23 chars: Ambient:  -196.60</br>
      snprintf(httpStr, 23, "%s:  %s</br>", sensorMap[i].location == ambient ? "Ambient" : "Boiler", tempFloat);
      html += httpStr;
    }
  }
  html += "</br>";

  // Show pump state
  getDateTimeMyWay(&timeinfo_pumpState, dateTime, 24);
  millisToDaysHoursMinutes(millis() - currentPumpStateStart, currentTime, 40);
  // Longest string example, 129 chars: Pump has been <span style="color:Green;">ON</span> for 9999 days, 23 hours and 59 minutes   (since 10:08:09 AM, 11/28/2021)</br>\0
  snprintf(httpStr, 129, "Pump has been %s for %s   (since %s)</br>",
  pumpIsOn ? "<span style=\"color:Green;\">ON</span>" : "<span style=\"color:Red;\">OFF</span>",
  currentTime,
  dateTime);
  html += httpStr;
  html += "</br>";

  // Show system uptime and notification status
  getDateTimeMyWay(&timeinfo_boot, dateTime, 24);
  html += "<span style=\"font-size:30px\">";

  // Longest string example, 82 chars: Notifications are <span id='notifications_span' style="color:Green;">ON</span>    
  snprintf(httpStr, 82, "Notifications are %s    ", notificationsMuted == 1 ? "<span id='notifications_span' style=\"color:Red;\">OFF</span>" : "<span id='notifications_span' style=\"color:Green;\">ON</span>");
  html += httpStr;
  html += "<input type='button' id='toggle_button' value='";
  html += notificationsMuted == 1 ? "Turn On" : "Turn Off";
  html += "' onclick='toggle()'>";
  html += "</br>";

  // Longest string example, 41 chars: Last boot:  10:08:09 AM, 11/28/2021</br>\0
  snprintf(httpStr, 41, "Last boot:  %s</br>", dateTime);
  html += httpStr;
  millisToDaysHoursMinutes(millis() - systemBootTime, currentTime, 40);
  // Longest string example, 56 chars: System uptime:  9999 days, 23 hours and 59 minutes</br>\0
  snprintf(httpStr, 56, "System uptime:  %s</br>", currentTime);
  html += httpStr;
  html += "</span><span style=\"font-size:20px\">";
  html += "Version: ";
  html += VERSION;
  html += "</span>";
  
  // Close it off
  html += "</p></body></html>";

  memset(systemStatusPageStr, 0, SYS_STATUS_PAGE_STR_LEN);
  html.toCharArray(systemStatusPageStr, html.length() + 1);
  return systemStatusPageStr;
}

void setupOta() {
  
  if (otaHasBeenSetup) return;

  server.on("/", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", getSystemStatus());
  });
  server.on("/toggle_mute", HTTP_POST, []() {
    char notificationsMuted = EEPROM.read(EEPROM_MUTE_NOTIFICATIONS_BYTE);
    notificationsMuted = notificationsMuted == 1 ? 0 : 1;
    EEPROM.write(EEPROM_MUTE_NOTIFICATIONS_BYTE, (byte)notificationsMuted);
    EEPROM.commit();
    server.send(200, "text/plain", notificationsMuted == 1 ? "Turn On" : "Turn Off");
  });
  server.on("/upgrade", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", upgradeHtml);
  });
  /*handling uploading firmware file */
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { //start with max available size
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      /* flashing firmware to ESP*/
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { //true to set the size to the current progress
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });
  server.begin();
  Serial.println("Web server initialized");
  otaHasBeenSetup = true;
}

void connectToWifi()
{
  int dotLocation = 13;
  int dots = 0;
  int wifiRetries = 0;
  if (!lcdIsOn) {
    lcd.display();
    lcd.backlight();
    lcdIsOn = true;
  }
  lcd.clear();
  Serial.print("WiFi is down. Connecting");
  lcd.setCursor(0,0);
  lcd.print("Connecting to");
  lcd.setCursor(0,1);
  lcd.print(ssid);
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED && wifiRetries < MAX_WIFI_WAIT) {
    delay(1000);
    wifiRetries++;
    Serial.print(".");
    if (dots == 0) {
      lcd.setCursor(dotLocation,0);
      lcd.print(".  ");
      dots++;
    } else if (dots == 1) {
      lcd.setCursor(dotLocation,0);
      lcd.print(".. ");
      dots++;
    } else if (dots == 2) {
      lcd.setCursor(dotLocation,0);
      lcd.print("...");
      dots = 0;
    }
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected");
    if (!timeInitialized) {
      Serial.println("Initializing local time");
      // Looks like this is our first time successfully connecting.
      // Setup the time stuff
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      getLocalTime(&timeinfo_boot);
      // Initialize pump state time to something.
      // If the temperature is in range, then the code path to set the pump state time will not be followed.
      memcpy(&timeinfo_pumpState, &timeinfo_boot, sizeof(timeinfo_boot));
      timeInitialized = true;
     }
     // setupOta will only do its thing once per boot,
     // so it's ok to call it every time we connect. It won't duplicate its work.
     setupOta();
  } else {
     Serial.println("WiFi failed to connect");
  }
}

String getResetReasonString(RESET_REASON reason)
{
  switch (reason)
  {
    // Note the trailing spaces. For some reason, AWS trims the last character.
    case 1  : return String("POWERON_RESET - power on reset ");break;
    case 3  : return String("SW_RESET - Software reset digital core ");break;
    case 4  : return String("OWDT_RESET - Legacy watch dog reset digital core ");break;
    case 5  : return String("DEEPSLEEP_RESET - Deep Sleep reset digital core ");break;
    case 6  : return String("SDIO_RESET - Reset by SLC module, reset digital core ");break;
    case 7  : return String("TG0WDT_SYS_RESET - Timer Group0 Watch dog reset digital core ");break;
    case 8  : return String("TG1WDT_SYS_RESET - Timer Group1 Watch dog reset digital core ");break;
    case 9  : return String("RTCWDT_SYS_RESET - RTC Watch dog Reset digital core ");break;
    case 10 : return String("INTRUSION_RESET - Instrusion tested to reset CPU ");break;
    case 11 : return String("TGWDT_CPU_RESET - Time Group reset CPU ");break;
    case 12 : return String("SW_CPU_RESET - Software reset CPU ");break;
    case 13 : return String("RTCWDT_CPU_RESET - RTC Watch dog Reset CPU ");break;
    case 14 : return String("EXT_CPU_RESET - APP CPU reset by PRO CPU ");break;
    case 15 : return String("RTCWDT_BROWN_OUT_RESET - The supply voltage became unstable ");break;
    case 16 : return String("RTCWDT_RTC_RESET - RTC Watch dog reset digital core and rtc module ");break;
    default : return String("Unknown ");
  }
  return String("Unknown ");
}
































unsigned long lastInterruptTime = 0;
unsigned long interruptTime;
void IRAM_ATTR PbVector() {
  // Hit the semaphore ONLY within the bounds of the debounce timer.
  // In other words, the only piece of code in here that's not debounce
  // logic is "pushButtonSemaphore++"
  // I learned the hard way that ISR's must be blazingly quick on ESP32,
  // else the watchdog will think the loop() is in the weeds and will reboot
  // the damn cpu.
  interruptTime = millis();
  if (interruptTime - lastInterruptTime > 200) pushButtonSemaphore++;
  lastInterruptTime = interruptTime;
}

#if DEBUG
bool okgo = false;
void IRAM_ATTR WaitVector() {
  okgo = true;
}
#endif







bool senorsInitialized = false;
byte recoveryByte = 0;
void setup() {
  Serial.begin(115200);
  delay(500);
  systemBootTime = millis();

  EEPROM.begin(EEPROM_RECOVERY_NUMBER_OF_BYTES_TO_ACCESS);

  // init LCD
  lcd.init();

  connectToWifi();

  RESET_REASON reason_cpu0 = rtc_get_reset_reason(0);
  RESET_REASON reason_cpu1 = rtc_get_reset_reason(1);
  if (reason_cpu0 == POWERON_RESET) {
    // Just say there was a power failure
    sendMessageToAWS("Fishtank controller has recovered from a power failure.");
  } else {
    // This was something weirder than a power blip/failure.
    // I need to know what it was.
    char str[512];
    memset(str, 0, 512);
    String recoveryMessage = "Fishtank controller has recovered from a reset.\nCPU0 Reason: ";
    recoveryMessage += getResetReasonString(reason_cpu0);
    recoveryMessage += "\nCPU1 Reason: ";
    recoveryMessage += getResetReasonString(reason_cpu1);
    recoveryMessage.toCharArray(str, recoveryMessage.length());
    sendMessageToAWS(str);
  }


  server.begin();
  Serial.print("ssid: ");
  Serial.println(WiFi.SSID());
  localIP = WiFi.localIP();
  Serial.print("IP Address: ");
  Serial.println(localIP);

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("SSID:");
  lcd.print(WiFi.SSID());
  lcd.setCursor(0,1);
  lcd.print(localIP);

  sensors.begin();

  DeviceAddress addr;
  for(int i = 0; i < NUMBER_OF_SENSORS; i++) {
    if (sensors.getAddress(addr, i)) {
      if (sensorMap[i].location == tank) senorsInitialized = true; // Only set this true if we found a WATER sensor. Those are critical.
      for (int j = 0; j < NUMBER_OF_SENSORS; j++) {
        if (compareAddresses(addr, sensorMap[j].address)) {
          // found a match
          sensorMap[j].busIndex = i;
          Serial.print("Sensor ");
          Serial.print(sensorMap[j].stickerId);
          Serial.print(" is at bus index ");
          // Serial.println(sensorMap[j].busIndex);
          Serial.print(sensorMap[j].busIndex);
          Serial.print(" ");
          Serial.println(sensors.getTempF(sensorMap[i].address));
          if (sensorMap[i].location == tank) goodSensors++;
          break;
        }//if
      }// for(j)
    } else {
      // There is logic in the main loop() that will add this sensor to the blacklist,
      // and alert the user.
      Serial.print("No sensor at bus index ");
      Serial.println(i);
    }
  }// for(i)

  recoveryByte = EEPROM.read(EEPROM_RECOVERY_BYTE);

  if (!senorsInitialized) {
    // We're going to reboot to try to recover from this. Set the recovery byte to indicate this.
    if (recoveryByte != EEPROM_RECOVERY_NO_SENSORS) {
      // Only write if we need to
      EEPROM.write(EEPROM_RECOVERY_BYTE, (byte)EEPROM_RECOVERY_NO_SENSORS);
      EEPROM.commit();
    }

    if (sendMessageToAWS("!ALERT! Could not connect to temperature probes!\nI'M COMPLETELY DOWN!\nGet out here and fix me now!!")) {
      // Couldn't find any sensors, but we managed to send out an alert.
      // Go to deep sleep and try again in an hour.
      Serial.println("Alert sent. Going to deep sleep...");
      esp_sleep_enable_timer_wakeup(60L * 1000000L);
      esp_deep_sleep_start();
    } else {
      // Oh boy, this is bad. Very bad.
      // We're in trouble, and we can't signal for help.
      // We're on our own. Sleep for a minute and try again after a reboot.
      Serial.println("Alert NOT sent. Going to deep sleep...");
      esp_sleep_enable_timer_wakeup(60L * 1000000L);
      esp_deep_sleep_start();
    }
  } else {
    // Sensors are talking.
    if (recoveryByte == EEPROM_RECOVERY_NO_SENSORS) {
      // Looks like we just recovered from a forced reboot due to sensors not responding.
      // Let the user know everything is ok now.
      EEPROM.write(EEPROM_RECOVERY_BYTE, (byte)0);
      EEPROM.commit();
      sendMessageToAWS("Sensors are responding now. A reboot seems to have fixed it.\nStill... that's not supposed to happen. Maybe go check the connections.");
      Serial.println("Warning: Previously recovered from a sensor failure.");
    }
  }

  // Set the push button for GPIO17
  pinMode(PUSH_BUTTON, INPUT_PULLUP);

#if DEBUG
  attachInterrupt(PUSH_BUTTON, WaitVector, FALLING);

  Serial.print("Waiting");
  while(!okgo) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  Serial.println("GO!");

  // todo: remove for production
  detachInterrupt(PUSH_BUTTON);
#endif

  // Set the push button for GPIO17
  attachInterrupt(PUSH_BUTTON, PbVector, FALLING);

  pinMode(PUMP_RELAY, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);

  Serial.println("Starting main program loop...");
  Serial.println();
}




























void loop() {
  
  if (WiFi.status() != WL_CONNECTED) {
    // that's the wifi deid
    connectToWifi();
  }

  sensors.requestTemperatures();

  if (pendingAlert) {
    // If sendMessageToAWS fails, set pendingAlert to true so it keeps trying.
    // Don't give up until the error gets out!
    Serial.println("Sending message to AWS:");
    pendingAlert = !sendMessageToAWS(charErrorMessage);
    if (!pendingAlert) {
      // Alert sent successfully
      Serial.println("Message sent successfully.");
      if (badTempAlertPending) {
        badTempAlertPending = false;
        badTempAlertSent = true;
      }
      if (extremeLowTempAlertPending) {
        extremeLowTempAlertPending = false;
        extremeLowTempAlertSent = true;
      }
      if (extremeHighTempAlertPending) {
        extremeHighTempAlertPending = false;
        extremeHighTempAlertSent = true;
      }
    } else {
      Serial.println("Message failed to send.");
    }
  }

  if (lcdIsOn) {
    if (lcdTimeoutCounter == 0) {
      lcdTimeoutCounter = millis();
    } else if (millis() - lcdTimeoutCounter > LCD_TIMEOUT) {
      lcdIsOn = false;
      lcd.noDisplay();
      lcd.noBacklight();
    }
  }

  if (pushButtonSemaphore) {
    if (!lcdIsOn) {
      lcd.display();
      lcd.backlight();
    }
    lcd.clear();
    lcd.setCursor(0,0);
    lcdIsOn = true;
    lcdTimeoutCounter = 0;
    if (displaySelector == showWifi) {
      displaySelector = showWaterTemp;
      lcd.print("SSID:");
      lcd.print(WiFi.SSID());
      lcd.setCursor(0,1);
      lcd.print(WiFi.localIP());
    } else if (displaySelector == showWaterTemp) {
      displaySelector = showAmbientTemp;
      lcd.print("Water Temp:");
      lcd.setCursor(0,1);
      lcd.print(trustedTemp);
    } else if (displaySelector == showAmbientTemp) {
      displaySelector = showPumpStatus;
      lcd.print("Ambient Temp:");
      lcd.setCursor(0,1);
      for(int i = 0; i < NUMBER_OF_SENSORS; i++) {
        if (sensorMap[i].location == ambient) {
          lcd.print(sensors.getTempF(sensorMap[i].address));
          break;
        }
      }
    } else if (displaySelector == showPumpStatus) {
      displaySelector = showNotificationStatus;
      lcd.print(pumpIsOn ? "Pump is ON" : "Pump is OFF");
      lcd.setCursor(0,1);
      lcd.print(((millis() - currentPumpStateStart) / 1000) / 60);
      lcd.print(" minutes");
    } else if (displaySelector == showNotificationStatus) {
      displaySelector = showWifi;
      lcd.print("Notifications");
      lcd.setCursor(0,1);
      lcd.print(EEPROM.read(EEPROM_MUTE_NOTIFICATIONS_BYTE) == 1 ? "are OFF" : "are ON");
    }
    pushButtonSemaphore = 0;
  }

  // Logic for activating thermal remediation. hehe
  if (goodSensors > 1) {
    deltaHi = 0;
    deltaLo = 999;
    for (int i = 0; i < NUMBER_OF_SENSORS; i++) {
      if (sensorMap[i].blacklisted || sensorMap[i].location != tank) continue;
      temp = sensors.getTempF(sensorMap[i].address);
      if (temp < deltaLo) deltaLo = temp;
      if (temp > deltaHi) deltaHi = temp;
    }
    delta = deltaHi - deltaLo;

    if(delta > MAX_ACCEPTABLE_TEMPERATURE_DELTA) {
      // Houston, we have a problem
      // Do something.

      if (goodSensors == 2) {
        // We can't arbitrarily decide which of 2 to trust, UNLESS... one of them is obviously dead.
        // So before we give up, see if one of them is completely dead. If so, trust the other one.
        int lastTwoSensorIndecies[2];
        int idx = 0;
        memset(lastTwoSensorIndecies, 0, sizeof(*lastTwoSensorIndecies) * 2);
        for (int i = 0; i < NUMBER_OF_SENSORS; i++) {
          if (sensorMap[i].blacklisted || sensorMap[i].location != tank) continue;
          if (idx++ == 0) {
            tempA = sensors.getTempF(sensorMap[i].address);
            lastTwoSensorIndecies[0] = i;
          } else {
            tempB = sensors.getTempF(sensorMap[i].address);
            lastTwoSensorIndecies[1] = i;
            break;
          }
        }
        if (tempA == TEMP_SENSOR_DISCONNECTED && tempB != TEMP_SENSOR_DISCONNECTED) {
          // A is dead; B is not. Trust B. --> Set A as the outlier
          outlyingSensor = sensorMap[lastTwoSensorIndecies[0]].stickerId;
          String strTemp = String(tempB);
          // Longest string example, 221 chars: !ALERT! Sensor 999 is dead, and has been blacklisted. You only have ONE functioning sensor now, Sensor 999. It reads -196.60. I'm gonna go with that because it's all I got left. You really need to come fix me! Seriously.\0
          snprintf(charErrorMessage, 221, "!ALERT! Sensor %d is dead, and has been blacklisted. You only have ONE functioning sensor now, Sensor %d. It reads %s. I'm gonna go with that because it's all I got left. You really need to come fix me! Seriously.\0", outlyingSensor, sensorMap[lastTwoSensorIndecies[1]].stickerId, strTemp.c_str());
        } else if (tempA != TEMP_SENSOR_DISCONNECTED && tempB == TEMP_SENSOR_DISCONNECTED) {
          // B is dead; A is not. Trust A. --> Set B as the outlier
          outlyingSensor = sensorMap[lastTwoSensorIndecies[1]].stickerId;
          String strTemp = String(tempA);
          // Longest string example, 221 chars: !ALERT! Sensor 999 is dead, and has been blacklisted. You only have ONE functioning sensor now, Sensor 999. It reads -196.60. I'm gonna go with that because it's all I got left. You really need to come fix me! Seriously.\0
          snprintf(charErrorMessage, 221, "!ALERT! Sensor %d is dead, and has been blacklisted. You only have ONE functioning sensor now, Sensor %d. It reads %s. I'm gonna go with that because it's all I got left. You really need to come fix me! Seriously.\0", outlyingSensor, sensorMap[lastTwoSensorIndecies[0]].stickerId, strTemp.c_str());
        } else {
          // Ok, so the 2 remaining sensors are not in agreement, and the "bad" one is not obvious.
          // This is BAD. Real bad.
          // Now we don't know WHAT to believe.
          // Send message to the cloud
          String strTempA = String(tempA);
          String strTempB = String(tempB);
          // Longest string example, 262 chars: !ALERT! I'm down to only 2 sensors, and they disagree with each other! Sensor 999 says -196.60, and Sensor 999 says -196.60, and I have no way of knowing which is correct! In other words, I'M DOWN, and YOUR FISH ARE IN DANGER!! Come fix me! (Trying a reboot...)\0
          snprintf(charErrorMessage, 262, "!ALERT! I'm down to only 2 sensors, and they disagree with each other! Sensor %d says %s, and Sensor %d says %s, and I have no way of knowing which is correct! In other words, I'M DOWN, and YOUR FISH ARE IN DANGER!! Come fix me! (Trying a reboot...)\0", sensorMap[lastTwoSensorIndecies[0]].stickerId, strTempA.c_str(), sensorMap[lastTwoSensorIndecies[1]].stickerId, strTempB.c_str());
          
          Serial.println(charErrorMessage);
          sendMessageToAWS(charErrorMessage);

          EEPROM.write(EEPROM_RECOVERY_BYTE, (byte)EEPROM_RECOVERY_WHAT_TO_BELIEVE);
          EEPROM.commit();
          Serial.println("Alert sent. Going to deep sleep...");
          esp_sleep_enable_timer_wakeup(60L * 1000000L);
          esp_deep_sleep_start();
        }
      } else {
        // Find the outlier
        highestAverageDelta = 0;
        for (int i = 0; i < NUMBER_OF_SENSORS; i++) {
          if (sensorMap[i].blacklisted || sensorMap[i].location != tank) continue;

          runningAverageDelta = 0;
          tempA = sensors.getTempF(sensorMap[i].address);
          for (int j = 0; j < NUMBER_OF_SENSORS; j++) {
            if (j == i) continue; // Don't compare a sensor to itself!
            if (sensorMap[j].blacklisted || sensorMap[j].location != tank) continue;
            tempB = sensors.getTempF(sensorMap[j].address);
            runningAverageDelta += abs(tempA - tempB);
          }
          runningAverageDelta /= goodSensors - 1; // Minus 1 because we divide by how many deltas there are, not how many sensors.
          Serial.print("runningAverageDelta for sensor ");
          Serial.print(sensorMap[i].stickerId);
          Serial.print(" is ");
          Serial.println(runningAverageDelta);
          if (runningAverageDelta > highestAverageDelta) {
            Serial.print("This is higher than ");
            Serial.println(highestAverageDelta);
            highestAverageDelta = runningAverageDelta;
            outlyingSensor = sensorMap[i].stickerId;
            outlyingTemp = tempA;
          }
        }
        Serial.println();

        // Send message to the cloud
        String strOutlyingReading = String(outlyingTemp);
        // Longest string example, 223 chars: ALERT! Sensor 999 is off in the weeds with a reading of -196.60. It has been blacklisted. You can try a reboot to see if the problem goes away. If you end up replacing it, call Daniel. The procedure is not straightforward.\0
        snprintf(charErrorMessage, 223, "ALERT! Sensor %d is off in the weeds with a reading of %s. It has been blacklisted. You can try a reboot to see if the problem goes away. If you end up replacing it, call Daniel. The procedure is not straightforward.\0", outlyingSensor, strOutlyingReading.c_str());
        Serial.println(charErrorMessage);
      }

      // Blacklist the sensor
      for (int i = 0; i < NUMBER_OF_SENSORS; i++) if (sensorMap[i].stickerId == outlyingSensor) { sensorMap[i].blacklisted = true; break; }
      goodSensors--;
      pendingAlert = true;
    } else {
      // They all seem to be in agreement.
      if (recoveryByte == EEPROM_RECOVERY_WHAT_TO_BELIEVE) {
        // Looks like we just recovered from one too many sensors off in the weeds.
        // They seem to be behaving now. At least, enough are behaving for us to function.
        recoveryByte = 0;
        EEPROM.write(EEPROM_RECOVERY_BYTE, (byte)0);
        EEPROM.commit();
        sendMessageToAWS("Ok, looks like we were having issues with sensors going off in the weeds, but I have rebooted and that appears to have resolved it. Still, that's not supposed to happen. Maybe check my connections, make sure the fish aren't messing with my senors, etc.");
      }
    }
  } else {
    // There's only one sensor still working. Bad news.
    // Nag.
    if (!oneSensorNagSent) {
      // Send it
      int oneSensor = 0;
      for (int i = 0; i < NUMBER_OF_SENSORS; i++) if (!sensorMap[i].blacklisted && sensorMap[i].location == tank) { oneSensor = sensorMap[i].stickerId; break; }
      String oneSensorTemp = String(trustedTemp);
      // Longest string example, 77 chars: ALERT! I'm limping along with only ONE sensor right now! Sensor 999: -196.60\0
      snprintf(charErrorMessage, 77, "ALERT! I'm limping along with only ONE sensor right now! Sensor %d: %s\0", oneSensor, oneSensorTemp.c_str());
      if (oneSensorNagSent = sendMessageToAWS(charErrorMessage)) {
        // Reset the timer
        millisSinceOneSensorTimerNagSent = 0;
        currentOneSensorTimerTick = lastOneSensorTimerTick = millis();
      }
    }

    if (millisSinceOneSensorTimerNagSent > MILLISECONDS_IN_ONE_DAY) {
      oneSensorNagSent = false;
    } else {
      currentOneSensorTimerTick = millis();
      millisSinceOneSensorTimerNagSent += currentOneSensorTimerTick - lastOneSensorTimerTick;
      lastOneSensorTimerTick = currentOneSensorTimerTick;
    }
  }

  for (int i = 0; i < NUMBER_OF_SENSORS; i++) {
    // Now that we have some idea of what we can trust,
    // go ahead and take the reading of the first one (that's trusted)
    // as the temperature.
    if (sensorMap[i].blacklisted || sensorMap[i].location != tank) continue;
    trustedTemp = sensors.getTempF(sensorMap[i].address);
    trustedSensor = sensorMap[i].stickerId;
    break;
  }

  // Get the boiler temp
  for (int i = 0; i < NUMBER_OF_SENSORS; i++) {
    if (sensorMap[i].location == boiler) {
      boilerTemp = sensors.getTempF(sensorMap[i].address);
      break;
    }
  }
/*
  if (!temperatureIsGood) {
    // Temperature is out of range.
    // Rectification should be in progress, but increment the timer.
    // We don't want to wait too long for rectification.
    if ((correctionTimer = millis() - correctionTimerStart) > CORRECTION_TIMEOUT && !badTempAlertSent) {
      tempOutOfRangeForTooLong = true;
      pendingAlert = true;
      badTempAlertPending = true;
      badTempAlertSent = false;
      // This is bad. The temperature is not correcting.
      // Say something!
      dtostrf(trustedTemp, 4, 2, tempFloat);
      // Longest string example, 80 chars: ALERT: Temperature has been out of range for 201600 seconds. It is now -196.60!\0
      snprintf(charErrorMessage, 80, "ALERT: Temperature has been out of range for %d seconds. It is now %s!\0", correctionTimer / 1000, tempFloat);
      Serial.println(charErrorMessage);
    }
  }
*/
  // If we get here, then we have a temperature reading that we trust.
  // Act on it.
  if (!pumpIsOn && trustedTemp <= TEMP_LOWER_TRIGGER && boilerTemp >= TEMP_BOILER_UPPER_TRIGGER) {
    // We hit the lower trigger, the pump is not on, and there's heat available from the boiler.
    // Turn it on.
    Serial.println("Turning on pump");
    digitalWrite(PUMP_RELAY, HIGH);
    digitalWrite(LED_BUILTIN, HIGH);
    pumpIsOn = true;

    // We are now in a correction. Reset the timer.
    correctionTimerStart = millis();
    getLocalTime(&timeinfo_pumpState);
    temperatureIsGood = false;

    // Make a note of when this pump state changed. We will use this to tell the user how long it's been on (if they ask).
    currentPumpStateStart = correctionTimerStart;
  } else if (pumpIsOn && (trustedTemp >= TEMP_UPPER_TRIGGER || boilerTemp <= TEMP_BOILER_LOWER_TRIGGER)) {
    // We hit the upper trigger, or the boiler is too cold, and the pump is on.
    // Unless you want boiled tilapia for dinner, turn it off.
    if (trustedTemp >= TEMP_UPPER_TRIGGER) Serial.println("Turning off pump because the fish are nice and warm now");
    if (boilerTemp <= TEMP_BOILER_LOWER_TRIGGER) Serial.println("Turning off pump because there's not enough heat from the boiler");
    digitalWrite(PUMP_RELAY, LOW);
    digitalWrite(LED_BUILTIN, LOW);
    pumpIsOn = false;

    // We are now in a correction. Reset the timer.
    correctionTimerStart = millis();
    getLocalTime(&timeinfo_pumpState);
    temperatureIsGood = false;

    // Make a note of when this pump state changed. We will use this to tell the user how long it's been off (if they ask).
    currentPumpStateStart = correctionTimerStart;
  }

  // Check for absolute thresholds
  /*
  if (trustedTemp <= TEMP_ABSOLUTE_LOWER && !extremeLowTempAlertSent) {
    // This is very bad. The temp should never, ever get this low.
    pendingAlert = true;
    dtostrf(trustedTemp, 4, 2, tempFloat);
    // Longest string example, 43 chars: DANGER: Temperature has dropped to -196.60\0
    snprintf(charErrorMessage, 43, "DANGER: Temperature has dropped to %s\0", tempFloat);
    extremeLowTempAlertPending = true;
    extremeLowTempAlertSent = false;
  } else if (trustedTemp >= TEMP_ABSOLUTE_UPPER && !extremeHighTempAlertSent) {
    // This is very bad. The temp should never, ever get this high.
    pendingAlert = true;
    dtostrf(trustedTemp, 4, 2, tempFloat);
    // Longest string example, 41 chars: DANGER: Temperature has risen to -196.60\0
    snprintf(charErrorMessage, 41, "DANGER: Temperature has risen to %s\0", tempFloat);
    extremeHighTempAlertPending = true;
    extremeHighTempAlertSent = false;
  } else {
    // Reset all error flags
    temperatureIsGood = true;
    badTempAlertPending = false;
    badTempAlertSent = false;
    extremeLowTempAlertSent = false;
    extremeLowTempAlertPending = false;
    extremeHighTempAlertSent = false;
    extremeHighTempAlertPending = false;
    if (tempOutOfRangeForTooLong) {
      // Looks like we had a close call, but things are good now.
      tempOutOfRangeForTooLong = false;

      String strRecoveredTemp = String(trustedTemp);
      // Longest string example, 65 chars: Looks like we're back in range again. Temperature is now -196.60\0
      snprintf(charErrorMessage, 65, "Looks like we're back in range again. Temperature is now %s\0", strRecoveredTemp.c_str());
      pendingAlert = true;
      Serial.println(charErrorMessage);
    }
  }*/

  // This is just to listen for incoming http queries
  server.handleClient();
}
