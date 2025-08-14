#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <ThingSpeak.h>
#include <EEPROM.h>

// Pin definitions
#define SS_PIN 5
#define RST_PIN 16
#define RELAY_PIN 25
#define BUZZER_PIN 26
#define BUTTON_PIN 27
#define GREEN_LED_PIN 32
#define RED_LED_PIN 33
#define SDA_PIN 21
#define SCL_PIN 22

// OLED Display settings
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// RFID setup
MFRC522 mfrc522(SS_PIN, RST_PIN);

// WiFi Credentials
const char* ssid = "Me";
const char* password = "mehedi113";

// ThingSpeak Settings
unsigned long channelID = 3034695;
const char* writeAPIKey = "4NW2S80FPJ4837YE";

// Authorized UIDs and corresponding employee names
String authorizedUIDs[] = {
  "233B1FBE",
  "63608205"
};

String employeeNames[] = {
  "Mehedi Hasan",
  "Abdul Karim"
};

// WiFi client for ThingSpeak
WiFiClient client;

// EEPROM Configuration
#define EEPROM_SIZE 512
#define RELAY_STATE_ADDRESS 0
#define WRONG_ATTEMPTS_ADDRESS 1
#define EEPROM_SIGNATURE_ADDRESS 2
#define EEPROM_SIGNATURE 0xAB  // Signature to verify valid data

// Variables for non-blocking operations
int wrongAttempts = 0;
bool relayState = false;
bool lastButtonState = HIGH;
bool buttonState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;
unsigned long lastThingSpeakUpdate = 0;
unsigned long thingSpeakInterval = 20000;

// Non-blocking timing variables
unsigned long autoLockTime = 0;
bool autoLockActive = false;
unsigned long buzzerEndTime = 0;
bool buzzerActive = false;
unsigned long accessDeniedEndTime = 0;
bool accessDeniedActive = false;
unsigned long alarmEndTime = 0;
bool alarmActive = false;
unsigned long displayUpdateTime = 0;
unsigned long cardScanCooldown = 0;
unsigned long lastCardUID = 0; // To prevent same card repeated scanning

// Display states
enum DisplayState {
  DISPLAY_STARTUP,
  DISPLAY_CONNECTING,
  DISPLAY_READY,
  DISPLAY_ACCESS_GRANTED,
  DISPLAY_ACCESS_DENIED,
  DISPLAY_BUTTON_PRESSED,
  DISPLAY_MANUAL_UNLOCK,
  DISPLAY_MANUAL_LOCK,
  DISPLAY_SECURITY_ALERT,
  DISPLAY_ALARM_COMPLETE
};

DisplayState currentDisplay = DISPLAY_STARTUP;
String currentEmployee = "";
int displayAnimationFrame = 0;
unsigned long displayAnimationTime = 0;

void setup() {
  Serial.begin(115200);
  
  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);
  
  // Load previous state from EEPROM
  loadStateFromEEPROM();
  
  // Initialize pins
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  
  // Restore previous relay state
  digitalWrite(RELAY_PIN, relayState ? LOW : HIGH); // LOW = Unlocked, HIGH = Locked
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, HIGH); // Green LED on by default
  digitalWrite(RED_LED_PIN, LOW);
  
  // Print restored state
  Serial.println("=== STATE RESTORED FROM MEMORY ===");
  Serial.print("Relay State: ");
  Serial.println(relayState ? "UNLOCKED" : "LOCKED");
  Serial.print("Wrong Attempts: ");
  Serial.println(wrongAttempts);
  Serial.println("===================================");
  
  // Test buzzer on startup - loud beep
  playStartupBeep();
  
  // Initialize SPI bus
  SPI.begin();
  mfrc522.PCD_Init();
  
  // Initialize I2C for OLED
  Wire.begin(SDA_PIN, SCL_PIN);
  
  // Initialize OLED display
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;);
  }
  
  // Show startup animation
  currentDisplay = DISPLAY_STARTUP;
  displayAnimationTime = millis();
  updateDisplay();
  
  // Start WiFi connection (non-blocking)
  WiFi.begin(ssid, password);
  currentDisplay = DISPLAY_CONNECTING;
  
  // Initialize ThingSpeak
  ThingSpeak.begin(client);
  
  Serial.println("RFID Door Lock System Starting...");
}

