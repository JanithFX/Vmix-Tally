/*
 * ============================================================================
 * Tally Light for ESP32/ESP8266 - vMix Compatible HTTP API
 * ============================================================================
 * 
 * Features:
 * - Polls vMix Tally API to extract hex color from state parameter
 * - Smooth RGB LED color transitions
 * - Web-based configuration (Settings page + Captive Portal)
 * - Persistent storage of Wi-Fi credentials and settings
 * - 3-second reset button hold to clear credentials and enter AP mode
 * - Wi-Fi reconnection with exponential backoff
 * - LED blink indicators for Wi-Fi disconnect and API errors
 * - Serial debug logging
 * - Support for both ESP32 and ESP8266
 * 
 * Hardware:
 * - ESP32 or ESP8266 microcontroller
 * - 3-pin RGB LED (common cathode or common anode, user-configurable)
 * - Momentary push button (for Wi-Fi reset)
 * - Optional: current-limiting resistors for LED
 * 
 * Author: Tally Light Project
 * Date: April 2026
 * Version: 1.0
 * ============================================================================
 */

// ============================================================================
// LIBRARIES & INCLUDES
// ============================================================================

#include <WiFi.h>
#include <Preferences.h>
#include <HttpClient.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>

// ============================================================================
// PREPROCESSOR COMPATIBILITY DEFINITIONS
// ============================================================================

#ifdef ESP32
  #define LED_PWM_FREQ 5000      // 5kHz PWM frequency for ESP32
  #define LED_PWM_RESOLUTION 8   // 8-bit resolution (0-255)
  #define BUTTON_INPUT_MODE INPUT_PULLUP
#endif

#ifdef ESP8266
  #define LED_PWM_FREQ 1000      // 1kHz PWM frequency for ESP8266
  #define LED_PWM_RESOLUTION 10  // 10-bit resolution (0-1023)
  #define BUTTON_INPUT_MODE INPUT_PULLUP
#endif

// ============================================================================
// CONSTANTS - USER CONFIGURABLE (DEFAULTS)
// ============================================================================

// Default GPIO pins (user can change via web interface)
#ifdef ESP32
  const uint8_t DEFAULT_RED_PIN = 25;
  const uint8_t DEFAULT_GREEN_PIN = 26;
  const uint8_t DEFAULT_BLUE_PIN = 27;
  const uint8_t DEFAULT_BUTTON_PIN = 34;
#endif

#ifdef ESP8266
  const uint8_t DEFAULT_RED_PIN = D8;      // GPIO15
  const uint8_t DEFAULT_GREEN_PIN = D7;    // GPIO13
  const uint8_t DEFAULT_BLUE_PIN = D6;     // GPIO12
  const uint8_t DEFAULT_BUTTON_PIN = D0;   // GPIO16
#endif

// Default network settings
const char* DEFAULT_AP_SSID_PREFIX = "Tally-Config-";
const char* DEFAULT_AP_PASSWORD = "tally12345";

// Default API settings
const char* DEFAULT_API_IP = "192.168.1.72";
const uint16_t DEFAULT_API_PORT = 8088;
const char* DEFAULT_API_KEY = "b5b4f18d-adb0-4072-8815-3757fae437a4";
const uint32_t DEFAULT_POLL_INTERVAL = 500;      // milliseconds
const uint32_t DEFAULT_FADE_DURATION = 300;      // milliseconds

// Timeout settings
const uint32_t WIFI_CONNECT_TIMEOUT = 10000;     // 10 seconds
const uint32_t API_REQUEST_TIMEOUT = 5000;       // 5 seconds

// Reset button settings
const uint32_t RESET_BUTTON_HOLD_TIME = 3000;    // 3 seconds
const uint16_t BUTTON_DEBOUNCE_TIME = 20;        // 20 milliseconds
const uint16_t BUTTON_CHECK_INTERVAL = 10;       // 10 milliseconds

// LED blink settings
const uint16_t LED_BLINK_ON_TIME = 100;
const uint16_t LED_BLINK_OFF_TIME = 100;

// LED fade settings
const uint16_t LED_FADE_STEP_TIME = 16;          // ~60 FPS

// ============================================================================
// GLOBAL VARIABLES - STATE MANAGEMENT
// ============================================================================

// Preferences (persistent storage)
Preferences preferences;

// Web server
AsyncWebServer webServer(80);

// GPIO pins (loaded from preferences)
uint8_t redPin = DEFAULT_RED_PIN;
uint8_t greenPin = DEFAULT_GREEN_PIN;
uint8_t bluePin = DEFAULT_BLUE_PIN;
uint8_t buttonPin = DEFAULT_BUTTON_PIN;

// LED state variables
struct {
  uint8_t currentR = 0;
  uint8_t currentG = 0;
  uint8_t currentB = 0;
  uint8_t targetR = 0;
  uint8_t targetG = 0;
  uint8_t targetB = 0;
  uint32_t fadeDuration = DEFAULT_FADE_DURATION;
  uint32_t fadeStartTime = 0;
  bool isFading = false;
} ledState;

// LED cathode type (true = common cathode, false = common anode)
bool isCommonCathode = true;

// API settings
struct {
  char url[128] = "";
  char ip[16] = "";
  uint16_t port = DEFAULT_API_PORT;
  char key[64] = "";
  uint32_t pollInterval = DEFAULT_POLL_INTERVAL;
  uint32_t lastPollTime = 0;
} apiConfig;

// Current API response (hex color)
String lastHexColor = "#000000";
String lastErrorMsg = "";

// Wi-Fi state management
struct {
  bool isConnected = false;
  bool isAPMode = false;
  uint8_t reconnectAttempts = 0;
  uint32_t lastReconnectTime = 0;
  uint32_t reconnectBackoff = 1000;  // Start with 1 second
} wifiState;

// Button state management
struct {
  uint32_t lastDebounceTime = 0;
  uint32_t pressStartTime = 0;
  bool isPressed = false;
  bool holdDetected = false;
} buttonState;

// Blink animation state
struct {
  bool isBlinking = false;
  uint8_t blinkR = 0;
  uint8_t blinkG = 0;
  uint8_t blinkB = 0;
  uint8_t blinkCount = 0;
  uint8_t blinkCountRemaining = 0;
  uint32_t blinkStartTime = 0;
  bool blinkIsOn = true;
} blinkState;

// Timing variables
uint32_t lastWiFiCheckTime = 0;
uint32_t lastButtonCheckTime = 0;
uint32_t lastLEDFadeUpdateTime = 0;
uint32_t lastSerialLogTime = 0;

// Debug flag
const bool ENABLE_SERIAL_DEBUG = true;

// ============================================================================
// SECTION 1: PREFERENCES (PERSISTENT STORAGE) FUNCTIONS
// ============================================================================

