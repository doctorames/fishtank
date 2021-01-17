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

#define DEBUG 0
#define PRODUCTION_UNIT 1
#define AT_PEEPAWS 1

#if AT_PEEPAWS
char ssid[] = "ATT4meZ5qR";
char pass[] = "7xj%4%82sxz5";
#else
char ssid[] = "ATT8Vnn5MG";
char pass[] = "72+wwi7w6b=q";
#endif

// char ssid[] = "Ugh";
// char pass[] = "aaadaa001";

#define NUMBER_OF_AMBIENT_SENSORS 1
#define NUMBER_OF_WATER_SENSORS 3
#define NUMBER_OF_SENSORS  (NUMBER_OF_AMBIENT_SENSORS + NUMBER_OF_WATER_SENSORS)
#define MILLISECONDS_IN_ONE_DAY  86400000u

// GPIO pins
#define ONE_WIRE_BUS 16
#define PUMP_RELAY 19
#define START_BUTTON 17
#define PUSH_BUTTON 5
#define ALERT_LED LED_BUILTIN

// EEPROM bytes
#define EEPROM_RECOVERY_BYTE 0
#define EEPROM_RECOVERY_NUMBER_OF_BYTES_TO_ACCESS 1

#define EEPROM_RECOVERY_NO_SENSORS 1         // Rebooted because I could not detect any sensors at startup
#define EEPROM_RECOVERY_WHAT_TO_BELIEVE 2    // Rebooted because all sensors disagree and I don't know what to believe anymore

// Temperature set points
#define MAX_ACCEPTABLE_TEMPERATURE_DELTA 4.0  // If a sensor's average drift from the others exceeds this amount, it will be blacklisted
#define TEMP_LOWER_TRIGGER 70.00              // Turn on the pump when temperature is less than this amount
#define TEMP_UPPER_TRIGGER 75.00              // Turn off the pump when temperature is more than this amount
#define TEMP_ABSOLUTE_LOWER  50.00            // Send an alert if the temperature ever drops below this amount
#define TEMP_ABSOLUTE_UPPER  85.00            // Send an alert if the temperature ever rises above this amount
#define TEMP_SENSOR_DISCONNECTED -196.60f     // The sensor library will return this specific reading for a sensor that becomes unresponsive

// Time-outs and retry thresholds
#define MAX_WIFI_ATTEMPTS   120               // Reboot if we can't connect after this many attempts
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

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = -21600;
const int   daylightOffset_sec = 3600;
struct tm timeinfo_boot;
struct tm timeinfo_pumpState;

bool internetIsUp = false;  // todo: make logic to deal with failed connectivity
bool lcdIsOn = false;
IPAddress localIP;
unsigned long lcdTimeoutCounter = 0;
float trustedTemp;
char httpStr[256];
#define SYS_STATUS_PAGE_STR_LEN 512
char systemStatusPageStr[SYS_STATUS_PAGE_STR_LEN];
char tempFloat[12];
short pushButtonSemaphore = 0;

