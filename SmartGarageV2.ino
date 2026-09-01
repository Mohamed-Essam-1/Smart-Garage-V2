#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>

const char* ssid = "SmartGarageV2"; 
const char* password = "vincere123";  

WebServer server(80);

const String PROJECT_NAME = "Smart Garage V2";
const String TEAM_NAME = "Vincere";

// ============================================================
// LIGHTING SYSTEM
// ============================================================
#define led2 27   
#define pot 35    
#define ldr 32   

// ============================================================
// FIRE ALARM SYSTEM
// ============================================================
#define alarm_led 25
#define buzzer 23
#define STATE_FIRE_ALARM 4
#define flameSensor 19

// ============================================================
// PARKING SPOT SYSTEM
// ============================================================
#define ir1 5
#define ir2 18
#define ir3 13
#define ir4 4

// ============================================================
// GATE SYSTEM
// ============================================================
#define gateTriggerIR 33  // outer sensor - before the gate, detects an approaching car
#define gateClearIR 34    // inner sensor - after the gate, confirms the car has passed through
#define SERVO_PIN 14

// ============================================================
// DISPLAY SYSTEM
// ============================================================
#define SDA 21
#define SCL 22
#define totalSpots 4
#define RATE_PER_MINUTE 2   // parking fee rate (EGP per minute)
#define MIN_FEE 1           // minimum charge even for very short stays

// Display states 
#define STATE_IDLE_COUNT 0
#define STATE_IDLE_MAP 1
#define STATE_WELCOME 2
#define STATE_GOODBYE 3

LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo gateServo;

// ---- Gate system state ----
bool gateOpen = false;
bool gateSequenceActive = false;  // true while a car is currently mid-passage through the gate
bool carEntering = false;         // true = entering, false = exiting (set when the sequence starts)
bool lastGateTriggerState = false;
bool lastGateClearState = false;

int currentServoAngle = 0;   // servo's current position
int targetServoAngle = 0;    // where the servo is moving toward
unsigned long lastServoStepTime = 0;
const unsigned long SERVO_STEP_INTERVAL = 30;  // ms per degree - smaller = faster sweep, larger = slower/smoother

// ---- Display state machine ----
int currentState = STATE_IDLE_COUNT;
unsigned long stateStartTime = 0;  // when we entered the current display state (for millis()-based timing)

const unsigned long WELCOME_DURATION = 2000;
const unsigned long GOODBYE_DURATION = 3000;
const unsigned long IDLE_SWITCH_INTERVAL = 3000;  // how often the idle screen alternates between count/map
const unsigned long BLINK_INTERVAL = 400;         // LCD fire alarm blink rate
const unsigned long SCROLL_INTERVAL = 300;        // fire alarm scrolling text speed

unsigned long lastBlinkTime = 0;
unsigned long lastScrollTime = 0;
bool blinkVisible = true;
unsigned long scrollPos = 0;  // current position in the scrolling message "window"

const String fireMessage = "EVACUATE NOW - FIRE DETECTED - ";

// Custom LCD character bitmap - flame icon 
byte flameIcon[8] = {
  0b00100,
  0b00100,
  0b01110,
  0b01010,
  0b10101,
  0b10001,
  0b01110,
  0b00000
};

// ---- Parking spot system state ----
int irPins[totalSpots] = { ir1, ir2, ir3, ir4 };
bool spotOccupied[totalSpots] = { false, false, false, false };
bool lastSpotState[totalSpots] = { false, false, false, false };  // used for edge detection (only react on change)
unsigned long spotEntryTime[totalSpots] = { 0, 0, 0, 0 };          // per-spot timestamp, used to calculate parking fee
int occupiedCount = 0;

// ---- Fire alarm system state ----
bool fireDetected = false;

// ---- Pricing / last transaction (shown on LCD Goodbye screen)  ----
unsigned long lastParkedSeconds = 0;
int lastFee = 0;
bool hasTransaction = false;

void setup() {
  Serial.begin(9600);
  Wire.begin(SDA, SCL);
  lcd.init();
  lcd.backlight();
  lcd.createChar(0, flameIcon);
  pinMode(gateTriggerIR, INPUT);
  pinMode(led2, OUTPUT);
  pinMode(pot, INPUT);
  pinMode(ldr, INPUT);
  pinMode(gateClearIR, INPUT);
  pinMode(buzzer, OUTPUT);
  pinMode(alarm_led, OUTPUT);
  gateServo.attach(SERVO_PIN);
  closeGate();  

  for (int i = 0; i < totalSpots; i++) {
    pinMode(irPins[i], INPUT);
  }
  pinMode(flameSensor, INPUT);

  connectWiFi();

  server.on("/", handleRoot);
  server.begin();

  enterState(STATE_IDLE_COUNT, millis());
}

