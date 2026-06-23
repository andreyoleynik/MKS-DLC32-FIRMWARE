// Include this file in main.cpp to add advanced features
// #include "features.h"

// ============== ADVANCED FEATURE: SPEED CONTROL WITH ENCODER BUTTON ==============
// When encoder button is pressed + encoder rotation = change feedrate
// Otherwise = normal movement

#ifndef FEATURES_H
#define FEATURES_H

// Uncomment to enable features
// #define FEATURE_SPEED_CONTROL_WITH_BUTTON
// #define FEATURE_ACCELERATION_MODE
// #define FEATURE_MOVEMENT_HISTORY

// ============== FEATURE 1: Speed Control with Encoder Button ==============
#ifdef FEATURE_SPEED_CONTROL_WITH_BUTTON

volatile bool encoderButtonHeld = false;
uint32_t encoderButtonPressTime = 0;

void checkEncoderButton() {
  bool pressed = (digitalRead(ENCODER_SW) == LOW);
  
  if (pressed && !encoderButtonHeld) {
    encoderButtonHeld = true;
    encoderButtonPressTime = millis();
    Serial.println("Encoder button: FEEDRATE MODE");
  }
  
  if (!pressed && encoderButtonHeld) {
    encoderButtonHeld = false;
    Serial.println("Encoder button: MOVEMENT MODE");
  }
}

void processEncoderWithButton() {
  checkEncoderButton();
  
  if (encoderButtonHeld && encoderCount != 0) {
    // Change feedrate
    currentFeedrate += encoderCount * 5;  // 5 mm/min per step
    if (currentFeedrate < MIN_FEEDRATE) currentFeedrate = MIN_FEEDRATE;
    if (currentFeedrate > MAX_FEEDRATE) currentFeedrate = MAX_FEEDRATE;
    
    Serial.printf("Feedrate: %.0f mm/min\n", currentFeedrate);
    encoderCount = 0;
  } else {
    // Normal movement
    processEncoderMovement();
  }
}

#endif

// ============== FEATURE 2: Acceleration Mode ==============
// Each successive encoder tick in same direction increases movement
// Useful for rapid positioning

#ifdef FEATURE_ACCELERATION_MODE

volatile uint32_t lastEncoderDirection = 0;  // 0=none, 1=clockwise, 2=counter
volatile int accelerationLevel = 1;

void processEncoderAcceleration() {
  if (encoderCount == 0) return;
  
  uint32_t direction = (encoderCount > 0) ? 1 : 2;
  
  // Check if direction changed
  if (direction != lastEncoderDirection) {
    accelerationLevel = 1;
    lastEncoderDirection = direction;
  } else {
    // Same direction, increase acceleration
    accelerationLevel = min(5, accelerationLevel + 1);  // Max 5x speed
  }
  
  // Calculate accelerated movement
  float moveDistance = abs(encoderCount) * MOVE_DISTANCE * accelerationLevel;
  if (encoderCount < 0) moveDistance = -moveDistance;
  
  sendJogCommand(currentAxis, moveDistance, currentFeedrate);
  
  Serial.printf("Acceleration: x%d, Move: %.1f mm\n", accelerationLevel, moveDistance);
  encoderCount = 0;
}

#endif

// ============== FEATURE 3: Movement History (EEPROM) ==============
// Remember last movements and feedrate
// Note: Requires EEPROM implementation

#ifdef FEATURE_MOVEMENT_HISTORY

#include <Preferences.h>

Preferences preferences;

void saveLastSettings() {
  preferences.begin("joystick", false);
  preferences.putFloat("lastFeedrate", currentFeedrate);
  preferences.putInt("lastAxis", (int)currentAxis);
  preferences.end();
  
  Serial.println("Settings saved to EEPROM");
}

void loadLastSettings() {
  preferences.begin("joystick", true);
  currentFeedrate = preferences.getFloat("lastFeedrate", DEFAULT_FEEDRATE);
  currentAxis = (Axis)preferences.getInt("lastAxis", AXIS_X);
  preferences.end();
  
  Serial.printf("Loaded: Feedrate=%.0f, Axis=%s\n", currentFeedrate, axisNames[currentAxis]);
}

#endif

#endif  // FEATURES_H
