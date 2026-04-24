#include <ESP8266WiFi.h>

// --- Configuration ---
const char* ssid     = "SSID HERE";
const char* password = "PASSWORD HERE";
const char* vmixIp   = "192.168.1.72"; // The IP of your vMix PC
const int vmixPort   = 8099;

// Which input number to track
const int targetInputIndex = 24; 

WiFiClient client;

// --- LED Pins (D1 Mini) ---
const int ledOffPin     = D5; // OFF
const int ledLivePin    = D1; // PROGRAM
const int ledPreviewPin = D2; // PREVIEW

void setup() {
  Serial.begin(115200);
  delay(10);

  // Setup LED pins
  pinMode(ledOffPin, OUTPUT);
  pinMode(ledLivePin, OUTPUT);
  pinMode(ledPreviewPin, OUTPUT);

  // Turn all LEDs OFF initially
  digitalWrite(ledOffPin, LOW);
  digitalWrite(ledLivePin, LOW);
  digitalWrite(ledPreviewPin, LOW);

  // Connect to WiFi
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected");
  connectToVmix();
}

void connectToVmix() {
  Serial.print("Connecting to vMix at ");
  Serial.println(vmixIp);

  if (client.connect(vmixIp, vmixPort)) {
    Serial.println("Connected to vMix TCP API!");
    client.print("SUBSCRIBE TALLY\r\n");
  } else {
    Serial.println("Connection failed. Retrying in 5 seconds...");
  }
}

void loop() {
  if (!client.connected()) {
    delay(5000);
    connectToVmix();
    return;
  }

  if (client.available()) {
    String line = client.readStringUntil('\n');
    line.trim();

    if (line.startsWith("TALLY OK")) {
      parseTally(line);
    }
  }
}

void parseTally(String tallyLine) {
  String states = tallyLine.substring(9);
  
  if (targetInputIndex <= states.length()) {
    char stateChar = states.charAt(targetInputIndex - 1);
    
    Serial.print("Source ");
    Serial.print(targetInputIndex);
    Serial.print(" Status: ");

    // Turn all LEDs OFF first
    digitalWrite(ledOffPin, LOW);
    digitalWrite(ledLivePin, LOW);
    digitalWrite(ledPreviewPin, LOW);

    if (stateChar == '0') {
      Serial.println("OFF");
      digitalWrite(ledOffPin, HIGH);
    } 
    else if (stateChar == '1') {
      Serial.println("LIVE (PROGRAM)");
      digitalWrite(ledLivePin, HIGH);
    } 
    else if (stateChar == '2') {
      Serial.println("PREVIEW");
      digitalWrite(ledPreviewPin, HIGH);
    }
  } 
}