void loop() {
  server.handleClient();
  lightningsystem();
  checkIRSensors();
  checkFlameSensor();
  checkGateSensors();
  updateServoMovement(millis());
  updateDisplay();
}

// ============================================================
// LIGHTING SYSTEM
// ============================================================
void lightningsystem() {
  int reading = analogRead(pot);
  reading = map(reading, 0, 4095, 0, 255);  
  int sun_light = analogRead(ldr);

  if (sun_light <= 1600) {
    analogWrite(led2, 0);
  } else {
    analogWrite(led2, reading);
  }
}

// ============================================================
// WIFI
// ============================================================
void connectWiFi() {
  WiFi.softAP(ssid, password);  

  lcd.clear();
  lcd.print("Access Point:");
  lcd.setCursor(0, 1);
  lcd.print(ssid);
  delay(2000);

  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);

  lcd.clear();
  lcd.print("IP:");
  lcd.setCursor(0, 1);
  lcd.print(IP);
  delay(3000);
}

// ============================================================
// GATE SYSTEM
// ============================================================

// Moves the servo one degree at a time toward targetServoAngle - smooth, non-blocking movement
void updateServoMovement(unsigned long now) {
  if (currentServoAngle == targetServoAngle) return;
  if (now - lastServoStepTime >= SERVO_STEP_INTERVAL) {
    lastServoStepTime = now;
    if (currentServoAngle < targetServoAngle) currentServoAngle++;
    else currentServoAngle--;
    gateServo.write(currentServoAngle);
  }
}

void openGate() {
  gateOpen = true;
  targetServoAngle = 90;
}

void closeGate() {
  gateOpen = false;
  targetServoAngle = 0;
}

// Determines gate direction by checking which sensor fires first:
// trigger-then-clear = car entering, clear-then-trigger = car exiting.
// Closes the gate only once the car has fully passed the "far" sensor for its direction.
void checkGateSensors() {
  bool triggerDetected = (digitalRead(gateTriggerIR) == LOW);
  bool clearDetected = (digitalRead(gateClearIR) == LOW);

  // No car passing right now? See which sensor fires first to determine direction
  if (!gateSequenceActive) {
    if (triggerDetected && !lastGateTriggerState) {
      gateSequenceActive = true;
      carEntering = true;  // outer sensor fired first -> entering
      openGate();
    } else if (clearDetected && !lastGateClearState) {
      gateSequenceActive = true;
      carEntering = false;  // inner sensor fired first -> exiting
      openGate();
    }
  }

  // Car has fully cleared the "far" sensor for its direction -> safe to close + show message
  if (gateSequenceActive) {
    if (carEntering && !clearDetected && lastGateClearState) {
      closeGate();
      if (!fireDetected) enterState(STATE_WELCOME, millis());
      gateSequenceActive = false;
    } else if (!carEntering && !triggerDetected && lastGateTriggerState) {
      closeGate();
      if (!fireDetected) enterState(STATE_GOODBYE, millis());
      gateSequenceActive = false;
    }
  }

  lastGateTriggerState = triggerDetected;
  lastGateClearState = clearDetected;
}

// ============================================================
// PARKING SPOT SYSTEM
// ============================================================

// Edge-detection: only reacts when a spot's state actually changes, not on every read
void checkIRSensors() {
  for (int i = 0; i < totalSpots; i++) {
    bool detected = (digitalRead(irPins[i]) == LOW);
    if (detected != lastSpotState[i]) {
      lastSpotState[i] = detected;
      spotOccupied[i] = detected;
      recalculateOccupied();

      if (detected) {
        spotEntryTime[i] = millis();
      } else {
        calculateFee(i);
      }
    }
  }
}

// Billed by the minute, but duration is shown in seconds on the LCD
void calculateFee(int spotIndex) {
  unsigned long parkedMs = millis() - spotEntryTime[spotIndex];
  lastParkedSeconds = parkedMs / 1000;

  unsigned long parkedMinutes = lastParkedSeconds / 60;  
  lastFee = parkedMinutes * RATE_PER_MINUTE;
  if (lastFee < MIN_FEE) lastFee = MIN_FEE;
  hasTransaction = true;
}