void initPreferences() {
  /*
   * Initialize Preferences namespace for persistent storage.
   * Creates "tally" namespace for all configuration data.
   */
  preferences.begin("tally", false);  // false = read-write mode
}

void loadSettings() {
  /*
   * Load all settings from Preferences.
   * If settings don't exist, uses default values.
   */
  // Load LED pins
  redPin = preferences.getUChar("redPin", DEFAULT_RED_PIN);
  greenPin = preferences.getUChar("greenPin", DEFAULT_GREEN_PIN);
  bluePin = preferences.getUChar("bluePin", DEFAULT_BLUE_PIN);
  buttonPin = preferences.getUChar("buttonPin", DEFAULT_BUTTON_PIN);
  
  // Load LED cathode type
  isCommonCathode = preferences.getBool("cathode", true);
  
  // Load API settings
  preferences.getString("apiIP", apiConfig.ip, sizeof(apiConfig.ip));
  if (strlen(apiConfig.ip) == 0) {
    strcpy(apiConfig.ip, DEFAULT_API_IP);
  }
  
  apiConfig.port = preferences.getUShort("apiPort", DEFAULT_API_PORT);
  
  preferences.getString("apiKey", apiConfig.key, sizeof(apiConfig.key));
  if (strlen(apiConfig.key) == 0) {
    strcpy(apiConfig.key, DEFAULT_API_KEY);
  }
  
  apiConfig.pollInterval = preferences.getUInt("pollInt", DEFAULT_POLL_INTERVAL);
  
  // Construct full API URL
  snprintf(apiConfig.url, sizeof(apiConfig.url), 
           "http://%s:%d/tallyupdate/?key=%s",
           apiConfig.ip, apiConfig.port, apiConfig.key);
  
  if (ENABLE_SERIAL_DEBUG) {
    Serial.println("[PREF] Settings loaded:");
    Serial.printf("  LED pins: R=%d, G=%d, B=%d\n", redPin, greenPin, bluePin);
    Serial.printf("  Cathode: %s\n", isCommonCathode ? "common cathode" : "common anode");
    Serial.printf("  API: %s\n", apiConfig.url);
    Serial.printf("  Poll interval: %dms\n", apiConfig.pollInterval);
  }
}

void saveSettings() {
  /*
   * Save all settings to Preferences.
   * Called after user modifies settings via web interface.
   */
  preferences.putUChar("redPin", redPin);
  preferences.putUChar("greenPin", greenPin);
  preferences.putUChar("bluePin", bluePin);
  preferences.putUChar("buttonPin", buttonPin);
  preferences.putBool("cathode", isCommonCathode);
  preferences.putString("apiIP", apiConfig.ip);
  preferences.putUShort("apiPort", apiConfig.port);
  preferences.putString("apiKey", apiConfig.key);
  preferences.putUInt("pollInt", apiConfig.pollInterval);
  
  // Reconstruct URL after saving
  snprintf(apiConfig.url, sizeof(apiConfig.url), 
           "http://%s:%d/tallyupdate/?key=%s",
           apiConfig.ip, apiConfig.port, apiConfig.key);
  
  if (ENABLE_SERIAL_DEBUG) {
    Serial.println("[PREF] Settings saved");
  }
}

void resetSettings() {
  /*
   * Reset all settings to factory defaults.
   * Clears entire "tally" preferences namespace.
   */
  preferences.clear();
  loadSettings();
  
  if (ENABLE_SERIAL_DEBUG) {
    Serial.println("[PREF] Settings reset to defaults");
  }
}

void eraseWiFiCredentials() {
  /*
   * Erase Wi-Fi SSID and password from Preferences.
   * Forces device into AP mode on next boot.
   */
  preferences.remove("ssid");
  preferences.remove("password");
  
  if (ENABLE_SERIAL_DEBUG) {
    Serial.println("[PREF] Wi-Fi credentials erased");
  }
}

void saveWiFiCredentials(const char* ssid, const char* password) {
  /*
   * Save Wi-Fi SSID and password to Preferences.
   * Called after successful connection from AP mode.
   */
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  
  if (ENABLE_SERIAL_DEBUG) {
    Serial.printf("[PREF] Wi-Fi credentials saved: %s\n", ssid);
  }
}

bool hasWiFiCredentials() {
  /*
   * Check if Wi-Fi credentials exist in Preferences.
   * Returns true if SSID is stored, false otherwise.
   */
  return preferences.isKey("ssid");
}

void getWiFiCredentials(char* ssid, char* password, size_t ssidLen, size_t passLen) {
  /*
   * Retrieve Wi-Fi SSID and password from Preferences.
   * Assumes credentials exist (check with hasWiFiCredentials() first).
   */
  preferences.getString("ssid", ssid, ssidLen);
  preferences.getString("password", password, passLen);
}

// ============================================================================
// SECTION 2: LED CONTROL FUNCTIONS
// ============================================================================

void initLED() {
  /*
   * Initialize LED pins as PWM output.
   * Configures GPIO pins for RGB LED control.
   */
  pinMode(redPin, OUTPUT);
  pinMode(greenPin, OUTPUT);
  pinMode(bluePin, OUTPUT);
  
  // Set initial PWM values
  digitalWrite(redPin, 0);
  digitalWrite(greenPin, 0);
  digitalWrite(bluePin, 0);
  
  if (ENABLE_SERIAL_DEBUG) {
    Serial.printf("[LED] Initialized: R=%d, G=%d, B=%d\n", redPin, greenPin, bluePin);
  }
}

void setLEDColor(uint8_t r, uint8_t g, uint8_t b) {
  /*
   * Immediately set LED to specified RGB color.
   * Handles common cathode vs common anode inversion.
   * 
   * For common cathode: HIGH = LED on
   * For common anode: LOW = LED on (values inverted)
   */
  
  // Apply cathode type inversion if needed
  uint8_t outR = r;
  uint8_t outG = g;
  uint8_t outB = b;
  
  if (!isCommonCathode) {
    // Common anode: invert values (255 - value)
    outR = 255 - r;
    outG = 255 - g;
    outB = 255 - b;
  }
  
  // Write PWM values to pins
  analogWrite(redPin, outR);
  analogWrite(greenPin, outG);
  analogWrite(bluePin, outB);
  
  // Update current LED state
  ledState.currentR = r;
  ledState.currentG = g;
  ledState.currentB = b;
}

