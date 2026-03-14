const int targetPin = 13;

void setup() {
  pinMode(targetPin, OUTPUT);
  // Standard MIDI baud rate
  Serial.begin(31250);
  while (!Serial); // Wait for Serial to initialize
  Serial.println("MIDI Serial Initialized");
}

void loop() {
  if (Serial.available() > 1) {
    byte statusByte = Serial.read();
    Serial.print("MIDI Received: #");    
    // Check if it's a Program Change (0xC0 to 0xCF)
    if ((statusByte & 0xF0) == 0xC0) {
      byte programNum = Serial.read();
      Serial.print(statusByte, HEX);
      Serial.println(programNum, HEX);
      // Control pin based on program number
      if (programNum == 1) {
        digitalWrite(targetPin, HIGH);
      } else {
        digitalWrite(targetPin, LOW);
      }
    }
  }
}