void recalculateOccupied() {
  occupiedCount = 0;
  for (int i = 0; i < totalSpots; i++) {
    if (spotOccupied[i]) occupiedCount++;
  }
}

// ============================================================
// DISPLAY STATE MACHINE
// ============================================================

// The ONLY place that changes currentState - clears the screen and renders the new state once
void enterState(int newState, unsigned long now) {
  currentState = newState;
  stateStartTime = now;
  lcd.clear();

  if (newState == STATE_IDLE_COUNT) {
    renderIdleCount();
  } else if (newState == STATE_IDLE_MAP) {
    renderSpotMap();
  } else if (newState == STATE_WELCOME) {
    renderWelcome();
  } else if (newState == STATE_GOODBYE) {
    renderGoodbye();
  } else if (newState == STATE_FIRE_ALARM) {
    blinkVisible = true;
    lastBlinkTime = now;
    lastScrollTime = now;
    scrollPos = 0;
    lcd.setCursor(0, 0);
    lcd.print("!! FIRE ALARM !!");
    renderScrollingText(fireMessage, 1);
  }
}

// ============================================================
// FIRE ALARM SYSTEM
// ============================================================

// No edge detection here on purpose - fireDetected should always reflect the live sensor state
void checkFlameSensor() {
  fireDetected = (digitalRead(flameSensor) == HIGH);
}

void updateDisplay() {
  unsigned long now = millis();

  // Fire alarm always takes top priority - overrides any other screen
  if (fireDetected) {
    if (currentState != STATE_FIRE_ALARM) {
      enterState(STATE_FIRE_ALARM, now);
    }
    renderFireAlarm(now);
    return;
  } else if (currentState == STATE_FIRE_ALARM) {
    enterState(STATE_IDLE_COUNT, now);
    noTone(buzzer);            
    digitalWrite(alarm_led, LOW);
    return;
  }

  if (currentState == STATE_WELCOME) {
    if (now - stateStartTime >= WELCOME_DURATION) enterState(STATE_IDLE_COUNT, now);
  } else if (currentState == STATE_GOODBYE) {
    if (now - stateStartTime >= GOODBYE_DURATION) enterState(STATE_IDLE_COUNT, now);
  } else if (currentState == STATE_IDLE_COUNT) {
    if (now - stateStartTime >= IDLE_SWITCH_INTERVAL) enterState(STATE_IDLE_MAP, now);
  } else if (currentState == STATE_IDLE_MAP) {
    if (now - stateStartTime >= IDLE_SWITCH_INTERVAL) enterState(STATE_IDLE_COUNT, now);
  }
}

void renderIdleCount() {
  int freeSpots = totalSpots - occupiedCount;
  lcd.setCursor(0, 0);
  lcd.print(freeSpots == 0 ? "Garage Full" : "Available Spots");
  lcd.setCursor(0, 1);
  lcd.print(freeSpots);
  lcd.print(" / ");
  lcd.print(totalSpots);
}

// Each spot gets 3 LCD columns (number + status), 4 spots = 12 of 16 columns used
void renderSpotMap() {
  for (int i = 0; i < totalSpots; i++) {
    lcd.setCursor(i * 3, 0);
    lcd.print("P");
    lcd.print(i + 1);
  }
  for (int i = 0; i < totalSpots; i++) {
    lcd.setCursor(i * 3, 1);
    lcd.print(spotOccupied[i] ? "X" : "O");
  }
}

void renderWelcome() {
  lcd.setCursor(0, 0);
  lcd.print("Welcome");
  lcd.setCursor(0, 1);
  lcd.print("Free Spots: ");
  lcd.print(totalSpots - occupiedCount);
}

void renderGoodbye() {
  lcd.setCursor(0, 0);
  lcd.print("Goodbye");
  lcd.setCursor(0, 1);
  lcd.print(lastParkedSeconds);
  lcd.print("s - ");
  lcd.print(lastFee);
  lcd.print(" EGP");
}

void renderFireAlarm(unsigned long now) {
  if (now - lastBlinkTime >= BLINK_INTERVAL) {
    lastBlinkTime = now;
    blinkVisible = !blinkVisible;
    if (blinkVisible) {
      tone(buzzer, 1000);
      digitalWrite(alarm_led, HIGH);
    } else {
      tone(buzzer, 1400);
      digitalWrite(alarm_led, LOW);
    }

    lcd.setCursor(0, 0);
    if (blinkVisible) {
      lcd.print("!! FIRE ALARM !!");
    } else {
      // alternate row of flame icons instead of text
      for (int i = 0; i < 16; i++) {
        lcd.write(byte(0));
      }
    }
  }

  if (now - lastScrollTime >= SCROLL_INTERVAL) {
    lastScrollTime = now;
    scrollPos++;
    renderScrollingText(fireMessage, 1);
  }
}

