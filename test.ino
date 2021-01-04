#include <WiFiClientSecure.h>
#include <MQTTClient.h>
#include <ArduinoJson.h>
#include "WiFi.h"



bool okgo = false;

void IRAM_ATTR WaitVector() {
  okgo = true;
}





// Wifi credentials
// const char *WIFI_SSID = "ATT4meZ5qR";
// const char *WIFI_PASSWORD = "7xj%4%82sxz5";

const char *WIFI_SSID = "ATT8Vnn5MG";
const char *WIFI_PASSWORD = "72+wwi7w6b=q";

// The name of the device. This MUST match up with the name defined in the AWS console
#define DEVICE_NAME "fishtemp-controller"

// The MQTTT endpoint for the device (unique for each AWS account but shared amongst devices within the account)
#define AWS_IOT_ENDPOINT "a3u6y1u8z9tj3s-ats.iot.us-east-1.amazonaws.com"

// The MQTT topic that this device should publish to
#define AWS_IOT_TOPIC "$aws/things/" DEVICE_NAME "/shadow/update"
// #define AWS_IOT_TOPIC "topic/testTopic"

// How many times we should attempt to connect to AWS
#define AWS_MAX_RECONNECT_TRIES 50

WiFiClientSecure net = WiFiClientSecure();
MQTTClient client = MQTTClient(512);

void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Only try 15 times to connect to the WiFi
  Serial.print("\nConnecting to ");
  Serial.print(WIFI_SSID);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 100){
    delay(500);
    Serial.print(".");
    retries++;
  }

  // If we still couldn't connect to the WiFi, go to deep sleep for a minute and try again.
  if(WiFi.status() != WL_CONNECTED){
    esp_sleep_enable_timer_wakeup(1 * 60L * 1000000L);
    esp_deep_sleep_start();
  } else {
      Serial.println("connected!");
  }
}

void connectToAWS()
{
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
    return;
  }

  // If we land here, we have successfully connected to AWS!
  // And we can subscribe to topics and send messages.
  Serial.println(" ..Connected!");
}

void sendJsonToAWS()
{
  StaticJsonDocument<128> jsonDoc;
  JsonObject stateObj = jsonDoc.createNestedObject("state");
  JsonObject reportedObj = stateObj.createNestedObject("reported");
  
  // Write the temperature & humidity. Here you can use any C++ type (and you can refer to variables)
  reportedObj["message"] = "test test 1 2 3...";

  Serial.println("Publishing message to AWS...");
  char jsonBuffer[512];
  serializeJson(jsonDoc, jsonBuffer);

  // Serial.print(jsonBuffer);

  bool ret = client.publish(AWS_IOT_TOPIC, jsonBuffer);

  Serial.print("\nJSON Status: ");
  Serial.println(ret);


//   Serial.println("Publishing message to AWS...");
//   bool ret = client.publish(AWS_IOT_TOPIC, "{\"message\":\"should work\"}");
//   Serial.print("Text Status: ");
//   Serial.println(ret);
}

void setup() {
  Serial.begin(9600);
  delay(500);

  connectToWiFi();

  pinMode(22, INPUT_PULLUP);
  attachInterrupt(22, WaitVector, RISING);

  Serial.print("\n\nWaiting");
  while(!okgo) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("GO!");

  connectToAWS();
  sendTextToAWS();
}

void loop() {
  client.loop();
  delay(1000);
}