void loop() {
  unsigned long currentTime = millis();
  
  // Handle WiFi connection
  handleWiFiConnection();
  
  // Handle non-blocking button press
  handleButtonFast();
  
  // Handle RFID scanning - live continuous scanning
  handleRFIDFast();
  
  // Live RFID module reset - non-blocking periodic refresh
  static unsigned long lastRFIDReset = 0;
  if (currentTime - lastRFIDReset > 2000) { // Reset every 2 seconds for better performance
    mfrc522.PCD_Init(); // Quick reinitialize
    lastRFIDReset = currentTime;
  }
  
  // Handle auto-lock timing
  handleAutoLock(currentTime);
  
  // Handle buzzer timing
  handleBuzzer(currentTime);
  
  // Handle access denied sequence
  handleAccessDenied(currentTime);
  
  // Handle alarm sequence
  handleAlarm(currentTime);
  
  // Update display
  handleDisplayUpdate(currentTime);
  
  // Send data to ThingSpeak periodically
  if (currentTime - lastThingSpeakUpdate > thingSpeakInterval) {
    sendToThingSpeak("System Status", relayState ? "Unlocked" : "Locked");
    lastThingSpeakUpdate = currentTime;
  }
  
  // Minimal delay for stability
  delay(10);
}

// ================== EEPROM STATE MANAGEMENT ==================

void loadStateFromEEPROM() {
  // Check if EEPROM contains valid data
  byte signature = EEPROM.read(EEPROM_SIGNATURE_ADDRESS);
  
  if (signature == EEPROM_SIGNATURE) {
    // Valid data found, load previous state
    relayState = EEPROM.read(RELAY_STATE_ADDRESS);
    wrongAttempts = EEPROM.read(WRONG_ATTEMPTS_ADDRESS);
    
    // Validate loaded data
    if (wrongAttempts > 10) { // Sanity check
      wrongAttempts = 0;
    }
    
    Serial.println("Previous state loaded from memory");
  } else {
    // No valid data found, use default values
    relayState = false; // Default to locked
    wrongAttempts = 0;
    
    // Save default values to EEPROM
    saveStateToEEPROM();
    Serial.println("No previous state found, using defaults");
  }
}

void saveStateToEEPROM() {
  // Save current state to EEPROM
  EEPROM.write(RELAY_STATE_ADDRESS, relayState);
  EEPROM.write(WRONG_ATTEMPTS_ADDRESS, wrongAttempts);
  EEPROM.write(EEPROM_SIGNATURE_ADDRESS, EEPROM_SIGNATURE);
  EEPROM.commit(); // Important: commit changes to flash memory
  
  Serial.print("State saved to memory - Relay: ");
  Serial.print(relayState ? "UNLOCKED" : "LOCKED");
  Serial.print(", Wrong Attempts: ");
  Serial.println(wrongAttempts);
}

void handleWiFiConnection() {
  static bool wifiConnected = false;
  
  if (!wifiConnected && WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    
    // Play connection success beep
    playSuccessBeep();
    
    currentDisplay = DISPLAY_READY;
    displayAnimationTime = millis();
    Serial.println("RFID Door Lock System Ready");
  }
}

void handleButtonFast() {
  int reading = digitalRead(BUTTON_PIN);
  unsigned long currentTime = millis();
  
  if (reading != lastButtonState) {
    lastDebounceTime = currentTime;
  }
  
  if ((currentTime - lastDebounceTime) > debounceDelay) {
    if (reading != buttonState) {
      buttonState = reading;
      
      if (buttonState == LOW) { // Button pressed
        currentDisplay = DISPLAY_BUTTON_PRESSED;
        displayAnimationTime = currentTime;
        
        if (relayState) {
          // Currently unlocked - lock it
          lockDoor();
          currentDisplay = DISPLAY_MANUAL_LOCK;
          playSuccessBeep();
        } else {
          // Currently locked - unlock it
          unlockDoor();
          currentDisplay = DISPLAY_MANUAL_UNLOCK;
          autoLockTime = currentTime + 5000; // Auto-lock in 5 seconds
          autoLockActive = true;
          playSuccessBeep();
        }
        
        // Save new state to EEPROM
        saveStateToEEPROM();
        
        sendToThingSpeak("Manual Override", relayState ? "Unlocked" : "Locked");
        Serial.println("Manual button pressed - Relay toggled instantly");
      }
    }
  }
  
  lastButtonState = reading;
}