// Slides a 16-character "window" over a longer message, wrapping around with modulo
void renderScrollingText(const String& message, int row) {
  String padded = message + "    ";
  int len = padded.length();

  String visible = "";
  for (int i = 0; i < 16; i++) {
    visible += padded[(scrollPos + i) % len];
  }

  lcd.setCursor(0, row);
  lcd.print(visible);
}

// ============================================================
// WEB DASHBOARD
// ============================================================
void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='2'>";
  html += "<title>" + PROJECT_NAME + "</title>";
  html += "<style>";
  html += "@keyframes pulse{0%{opacity:1;}50%{opacity:0.6;}100%{opacity:1;}}";
  html += ".alert-fire{background:#e04b4b;color:#fff;font-weight:bold;padding:12px;border-radius:8px;margin-bottom:20px;font-size:16px;animation:pulse 1s infinite;}";
  html += ".alert-gate{background:#e0a83d;color:#1a1a1a;font-weight:bold;padding:10px;border-radius:8px;margin-bottom:20px;}";
  html += ".status-normal{background:#1a222b;color:#888;padding:8px;border-radius:8px;margin-bottom:20px;font-size:13px;}";
  html += "body{font-family:Arial, sans-serif;text-align:center;background:#12181f;color:#f0f0f0;margin:0;padding:20px;}";
  html += "h1{color:#3ddc84;margin-bottom:4px;}";
  html += ".team{color:#888;margin-top:0;margin-bottom:24px;font-size:14px;}";
  html += ".count{font-size:22px;margin-bottom:24px;}";
  html += ".count span{color:#3ddc84;font-weight:bold;}";
  html += ".garage{display:flex;justify-content:center;gap:14px;flex-wrap:wrap;max-width:640px;margin:0 auto 28px;}";
  html += ".spot{width:90px;height:110px;border-radius:10px;display:flex;flex-direction:column;align-items:center;justify-content:center;font-size:13px;}";
  html += ".spot.free{background:#173a2b;border:2px dashed #3ddc84;color:#3ddc84;}";
  html += ".spot.occupied{background:#3a1717;border:2px solid #e04b4b;color:#ffb3b3;}";
  html += ".car{font-size:34px;margin-bottom:6px;}";
  html += ".panel{max-width:360px;margin:0 auto 24px;background:#1a222b;border-radius:10px;padding:16px;font-size:14px;color:#ccc;}";
  html += ".panel b{color:#3ddc84;}";
  html += "footer{color:#555;font-size:12px;margin-top:20px;}";
  html += "</style></head><body>";

  html += "<h1>" + PROJECT_NAME + "</h1>";
  html += "<p class='team'>Team: " + TEAM_NAME + "</p>";
  if (fireDetected) {
    html += "<div class='alert-fire'>FIRE ALARM ACTIVE - EVACUATE</div>";
  } else if (gateSequenceActive) {
    html += "<div class='alert-gate'>Gate: " + String(carEntering ? "Car Entering..." : "Car Exiting...") + "</div>";
  } else {
    html += "<div class='status-normal'>Gate: " + String(gateOpen ? "Open" : "Closed") + " &middot; All Systems Normal</div>";
  }
  int freeSpots = totalSpots - occupiedCount;
  html += "<div class='count'>Available Spots: <span>" + String(freeSpots) + " / " + String(totalSpots) + "</span></div>";

  html += "<div class='garage'>";
  for (int i = 0; i < totalSpots; i++) {
    if (spotOccupied[i]) {
      html += "<div class='spot occupied'><div class='car'>&#128663;</div>P" + String(i + 1) + "</div>";
    } else {
      html += "<div class='spot free'>Empty<br>P" + String(i + 1) + "</div>";
    }
  }
  html += "</div>";

  html += "<div class='panel'>";
  if (hasTransaction) {
    html += "Last car parked for <b>" + String(lastParkedSeconds) + "s</b><br>";
    html += "Fee: <b>" + String(lastFee) + " EGP</b>";
  } else {
    html += "No transactions yet";
  }
  html += "</div>";

  html += "<footer>by ELESS</footer>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}