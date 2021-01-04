/*
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

#include <WiFiClientSecure.h>
#include <MQTTClient.h>
#include <ArduinoJson.h>


#include <WiFiServer.h>
// #include <WiFiServerSecure.h>
// #include <WiFiServerSecureAxTLS.h>
// #include <WiFiServerSecureBearSSL.h>

#ifndef TEST_CODE


//#include <OneWire.h>  <-- OneWire.h is already included in DallasTemperature.h
#include <DallasTemperature.h>
#include <SPI.h>
#include <LiquidCrystal_I2C.h>

char ssid[] = "ATT4meZ5qR";
char pass[] = "7xj%4%82sxz5";
// char ssid[] = "ATT8Vnn5MG";
// char pass[] = "72+wwi7w6b=q";

// char ssid[] = "Ugh";
// char pass[] = "aaadaa001";


#define ONE_WIRE_BUS 16
#define PUMP_RELAY 18
#define START_BUTTON 17
#define PUSH_BUTTON 5

#define MAX_ACCEPTABLE_TEMPERATURE_DELTA 4.0
#define ALERT_LED LED_BUILTIN
#define NUMBER_OF_SENSORS 3
// #define CORRECTION_TIMEOUT 7200000   // The max amount of time it should take for temperature to return to an acceptable range.
#define CORRECTION_TIMEOUT 60000   // The max amount of time it should take for temperature to return to an acceptable range.
#define TEMP_LOWER_TRIGGER 60.00
#define TEMP_UPPER_TRIGGER 75.00

#define TEMP_ABSOLUTE_LOWER  50.00
#define TEMP_ABSOLUTE_UPPER  85.00

#define     TIMEOUT_MS             15000



int count = 0;
bool lcdIsOn = false;
int sensorIndexMap[NUMBER_OF_SENSORS];
IPAddress localIP;
unsigned long lcdTimeoutCounter = 0;
float trustedTemp;
char str[32];
char tempFloat[12];

unsigned long lastPolledTime = 0;
unsigned long sketchTime = 0;
const long utcOffsetInSeconds = -21600;


float deltaHi;
float deltaLo;
float delta;
float temp;
float tempA;
float tempB;

int sensorBlacklist[NUMBER_OF_SENSORS];
int blacklistedSensors = 0;
int goodSensors = NUMBER_OF_SENSORS;
int sensorHi;
int sensorLo;
int trustedSensor;
int outlyingSensor;

float highestAverageDelta;
float runningAverageDelta;
char charErrorMessage[90];

bool pendingError = false;
bool pumpIsOn = false;
bool temperatureIsGood = true;
bool tempOutOfRangeForTooLong = false;

int correctionTimer = 0;
int correctionTimerStart = 0;
bool badTempAlertPending = false;
bool badTempAlertSent = false;
bool extremeLowTempAlertPending = false;
bool extremeLowTempAlertSent = false;
bool extremeHighTempAlertPending = false;
bool extremeHighTempAlertSent = false;

int keyIndex = 0;

int status = WL_IDLE_STATUS;
WiFiServer server(80);

LiquidCrystal_I2C lcd(0x27, 16, 4);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
DeviceAddress addr;
DeviceAddress knownAddresses[] = {
  {0x28, 0xF8, 0x13, 0x07, 0xB6, 0x01, 0x3C, 0xCD},  // Labeled as "1"
  {0x28, 0x4E, 0xE3, 0x07, 0xB6, 0x01, 0x3C, 0xDE},  // Labeled as "2"
  {0x28, 0x69, 0xA8, 0x07, 0xB6, 0x01, 0x3C, 0x74},  // Labeled as "3"
  {0x28, 0xC2, 0xDC, 0x07, 0xB6, 0x01, 0x3C, 0xA2},  // Labeled as "4"
  {0x28, 0x4F, 0x41, 0x07, 0xB6, 0x01, 0x3C, 0x59}   // Labeled as "5"
};









































// void printAddress(DeviceAddress deviceAddress) {
  
//   for (uint8_t i = 0; i < 8; i++) {
    
//     if (deviceAddress[i] < 16) 
//       Serial.print("0");
//       Serial.print(deviceAddress[i], HEX);
//   }
// }

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

int address2index(DeviceAddress a){
  DeviceAddress addr;
  for(int i = 0; i < NUMBER_OF_SENSORS; i++) {
    sensors.getAddress(addr, i);
    if(compareAddresses(addr, a)) return i;
  }
  return -1;
}

bool isBlacklisted(int sensor, int *blacklist, int originalNumOfSensors) {
  for(int i = 0; i < originalNumOfSensors; i++){
    if(sensor == blacklist[i]){
      return true;
    }
  }
  return false;
}

// Print a value from 0-99 to a 2 character 0 padded character buffer.
// Buffer MUST be at least 2 characters long!
void btoa2Padded(uint8_t value, char* buffer, int base) {
  if (value < base) {
    *buffer = '0';
    ultoa(value, buffer+1, base);
  }
  else {
    ultoa(value, buffer, base); 
  }
}





































bool showWifi = false;
// int displayIndex = 0;
unsigned long lastInterruptTime = 0;
unsigned long interruptTime;
short semaphore = 0;
void IRAM_ATTR PbVector() {

  interruptTime = millis();
  if (interruptTime - lastInterruptTime > 200) semaphore++;
  lastInterruptTime = interruptTime;
}

bool okgo = false;
void IRAM_ATTR WaitVector() {
  okgo = true;
}









void setup() {
  Serial.begin(115200);
  delay(500);

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
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
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
  bool foundSensors = false;

  for(int i = 0; i < NUMBER_OF_SENSORS; i++) {
    if(sensors.getAddress(addr, i)) {
      foundSensors = true;
      for(int j = 0; j < NUMBER_OF_SENSORS; j++) {
        if(compareAddresses(addr, knownAddresses[j])){
          // found a match
          Serial.print("Sensor ");
          Serial.print(j + 1);
          Serial.print(" is at index ");
          Serial.println(i);
          sensorIndexMap[i] = j + 1;
          break;
        }//if
      }// for(j)
    } else {
      Serial.print("No sensor at index ");
      Serial.println(i);
    }
  }// for(i)

  if (!foundSensors) {
    while(1) {
      Serial.println("No sensors found! System halted.");
      delay(10000);
    }
  }

//   setupOta();

  // Set the push button for pin D5
  pinMode(START_BUTTON, INPUT_PULLUP);
  attachInterrupt(START_BUTTON, WaitVector, FALLING);

  Serial.print("Waiting");
  while(!okgo) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  Serial.println("GO!");

  detachInterrupt(START_BUTTON);
  

  // // Set the push button for pin D5
//   pinMode(PUSH_BUTTON, INPUT_PULLUP);
  attachInterrupt(START_BUTTON, PbVector, FALLING);
  

  pinMode(PUMP_RELAY, OUTPUT);

  Serial.println("Starting main program loop...");
  Serial.println();
}






























void loop() {
  ArduinoOTA.handle();
  sensors.requestTemperatures();

  if (lcdIsOn) {
    if (lcdTimeoutCounter == 0) {
      lcdTimeoutCounter = millis();
    } else if (millis() - lcdTimeoutCounter > 5000) {
      lcdIsOn = false;
      lcd.noDisplay();
      lcd.noBacklight();
    }
  }

  if (semaphore) {
    if (!lcdIsOn) {
      lcd.display();
      lcd.backlight();
    }
    lcd.clear();
    lcd.setCursor(0,0);
    lcdIsOn = true;
    lcdTimeoutCounter = 0;
    if (!showWifi) {
      showWifi = true;
      lcd.print("SSID:");
      lcd.print(WiFi.SSID());
      lcd.setCursor(0,1);
      lcd.print(WiFi.localIP());
    } else {
      showWifi = false;
      lcd.print("Temperature");
      lcd.setCursor(0,1);
      lcd.print(trustedTemp);
    }
    semaphore = 0;
  }

  // Logic for activating thermal remediation. hehe
  deltaHi = 0;
  deltaLo = 999;
  for(int i = 0; i < NUMBER_OF_SENSORS; i++) {
    if(isBlacklisted(sensorIndexMap[i], sensorBlacklist, NUMBER_OF_SENSORS)){
      Serial.print("Sensor ");
      Serial.print(sensorIndexMap[i]);
      Serial.println(" is blacklisted!");
      continue;
    }
    temp = sensors.getTempFByIndex(address2index(knownAddresses[i]));
    // Serial.print("Sensor ");
    // Serial.print(sensorIndexMap[i]);
    // Serial.print(" is ");
    // Serial.println(temp);
    if(temp < deltaLo){
      deltaLo = temp;
      sensorLo = sensorIndexMap[i];
    }
    if(temp > deltaHi) {
      deltaHi = temp;
      sensorHi = sensorIndexMap[i];
    }
  }
  // Serial.println();
  delta = deltaHi - deltaLo;

  if(delta > MAX_ACCEPTABLE_TEMPERATURE_DELTA) {
    // Houston, we have a problem
    // Do something.
    // 1. Alert peepaw (maybe just light LED for nowsies)

    // if(goodSensors == 2){
    //   // This is BAD. Real bad.
    //   // Now we don't know WHAT to believe.
    //   // Halt.
    //   while(1){
    //     digitalWrite(ALERT_LED, HIGH);
    //     delay(250);
    //     digitalWrite(ALERT_LED, LOW);
    //     delay(250);
    //   }
    // }
    // 2. Find the outlier
    highestAverageDelta = 0;
    runningAverageDelta = 0;
    for(int i = 0; i < NUMBER_OF_SENSORS; i++) {
      if(isBlacklisted(sensorIndexMap[i], sensorBlacklist, NUMBER_OF_SENSORS)){
        continue;
      }
      tempA = sensors.getTempFByIndex(address2index(knownAddresses[i]));
      for(int j = 0; j < NUMBER_OF_SENSORS; j++) {
        if(j == i) continue;
        if(isBlacklisted(sensorIndexMap[j], sensorBlacklist, NUMBER_OF_SENSORS)){
          continue;
        }
        tempB = sensors.getTempFByIndex(address2index(knownAddresses[j]));
        runningAverageDelta += abs(tempA - tempB);
      }
      runningAverageDelta /= goodSensors - 1;
       Serial.print("runningAverageDelta for sensor ");
       Serial.print(sensorIndexMap[i]);
       Serial.print(" is ");
       Serial.println(runningAverageDelta);
      if(runningAverageDelta > highestAverageDelta){
        Serial.print("This is higher than ");
        Serial.println(highestAverageDelta);
        highestAverageDelta = runningAverageDelta;
        outlyingSensor = sensorIndexMap[i];
      }
    }
    Serial.println();
    Serial.print("Uh oh! Sensor ");
    Serial.print(outlyingSensor);
    Serial.println(" is off in the weeds!!");


    // Send message to the cloud
    // "Sensor XX is off in the weeds with a reading of xx.xx! It has been blacklisted."
    //digitalWrite(ALERT_LED, HIGH);
    
    String strOutlyingReading = String(tempA);
    sprintf(charErrorMessage, "Sensor %d is off in the weeds with a reading of %s It has been blacklisted.\0", outlyingSensor, strOutlyingReading.c_str());
    pendingError = true;

    // Blacklist the sensor
    sensorBlacklist[blacklistedSensors++] = outlyingSensor;
    goodSensors--;
    
  } else {
    // They all seem to be in agreement.
    //digitalWrite(ALERT_LED, LOW);
  }

  for(int i = 0; i < NUMBER_OF_SENSORS; i++) {
    // Now that we have some idea of what we can trust,
    // go ahead and take the reading of the first one (that's trusted)
    // as the temperature.
    if(isBlacklisted(sensorIndexMap[i], sensorBlacklist, NUMBER_OF_SENSORS)){
      continue;
    }
    trustedTemp = sensors.getTempFByIndex(i);
    trustedSensor = sensorIndexMap[i];
    break;
  }

  if (pendingError) {
    // If sendSms fails, set pendingError to true so it keeps trying.
    // Don't give up until the error gets out!
    // pendingError = !sendSms(charErrorMessage, 3, 2000);
    pendingError = !pendingError;
    if (!pendingError) {
      // SMS sent successfully
      if(badTempAlertPending) {
        badTempAlertPending = false;
        badTempAlertSent = true;
      }
      if(extremeLowTempAlertPending) {
        extremeLowTempAlertPending = false;
        extremeLowTempAlertSent = true;
      }
      if(extremeHighTempAlertPending) {
        extremeHighTempAlertPending = false;
        extremeHighTempAlertSent = true;
      }
    }
  }

  if(!temperatureIsGood) {
    // Temperature is out of range.
    // Rectification should be in progress, but increment the timer.
    // We don't want to wait too long for rectification.
    if((correctionTimer = millis() - correctionTimerStart) > CORRECTION_TIMEOUT && !badTempAlertSent) {
      tempOutOfRangeForTooLong = true;
      pendingError = true;
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
  if(trustedTemp <= TEMP_LOWER_TRIGGER) {
    if(!pumpIsOn) {
      // We hit the lower trigger, and the pump is not on.
      // Turn it on.
      digitalWrite(PUMP_RELAY, HIGH);
      pumpIsOn = true;

      // We are now in a correction. Reset the timer.
      correctionTimerStart = millis();
      temperatureIsGood = false;
    }
    // Check for absolute lower threshold
    if (trustedTemp <= TEMP_ABSOLUTE_LOWER && !extremeLowTempAlertSent) {
      // This is very bad. The temp should never, ever get this low.
      pendingError = true;
      dtostrf(trustedTemp, 4, 2, tempFloat);
      sprintf(charErrorMessage, "DANGER: Temperature has dropped to %s\0", tempFloat);
      extremeLowTempAlertPending = true;
      extremeLowTempAlertSent = false;
    }
  } else if(trustedTemp >= TEMP_UPPER_TRIGGER) {
    if(pumpIsOn) {
      // We hit the upper trigger, and the pump is on.
      // Unless you want boiled tilapia for dinner, turn it off.
      digitalWrite(PUMP_RELAY, LOW);
      pumpIsOn = false;

      // We are now in a correction. Reset the timer.
      correctionTimerStart = millis();
      temperatureIsGood = false;
    }
    // Check for absolute upper threshold
    if (trustedTemp >= TEMP_ABSOLUTE_UPPER && !extremeHighTempAlertSent) {
      // This is very bad. The temp should never, ever get this high.
      pendingError = true;
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
    if(tempOutOfRangeForTooLong) {
      // Looks like we had a close call, but things are god now.
      tempOutOfRangeForTooLong = false;
      Serial.println("Whew! Looks like we're back in range again.");
    }
  }

  // This is just to listen for incoming http queries
  WiFiClient client = server.available();
  if(client) {
    Serial.println("new client");
    String curLine = "";
    while(client.connected()){
      if(client.available()){
        char c = client.read();
        Serial.write(c);
        if(c == '\n'){
          if(curLine.length() == 0){
            // Pardon the html mess. Gotta tell the browser to not make the text super tiny.
            client.print("<!DOCTYPE html><html><head><title>Temperatures</title></head><body><p style=\"font-size:30px\">");
            for(int i = 0; i < NUMBER_OF_SENSORS; i++) {
              // sprintf() does not support %f on arduino, so we have to convert the temperature
              // to a string first, and pass the string into sprintf().
              dtostrf(sensors.getTempFByIndex(address2index(knownAddresses[i])), 4, 2, tempFloat);
              sprintf(str, "Sensor %d:  %s</br>", sensorIndexMap[i], tempFloat);
              client.print(str);
            }
            client.print("</p></body></html>");
            break;
          } else {
            curLine = "";
          }
        } else if(c != '\r') {
          curLine += c;
        }
      }
    }
    client.stop();
  }
}
#endif
*/