void handleRFIDFast() {
  // Skip if still in cooldown period
  if (millis() < cardScanCooldown) {
    return;
  }
  
  // Look for new cards
  if (!mfrc522.PICC_IsNewCardPresent()) {
    return;
  }
  
  // Select one of the cards
  if (!mfrc522.PICC_ReadCardSerial()) {
    return;
  }
  
  // Set minimal cooldown for smooth operation
  cardScanCooldown = millis() + 300; // Just 300ms cooldown
  
  // Read UID and convert to hash for comparison
  String cardUID = "";
  unsigned long uidHash = 0;
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    cardUID += String(mfrc522.uid.uidByte[i] < 0x10 ? "0" : "");
    cardUID += String(mfrc522.uid.uidByte[i], HEX);
    uidHash += mfrc522.uid.uidByte[i] * (i + 1); // Simple hash
  }
  cardUID.toUpperCase();
  
  // Prevent same card repeated scanning within short time
  if (uidHash == lastCardUID && (millis() - cardScanCooldown) < 1000) {
    // Same card scanned too quickly, skip
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    return;
  }
  lastCardUID = uidHash;
  
  Serial.print("Card UID: ");
  Serial.println(cardUID);
  
  // Check if card is authorized
  bool authorized = false;
  String employeeName = "";
  
  for (int i = 0; i < sizeof(authorizedUIDs)/sizeof(authorizedUIDs[0]); i++) {
    if (cardUID == authorizedUIDs[i]) {
      authorized = true;
      employeeName = employeeNames[i];
      break;
    }
  }
  
  unsigned long currentTime = millis();
  
  if (authorized) {
    // Access granted - instant response
    wrongAttempts = 0;
    currentEmployee = employeeName;
    currentDisplay = DISPLAY_ACCESS_GRANTED;
    displayAnimationTime = millis();
    
    unlockDoor();
    autoLockTime = millis() + 10000; // Auto-lock in 10 seconds
    autoLockActive = true;
    playSuccessBeep();
    
    // Save state changes to EEPROM
    saveStateToEEPROM();
    
    sendToThingSpeak("Access Granted", employeeName);
    Serial.println("Access Granted: " + employeeName);
    
  } else {
    // Access denied - instant response
    wrongAttempts++;
    currentDisplay = DISPLAY_ACCESS_DENIED;
    displayAnimationTime = millis();
    accessDeniedEndTime = millis() + 2000; // Show for 2 seconds
    accessDeniedActive = true;
    
    // Start LED blinking and error beep
    digitalWrite(RED_LED_PIN, HIGH);
    digitalWrite(GREEN_LED_PIN, LOW);
    playErrorBeep();
    
    // Save updated wrong attempts to EEPROM
    saveStateToEEPROM();
    
    sendToThingSpeak("Access Denied", "Unknown Card - " + cardUID);
    Serial.println("Access Denied - Unknown card: " + cardUID);
    Serial.println("Wrong attempts: " + String(wrongAttempts));
    
    // Check if maximum wrong attempts reached
    if (wrongAttempts >= 5) {
      currentDisplay = DISPLAY_SECURITY_ALERT;
      alarmEndTime = millis() + 10000; // Alarm for 10 seconds
      alarmActive = true;
      wrongAttempts = 0; // Reset counter
      
      // Save reset counter to EEPROM
      saveStateToEEPROM();
      
      sendToThingSpeak("Security Alert", "5 Wrong Attempts - Alarm Triggered");
      Serial.println("SECURITY ALERT: 5 wrong attempts - Triggering alarm!");
    }
  }
  
  // Properly halt and reset PICC for next scan (non-blocking)
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  
  // Immediate reset for next scan - no delay needed
}

void handleAutoLock(unsigned long currentTime) {
  if (autoLockActive && currentTime >= autoLockTime) {
    lockDoor();
    autoLockActive = false;
    currentDisplay = DISPLAY_READY;
    displayAnimationTime = currentTime;
    playLockBeep(); // Play lock confirmation beep
    
    // Save auto-lock state to EEPROM
    saveStateToEEPROM();
    
    Serial.println("Auto-lock activated");
  }
}

void handleBuzzer(unsigned long currentTime) {
  if (buzzerActive && currentTime >= buzzerEndTime) {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerActive = false;
  }
}

