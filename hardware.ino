#define TRIG_PIN  13
#define ECHO_PIN  12
#define PUMP_PIN  14
#define UV_PIN    27
 
bool personWasDetected = false;
bool sequenceRunning = false;
 
void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(UV_PIN,   OUTPUT);
  digitalWrite(PUMP_PIN, HIGH); // relay OFF
  digitalWrite(UV_PIN,   HIGH); // relay OFF
  Serial.println("System ready. Waiting for person...");
}
 
float getDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 60000);
  return duration / 58.0;
}
 
void runSequence() {
  Serial.println("Pump ON - flushing...");
  digitalWrite(PUMP_PIN, LOW);
  delay(3000);
  digitalWrite(PUMP_PIN, HIGH);
  Serial.println("Pump OFF");
  delay(1000);
  Serial.println("UV ON - disinfecting... look away!");
  digitalWrite(UV_PIN, LOW);
  delay(5000);
  digitalWrite(UV_PIN, HIGH);
  Serial.println("UV OFF");
  Serial.println("Sequence complete. Ready for next person.");
}
 
void loop() {
  if (sequenceRunning) return;
  float distance = getDistance();
  if (distance <= 0 || distance > 400) { delay(500); return; }
  Serial.print("Distance: "); Serial.print(distance); Serial.println(" cm");
  if (distance < 30 && !personWasDetected) {
    personWasDetected = true;
    Serial.println("Person detected - waiting for them to leave...");
  }
  if (distance >= 30 && personWasDetected) {
    Serial.println("Person left - starting in 3 seconds...");
    delay(1000); Serial.println("3...");
    delay(1000); Serial.println("2...");
    delay(1000); Serial.println("1...");
    sequenceRunning = true;
    personWasDetected = false;
    runSequence();
    sequenceRunning = false;
  }
  delay(500);
}
 


