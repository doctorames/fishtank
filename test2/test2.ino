
int foo;

int* getList(int howBig){
  static int list[howBig];
  return list;
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(9600);
  foo = 2;
  int bar[foo];
}



void loop() {
  // put your main code here, to run repeatedly:
//  bar[0] = 5;
//  bar[1] = 6;

//  Serial.println(bar[0]);
//  Serial.println(bar[1]);
  delay(90000);
}