void handleAccessDenied(unsigned long currentTime) {
  static unsigned long lastBlink = 0;
  static bool ledState = false;
  
  if (accessDeniedActive) {
    // Blink red LED
    if (currentTime - lastBlink > 200) {
      ledState = !ledState;
      digitalWrite(RED_LED_PIN, ledState ? HIGH : LOW);
      lastBlink = currentTime;
    }
    
    // End access denied sequence
    if (currentTime >= accessDeniedEndTime) {
      accessDeniedActive = false;
      digitalWrite(RED_LED_PIN, LOW);
      digitalWrite(GREEN_LED_PIN, HIGH);
      currentDisplay = DISPLAY_READY;
      displayAnimationTime = currentTime;
    }
  }
}

void handleAlarm(unsigned long currentTime) {
  static unsigned long lastAlarmBlink = 0;
  static bool alarmLedState = false;
  static unsigned long lastAlarmTone = 0;
  static bool alarmToneState = false;
  
  if (alarmActive) {
    // Fast blinking alarm
    if (currentTime - lastAlarmBlink > 100) {
      alarmLedState = !alarmLedState;
      digitalWrite(RED_LED_PIN, alarmLedState ? HIGH : LOW);
      lastAlarmBlink = currentTime;
    }
    
    // Alternating high/low alarm tones
    if (currentTime - lastAlarmTone > 300) {
    alarmToneState = !alarmToneState;

    if (alarmToneState) {
        digitalWrite(BUZZER_PIN, HIGH); // Buzzer ON
    } else {
        digitalWrite(BUZZER_PIN, LOW);  // Buzzer OFF
    }

    lastAlarmTone = currentTime;
}

    
    // End alarm sequence
    if (currentTime >= alarmEndTime) {
      alarmActive = false;
      digitalWrite(RED_LED_PIN, LOW);
      digitalWrite(GREEN_LED_PIN, HIGH);
      noTone(BUZZER_PIN); // Stop any ongoing tone
      digitalWrite(BUZZER_PIN, LOW);
      currentDisplay = DISPLAY_READY;
      displayAnimationTime = currentTime;
      Serial.println("Alarm sequence complete - System ready");
    }
  }
}

void handleDisplayUpdate(unsigned long currentTime) {
  // Update display every 100ms for smooth animations
  if (currentTime - displayUpdateTime > 100) {
    updateDisplay();
    displayUpdateTime = currentTime;
  }
}

void unlockDoor() {
  relayState = true;
  digitalWrite(RELAY_PIN, LOW); // LOW = Unlocked
  digitalWrite(GREEN_LED_PIN, HIGH);
  digitalWrite(RED_LED_PIN, LOW);
}

void lockDoor() {
  relayState = false;
  digitalWrite(RELAY_PIN, HIGH); // HIGH = Locked
  digitalWrite(GREEN_LED_PIN, HIGH);
  digitalWrite(RED_LED_PIN, LOW);
}

// ================== BUZZER FUNCTIONS ==================

// Startup beep - 3 ascending tones
 
// Startup beep - 3 simple ascending beeps
void playStartupBeep() {
  digitalWrite(BUZZER_PIN, HIGH); // first beep
  delay(150);
  digitalWrite(BUZZER_PIN, LOW);
  delay(200);

  digitalWrite(BUZZER_PIN, HIGH); // second beep
  delay(150);
  digitalWrite(BUZZER_PIN, LOW);
  delay(200);

  digitalWrite(BUZZER_PIN, HIGH); // third beep (longer)
  delay(200);
  digitalWrite(BUZZER_PIN, LOW);
  delay(300);
}

// Success beep - 2 quick high beeps
void playSuccessBeep() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(100);
  digitalWrite(BUZZER_PIN, LOW);
  delay(150);

  digitalWrite(BUZZER_PIN, HIGH);
  delay(100);
  digitalWrite(BUZZER_PIN, LOW);
  delay(150);

  Serial.println("Success beep played");
}

// Error beep - 3 low beeps
void playErrorBeep() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(200);
  digitalWrite(BUZZER_PIN, LOW);
  delay(250);

  digitalWrite(BUZZER_PIN, HIGH);
  delay(200);
  digitalWrite(BUZZER_PIN, LOW);
  delay(250);

  digitalWrite(BUZZER_PIN, HIGH);
  delay(300);
  digitalWrite(BUZZER_PIN, LOW);
  delay(350);

  Serial.println("Error beep played");
}

