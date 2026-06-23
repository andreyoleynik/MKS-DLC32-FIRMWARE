/*
  MKS-DLC32 CNC Joystick Controller
  
  Hardware:
  - ESP32 DevKit v1
  - 1x Rotary Encoder (100 pulses/revolution)
  - 3x Pushbuttons (X, Y, Z axis selection)
  - 3x LEDs (RGB indicators for axis)
  
  Protocol: Bluetooth Serial (SPP) to MKS-DLC32
  
  Operation:
  1. Press button to select axis (X/Y/Z) - LED lights up
  2. Rotate encoder to move selected axis
  3. Bluetooth automatically connects to station on startup
*/

#include <Arduino.h>
#include <BluetoothSerial.h>

// ============== PIN CONFIGURATION ==============
#define ENCODER_CLK    25   // Encoder clock pin
#define ENCODER_DT     26   // Encoder data pin
#define ENCODER_SW     27   // Encoder button (optional)

#define BTN_AXIS_X     14   // Button for X axis
#define BTN_AXIS_Y     12   // Button for Y axis
#define BTN_AXIS_Z     13   // Button for Z axis

#define LED_X_PIN      32   // LED for X axis (red)
#define LED_Y_PIN      33   // LED for Y axis (green)
#define LED_Z_PIN      15   // LED for Z axis (blue)

#define STATUS_LED     2    // Connection status LED

// ============== CONFIGURATION ==============
#define BT_DEVICE_NAME "MKS-DLC32"
#define ENCODER_PULSES_PER_REV 100
#define MOVE_DISTANCE 1.0           // mm per encoder step
#define DEFAULT_FEEDRATE 100        // mm/min
#define MAX_FEEDRATE 300
#define MIN_FEEDRATE 50
#define BT_CONNECT_TIMEOUT 5000     // ms

// ============== AXIS ENUM ==============
enum Axis {
  AXIS_X = 0,
  AXIS_Y = 1,
  AXIS_Z = 2,
  AXIS_NONE = 3
};

// ============== GLOBAL VARIABLES ==============
BluetoothSerial SerialBT;
volatile long encoderCount = 0;
volatile int lastCLK = LOW;
volatile Axis currentAxis = AXIS_X;
float currentFeedrate = DEFAULT_FEEDRATE;
bool bluetoothConnected = false;
uint32_t lastEncoderTime = 0;
uint32_t lastButtonCheckTime = 0;

// Axis names for logging
const char* axisNames[3] = {"X", "Y", "Z"};
const uint8_t ledPins[3] = {LED_X_PIN, LED_Y_PIN, LED_Z_PIN};

// ============== SETUP ==============
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n");
  Serial.println("╔═══════════════════════════════════════╗");
  Serial.println("║   MKS-DLC32 Joystick Controller v1.0  ║");
  Serial.println("╚═══════════════════════════════════════╝");
  
  // Initialize I/O
  initializePins();
  
  // Initialize encoder interrupt
  attachInterrupt(digitalPinToInterrupt(ENCODER_CLK), onEncoderClk, CHANGE);
  
  // Initialize Bluetooth
  if (!SerialBT.begin(BT_DEVICE_NAME)) {
    Serial.println("ERROR: Bluetooth initialization failed!");
    blinkError();
  }
  
  Serial.printf("Bluetooth device name: %s\n", BT_DEVICE_NAME);
  Serial.println("Waiting for connection...\n");
  
  // Set default LED (X axis)
  updateAxisLED();
}

// ============== MAIN LOOP ==============
void loop() {
  uint32_t now = millis();
  
  // Update Bluetooth connection status
  if (now - lastButtonCheckTime > 100) {
    updateConnectionStatus();
    handleButtons();
    lastButtonCheckTime = now;
  }
  
  // Process encoder movement
  if (encoderCount != 0) {
    processEncoderMovement();
  }
  
  // Process Bluetooth responses
  processBluetoothResponses();
  
  delay(10);
}

// ============== PIN INITIALIZATION ==============
void initializePins() {
  // Encoder pins
  pinMode(ENCODER_CLK, INPUT);
  pinMode(ENCODER_DT, INPUT);
  pinMode(ENCODER_SW, INPUT_PULLUP);
  
  // Button pins
  pinMode(BTN_AXIS_X, INPUT_PULLUP);
  pinMode(BTN_AXIS_Y, INPUT_PULLUP);
  pinMode(BTN_AXIS_Z, INPUT_PULLUP);
  
  // LED pins
  pinMode(LED_X_PIN, OUTPUT);
  pinMode(LED_Y_PIN, OUTPUT);
  pinMode(LED_Z_PIN, OUTPUT);
  pinMode(STATUS_LED, OUTPUT);
  
  // Turn off all LEDs initially
  digitalWrite(LED_X_PIN, LOW);
  digitalWrite(LED_Y_PIN, LOW);
  digitalWrite(LED_Z_PIN, LOW);
  digitalWrite(STATUS_LED, LOW);
  
  Serial.println("✓ Pins initialized");
}

