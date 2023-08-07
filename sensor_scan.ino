//#include <OneWire.h>  <-- OneWire.h is already included in DallasTemperature.h
#include <DallasTemperature.h>


// GPIO pins
#define ONE_WIRE_BUS 16

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// Outputs the address in c-style byte array format.
// For example:
//   {0x28, 0xF8, 0x13, 0x07, 0xB6, 0x01, 0x3C, 0xCD}
void printDeviceAddress(DeviceAddress deviceAddress) {
  Serial.print("{");
  for (uint8_t i = 0; i < 8; i++) {
    Serial.print("0x");
    if (deviceAddress[i] < 16)
      Serial.print("0");
      Serial.print(deviceAddress[i], HEX);
      if(i != 7) Serial.print(", ");
  }
  Serial.print("}");
}

void setup()
{
  Serial.begin(115200);
  while (!Serial);
  Serial.println("\nSensor address probe start...");

  sensors.begin();

  DeviceAddress addr;
  for(int i = 0; i < 5; i++) // look for 5 sensors: 3 tank, 1 ambient, 1 boiler
  {
    if (sensors.getAddress(addr, i)) {
      Serial.print("Index ");
      Serial.print(i);
      Serial.print(" address: ");
      printDeviceAddress(addr);
      Serial.println();
    } else {
      Serial.print("No sensor at bus index ");
      Serial.println(i);
    }
  }
}

void loop()
{
  // spin
  delay(5000);
}
