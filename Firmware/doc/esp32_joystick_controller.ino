/*
  Bluetooth Joystick Controller for MKS-DLC32 CNC Machine
  
  Hardware:
  - ESP32 Development Board
  - 2-axis or 3-axis analog joystick
  - Rotary encoder with button (optional)
  - Pushbuttons for Home/Zero/Pause/Reset
  - LED for connection status
  
  Protocol: Bluetooth Serial (SPP) to MKS-DLC32 Bluetooth interface
  
  Commands: Grbl Jog ($J=...) + Realtime commands (!, ~, ?, Ctrl+X)
*/

#include <BluetoothSerial.h>

// ============== PIN CONFIGURATION ==============
// Joystick analog inputs
#define JOY_X_PIN     35   // ADC1_CH7 (GPIO35)
#define JOY_Y_PIN     34   // ADC1_CH6 (GPIO34)
#define JOY_Z_PIN     33   // ADC1_CH5 (GPIO33)

// Encoder (optional)
#define ENCODER_CLK   25
#define ENCODER_DT    26
#define ENCODER_SW    27

// Control buttons
#define BTN_HOME      14
#define BTN_ZERO      12
#define BTN_PAUSE     13
#define BTN_RESET     15

// Status LED
#define LED_BT        32   // Blink when BT connected

// ============== CONFIGURATION ==============
#define BT_DEVICE_NAME "MKS-DLC32"  // Bluetooth device name
#define JOYSTICK_DEADZONE 0.1       // Joystick deadzone (0-1.0)
#define MAX_MOVE_PER_CMD 10.0        // Max movement per command (mm)
#define MIN_FEEDRATE 10
#define MAX_FEEDRATE 500
#define DEFAULT_FEEDRATE 100
#define JOG_SEND_INTERVAL 100        // ms between jog commands

// ============== GLOBAL VARIABLES ==============
BluetoothSerial SerialBT;
uint32_t lastJogTime = 0;
uint32_t lastStatusTime = 0;
float currentFeedrate = DEFAULT_FEEDRATE;
volatile int encoderCount = 0;
volatile bool encoderButtonPressed = false;

String currentStatus = "Idle";
String currentMachineState = "Idle";

// ============== SETUP ==============
void setup() {
  Serial.begin(115200);
  
  // Initialize analog inputs
  analogReadResolution(12);  // 12-bit (0-4095)
  pinMode(JOY_X_PIN, INPUT);
  pinMode(JOY_Y_PIN, INPUT);
  pinMode(JOY_Z_PIN, INPUT);
  
  // Initialize buttons
  pinMode(BTN_HOME, INPUT_PULLUP);
  pinMode(BTN_ZERO, INPUT_PULLUP);
  pinMode(BTN_PAUSE, INPUT_PULLUP);
  pinMode(BTN_RESET, INPUT_PULLUP);
  
  // Initialize LED
  pinMode(LED_BT, OUTPUT);
  digitalWrite(LED_BT, LOW);
  
  // Initialize encoder (if used)
  pinMode(ENCODER_CLK, INPUT);
  pinMode(ENCODER_DT, INPUT);
  pinMode(ENCODER_SW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENCODER_CLK), onEncoderClk, CHANGE);
  
  // Initialize Bluetooth
  if (!SerialBT.begin(BT_DEVICE_NAME)) {
    Serial.println("Error: Bluetooth initialization failed!");
    while (1) {
      digitalWrite(LED_BT, HIGH);
      delay(100);
      digitalWrite(LED_BT, LOW);
      delay(100);
    }
  }
  
  Serial.println("Bluetooth Joystick Controller initialized");
  Serial.printf("Looking for device: %s\n", BT_DEVICE_NAME);
}

// ============== MAIN LOOP ==============
void loop() {
  // Update connection status
  updateConnectionStatus();
  
  // Handle buttons
  handleButtons();
  
  // Handle joystick
  handleJoystick();
  
  // Handle encoder
  handleEncoder();
  
  // Process responses from machine
  processResponses();
  
  // Periodic status request
  uint32_t now = millis();
  if (now - lastStatusTime > 500) {
    requestStatus();
    lastStatusTime = now;
  }
  
  delay(20);  // Main loop timing
}

