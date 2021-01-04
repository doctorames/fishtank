#include <EEPROM.h>


void setup() {
  Serial.begin(115200);
  delay(500);

  EEPROM.begin(1);

  byte val = EEPROM.read(0);

  Serial.print("Value: ");
  Serial.println(val);

  EEPROM.write(0, ++val);
  EEPROM.commit();
}




void loop() {

}