// ============== ENCODER INTERRUPT ==============
void IRAM_ATTR onEncoderClk() {
  int currentCLK = digitalRead(ENCODER_CLK);
  
  if (currentCLK != lastCLK) {
    lastCLK = currentCLK;
    
    if (currentCLK == HIGH) {
      if (digitalRead(ENCODER_DT) != currentCLK) {
        encoderCount++;  // Clockwise
      } else {
        encoderCount--;  // Counter-clockwise
      }
    }
  }
}

// ============== ENCODER PROCESSING ==============
void processEncoderMovement() {
  uint32_t now = millis();
  
  // Rate limit to prevent flooding
  if (now - lastEncoderTime < 50) {
    return;
  }
  
  if (currentAxis == AXIS_NONE) {
    encoderCount = 0;
    return;
  }
  
  long steps = encoderCount;
  encoderCount = 0;
  
  // Calculate movement
  float moveDistance = steps * MOVE_DISTANCE;
  
  // Send jog command
  sendJogCommand(currentAxis, moveDistance, currentFeedrate);
  lastEncoderTime = now;
}

// ============== JOG COMMAND ==============
void sendJogCommand(Axis axis, float distance, float feedrate) {
  if (!bluetoothConnected) {
    Serial.println("⚠ Not connected to machine");
    return;
  }
  
  char cmd[100];
  
  // Build jog command based on axis
  sprintf(cmd, "$J=");
  
  switch (axis) {
    case AXIS_X:
      sprintf(cmd + strlen(cmd), "X%.1f", distance);
      break;
    case AXIS_Y:
      sprintf(cmd + strlen(cmd), "Y%.1f", distance);
      break;
    case AXIS_Z:
      sprintf(cmd + strlen(cmd), "Z%.1f", distance);
      break;
    default:
      return;
  }
  
  // Add feedrate and newline
  sprintf(cmd + strlen(cmd), "F%.0f\n", feedrate);
  
  // Send command
  SerialBT.print(cmd);
  Serial.printf("[%s] Move %+.1f mm @ %.0f mm/min\n", axisNames[axis], distance, feedrate);
}

// ============== BUTTON HANDLING ==============
void handleButtons() {
  // Check X axis button
  if (digitalRead(BTN_AXIS_X) == LOW) {
    if (currentAxis != AXIS_X) {
      currentAxis = AXIS_X;
      updateAxisLED();
      Serial.printf("→ Axis selected: %s\n", axisNames[AXIS_X]);
    }
    delay(50);  // Debounce
  }
  
  // Check Y axis button
  if (digitalRead(BTN_AXIS_Y) == LOW) {
    if (currentAxis != AXIS_Y) {
      currentAxis = AXIS_Y;
      updateAxisLED();
      Serial.printf("→ Axis selected: %s\n", axisNames[AXIS_Y]);
    }
    delay(50);
  }
  
  // Check Z axis button
  if (digitalRead(BTN_AXIS_Z) == LOW) {
    if (currentAxis != AXIS_Z) {
      currentAxis = AXIS_Z;
      updateAxisLED();
      Serial.printf("→ Axis selected: %s\n", axisNames[AXIS_Z]);
    }
    delay(50);
  }
  
  // Encoder button: change feedrate mode (optional)
  if (digitalRead(ENCODER_SW) == LOW) {
    // Could implement feedrate adjustment or other features here
    delay(500);
  }
}

// ============== LED UPDATE ==============
void updateAxisLED() {
  // Turn off all axis LEDs
  digitalWrite(LED_X_PIN, LOW);
  digitalWrite(LED_Y_PIN, LOW);
  digitalWrite(LED_Z_PIN, LOW);
  
  // Turn on selected axis LED
  if (currentAxis < 3) {
    digitalWrite(ledPins[currentAxis], HIGH);
  }
}

// ============== CONNECTION STATUS ==============
void updateConnectionStatus() {
  static uint32_t lastStatusCheck = 0;
  static bool wasConnected = false;
  
  bool isConnected = SerialBT.hasClient();
  
  // Handle connection state change
  if (isConnected != wasConnected) {
    wasConnected = isConnected;
    
    if (isConnected) {
      Serial.println("✓ Connected to MKS-DLC32!");
      bluetoothConnected = true;
      digitalWrite(STATUS_LED, HIGH);  // Solid on
    } else {
      Serial.println("✗ Disconnected from MKS-DLC32");
      bluetoothConnected = false;
    }
  }
  
  // Blink status LED if not connected
  if (!isConnected) {
    uint32_t now = millis();
    if (now - lastStatusCheck > 300) {
      digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));
      lastStatusCheck = now;
    }
  }
}