// Lock confirmation beep - single medium beep
void playLockBeep() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(150);
  digitalWrite(BUZZER_PIN, LOW);
  delay(200);

  Serial.println("Lock beep played");
}


void updateDisplay() {
  unsigned long currentTime = millis();
  
  switch (currentDisplay) {
    case DISPLAY_STARTUP:
      showAnimatedStartup();
      if (currentTime - displayAnimationTime > 3000) {
        currentDisplay = DISPLAY_CONNECTING;
        displayAnimationTime = currentTime;
      }
      break;
      
    case DISPLAY_CONNECTING:
      showConnectingWiFi();
      break;
      
    case DISPLAY_READY:
      showReadyScreen();
      break;
      
    case DISPLAY_ACCESS_GRANTED:
      showAccessGranted(currentEmployee);
      break;
      
    case DISPLAY_ACCESS_DENIED:
      showAccessDenied();
      break;
      
    case DISPLAY_BUTTON_PRESSED:
      showButtonPressed();
      if (currentTime - displayAnimationTime > 500) {
        currentDisplay = relayState ? DISPLAY_MANUAL_UNLOCK : DISPLAY_MANUAL_LOCK;
      }
      break;
      
    case DISPLAY_MANUAL_UNLOCK:
      showManualUnlock();
      break;
      
    case DISPLAY_MANUAL_LOCK:
      showManualLock();
      if (currentTime - displayAnimationTime > 1000) {
        currentDisplay = DISPLAY_READY;
      }
      break;
      
    case DISPLAY_SECURITY_ALERT:
      showSecurityAlert();
      break;
  }
}

void sendToThingSpeak(String event, String details) {
  if (WiFi.status() == WL_CONNECTED) {
    ThingSpeak.setField(1, event);
    ThingSpeak.setField(2, details);
    ThingSpeak.setField(3, wrongAttempts);
    ThingSpeak.setField(4, relayState ? 1 : 0);
    
    int response = ThingSpeak.writeFields(channelID, writeAPIKey);
    
    if (response == 200) {
      Serial.println("Data sent to ThingSpeak successfully");
    } else {
      Serial.println("Error sending data to ThingSpeak: " + String(response));
    }
  }
}

// ================== DISPLAY FUNCTIONS ==================

void showAnimatedStartup() {
  display.clearDisplay();
  
  // Draw border
  display.drawRect(0, 0, 128, 64, SSD1306_WHITE);
  
  // Company logo
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(25, 8);
  display.println("Mehedi");
  
  display.setTextSize(1);
  display.setCursor(15, 28);
  display.println("IoT Door Lock");
  
  // Show restored state
  display.setCursor(10, 38);
  display.print("State: ");
  display.println(relayState ? "UNLOCKED" : "LOCKED");
  
  // Animated loading
  int frame = ((millis() / 300) % 4);
  display.setCursor(40, 50);
  display.print("Loading");
  for (int i = 0; i < frame; i++) {
    display.print(".");
  }
  
  display.display();
}

void showConnectingWiFi() {
  display.clearDisplay();
  
  display.setTextSize(1);
  display.setCursor(25, 10);
  display.println("CONNECTING");
  
  display.setCursor(10, 25);
  display.print("Network: ");
  display.println(ssid);
  
  // Show current state
  display.setCursor(10, 35);
  display.print("Current: ");
  display.println(relayState ? "UNLOCKED" : "LOCKED");
  
  // Animated WiFi bars
  int bars = ((millis() / 500) % 3) + 1;
  for (int i = 0; i < bars; i++) {
    int height = (i + 1) * 4;
    display.fillRect(50 + i * 8, 55 - height, 6, height, SSD1306_WHITE);
  }
  
  display.display();
}