typedef enum {
  showWifi,
  showWaterTemp,
  showAmbientTemp,
  showPumpStatus
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

typedef struct _SensorInfo {
  int           busIndex;  // The index of the sensor as seen by DallasTemperature library
  byte          stickerId; // The number we assign to the sensor when we physically put a sticker on it
  bool          blacklisted;
  bool          ambient;
  DeviceAddress address;   // The factory-assigned ID of the sensor (we have no say in this)
} SensorInfo;

#if PRODUCTION_UNIT
SensorInfo sensorMap[NUMBER_OF_SENSORS] = {
  { 0xff, (byte)1, false, false, {0x28, 0xF8, 0x13, 0x07, 0xB6, 0x01, 0x3C, 0xCD}},
  { 0xff, (byte)2, false, false, {0x28, 0x4E, 0xE3, 0x07, 0xB6, 0x01, 0x3C, 0xDE}},
  { 0xff, (byte)3, false, false, {0x28, 0x69, 0xA8, 0x07, 0xB6, 0x01, 0x3C, 0x74}},
  { 0xff, (byte)4, false, true,  {0x28, 0xC2, 0xDC, 0x07, 0xB6, 0x01, 0x3C, 0xA2}} // ambient sensor
};
#else
SensorInfo sensorMap[NUMBER_OF_SENSORS] = {
  { 0xff, (byte)1, false, false, {0x28, 0x6B, 0xB7, 0x07, 0xD6, 0x01, 0x3C, 0xAC}}, // purple
  { 0xff, (byte)2, false, false, {0x28, 0x90, 0x2D, 0x07, 0xD6, 0x01, 0x3C, 0x2C}}, // white
  { 0xff, (byte)3, false, false, {0x28, 0x16, 0x2C, 0x07, 0xD6, 0x01, 0x3C, 0xB9}}, // green
  { 0xff, (byte)4, false, true,  {0x28, 0x4F, 0x41, 0x07, 0xB6, 0x01, 0x3C, 0x59}}  // orange (ambient)
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
  if (!internetIsUp) return false;

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
  if (!internetIsUp) return false;
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

  // Serial.print(jsonBuffer);

  bool ret = client.publish(AWS_IOT_TOPIC, jsonBuffer);

  Serial.print("\nJSON Status: ");
  Serial.println(ret);

  client.disconnect();

  return ret;
}

// Returns a timestamp string in the format:
// h:mm:ss AM|PM, mm/dd/yyyy
void getDateTimeMyWay(tm* time, char* ptr, int length) {
  memset(ptr, 0, length);
  sprintf(ptr, "%d:%02d:%02d %s, %d/%d/%d",
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
    sprintf(str, "%d second%s", seconds, seconds == 1 ? "" : "s");
    return;
  }
  uint minutes = seconds / 60;
  if (minutes <= 60) {
    // It's only been a few minutes
    sprintf(str, "%d minute%s", minutes, minutes == 1 ? "" : "s");
    return;
  }
  uint hours = minutes / 60;
  minutes -= hours * 60;
  if (hours <= 24) {
    // It's only been a few hours
    if (minutes == 0)
      sprintf(str, "%d hour%s", hours, hours > 1 ? "s" : "");
    else
      sprintf(str, "%d hour%s and %d minute%s", hours, hours > 1 ? "s" : "", minutes, minutes > 1 ? "s" : "");
    return;
  }

  // It's been more than a day
  uint days = hours / 24;
  hours -= days * 24;
  if (minutes == 0)
      sprintf(str, "%d day%s and %d hour%s", days, days > 1 ? "s" : "", hours, hours > 1 ? "s" : "");
  else
    sprintf(str, "%d day%s, %d hour%s and %d minute%s", days, days > 1 ? "s" : "", hours, hours > 1 ? "s" : "", minutes, minutes > 1 ? "s" : "");
}

char* getSystemStatus() {

  // Pardon the html mess. Gotta tell the browser to not make the text super tiny.
  String html = "<!DOCTYPE html><html><head><title>Temperatures</title></head><body><p style=\"font-size:36px\">";
#if !PRODUCTION_UNIT
  html += "<span style=\"color:Red;font-size:60px\">---THIS IS THE DEBUG UNIT---</span></br>";
#endif

  char dateTime[24];
  struct tm ti;
  getLocalTime(&ti);
  char currentTime[40];
  memset(currentTime, 0, 40);
  getDateTimeMyWay(&ti, dateTime, 24);
  sprintf(currentTime, "Readings as of:  %s", dateTime);
  html += currentTime;
  html+= "</br></br>";

  // Show the water sensors
  for(int i = 0; i < NUMBER_OF_SENSORS; i++) {
    // sprintf() does not support %f on arduino, so we have to convert the temperature
    // to a string first, and pass the string into sprintf().
    if (!sensorMap[i].ambient) {
      dtostrf(sensors.getTempF(sensorMap[i].address), 4, 2, tempFloat);
      sprintf(httpStr, "Sensor %d:  %s%s</br>", sensorMap[i].stickerId, tempFloat, sensorMap[i].blacklisted ? "   (blacklisted)" : "");
      html += httpStr;
    }
  }
  html += "</br>";
  
  // Show the ambient sensor
  for(int i = 0; i < NUMBER_OF_SENSORS; i++) {
    // sprintf() does not support %f on arduino, so we have to convert the temperature
    // to a string first, and pass the string into sprintf().
    if (sensorMap[i].ambient) {
      dtostrf(sensors.getTempF(sensorMap[i].address), 4, 2, tempFloat);
      sprintf(httpStr, "Ambient:  %s</br>", tempFloat);
      html += httpStr;
    }
  }
  html += "</br>";

  // Show pump state
  getDateTimeMyWay(&timeinfo_pumpState, dateTime, 24);
  millisToDaysHoursMinutes(millis() - currentPumpStateStart, currentTime, 40);
  sprintf(httpStr, "Pump has been %s for %s   (since %s)</br>",
  pumpIsOn ? "<span style=\"color:Green;\">ON</span>" : "<span style=\"color:Red;\">OFF</span>",
  currentTime,
  dateTime);
  html += httpStr;
  html += "</br>";

  // Show system uptime
  getDateTimeMyWay(&timeinfo_boot, dateTime, 24);
  html += "<span style=\"font-size:30px\">";
  sprintf(httpStr, "Last boot:  %s</br>", dateTime);
  html += httpStr;
  millisToDaysHoursMinutes(millis() - systemBootTime, currentTime, 40);
  sprintf(httpStr, "System uptime:  %s</br>", currentTime);
  html += httpStr;
  html += "</span>";
  
  // Close it off
  html += "</p></body></html>";

  memset(systemStatusPageStr, 0, SYS_STATUS_PAGE_STR_LEN);
  html.toCharArray(systemStatusPageStr, html.length());
  return systemStatusPageStr;
}

bool setupOta() {
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", getSystemStatus());
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
}

bool restartInternetServices() {
  // todo
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
int wifiRetries = 0;
byte recoveryByte = 0;
void setup() {
  Serial.begin(115200);
  delay(500);

  EEPROM.begin(EEPROM_RECOVERY_NUMBER_OF_BYTES_TO_ACCESS);

  // init LCD
  int dotLocation = 13;
  int dots = 0;
  lcd.init();
  lcd.backlight();
  lcdIsOn = true;
  lcd.setCursor(0,0);
  lcd.print("Connecting to");
  lcd.setCursor(0,1);
  lcd.print(ssid);

  WiFi.begin(ssid, pass);
  Serial.print("Connecting to: ");
  Serial.print(ssid);
  while (WiFi.status() != WL_CONNECTED && wifiRetries < MAX_WIFI_ATTEMPTS) {
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
  if (wifiRetries >= MAX_WIFI_ATTEMPTS) {
    // Could not connect
    internetIsUp = false;
  } else {
    internetIsUp = true;
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    getLocalTime(&timeinfo_boot);
    systemBootTime = millis();
    // Initialize pump state time to something.
    // If the temperature is in range, then the code path to set the pump state time will not be followed.
    memcpy(&timeinfo_pumpState, &timeinfo_boot, sizeof(timeinfo_boot));
    setupOta();
  }

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
      if (!sensorMap[i].ambient) senorsInitialized = true; // Only set this true if we found a WATER sensor. Those are critical.
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
          if (!sensorMap[i].ambient) goodSensors++;
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
  pinMode(START_BUTTON, INPUT_PULLUP);

#if DEBUG
  attachInterrupt(START_BUTTON, WaitVector, FALLING);

  Serial.print("Waiting");
  while(!okgo) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  Serial.println("GO!");

  // todo: remove for production
  detachInterrupt(START_BUTTON);
#endif

  // Set the push button for GPIO17
  attachInterrupt(START_BUTTON, PbVector, FALLING);

  pinMode(PUMP_RELAY, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);

  Serial.println("Starting main program loop...");
  Serial.println();
}




























void loop() {
  sensors.requestTemperatures();

  if (pendingAlert) {
    // If sendMessageToAWS fails, set pendingAlert to true so it keeps trying.
    // Don't give up until the error gets out!
    Serial.println("Sending message to AWS:");
    Serial.println(charErrorMessage);
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
      Serial.println("Message sent successfully.");
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
        if (sensorMap[i].ambient) {
          lcd.print(sensors.getTempF(sensorMap[i].address));
          break;
        }
      }
    } else if (displaySelector == showPumpStatus) {
      displaySelector = showWifi;
      lcd.print(pumpIsOn ? "Pump is ON" : "Pump is OFF");
      lcd.setCursor(0,1);
      lcd.print(((millis() - currentPumpStateStart) / 1000) / 60);
      lcd.print(" minutes");
    }
    pushButtonSemaphore = 0;
  }

  // Logic for activating thermal remediation. hehe
  if (goodSensors > 1) {
    deltaHi = 0;
    deltaLo = 999;
    for (int i = 0; i < NUMBER_OF_SENSORS; i++) {
      if (sensorMap[i].blacklisted || sensorMap[i].ambient) continue;
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
          if (sensorMap[i].blacklisted || sensorMap[i].ambient) continue;
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
          sprintf(charErrorMessage, "!ALERT! Sensor %d is dead, and has been blacklisted. You only have ONE functioning sensor now, Sensor %d. It reads %s. I'm gonna go with that because it's all I got left. You really need to come fix me! Seriously.\0", outlyingSensor, sensorMap[lastTwoSensorIndecies[1]].stickerId, strTemp.c_str());
        } else if (tempA != TEMP_SENSOR_DISCONNECTED && tempB == TEMP_SENSOR_DISCONNECTED) {
          // B is dead; A is not. Trust A. --> Set B as the outlier
          outlyingSensor = sensorMap[lastTwoSensorIndecies[1]].stickerId;
          String strTemp = String(tempA);
          sprintf(charErrorMessage, "!ALERT! Sensor %d is dead, and has been blacklisted. You only have ONE functioning sensor now, Sensor %d. It reads %s. I'm gonna go with that because it's all I got left. You really need to come fix me! Seriously.\0", outlyingSensor, sensorMap[lastTwoSensorIndecies[0]].stickerId, strTemp.c_str());
        } else {
          // Ok, so the 2 remaining sensors are not in agreement, and the "bad" one is not obvious.
          // This is BAD. Real bad.
          // Now we don't know WHAT to believe.
          // Send message to the cloud
          String strTempA = String(tempA);
          String strTempB = String(tempB);
          sprintf(charErrorMessage, "!ALERT! I'm down to only 2 sensors, and they disagree with each other! Sensor %d says %s, and Sensor %d says %s, and I have no way of knowing which is correct! In other words, I'M DOWN, and YOUR FISH ARE IN DANGER!! Come fix me! (Trying a reboot...)\0", sensorMap[lastTwoSensorIndecies[0]].stickerId, strTempA.c_str(), sensorMap[lastTwoSensorIndecies[1]].stickerId, strTempB.c_str());
          
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
          if (sensorMap[i].blacklisted || sensorMap[i].ambient) continue;

          runningAverageDelta = 0;
          tempA = sensors.getTempF(sensorMap[i].address);
          for (int j = 0; j < NUMBER_OF_SENSORS; j++) {
            if (j == i) continue; // Don't compare a sensor to itself!
            if (sensorMap[j].blacklisted || sensorMap[j].ambient) continue;
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
        sprintf(charErrorMessage, "ALERT! Sensor %d is off in the weeds with a reading of %s. It has been blacklisted. You can try a reboot to see if the problem goes away. If you end up replacing it, call Daniel. The procedure is not straightforward.\0", outlyingSensor, strOutlyingReading.c_str());
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
      for (int i = 0; i < NUMBER_OF_SENSORS; i++) if (!sensorMap[i].blacklisted && !sensorMap[i].ambient) { oneSensor = sensorMap[i].stickerId; break; }
      String oneSensorTemp = String(trustedTemp);
      sprintf(charErrorMessage, "ALERT! I'm limping along with only ONE sensor right now! Sensor %d: %s\0", oneSensor, oneSensorTemp.c_str());
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
    if (sensorMap[i].blacklisted || sensorMap[i].ambient) continue;
    trustedTemp = sensors.getTempF(sensorMap[i].address);
    trustedSensor = sensorMap[i].stickerId;
    break;
  }

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
      sprintf(charErrorMessage, "ALERT: Temperature has been out of range for %d seconds. It is now %s!\0", correctionTimer / 1000, tempFloat);
      Serial.println(charErrorMessage);
    }
  }

  // If we get here, then we have a temperature reading that we trust.
  // Act on it.
  if (trustedTemp <= TEMP_LOWER_TRIGGER) {
    if (!pumpIsOn) {
      // We hit the lower trigger, and the pump is not on.
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
    }
    // Check for absolute lower threshold
    if (trustedTemp <= TEMP_ABSOLUTE_LOWER && !extremeLowTempAlertSent) {
      // This is very bad. The temp should never, ever get this low.
      pendingAlert = true;
      dtostrf(trustedTemp, 4, 2, tempFloat);
      sprintf(charErrorMessage, "DANGER: Temperature has dropped to %s\0", tempFloat);
      extremeLowTempAlertPending = true;
      extremeLowTempAlertSent = false;
    }
  } else if (trustedTemp >= TEMP_UPPER_TRIGGER) {
    if (pumpIsOn) {
      // We hit the upper trigger, and the pump is on.
      // Unless you want boiled tilapia for dinner, turn it off.
      Serial.println("Turning off pump");
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
    // Check for absolute upper threshold
    if (trustedTemp >= TEMP_ABSOLUTE_UPPER && !extremeHighTempAlertSent) {
      // This is very bad. The temp should never, ever get this high.
      pendingAlert = true;
      dtostrf(trustedTemp, 4, 2, tempFloat);
      sprintf(charErrorMessage, "DANGER: Temperature has risen to %s\0", tempFloat);
      extremeHighTempAlertPending = true;
      extremeHighTempAlertSent = false;
    }
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
      // Looks like we had a close call, but things are god now.
      tempOutOfRangeForTooLong = false;

      String strRecoveredTemp = String(trustedTemp);
      sprintf(charErrorMessage, "Whew! Looks like we're back in range again. Temperature is now %s\0", strRecoveredTemp.c_str());
      pendingAlert = true;
      Serial.println(charErrorMessage);
    }
  }

  // This is just to listen for incoming http queries
  server.handleClient();
}