// ============== JOYSTICK HANDLING ==============
void handleJoystick() {
  // Read raw values
  int rawX = analogRead(JOY_X_PIN);
  int rawY = analogRead(JOY_Y_PIN);
  int rawZ = analogRead(JOY_Z_PIN);
  
  // Normalize to -1.0 ... 1.0
  float joyX = (rawX - 2048.0) / 2048.0;
  float joyY = (rawY - 2048.0) / 2048.0;
  float joyZ = (rawZ - 2048.0) / 2048.0;
  
  // Clamp to -1.0 ... 1.0
  joyX = constrain(joyX, -1.0, 1.0);
  joyY = constrain(joyY, -1.0, 1.0);
  joyZ = constrain(joyZ, -1.0, 1.0);
  
  // Apply deadzone
  if (abs(joyX) < JOYSTICK_DEADZONE) joyX = 0;
  if (abs(joyY) < JOYSTICK_DEADZONE) joyY = 0;
  if (abs(joyZ) < JOYSTICK_DEADZONE) joyZ = 0;
  
  // If there's movement, send jog command
  if (joyX != 0 || joyY != 0 || joyZ != 0) {
    uint32_t now = millis();
    if (now - lastJogTime >= JOG_SEND_INTERVAL) {
      // Convert -1.0...1.0 to movement in mm
      float moveX = joyX * MAX_MOVE_PER_CMD;
      float moveY = joyY * MAX_MOVE_PER_CMD;
      float moveZ = joyZ * MAX_MOVE_PER_CMD;
      
      jogMotion(moveX, moveY, moveZ, currentFeedrate);
      lastJogTime = now;
    }
  }
}

// ============== JOG COMMAND ==============
void jogMotion(float x, float y, float z, float feedrate) {
  // Validate feedrate
  if (feedrate < MIN_FEEDRATE) feedrate = MIN_FEEDRATE;
  if (feedrate > MAX_FEEDRATE) feedrate = MAX_FEEDRATE;
  
  // Build command string
  char cmd[100];
  int len = sprintf(cmd, "$J=");
  
  if (x != 0) len += sprintf(cmd + len, "X%.2f", x);
  if (y != 0) len += sprintf(cmd + len, "Y%.2f", y);
  if (z != 0) len += sprintf(cmd + len, "Z%.2f", z);
  
  len += sprintf(cmd + len, "F%.0f\n", feedrate);
  
  // Send command
  SerialBT.print(cmd);
  Serial.printf("-> %s", cmd);
}

// ============== BUTTON HANDLING ==============
void handleButtons() {
  // Home button
  if (digitalRead(BTN_HOME) == LOW) {
    sendCommand("$H");
    Serial.println("-> Home command");
    delay(300);  // Debounce
  }
  
  // Zero position button
  if (digitalRead(BTN_ZERO) == LOW) {
    sendCommand("G10L20P1X0Y0Z0");
    Serial.println("-> Zero position command");
    delay(300);
  }
  
  // Pause/Stop button
  if (digitalRead(BTN_PAUSE) == LOW) {
    SerialBT.write('!');  // Realtime command: pause/cancel jog
    Serial.println("-> Pause/Stop");
    delay(300);
  }
  
  // Reset button
  if (digitalRead(BTN_RESET) == LOW) {
    SerialBT.write(0x18);  // Ctrl+X: Soft reset
    Serial.println("-> Soft Reset");
    delay(300);
  }
}

// ============== ENCODER HANDLING ==============
void IRAM_ATTR onEncoderClk() {
  static uint32_t lastTime = 0;
  uint32_t now = millis();
  
  // Debounce
  if (now - lastTime < 5) return;
  lastTime = now;
  
  // Detect direction
  if (digitalRead(ENCODER_CLK) == digitalRead(ENCODER_DT)) {
    encoderCount++;  // Clockwise
  } else {
    encoderCount--;  // Counter-clockwise
  }
}

void handleEncoder() {
  if (encoderCount == 0) return;
  
  // Accumulate counts and update feedrate in steps of 10
  static int accumulatedCount = 0;
  accumulatedCount += encoderCount;
  encoderCount = 0;
  
  if (abs(accumulatedCount) >= 2) {  // Detect detent position (2 counts per step)
    if (accumulatedCount > 0) {
      currentFeedrate += 10;
      Serial.printf("Feedrate up: %.0f\n", currentFeedrate);
    } else {
      currentFeedrate -= 10;
      Serial.printf("Feedrate down: %.0f\n", currentFeedrate);
    }
    
    // Clamp
    if (currentFeedrate < MIN_FEEDRATE) currentFeedrate = MIN_FEEDRATE;
    if (currentFeedrate > MAX_FEEDRATE) currentFeedrate = MAX_FEEDRATE;
    
    accumulatedCount = 0;
  }
  
  // Encoder button
  if (digitalRead(ENCODER_SW) == LOW) {
    // Reset feedrate to default
    currentFeedrate = DEFAULT_FEEDRATE;
    Serial.printf("Feedrate reset to: %.0f\n", currentFeedrate);
    delay(500);
  }
}