void fadeLED(uint8_t targetR, uint8_t targetG, uint8_t targetB, uint32_t duration) {
  /*
   * Smoothly fade LED from current color to target color over specified duration.
   * Uses non-blocking interpolation: call this function to start fade, then
   * call updateLEDFade() in main loop to animate.
   * 
   * Parameters:
   *   targetR, targetG, targetB: RGB target values (0-255)
   *   duration: fade time in milliseconds
   */
  
  ledState.targetR = targetR;
  ledState.targetG = targetG;
  ledState.targetB = targetB;
  ledState.fadeDuration = duration;
  ledState.fadeStartTime = millis();
  ledState.isFading = true;
  
  if (ENABLE_SERIAL_DEBUG) {
    Serial.printf("[LED] Fade started: (%d,%d,%d) -> (%d,%d,%d) in %dms\n",
                  ledState.currentR, ledState.currentG, ledState.currentB,
                  targetR, targetG, targetB, duration);
  }
}

void updateLEDFade() {
  /*
   * Update LED fade animation.
   * Called frequently from main loop (~16ms intervals) to smoothly interpolate.
   * Uses linear interpolation between current and target RGB values.
   */
  
  if (!ledState.isFading) {
    return;  // No active fade
  }
  
  uint32_t elapsed = millis() - ledState.fadeStartTime;
  
  if (elapsed >= ledState.fadeDuration) {
    // Fade complete
    setLEDColor(ledState.targetR, ledState.targetG, ledState.targetB);
    ledState.isFading = false;
    return;
  }
  
  // Calculate interpolation factor (0.0 to 1.0)
  float progress = (float)elapsed / (float)ledState.fadeDuration;
  
  // Linear interpolation for each channel
  uint8_t interpR = ledState.currentR + (ledState.targetR - ledState.currentR) * progress;
  uint8_t interpG = ledState.currentG + (ledState.targetG - ledState.currentG) * progress;
  uint8_t interpB = ledState.currentB + (ledState.targetB - ledState.currentB) * progress;
  
  setLEDColor(interpR, interpG, interpB);
}

void blinkLED(uint8_t r, uint8_t g, uint8_t b, uint8_t count) {
  /*
   * Blink LED with specified color.
   * Non-blocking: starts blink animation, call updateLEDBlink() in loop to animate.
   * 
   * Parameters:
   *   r, g, b: RGB color for blink
   *   count: number of blinks
   */
  
  blinkState.isBlinking = true;
  blinkState.blinkR = r;
  blinkState.blinkG = g;
  blinkState.blinkB = b;
  blinkState.blinkCountRemaining = count * 2;  // on + off cycles
  blinkState.blinkStartTime = millis();
  blinkState.blinkIsOn = true;
  
  // Set initial LED color
  setLEDColor(r, g, b);
  
  if (ENABLE_SERIAL_DEBUG) {
    Serial.printf("[LED] Blink: RGB(%d,%d,%d) x%d\n", r, g, b, count);
  }
}

void updateLEDBlink() {
  /*
   * Update LED blink animation.
   * Called frequently from main loop to toggle LED on/off.
   */
  
  if (!blinkState.isBlinking) {
    return;  // No active blink
  }
  
  uint32_t elapsed = millis() - blinkState.blinkStartTime;
  uint32_t cycleTime = LED_BLINK_ON_TIME + LED_BLINK_OFF_TIME;
  uint32_t timeInCycle = elapsed % cycleTime;
  
  // Determine if LED should be on or off in current cycle
  bool shouldBeOn = (timeInCycle < LED_BLINK_ON_TIME);
  
  if (shouldBeOn && !blinkState.blinkIsOn) {
    // Transition to on
    setLEDColor(blinkState.blinkR, blinkState.blinkG, blinkState.blinkB);
    blinkState.blinkIsOn = true;
  } else if (!shouldBeOn && blinkState.blinkIsOn) {
    // Transition to off
    setLEDColor(0, 0, 0);
    blinkState.blinkIsOn = false;
    blinkState.blinkCountRemaining--;
    
    if (blinkState.blinkCountRemaining <= 0) {
      blinkState.isBlinking = false;
      if (ENABLE_SERIAL_DEBUG) {
        Serial.println("[LED] Blink complete");
      }
    }
  }
}

// ============================================================================
// SECTION 3: BUTTON HANDLING
// ============================================================================

void initButton() {
  /*
   * Initialize button pin as digital input with pull-up.
   * Button is active-low (pressed = LOW).
   */
  pinMode(buttonPin, BUTTON_INPUT_MODE);
  
  if (ENABLE_SERIAL_DEBUG) {
    Serial.printf("[BTN] Button initialized on pin %d\n", buttonPin);
  }
}

bool debounceButton() {
  /*
   * Read button with debounce filtering.
   * Returns true if button is currently pressed (after debounce).
   */
  
  uint32_t now = millis();
  
  // Skip if not enough time has passed
  if (now - buttonState.lastDebounceTime < BUTTON_DEBOUNCE_TIME) {
    return buttonState.isPressed;
  }
  
  bool buttonRead = digitalRead(buttonPin) == LOW;  // Active-low
  
  if (buttonRead != buttonState.isPressed) {
    buttonState.isPressed = buttonRead;
    buttonState.lastDebounceTime = now;
    
    if (buttonRead) {
      buttonState.pressStartTime = now;  // Record press start time
    }
  }
  
  return buttonState.isPressed;
}

void handleResetButton() {
  /*
   * Detect 3-second button hold for reset operation.
   * When held for 3+ seconds:
   *   1. Erase Wi-Fi credentials
   *   2. Reset settings
   *   3. Reboot into AP mode
   */
  
  uint32_t now = millis();
  
  // Skip if not time to check button
  if (now - lastButtonCheckTime < BUTTON_CHECK_INTERVAL) {
    return;
  }
  lastButtonCheckTime = now;
  
  bool isPressed = debounceButton();
  
  if (isPressed) {
    uint32_t holdDuration = now - buttonState.pressStartTime;
    
    // Detect 3-second hold
    if (!buttonState.holdDetected && holdDuration >= RESET_BUTTON_HOLD_TIME) {
      buttonState.holdDetected = true;
      
      if (ENABLE_SERIAL_DEBUG) {
        Serial.println("[BTN] Reset button held for 3 seconds - resetting device");
      }
      
      // Visual feedback: blink red 3 times
      blinkLED(255, 0, 0, 3);
      
      // Erase credentials and settings
      eraseWiFiCredentials();
      resetSettings();
      
      // Wait for blink to complete, then reboot
      delay(1000);
      ESP.restart();
    }
  } else {
    // Button released
    buttonState.holdDetected = false;
  }
}

// ============================================================================
// SECTION 4: WI-FI CONNECTION & RECONNECTION
// ============================================================================

