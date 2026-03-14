#ifndef BUZZER_H
#define BUZZER_H

#include <Arduino.h>

// ═══════════════════════════════════════════════
//  BUZZER PATTERN PLAYER — KXG1203C active buzzer
//  Pairs of {on_ms, off_ms} — ends with {0,0}
// ═══════════════════════════════════════════════
struct BuzzStep { uint16_t on_ms; uint16_t off_ms; };

// Boot: ta-ta-ta-TAAA
const BuzzStep PAT_BOOT[]       = { {80,60},{80,60},{80,60},{220,0},{0,0} };

// Engine restart: quick double tap
const BuzzStep PAT_ENGINE_ON[]  = { {60,50},{60,0},{0,0} };

// Overheat: urgent rapid triple, repeat
const BuzzStep PAT_OVERHEAT[]   = { {120,80},{120,80},{120,300},{0,0} };

// Handbrake: double beep, repeat
const BuzzStep PAT_HANDBRAKE[]  = { {150,100},{150,400},{0,0} };

// Cold start: 3 fast beeps, once
const BuzzStep PAT_COLD[]       = { {80,60},{80,60},{80,0},{0,0} };

// Cold + high RPM: rapid triple warning, repeat
const BuzzStep PAT_COLD_RPM[]   = { {80,60},{80,60},{80,60},{80,500},{0,0} };

// DTC fault: three medium beeps
const BuzzStep PAT_DTC[]        = { {150,100},{150,100},{150,500},{0,0} };

#endif // BUZZER_H