// ============== COMMAND SENDING ==============
void sendCommand(const char* cmd) {
  SerialBT.print(cmd);
  SerialBT.print("\n");
  Serial.printf("-> %s\n", cmd);
}

void requestStatus() {
  SerialBT.write('?');  // Realtime: status report
}

// ============== RESPONSE PROCESSING ==============
void processResponses() {
  if (!SerialBT.available()) return;
  
  String response = "";
  while (SerialBT.available()) {
    char c = SerialBT.read();
    response += c;
    
    // Complete line received
    if (c == '\n') {
      Serial.printf("<- %s", response.c_str());
      parseResponse(response);
      response = "";
    }
  }
}

void parseResponse(String resp) {
  resp.trim();
  
  // Status report
  if (resp.startsWith("<")) {
    // Format: <Idle|MPos:X,Y,Z|FS:F,S>
    parseStatus(resp);
  }
  
  // Ok response
  else if (resp == "ok") {
    // Command accepted
  }
  
  // Error
  else if (resp.startsWith("error:")) {
    int errorCode = resp.substring(6).toInt();
    Serial.printf("Error code: %d\n", errorCode);
    // TODO: Map error codes to messages
  }
  
  // Info message
  else if (resp.startsWith("[MSG:")) {
    Serial.println(resp);
  }
}

void parseStatus(String status) {
  // Extract state: <Idle|...>
  int pipe1 = status.indexOf('|');
  if (pipe1 > 0) {
    currentMachineState = status.substring(1, pipe1);
    
    // Blink LED if running
    if (currentMachineState != "Idle") {
      digitalWrite(LED_BT, (millis() / 200) % 2);
    } else {
      digitalWrite(LED_BT, HIGH);  // Steady on when idle
    }
  }
  
  // Extract position: MPos:X,Y,Z
  int mposIdx = status.indexOf("MPos:");
  if (mposIdx > 0) {
    int pipeIdx = status.indexOf('|', mposIdx);
    if (pipeIdx < 0) pipeIdx = status.indexOf('>', mposIdx);
    
    String mposStr = status.substring(mposIdx + 5, pipeIdx);
    Serial.printf("Machine position: %s\n", mposStr.c_str());
  }
}

// ============== CONNECTION STATUS ==============
void updateConnectionStatus() {
  static uint32_t lastBlink = 0;
  static bool connected = false;
  
  bool isConnected = SerialBT.hasClient();
  
  if (isConnected != connected) {
    connected = isConnected;
    if (connected) {
      Serial.println("Bluetooth connected!");
      digitalWrite(LED_BT, HIGH);  // LED on
    } else {
      Serial.println("Bluetooth disconnected!");
    }
  }
  
  // Blink LED if not connected
  if (!connected) {
    uint32_t now = millis();
    if (now - lastBlink > 500) {
      digitalWrite(LED_BT, !digitalRead(LED_BT));
      lastBlink = now;
    }
  }
}

// ============== DEBUG COMMANDS (via Serial Monitor) ==============
void serialEvent() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    
    if (cmd == "help") {
      Serial.println("Commands:");
      Serial.println("  h - Home");
      Serial.println("  z - Zero position");
      Serial.println("  p - Pause");
      Serial.println("  r - Reset");
      Serial.println("  ? - Request status");
      Serial.println("  f<value> - Set feedrate (e.g., f100)");
      Serial.println("  x<mm> - Move X axis");
      Serial.println("  y<mm> - Move Y axis");
      Serial.println("  z<mm> - Move Z axis");
      Serial.println("  $ - Enter Grbl command");
    }
    
    if (cmd == "h") sendCommand("$H");
    else if (cmd == "z") sendCommand("G10L20P1X0Y0Z0");
    else if (cmd == "p") SerialBT.write('!');
    else if (cmd == "r") SerialBT.write(0x18);
    else if (cmd == "?") SerialBT.write('?');
    
    else if (cmd.startsWith("f")) {
      currentFeedrate = cmd.substring(1).toFloat();
      Serial.printf("Feedrate set to: %.0f\n", currentFeedrate);
    }
    
    else if (cmd.startsWith("x")) {
      float dist = cmd.substring(1).toFloat();
      jogMotion(dist, 0, 0, currentFeedrate);
    }
    else if (cmd.startsWith("y")) {
      float dist = cmd.substring(1).toFloat();
      jogMotion(0, dist, 0, currentFeedrate);
    }
    else if (cmd.startsWith("z")) {
      float dist = cmd.substring(1).toFloat();
      jogMotion(0, 0, dist, currentFeedrate);
    }
    
    else if (cmd.startsWith("$")) {
      sendCommand(cmd.c_str());
    }
  }
}