void connectToWiFi(const char* ssid, const char* password) {
  /*
   * Connect to Wi-Fi network with specified SSID and password.
   * Uses timeout to avoid blocking indefinitely.
   * 
   * Parameters:
   *   ssid: Wi-Fi network name
   *   password: Wi-Fi password
   */
  
  if (ENABLE_SERIAL_DEBUG) {
    Serial.printf("[WiFi] Connecting to %s...\n", ssid);
  }
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  uint32_t startTime = millis();
  int attempts = 0;
  
  // Wait for connection with timeout
  while (WiFi.status() != WL_CONNECTED && 
         millis() - startTime < WIFI_CONNECT_TIMEOUT) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    if (ENABLE_SERIAL_DEBUG) {
      Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    }
    wifiState.isConnected = true;
    wifiState.reconnectAttempts = 0;
    wifiState.reconnectBackoff = 1000;  // Reset backoff
    
    // Save credentials for next boot
    saveWiFiCredentials(ssid, password);
  } else {
    if (ENABLE_SERIAL_DEBUG) {
      Serial.printf("\n[WiFi] Connection failed after %dms\n", 
                    millis() - startTime);
    }
    wifiState.isConnected = false;
    wifiState.reconnectAttempts++;
  }
}

void checkWiFiConnection() {
  /*
   * Check Wi-Fi connection status and attempt reconnection if dropped.
   * Uses exponential backoff to avoid overwhelming the AP with requests.
   * Called periodically from main loop.
   */
  
  uint32_t now = millis();
  
  // Skip if not time to check
  if (now - lastWiFiCheckTime < 1000) {
    return;
  }
  lastWiFiCheckTime = now;
  
  if (WiFi.status() == WL_CONNECTED) {
    // Connection is good
    if (!wifiState.isConnected) {
      wifiState.isConnected = true;
      if (ENABLE_SERIAL_DEBUG) {
        Serial.printf("[WiFi] Reconnected! IP: %s\n", WiFi.localIP().toString().c_str());
      }
    }
  } else {
    // Connection lost
    if (wifiState.isConnected) {
      wifiState.isConnected = false;
      if (ENABLE_SERIAL_DEBUG) {
        Serial.println("[WiFi] Connection lost");
      }
      
      // Visual feedback: blue blink
      blinkLED(0, 0, 255, 1);
    }
    
    // Attempt reconnection with exponential backoff
    if (now - wifiState.lastReconnectTime >= wifiState.reconnectBackoff) {
      if (ENABLE_SERIAL_DEBUG) {
        Serial.printf("[WiFi] Reconnection attempt #%d\n", wifiState.reconnectAttempts + 1);
      }
      
      WiFi.reconnect();
      wifiState.lastReconnectTime = now;
      
      // Exponential backoff: 1s, 2s, 4s, 8s, max 30s
      wifiState.reconnectBackoff = min(wifiState.reconnectBackoff * 2, 30000UL);
    }
  }
}

// ============================================================================
// SECTION 5: AP MODE (CAPTIVE PORTAL)
// ============================================================================

