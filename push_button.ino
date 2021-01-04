
bool okgo = false;

void IRAM_ATTR WaitVector() {
  okgo = true;
}



void setup() {
  Serial.begin(9600);
  // put your setup code here, to run once:
  pinMode(22, INPUT_PULLUP);
  attachInterrupt(22, WaitVector, RISING);

  Serial.print("Waiting");
  while(!okgo) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("GO!");
}

void loop() {
  // put your main code here, to run repeatedly:

}