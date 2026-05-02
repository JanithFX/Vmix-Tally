#include <ESP8266WiFi.h>
#include <IPAddress.h>

// --- WiFi Configuration ---
const char* ssid     = "YOUR SSID";
const char* password = "YOUR PASSWORD";

// --- Static IP ---
IPAddress local_IP(192, 168, 1, 118);   // Choose a free IP
IPAddress gateway(192, 168, 1, 3);       // Router IP
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(8, 8, 8, 8);

// --- vMix Configuration ---
const char* vmixIp   = "192.168.1.72"; // vMix PC IP
const int vmixPort   = 8099;

// Which input number to track
const int targetInputIndex = 18;

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

  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  // Apply static IP
  if (!WiFi.config(local_IP, gateway, subnet, dns)) {
    Serial.println("STA Failed to configure");
  }

  WiFi.begin(ssid, password);

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected");
  Serial.print("ESP IP Address: ");
  Serial.println(WiFi.localIP());

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