void startAPMode() {
  /*
   * Start ESP in Access Point (AP) mode to serve captive portal.
   * User can connect via Wi-Fi and configure SSID/password/API settings.
   */
  
  // Generate unique SSID with last 4 chars of MAC address
  String macAddr = WiFi.macAddress();
  String lastFour = macAddr.substring(macAddr.length() - 5);
  lastFour.replace(":", "");
  String apSSID = String(DEFAULT_AP_SSID_PREFIX) + lastFour;
  
  if (ENABLE_SERIAL_DEBUG) {
    Serial.printf("[AP] Starting AP mode: %s\n", apSSID.c_str());
  }
  
  // Start AP
  WiFi.softAP(apSSID.c_str(), DEFAULT_AP_PASSWORD);
  WiFi.softAPIP();
  
  if (ENABLE_SERIAL_DEBUG) {
    Serial.printf("[AP] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  }
  
  wifiState.isAPMode = true;
  
  // Set LED to indicate AP mode: cyan (0, 255, 255)
  fadeLED(0, 255, 255, 500);
}

String generateAPHTML() {
  /*
   * Generate HTML for captive portal (AP mode).
   * Allows user to enter Wi-Fi SSID, password, and configure basic API settings.
   */
  
  return String(R"(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Tally Light - Configuration</title>
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
        body {
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
            padding: 20px;
        }
        .container {
            background: white;
            border-radius: 12px;
            box-shadow: 0 10px 40px rgba(0, 0, 0, 0.1);
            max-width: 500px;
            width: 100%;
            padding: 40px;
        }
        h1 {
            color: #333;
            margin-bottom: 10px;
            font-size: 28px;
        }
        .subtitle {
            color: #888;
            margin-bottom: 30px;
            font-size: 14px;
        }
        .form-group {
            margin-bottom: 20px;
        }
        label {
            display: block;
            color: #333;
            font-weight: 600;
            margin-bottom: 8px;
            font-size: 14px;
        }
        input[type="text"],
        input[type="password"],
        input[type="number"] {
            width: 100%;
            padding: 12px;
            border: 2px solid #e0e0e0;
            border-radius: 6px;
            font-size: 14px;
            transition: border-color 0.3s;
        }
        input[type="text"]:focus,
        input[type="password"]:focus,
        input[type="number"]:focus {
            outline: none;
            border-color: #667eea;
        }
        button {
            width: 100%;
            padding: 12px;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            border: none;
            border-radius: 6px;
            font-size: 16px;
            font-weight: 600;
            cursor: pointer;
            transition: transform 0.2s, box-shadow 0.2s;
        }
        button:hover {
            transform: translateY(-2px);
            box-shadow: 0 5px 20px rgba(102, 126, 234, 0.4);
        }
        button:active {
            transform: translateY(0);
        }
        .info {
            background: #f0f7ff;
            border-left: 4px solid #667eea;
            padding: 12px;
            margin-top: 20px;
            border-radius: 4px;
            font-size: 13px;
            color: #333;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>Tally Light</h1>
        <p class="subtitle">Configure Wi-Fi & API Settings</p>
        
        <form action="/ap-config" method="POST">
            <div class="form-group">
                <label for="ssid">Wi-Fi Network (SSID)</label>
                <input type="text" id="ssid" name="ssid" placeholder="Enter your Wi-Fi network name" required>
            </div>
            
            <div class="form-group">
                <label for="password">Wi-Fi Password</label>
                <input type="password" id="password" name="password" placeholder="Enter your Wi-Fi password" required>
            </div>
            
            <div class="form-group">
                <label for="apiIP">API Server IP Address</label>
                <input type="text" id="apiIP" name="apiIP" placeholder="192.168.1.72">
            </div>
            
            <div class="form-group">
                <label for="apiPort">API Server Port</label>
                <input type="number" id="apiPort" name="apiPort" placeholder="8088" min="1" max="65535">
            </div>
            
            <div class="form-group">
                <label for="apiKey">API Key</label>
                <input type="text" id="apiKey" name="apiKey" placeholder="Enter your vMix API key">
            </div>
            
            <button type="submit">Connect & Save</button>
            
            <div class="info">
                ℹ️ After clicking "Connect & Save", the device will attempt to connect to your Wi-Fi network. 
                You can then access the Settings page to configure LED pins and colors.
            </div>
        </form>
    </div>
</body>
</html>
  )");
}

void setupAPModeServer() {
  /*
   * Setup web server for AP mode.
   * Serves captive portal and handles form submission.
   */
  
  // Catch-all handler for captive portal (redirect all requests to /ap)
  webServer.onNotFound([](AsyncWebServerRequest *request) {
    request->send(200, "text/html", generateAPHTML());
  });
  
  // Handle AP configuration form submission
  webServer.on("/ap-config", HTTP_POST, [](AsyncWebServerRequest *request) {
    // Extract form parameters
    String ssid = request->arg("ssid");
    String password = request->arg("password");
    String apiIP = request->arg("apiIP");
    String apiPort = request->arg("apiPort");
    String apiKey = request->arg("apiKey");
    
    // Update API settings if provided
    if (apiIP.length() > 0) {
      strncpy(apiConfig.ip, apiIP.c_str(), sizeof(apiConfig.ip) - 1);
    }
    if (apiPort.length() > 0) {
      apiConfig.port = atoi(apiPort.c_str());
    }
    if (apiKey.length() > 0) {
      strncpy(apiConfig.key, apiKey.c_str(), sizeof(apiConfig.key) - 1);
    }
    
    // Save settings
    saveSettings();
    
    if (ENABLE_SERIAL_DEBUG) {
      Serial.printf("[AP] Configuration submitted: SSID=%s\n", ssid.c_str());
    }
    
    // Send response
    String response = R"(
    <!DOCTYPE html>
    <html>
    <head>
        <meta charset="UTF-8">
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <title>Connecting...</title>
        <style>
            body {
                font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
                display: flex;
                justify-content: center;
                align-items: center;
                min-height: 100vh;
                margin: 0;
                background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            }
            .container {
                text-align: center;
                background: white;
                padding: 40px;
                border-radius: 12px;
                box-shadow: 0 10px 40px rgba(0, 0, 0, 0.1);
                max-width: 400px;
            }
            h1 {
                color: #333;
                margin-bottom: 20px;
            }
            p {
                color: #666;
                margin-bottom: 30px;
                line-height: 1.6;
            }
            .spinner {
                border: 4px solid #f3f3f3;
                border-top: 4px solid #667eea;
                border-radius: 50%;
                width: 40px;
                height: 40px;
                animation: spin 1s linear infinite;
                margin: 0 auto 20px;
            }
            @keyframes spin {
                0% { transform: rotate(0deg); }
                100% { transform: rotate(360deg); }
            }
        </style>
    </head>
    <body>
        <div class="container">
            <h1>Connecting...</h1>
            <div class="spinner"></div>
            <p>Device is attempting to connect to your Wi-Fi network.<br>
            Please wait approximately 10 seconds...</p>
            <p><small>After connection, the LED will stabilize and you can access the Settings page.</small></p>
        </div>
    </body>
    </html>
    )";
    
    request->send(200, "text/html", response);
    
    // Disconnect from AP mode and connect to provided Wi-Fi
    delay(1000);
    WiFi.softAPdisconnect(true);  // Turn off AP mode
    wifiState.isAPMode = false;
    connectToWiFi(ssid.c_str(), password.c_str());
  });
  
  if (ENABLE_SERIAL_DEBUG) {
    Serial.println("[AP] AP mode server setup complete");
  }
}

// ============================================================================
// SECTION 6: STA MODE (SETTINGS WEB INTERFACE)
// ============================================================================

String generateSettingsHTML() {
  /*
   * Generate HTML for Settings page (STA mode).
   * Allows user to configure LED pins, cathode type, API endpoint, polling interval.
   */
  
  String html = String(R"(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Tally Light - Settings</title>
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
        body {
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
            background: #f5f7fa;
            min-height: 100vh;
            padding: 20px;
        }
        .container {
            max-width: 600px;
            margin: 0 auto;
        }
        header {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 30px 20px;
            border-radius: 12px;
            margin-bottom: 30px;
            box-shadow: 0 5px 20px rgba(0, 0, 0, 0.1);
        }
        header h1 {
            font-size: 28px;
            margin-bottom: 5px;
        }
        header p {
            font-size: 14px;
            opacity: 0.9;
        }
        .section {
            background: white;
            border-radius: 12px;
            padding: 25px;
            margin-bottom: 20px;
            box-shadow: 0 2px 10px rgba(0, 0, 0, 0.05);
        }
        .section-title {
            font-size: 18px;
            font-weight: 600;
            color: #333;
            margin-bottom: 20px;
            padding-bottom: 10px;
            border-bottom: 2px solid #f0f0f0;
        }
        .form-group {
            margin-bottom: 20px;
        }
        .form-group:last-child {
            margin-bottom: 0;
        }
        label {
            display: block;
            color: #333;
            font-weight: 600;
            margin-bottom: 8px;
            font-size: 14px;
        }
        input[type="text"],
        input[type="number"],
        select {
            width: 100%;
            padding: 10px;
            border: 2px solid #e0e0e0;
            border-radius: 6px;
            font-size: 14px;
            transition: border-color 0.3s;
        }
        input[type="text"]:focus,
        input[type="number"]:focus,
        select:focus {
            outline: none;
            border-color: #667eea;
        }
        .pin-selector {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 15px;
            margin-bottom: 15px;
        }
        .pin-selector-item {
            display: flex;
            flex-direction: column;
        }
        .pin-selector-item label {
            margin-bottom: 5px;
            font-size: 13px;
        }
        .pin-selector-item input {
            font-size: 13px;
            padding: 8px;
        }
        .status-display {
            background: #f0f7ff;
            border-left: 4px solid #667eea;
            padding: 15px;
            border-radius: 6px;
            margin-top: 20px;
        }
        .status-row {
            display: flex;
            justify-content: space-between;
            margin-bottom: 10px;
            font-size: 13px;
        }
        .status-row:last-child {
            margin-bottom: 0;
        }
        .status-label {
            font-weight: 600;
            color: #333;
        }
        .status-value {
            color: #666;
            font-family: monospace;
        }
        .led-preview {
            width: 50px;
            height: 50px;
            border-radius: 50%;
            margin-top: 10px;
            box-shadow: 0 2px 8px rgba(0, 0, 0, 0.2);
            border: 2px solid #ddd;
        }
        .button-group {
            display: flex;
            gap: 10px;
            margin-top: 25px;
        }
        button {
            flex: 1;
            padding: 12px;
            border: none;
            border-radius: 6px;
            font-size: 14px;
            font-weight: 600;
            cursor: pointer;
            transition: all 0.2s;
        }
        .btn-save {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
        }
        .btn-save:hover {
            transform: translateY(-2px);
            box-shadow: 0 5px 20px rgba(102, 126, 234, 0.4);
        }
        .btn-test {
            background: #f0f0f0;
            color: #333;
            border: 2px solid #ddd;
        }
        .btn-test:hover {
            background: #e8e8e8;
        }
        .info {
            background: #fff3cd;
            border-left: 4px solid #ffc107;
            padding: 12px;
            margin-top: 20px;
            border-radius: 4px;
            font-size: 13px;
            color: #333;
        }
        #statusMessage {
            padding: 12px;
            border-radius: 6px;
            margin-bottom: 20px;
            display: none;
            font-size: 14px;
            font-weight: 600;
        }
        #statusMessage.success {
            background: #d4edda;
            color: #155724;
            border: 1px solid #c3e6cb;
            display: block;
        }
        #statusMessage.error {
            background: #f8d7da;
            color: #721c24;
            border: 1px solid #f5c6cb;
            display: block;
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1>Tally Light</h1>
            <p>Settings & Configuration</p>
        </header>
        
        <div id="statusMessage"></div>
        
        <form id="settingsForm">
            <!-- LED Configuration -->
            <div class="section">
                <div class="section-title">LED Configuration</div>
                
                <div class="form-group">
                    <label>LED Pins (GPIO)</label>
                    <div class="pin-selector">
                        <div class="pin-selector-item">
                            <label>Red (R)</label>
                            <input type="number" id="redPin" name="redPin" min="0" max="40">
                        </div>
                        <div class="pin-selector-item">
                            <label>Green (G)</label>
                            <input type="number" id="greenPin" name="greenPin" min="0" max="40">
                        </div>
                        <div class="pin-selector-item">
                            <label>Blue (B)</label>
                            <input type="number" id="bluePin" name="bluePin" min="0" max="40">
                        </div>
                    </div>
                </div>
                
                <div class="form-group">
                    <label for="cathode">LED Type</label>
                    <select id="cathode" name="cathode">
                        <option value="true">Common Cathode (GND shared)</option>
                        <option value="false">Common Anode (VCC shared)</option>
                    </select>
                </div>
            </div>
            
            <!-- API Configuration -->
            <div class="section">
                <div class="section-title">API Configuration</div>
                
                <div class="form-group">
                    <label for="apiIP">vMix Server IP Address</label>
                    <input type="text" id="apiIP" name="apiIP" placeholder="192.168.1.72">
                </div>
                
                <div class="form-group">
                    <label for="apiPort">API Port</label>
                    <input type="number" id="apiPort" name="apiPort" min="1" max="65535">
                </div>
                
                <div class="form-group">
                    <label for="apiKey">API Key</label>
                    <input type="text" id="apiKey" name="apiKey" placeholder="b5b4f18d-adb0-4072-8815-3757fae437a4">
                </div>
                
                <div class="form-group">
                    <label for="pollInterval">Poll Interval (ms)</label>
                    <input type="number" id="pollInterval" name="pollInterval" min="100" max="5000" step="100">
                    <small style="color: #999; font-size: 12px; margin-top: 5px; display: block;">Recommended: 500ms</small>
                </div>
            </div>
            
            <!-- Status Display -->
            <div class="section">
                <div class="section-title">Current Status</div>
                
                <div class="status-display">
                    <div class="status-row">
                        <span class="status-label">Last API Response (Hex):</span>
                        <span class="status-value" id="hexColor">-</span>
                    </div>
                    <div class="status-row">
                        <span class="status-label">RGB Values:</span>
                        <span class="status-value" id="rgbValues">-</span>
                    </div>
                    <div class="status-row">
                        <span class="status-label">Wi-Fi Status:</span>
                        <span class="status-value" id="wifiStatus">-</span>
                    </div>
                    <div class="status-row">
                        <span class="status-label">LED Preview:</span>
                    </div>
                    <div class="led-preview" id="ledPreview"></div>
                </div>
            </div>
            
            <!-- Buttons -->
            <div class="button-group">
                <button type="submit" class="btn-save">Save Settings</button>
                <button type="button" class="btn-test" onclick="testLED()">Test LED</button>
            </div>
            
            <div class="info">
                ℹ️ Hold the reset button for 3 seconds to clear Wi-Fi credentials and return to setup mode.
            </div>
        </form>
    </div>
    
    <script>
        // Auto-refresh status every 1 second
        async function updateStatus() {
            try {
                const response = await fetch('/api/status');
                const data = await response.json();
                
                document.getElementById('hexColor').textContent = data.hexColor || '-';
                document.getElementById('rgbValues').textContent = data.rgbValues || '-';
                document.getElementById('wifiStatus').textContent = data.wifiStatus || '-';
                
                // Update LED preview
                if (data.hexColor && data.hexColor !== '-') {
                    document.getElementById('ledPreview').style.backgroundColor = data.hexColor;
                } else {
                    document.getElementById('ledPreview').style.backgroundColor = '#f0f0f0';
                }
            } catch (error) {
                console.error('Status update failed:', error);
            }
        }
        
        // Load settings on page load
        async function loadSettings() {
            try {
                const response = await fetch('/api/settings');
                const data = await response.json();
                
                document.getElementById('redPin').value = data.redPin || '')");
                html += String(DEFAULT_RED_PIN);
                html += String(R"(';
                document.getElementById('greenPin').value = data.greenPin || ')");
                html += String(DEFAULT_GREEN_PIN);
                html += String(R"(';
                document.getElementById('bluePin').value = data.bluePin || ')");
                html += String(DEFAULT_BLUE_PIN);
                html += String(R"(';
                document.getElementById('cathode').value = data.cathode ? 'true' : 'false';
                document.getElementById('apiIP').value = data.apiIP || ')");
                html += String(DEFAULT_API_IP);
                html += String(R"(';
                document.getElementById('apiPort').value = data.apiPort || ')");
                html += String(DEFAULT_API_PORT);
                html += String(R"(';
                document.getElementById('apiKey').value = data.apiKey || '')");
                html += String(DEFAULT_API_KEY);
                html += String(R"(';
                document.getElementById('pollInterval').value = data.pollInterval || ')");
                html += String(DEFAULT_POLL_INTERVAL);
                html += String(R"(';
            } catch (error) {
                console.error('Failed to load settings:', error);
            }
        }
        
        // Handle form submission
        document.getElementById('settingsForm').addEventListener('submit', async (e) => {
            e.preventDefault();
            
            const data = {
                redPin: parseInt(document.getElementById('redPin').value),
                greenPin: parseInt(document.getElementById('greenPin').value),
                bluePin: parseInt(document.getElementById('bluePin').value),
                cathode: document.getElementById('cathode').value === 'true',
                apiIP: document.getElementById('apiIP').value,
                apiPort: parseInt(document.getElementById('apiPort').value),
                apiKey: document.getElementById('apiKey').value,
                pollInterval: parseInt(document.getElementById('pollInterval').value)
            };
            
            try {
                const response = await fetch('/api/settings', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(data)
                });
                
                const result = await response.json();
                
                const msgEl = document.getElementById('statusMessage');
                if (result.success) {
                    msgEl.textContent = 'Settings saved successfully!';
                    msgEl.className = 'success';
                } else {
                    msgEl.textContent = 'Error saving settings: ' + (result.error || 'Unknown error');
                    msgEl.className = 'error';
                }
                
                setTimeout(() => {
                    msgEl.className = '';
                    msgEl.textContent = '';
                }, 3000);
            } catch (error) {
                const msgEl = document.getElementById('statusMessage');
                msgEl.textContent = 'Error: ' + error.message;
                msgEl.className = 'error';
            }
        });
        
        // Test LED
        async function testLED() {
            try {
                await fetch('/api/test-led', { method: 'POST' });
            } catch (error) {
                console.error('Test LED failed:', error);
            }
        }
        
        // Initialize
        loadSettings();
        updateStatus();
        setInterval(updateStatus, 1000);
    </script>
</body>
</html>
  )");
  
  return html;
}

void setupSTAModeServer() {
  /*
   * Setup web server for STA mode (connected to main Wi-Fi).
   * Serves Settings page and API endpoints for configuration and status.
   */
  
  // Settings page
  webServer.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", generateSettingsHTML());
  });
  
  // API: Get current settings
  webServer.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{\"success\":true,\"redPin\":";
    json += redPin;
    json += ",\"greenPin\":";
    json += greenPin;
    json += ",\"bluePin\":";
    json += bluePin;
    json += ",\"cathode\":";
    json += (isCommonCathode ? "true" : "false");
    json += ",\"apiIP\":\"";
    json += apiConfig.ip;
    json += "\",\"apiPort\":";
    json += apiConfig.port;
    json += ",\"apiKey\":\"";
    json += apiConfig.key;
    json += "\",\"pollInterval\":";
    json += apiConfig.pollInterval;
    json += "}";
    
    request->send(200, "application/json", json);
  });
  
  // API: Save settings
  webServer.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("body", true)) {
      String body = request->getParam("body", true)->value();
      
      // Simple JSON parsing (without ArduinoJson to keep code simpler)
      // In production, use ArduinoJson library
      
      // Extract redPin
      int idx = body.indexOf("\"redPin\":");
      if (idx >= 0) {
        redPin = atoi(body.c_str() + idx + 9);
      }
      
      // Extract greenPin
      idx = body.indexOf("\"greenPin\":");
      if (idx >= 0) {
        greenPin = atoi(body.c_str() + idx + 11);
      }
      
      // Extract bluePin
      idx = body.indexOf("\"bluePin\":");
      if (idx >= 0) {
        bluePin = atoi(body.c_str() + idx + 10);
      }
      
      // Extract cathode
      idx = body.indexOf("\"cathode\":");
      if (idx >= 0) {
        isCommonCathode = (body.indexOf("true", idx) != -1);
      }
      
      // Extract apiIP
      idx = body.indexOf("\"apiIP\":\"");
      if (idx >= 0) {
        int end = body.indexOf("\"", idx + 9);
        strncpy(apiConfig.ip, body.c_str() + idx + 9, end - idx - 9);
        apiConfig.ip[end - idx - 9] = '\0';
      }
      
      // Extract apiPort
      idx = body.indexOf("\"apiPort\":");
      if (idx >= 0) {
        apiConfig.port = atoi(body.c_str() + idx + 10);
      }
      
      // Extract apiKey
      idx = body.indexOf("\"apiKey\":\"");
      if (idx >= 0) {
        int end = body.indexOf("\"", idx + 10);
        strncpy(apiConfig.key, body.c_str() + idx + 10, end - idx - 10);
        apiConfig.key[end - idx - 10] = '\0';
      }
      
      // Extract pollInterval
      idx = body.indexOf("\"pollInterval\":");
      if (idx >= 0) {
        apiConfig.pollInterval = atoi(body.c_str() + idx + 15);
      }
      
      // Reinitialize LED with new pins
      initLED();
      
      // Save to preferences
      saveSettings();
      
      if (ENABLE_SERIAL_DEBUG) {
        Serial.println("[WEB] Settings updated via API");
      }
      
      request->send(200, "application/json", "{\"success\":true}");
    } else {
      request->send(400, "application/json", "{\"success\":false,\"error\":\"No body\"}");
    }
  });
  
  // API: Get current status
  webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    String rgbStr = "(";
    rgbStr += ledState.currentR;
    rgbStr += ",";
    rgbStr += ledState.currentG;
    rgbStr += ",";
    rgbStr += ledState.currentB;
    rgbStr += ")";
    
    String json = "{\"hexColor\":\"";
    json += lastHexColor;
    json += "\",\"rgbValues\":\"";
    json += rgbStr;
    json += "\",\"wifiStatus\":\"";
    json += (wifiState.isConnected ? "Connected" : "Disconnected");
    json += "\",\"lastError\":\"";
    json += lastErrorMsg;
    json += "\"}";
    
    request->send(200, "application/json", json);
  });
  
  // API: Test LED
  webServer.on("/api/test-led", HTTP_POST, [](AsyncWebServerRequest *request) {
    // Flash LED: fade to yellow, then back to current color
    fadeLED(255, 255, 0, 200);
    
    if (ENABLE_SERIAL_DEBUG) {
      Serial.println("[WEB] LED test triggered");
    }
    
    request->send(200, "application/json", "{\"success\":true}");
  });
  
  if (ENABLE_SERIAL_DEBUG) {
    Serial.println("[WEB] STA mode server setup complete");
  }
}