void showReadyScreen() {
  display.clearDisplay();

  // Outer rounded box
  display.drawRoundRect(0, 0, 128, 64, 5, SSD1306_WHITE);

  // Inner small box for header
  display.drawRoundRect(5, 8, 40, 40, 3, SSD1306_WHITE);

  // Header Text
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(9, 14);
  display.println("ESRC");
  display.setCursor(9, 24);
  display.println("IoT");
  display.setCursor(9, 34);
  display.println("Lock");

  // Main instruction (Right side)
  display.setTextSize(1);
  display.setCursor(50, 15);
  display.println("SCAN CARD");
  
  // Show current state
  display.setCursor(50, 25);
  display.print(" ");
  display.println(relayState ? "OPEN" : "LOCKED");

  // ENTRY RESTRICTED warning
  int16_t x, y;
uint16_t w, h;
display.setTextSize(1);
display.getTextBounds("ENTRY RESTRICTED", 0, 0, &x, &y, &w, &h);
display.setCursor((128 - w) / 2, 50); // 50 is the Y position
display.println("ENTRY RESTRICTED");

  display.display();
}
void showAccessGranted(String employeeName) {
  display.clearDisplay();
  
  // Smaller checkmark circle
  display.drawCircle(64, 20, 10, SSD1306_WHITE); // reduced radius from 15 → 10
  display.drawLine(58, 20, 62, 24, SSD1306_WHITE); // adjusted checkmark
  display.drawLine(62, 24, 70, 16, SSD1306_WHITE);
  
  // Pulsing effect (also adjusted radius)
  if ((millis() / 200) % 2) {
    display.drawCircle(64, 20, 12, SSD1306_WHITE);
  }
  
  display.setTextSize(1);
  display.setCursor(26, 38); // moved up
  display.println("ACCESS GRANTED");
  
  // Center the employee name horizontally
  display.setCursor((128 - employeeName.length() * 6) / 2, 48);
  display.println(employeeName);
  
  display.display();
}


void showAccessDenied() {
  display.clearDisplay();
  
  // Large X
  display.drawCircle(64, 25, 20, SSD1306_WHITE);
  display.drawLine(52, 13, 76, 37, SSD1306_WHITE);
  display.drawLine(76, 13, 52, 37, SSD1306_WHITE);
  
  display.setTextSize(1);
  display.setCursor(25, 50);
  display.println("ACCESS DENIED");
  
  display.setCursor(35, 58);
  display.print("Attempts: ");
  display.print(wrongAttempts);
  display.println("/5");
  
  display.display();
}

void showButtonPressed() {
  display.clearDisplay();
  
  display.setTextSize(2);
  display.setCursor(15, 15);
  display.println("BUTTON");
  display.setCursor(20, 35);
  display.println("PRESSED");
  
  // Button animation
  display.fillCircle(64, 55, 6, SSD1306_WHITE);
  
  display.display();
}

void showManualUnlock() {
  display.clearDisplay();

  // ===== Outer box =====
  display.drawRoundRect(0, 0, 128, 64, 5, SSD1306_WHITE);

  // ===== Small inner box for header =====
 

  // ===== Manual Unlock header =====
 

  // ===== Key icon =====
  display.drawRect(55, 25, 12, 6, SSD1306_WHITE);
  display.drawLine(67, 28, 77, 28, SSD1306_WHITE);

  // ===== Main status =====
  display.setTextSize(1);
  display.setCursor(25, 35);  // below key icon
  display.println("UNLOCKED");

  // ===== Auto-lock countdown =====
  display.setTextSize(1);
  display.setCursor(20, 55);
  display.print("Auto-lock: ");
  if (autoLockActive) {
    int remaining = (autoLockTime - millis()) / 1000 + 1;
    display.print(remaining > 0 ? remaining : 0);
    display.println("s");
  } else {
    display.println("OFF");
  }

  display.display();
}


void showManualLock() {
  display.clearDisplay();
  
  display.setTextSize(1);
  display.setCursor(30, 10);
  display.println("MANUAL LOCK");
  
  // Lock icon
  display.drawRect(55, 25, 18, 12, SSD1306_WHITE);
  display.drawRect(60, 20, 8, 8, SSD1306_WHITE);
  
  display.setTextSize(1);
  display.setCursor(30, 45);
  display.println("LOCKED");
  
  display.display();
}

void showSecurityAlert() {
  display.clearDisplay();
  
  display.setTextSize(2);
  display.setCursor(10, 5);
  display.println("SECURITY");
  display.setCursor(25, 25);
  display.println("ALERT!");
  
  display.setTextSize(1);
  display.setCursor(15, 45);
  display.println("5 Wrong Attempts");
  display.setCursor(20, 55);
  display.print("Alarm: ");
  if (alarmActive) {
    int remaining = (alarmEndTime - millis()) / 1000 + 1;
    display.print(remaining > 0 ? remaining : 0);
    display.println("s");
  }
  
  display.display();
}
