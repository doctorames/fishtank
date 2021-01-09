//#include <OneWire.h>  <-- OneWire.h is already included in DallasTemperature.h
#include <DallasTemperature.h>

// purple: 286BB707D6013CAC
// white:  28902D07D6013C2C
// green:  28162C07D6013CB9
// orange: 284F4107B6013C59


// GPIO pins
#define ONE_WIRE_BUS 16

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);


void printDeviceAddress(DeviceAddress deviceAddress) {
  
  for (uint8_t i = 0; i < 8; i++) {
    
    if (deviceAddress[i] < 16) 
      Serial.print("0");
      Serial.print(deviceAddress[i], HEX);
  }
}

void setup()
{
  Serial.begin(115200);
  while (!Serial);
  Serial.println("\nSensor address probe start...");

  sensors.begin();

  DeviceAddress addr;
  if (sensors.getAddress(addr, 0)) {
    Serial.print("Address: ");
    printDeviceAddress(addr);
    Serial.println();
  } else {
    Serial.print("No sensor at bus index ");
    Serial.println(0);
  }
}

void loop()
{
  // spin
  delay(5000);
}