// ============================================================================
// SECTION 7: API POLLING & JSON PARSING
// ============================================================================

String extractHexFromURL(const String& response) {
  /*
   * Extract hex color from vMix API response.
   * Expected URL format: state=#RRGGBB
   * 
   * Example: "http://...?state=#ff8c00&_=1776942364662"
   * Returns: "#ff8c00"
   */
  
  int stateIdx = response.indexOf("state=");
  if (stateIdx == -1) {
    return "#000000";  // Default black if not found
  }
  
  // Extract characters after "state="
  String hex = "";
  int idx = stateIdx + 6;  // Length of "state="
  
  // Collect hex color (typically 7 characters: #RRGGBB)
  while (idx < response.length() && response[idx] != '&' && response[idx] != ' ') {
    hex += response[idx];
    idx++;
    if (hex.length() > 10) break;  // Prevent infinite loop
  }
  
  // Validate hex format
  if (hex.length() >= 7 && hex[0] == '#') {
    return hex.substring(0, 7);
  }
  
  return "#000000";  // Return black if invalid
}

void hexToRGB(const String& hex, uint8_t& r, uint8_t& g, uint8_t& b) {
  /*
   * Convert hex color string to RGB values.
   * Input format: "#RRGGBB"
   * Output: r, g, b (0-255 each)
   * 
   * Example: "#ff8c00" → r=255, g=140, b=0
   */
  
  if (hex.length() < 7 || hex[0] != '#') {
    r = 0; g = 0; b = 0;
    return;
  }
  
  // Parse hex strings to integers
  char rStr[3], gStr[3], bStr[3];
  strncpy(rStr, hex.c_str() + 1, 2);
  rStr[2] = '\0';
  strncpy(gStr, hex.c_str() + 3, 2);
  gStr[2] = '\0';
  strncpy(bStr, hex.c_str() + 5, 2);
  bStr[2] = '\0';
  
  r = strtol(rStr, NULL, 16);
  g = strtol(gStr, NULL, 16);
  b = strtol(bStr, NULL, 16);
}