// ============== BLUETOOTH RESPONSES ==============
void processBluetoothResponses() {
  static String buffer = "";
  
  if (!SerialBT.available()) return;
  
  while (SerialBT.available()) {
    char c = SerialBT.read();
    
    if (c == '\n') {
      if (buffer.length() > 0) {
        parseResponse(buffer);
        buffer = "";
      }
    } else if (c != '\r') {
      buffer += c;
    }
  }
}

void parseResponse(String response) {
  // Status report
  if (response.startsWith("<")) {
    // <Idle|MPos:X,Y,Z|...>
    int pipePos = response.indexOf('|');
    if (pipePos > 0) {
      String state = response.substring(1, pipePos);
      if (state != "Idle") {
        // Machine is moving, pulse status LED
        digitalWrite(STATUS_LED, (millis() / 100) % 2);
      } else {
        digitalWrite(STATUS_LED, HIGH);
      }
    }
  }
  
  // Command accepted
  else if (response == "ok") {
    // Success
  }
  
  // Error
  else if (response.startsWith("error:")) {
    int errorCode = response.substring(6).toInt();
    Serial.printf("✗ Machine error: %d\n", errorCode);
  }
  
  // Info messages
  else if (response.startsWith("[MSG:")) {
    Serial.println(response);
  }
}

// ============== ERROR HANDLING ==============
void blinkError() {
  while (1) {
    digitalWrite(STATUS_LED, HIGH);
    delay(100);
    digitalWrite(STATUS_LED, LOW);
    delay(100);
  }
}

// ============== DEBUG COMMANDS (Serial Monitor) ==============
void serialEvent() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    
    if (cmd == "help" || cmd == "h") {
      printHelp();
    }
    else if (cmd == "status" || cmd == "s") {
      Serial.printf("Connected: %s\n", bluetoothConnected ? "YES" : "NO");
      Serial.printf("Current axis: %s\n", axisNames[currentAxis]);
      Serial.printf("Feedrate: %.0f mm/min\n", currentFeedrate);
    }
    else if (cmd.startsWith("f")) {
      // Set feedrate: f150
      float feedrate = cmd.substring(1).toFloat();
      if (feedrate >= MIN_FEEDRATE && feedrate <= MAX_FEEDRATE) {
        currentFeedrate = feedrate;
        Serial.printf("✓ Feedrate set to: %.0f\n", currentFeedrate);
      } else {
        Serial.printf("✗ Feedrate must be between %.0f and %.0f\n", (float)MIN_FEEDRATE, (float)MAX_FEEDRATE);
      }
    }
    else if (cmd == "home") {
      if (bluetoothConnected) {
        SerialBT.print("$H\n");
        Serial.println("→ Home command sent");
      } else {
        Serial.println("✗ Not connected");
      }
    }
    else if (cmd == "zero") {
      if (bluetoothConnected) {
        SerialBT.print("G10L20P1X0Y0Z0\n");
        Serial.println("→ Zero position command sent");
      } else {
        Serial.println("✗ Not connected");
      }
    }
    else if (cmd == "pause") {
      if (bluetoothConnected) {
        SerialBT.write('!');  // Realtime command
        Serial.println("→ Pause/Stop sent");
      } else {
        Serial.println("✗ Not connected");
      }
    }
    else if (cmd == "status?") {
      if (bluetoothConnected) {
        SerialBT.write('?');  // Status request
        Serial.println("→ Status request sent");
      } else {
        Serial.println("✗ Not connected");
      }
    }
    else if (cmd.startsWith("x")) {
      float dist = cmd.substring(1).toFloat();
      sendJogCommand(AXIS_X, dist, currentFeedrate);
    }
    else if (cmd.startsWith("y")) {
      float dist = cmd.substring(1).toFloat();
      sendJogCommand(AXIS_Y, dist, currentFeedrate);
    }
    else if (cmd.startsWith("z")) {
      float dist = cmd.substring(1).toFloat();
      sendJogCommand(AXIS_Z, dist, currentFeedrate);
    }
    else {
      Serial.println("Unknown command. Type 'help' for commands list.");
    }
  }
}

void printHelp() {
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║      DEBUG COMMANDS (Serial Monitor)    ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.println("help, h          - Show this help");
  Serial.println("status, s        - Show current status");
  Serial.println("f<value>         - Set feedrate (e.g., f100)");
  Serial.println("home             - Send home command");
  Serial.println("zero             - Zero position");
  Serial.println("pause            - Pause/stop motion");
  Serial.println("status?          - Request machine status");
  Serial.println("x<mm>            - Manual move X axis");
  Serial.println("y<mm>            - Manual move Y axis");
  Serial.println("z<mm>            - Manual move Z axis");
  Serial.println("\n✓ Use hardware buttons for normal operation\n");
}
