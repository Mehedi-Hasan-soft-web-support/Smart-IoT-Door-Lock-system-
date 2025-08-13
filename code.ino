#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <ThingSpeak.h>

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

// Variables
int wrongAttempts = 0;
bool relayState = false;
bool lastButtonState = HIGH;
bool buttonState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;
unsigned long lastThingSpeakUpdate = 0;
unsigned long thingSpeakInterval = 20000; // Send data every 20 seconds

void setup() {
  Serial.begin(115200);
  
  // Initialize pins
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  
  // Initialize all outputs to OFF
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, HIGH); // Green LED on by default
  digitalWrite(RED_LED_PIN, LOW);
  
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
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("RFID Door Lock");
  display.println("System Starting...");
  display.display();
  delay(2000);
  
  // Connect to WiFi
  connectToWiFi();
  
  // Initialize ThingSpeak
  ThingSpeak.begin(client);
  
  // Display ready message
  displayMessage("System Ready", "Scan RFID Card", "or Press Button");
  
  Serial.println("RFID Door Lock System Ready");
}

void loop() {
  // Handle button press
  handleButton();
  
  // Handle RFID scanning
  handleRFID();
  
  // Send data to ThingSpeak periodically
  if (millis() - lastThingSpeakUpdate > thingSpeakInterval) {
    sendToThingSpeak("System Status", relayState ? "Unlocked" : "Locked");
    lastThingSpeakUpdate = millis();
  }
  
  delay(100);
}

void connectToWiFi() {
  WiFi.begin(ssid, password);
  
  displayMessage("Connecting WiFi", ssid, "Please wait...");
  
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  
  Serial.println();
  Serial.println("WiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  
  displayMessage("WiFi Connected", "IP: " + WiFi.localIP().toString(), "System Ready");
  delay(2000);
}

void handleButton() {
  int reading = digitalRead(BUTTON_PIN);
  
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }
  
  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading != buttonState) {
      buttonState = reading;
      
      if (buttonState == LOW) { // Button pressed
        toggleRelay();
        sendToThingSpeak("Manual Override", relayState ? "Unlocked" : "Locked");
        Serial.println("Manual button pressed - Relay toggled");
      }
    }
  }
  
  lastButtonState = reading;
}

void handleRFID() {
  // Look for new cards
  if (!mfrc522.PICC_IsNewCardPresent()) {
    return;
  }
  
  // Select one of the cards
  if (!mfrc522.PICC_ReadCardSerial()) {
    return;
  }
  
  // Read UID
  String cardUID = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    cardUID += String(mfrc522.uid.uidByte[i] < 0x10 ? "0" : "");
    cardUID += String(mfrc522.uid.uidByte[i], HEX);
  }
  cardUID.toUpperCase();
  
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
  
  if (authorized) {
    // Access granted
    wrongAttempts = 0;
    unlockDoor();
    displayMessage("ACCESS GRANTED", employeeName, "Door Unlocked");
    sendToThingSpeak("Access Granted", employeeName);
    
    Serial.println("Access Granted: " + employeeName);
    
    // Keep door unlocked for 5 seconds
    delay(5000);
    lockDoor();
    displayMessage("Door Locked", "System Ready", "Scan RFID Card");
  } else {
    // Access denied
    wrongAttempts++;
    accessDenied();
    displayMessage("ACCESS DENIED", "Unknown Card", "Attempts: " + String(wrongAttempts) + "/5");
    sendToThingSpeak("Access Denied", "Unknown Card - " + cardUID);
    
    Serial.println("Access Denied - Unknown card: " + cardUID);
    Serial.println("Wrong attempts: " + String(wrongAttempts));
    
    // Check if maximum wrong attempts reached
    if (wrongAttempts >= 5) {
      triggerAlarm();
      wrongAttempts = 0; // Reset counter after alarm
    }
    
    delay(3000);
    displayMessage("System Ready", "Scan RFID Card", "or Press Button");
  }
  
  // Halt PICC
  mfrc522.PICC_HaltA();
}

void unlockDoor() {
  relayState = true;
  digitalWrite(RELAY_PIN, HIGH);
  digitalWrite(GREEN_LED_PIN, HIGH);
  digitalWrite(RED_LED_PIN, LOW);
  
  // Success beep
  digitalWrite(BUZZER_PIN, HIGH);
  delay(200);
  digitalWrite(BUZZER_PIN, LOW);
}

void lockDoor() {
  relayState = false;
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, HIGH); // Keep green LED on
  digitalWrite(RED_LED_PIN, LOW);
}

void toggleRelay() {
  if (relayState) {
    lockDoor();
    displayMessage("Manual Lock", "Door Locked", "Button Pressed");
  } else {
    unlockDoor();
    displayMessage("Manual Unlock", "Door Unlocked", "Button Pressed");
    delay(5000); // Keep unlocked for 5 seconds
    lockDoor();
    displayMessage("Door Locked", "System Ready", "Scan RFID Card");
  }
}

void accessDenied() {
  // Turn on red LED for unauthorized access
  digitalWrite(RED_LED_PIN, HIGH);
  digitalWrite(GREEN_LED_PIN, LOW);
  
  // Error beep
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(150);
    digitalWrite(BUZZER_PIN, LOW);
    delay(150);
  }
  
  // Blink red LED
  for (int i = 0; i < 5; i++) {
    digitalWrite(RED_LED_PIN, LOW);
    delay(200);
    digitalWrite(RED_LED_PIN, HIGH);
    delay(200);
  }
  
  // Return to normal state (green LED on, red LED off)
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, HIGH);
}

void triggerAlarm() {
  displayMessage("SECURITY ALERT!", "5 Wrong Attempts", "Alarm Triggered");
  sendToThingSpeak("Security Alert", "5 Wrong Attempts - Alarm Triggered");
  
  Serial.println("SECURITY ALERT: 5 wrong attempts - Triggering alarm!");
  
  // Continuous alarm for 10 seconds
  for (int i = 0; i < 50; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    digitalWrite(RED_LED_PIN, LOW);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(RED_LED_PIN, HIGH);
    delay(100);
  }
  
  displayMessage("Alarm Complete", "System Reset", "Ready for Use");
}

void displayMessage(String line1, String line2, String line3) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  // Line 1
  display.setCursor(0, 0);
  display.println(line1);
  
  // Line 2
  display.setCursor(0, 20);
  display.println(line2);
  
  // Line 3
  display.setCursor(0, 40);
  display.println(line3);
  
  // Status line
  display.setCursor(0, 56);
  display.print("WiFi: ");
  display.print(WiFi.status() == WL_CONNECTED ? "OK" : "NO");
  display.print(" | ");
  display.print(relayState ? "UNLOCKED" : "LOCKED");
  
  display.display();
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
  } else {
    Serial.println("WiFi not connected - Cannot send to ThingSpeak");
  }
}