void pollTallyAPI() {
  /*
   * Poll vMix Tally API at configured endpoint.
   * Extract hex color from response and update LED.
   * Handle errors gracefully with red LED blink.
   */
  
  uint32_t now = millis();
  
  // Skip if not time to poll
  if (now - apiConfig.lastPollTime < apiConfig.pollInterval) {
    return;
  }
  
  // Skip if not connected to Wi-Fi
  if (!wifiState.isConnected) {
    return;
  }
  
  apiConfig.lastPollTime = now;
  
  if (ENABLE_SERIAL_DEBUG) {
    Serial.printf("[API] Polling: %s\n", apiConfig.url);
  }
  
  HTTPClient http;
  http.setTimeout(API_REQUEST_TIMEOUT);
  
  if (!http.begin(apiConfig.url)) {
    if (ENABLE_SERIAL_DEBUG) {
      Serial.println("[API] Failed to begin HTTP connection");
    }
    lastErrorMsg = "Connection failed";
    blinkLED(255, 0, 0, 1);  // Red blink for error
    return;
  }
  
  int httpCode = http.GET();
  
  if (httpCode != HTTP_CODE_OK) {
    if (ENABLE_SERIAL_DEBUG) {
      Serial.printf("[API] HTTP error: %d\n", httpCode);
    }
    lastErrorMsg = "HTTP error";
    http.end();
    blinkLED(255, 0, 0, 1);  // Red blink for error
    return;
  }
  
  // Get response body
  String response = http.getString();
  http.end();
  
  // Extract hex color from response
  String hex = extractHexFromURL(response);
  
  if (hex == "#000000" && lastHexColor != "#000000") {
    // Might be error, but let's display it anyway
    if (ENABLE_SERIAL_DEBUG) {
      Serial.println("[API] Warning: response might be invalid");
    }
  }
  
  // Convert hex to RGB
  uint8_t r, g, b;
  hexToRGB(hex, r, g, b);
  
  // Update LED with fade
  if (hex != lastHexColor) {
    lastHexColor = hex;
    fadeLED(r, g, b, DEFAULT_FADE_DURATION);
    
    if (ENABLE_SERIAL_DEBUG) {
      Serial.printf("[API] Color updated: %s → RGB(%d,%d,%d)\n", hex.c_str(), r, g, b);
    }
  }
  
  lastErrorMsg = "";
}

// ============================================================================
// SECTION 8: INITIALIZATION & SETUP
// ============================================================================

void setup() {
  /*
   * Initialize all systems:
   * 1. Serial debug output
   * 2. LED and button GPIO
   * 3. Preferences storage
   * 4. Wi-Fi mode (AP or STA based on stored credentials)
   * 5. Web server
   */
  
  // Initialize serial for debug output
  Serial.begin(115200);
  delay(1000);
  
  if (ENABLE_SERIAL_DEBUG) {
    Serial.println("\n\n");
    Serial.println("=====================================================");
    Serial.println("  Tally Light for vMix - ESP32/ESP8266");
    Serial.println("  Version 1.0 - April 2026");
    Serial.println("=====================================================");
  }
  
  // Initialize GPIO
  initLED();
  initButton();
  
  // Power-on indicator: green blink
  blinkLED(0, 255, 0, 1);
  
  // Initialize preferences storage
  initPreferences();
  loadSettings();
  
  if (ENABLE_SERIAL_DEBUG) {
    Serial.println("[INIT] System initialized");
  }
  
  // Decide between AP mode and STA mode
  if (!hasWiFiCredentials()) {
    // No saved credentials: start AP mode
    if (ENABLE_SERIAL_DEBUG) {
      Serial.println("[INIT] No Wi-Fi credentials found - starting AP mode");
    }
    startAPMode();
    setupAPModeServer();
  } else {
    // Credentials exist: connect to Wi-Fi
    char ssid[32] = "";
    char password[64] = "";
    getWiFiCredentials(ssid, password, sizeof(ssid), sizeof(password));
    
    if (ENABLE_SERIAL_DEBUG) {
      Serial.printf("[INIT] Credentials found - attempting connection to %s\n", ssid);
    }
    
    connectToWiFi(ssid, password);
    setupSTAModeServer();
  }
  
  // Start web server
  webServer.begin();
  
  if (ENABLE_SERIAL_DEBUG) {
    Serial.println("[INIT] Web server started");
    Serial.println("=====================================================\n");
  }
}

// ============================================================================
// SECTION 9: MAIN LOOP
// ============================================================================

void loop() {
  /*
   * Main event loop - called repeatedly by Arduino runtime.
   * Handles all non-blocking tasks:
   * - Wi-Fi connection monitoring
   * - Button input and reset detection
   * - API polling
   * - LED fade animation updates
   * - LED blink animation updates
   * - Periodic serial logging
   */
  
  // Check Wi-Fi connection and attempt reconnection if needed
  checkWiFiConnection();
  
  // Handle reset button
  handleResetButton();
  
  // Poll API (if connected to Wi-Fi)
  pollTallyAPI();
  
  // Update LED fade animation
  uint32_t now = millis();
  if (now - lastLEDFadeUpdateTime >= LED_FADE_STEP_TIME) {
    lastLEDFadeUpdateTime = now;
    updateLEDFade();
  }
  
  // Update LED blink animation
  updateLEDBlink();
  
  // Periodic status logging (every 30 seconds)
  if (ENABLE_SERIAL_DEBUG && now - lastSerialLogTime >= 30000) {
    lastSerialLogTime = now;
    Serial.printf("[LOOP] Status: WiFi=%s, LED=RGB(%d,%d,%d), API=%s\n",
                  wifiState.isConnected ? "CONNECTED" : "DISCONNECTED",
                  ledState.currentR, ledState.currentG, ledState.currentB,
                  lastHexColor.c_str());
  }
  
  // Small yield to prevent watchdog timeout
  delay(1);
}

// ============================================================================
// END OF FILE
// ============================================================================
