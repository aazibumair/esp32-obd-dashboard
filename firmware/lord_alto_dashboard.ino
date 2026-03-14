/*
 * LORD ALTO DASHBOARD  v5
 * Suzuki Alto VX – K6A VVT – ISO 15765-4 CAN 11-bit 500kbps
 *
 * PIN MAP
 * ─────────────────────────────────────────────
 *  SSD1306  SDA  → GPIO 21
 *  SSD1306  SCL  → GPIO 22
 *  MCP2515  CS   → GPIO 5
 *  MCP2515  MISO → GPIO 19
 *  MCP2515  MOSI → GPIO 23
 *  MCP2515  SCK  → GPIO 18
 *  MCP2515  INT  → GPIO 4
 *  HW-104 PAM8403 IN → GPIO 25  (DAC2)
 *  BUTTON  → GPIO 13  (other leg → GND)
 * ─────────────────────────────────────────────
 * Library: MCP_CAN by coryjfowler
 *
 * PAGES  (short-press GPIO13 to cycle)
 *  0 – Speedometer      (gauge1 style)
 *  1 – Tachometer/RPM   (gauge2 style)
 *  2 – Odometer + Trip  (gauge3 style)
 *  3 – Voltmeter        (gauge4 style)
 *  4 – Coolant Temp     (gauge5 arc needle style)
 *  5 – AFR              (arc needle, LEAN/RICH, numeric)
 *  6 – Long-term Fuel Average (dedicated)
 *  7 – Instant Fuel          (dedicated)
 *
 * OVERLAYS
 *  Overheat  ≥ 100°C  — banner over current page + looping sound
 *  DTC fault — full screen, 5 s per code, beep
 *
 * Hold GPIO13 for 4 s on page 6 or 7 → reset trip & fuel average
 */

#include <Arduino.h>
#include <SPI.h>
#include <mcp_can.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <math.h>
#include "buzzer.h"
#include "images.h"

// ═══════════════════════════════════════════════
//  PINS
// ═══════════════════════════════════════════════
#define PIN_OLED_SDA  21
#define PIN_OLED_SCL  22
#define PIN_MCP_CS    5
#define PIN_MCP_INT   4
#define PIN_AUDIO     26   // buzzer NPN transistor base (via 1kΩ resistor)
#define PIN_BTN       13
#define PIN_HANDBRAKE 34   // GPIO34 — input-only, safe for switched signals

// ── Handbrake wiring mode ──────────────────────────────────────────────
// ACTIVE_HIGH 1 = switch sends 12V (use resistor divider, see below)
// ACTIVE_HIGH 0 = switch sends GND when brake is up (use internal pull-up)
//
// 12V switch → ACTIVE_HIGH 1 wiring:
//   Handbrake wire ──── 33kΩ ──── GPIO34
//   GPIO34             ──── 10kΩ ──── GND
//   Result: 12V × (10k/43k) = 2.79V at GPIO34  ✓ safe for ESP32
//
// GND switch → ACTIVE_HIGH 0 wiring:
//   Handbrake wire (pulls LOW when up) ──── GPIO34
//   Internal pull-up enabled in code — no resistors needed
#define HANDBRAKE_ACTIVE_HIGH 0  // GND switch + 1N4148 diode: LOW = brake UP

// ═══════════════════════════════════════════════
//  DISPLAY
// ═══════════════════════════════════════════════
#define SCREEN_W  128
#define SCREEN_H   64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// ═══════════════════════════════════════════════
//  NVS
// ═══════════════════════════════════════════════
Preferences prefs;

// ═══════════════════════════════════════════════
//  CAN
// ═══════════════════════════════════════════════
MCP_CAN CAN(PIN_MCP_CS);
#define OBD_REQ_ID       0x7DF
#define OBD_RSP_ID       0x7E8
#define PID_ENGINE_LOAD  0x04
#define PID_COOLANT_TEMP 0x05
#define PID_STFT         0x06
#define PID_LTFT         0x07
#define PID_MAP          0x0B
#define PID_RPM          0x0C
#define PID_SPEED        0x0D
#define PID_IAT          0x0F
#define PID_VOLTAGE      0x42

// ═══════════════════════════════════════════════
//  CONSTANTS
// ═══════════════════════════════════════════════
#define VD_M3        0.000658f
#define VE           0.85f
#define R_AIR        287.0f
#define AFR_STOICH   14.7f
#define FUEL_DENSITY 745.0f
#define OVERHEAT_C   100          // ← 100°C threshold

// ═══════════════════════════════════════════════
//  LIVE OBD DATA
// ═══════════════════════════════════════════════
struct OBDData {
  int   rpm         = 0;
  int   speed       = 0;
  int   coolantTemp = 0;
  int   iat         = 20;
  int   map_kpa     = 101;
  float engineLoad  = 0.0f;
  float stft        = 0.0f;
  float ltft        = 0.0f;
  float voltage     = 12.0f;
  bool  valid       = false;
} obd;

unsigned long lastCanResponseMs = 0;
bool          engineOn          = false;
#define ENGINE_TIMEOUT_MS 3000UL

// ═══════════════════════════════════════════════
//  FUEL & ODOMETER
// ═══════════════════════════════════════════════
float         instKmL     = 0.0f;
float         instLph     = 0.0f;
float         tripFuelL   = 0.0f;
float         tripDistKm  = 0.0f;
float         avgKmL      = 12.0f;
float         afrLive     = 14.7f;
unsigned long odoKm       = 0;        // persisted total odometer
#define MIN_FUEL_FOR_AVG  0.3f

unsigned long lastNvsSave = 0;
#define NVS_SAVE_MS 30000UL

void saveTrip() {
  prefs.begin("lordalto", false);
  prefs.putFloat("tripFuel", tripFuelL);
  prefs.putFloat("tripDist", tripDistKm);
  prefs.putULong("odoKm",    odoKm);
  prefs.end();
  Serial.println(F("[NVS] Saved"));
}

void loadTrip() {
  prefs.begin("lordalto", true);
  tripFuelL  = prefs.getFloat("tripFuel", 0.0f);
  tripDistKm = prefs.getFloat("tripDist", 0.0f);
  odoKm      = prefs.getULong("odoKm",    0);
  prefs.end();
  avgKmL = (tripFuelL > MIN_FUEL_FOR_AVG)
           ? tripDistKm / tripFuelL : 12.0f;
  Serial.printf("[NVS] dist=%.1f km  fuel=%.2f L  odo=%lu km\n",
                tripDistKm, tripFuelL, odoKm);
}

void resetTrip() {
  tripFuelL = tripDistKm = 0.0f;
  avgKmL    = 12.0f;
  saveTrip();
  Serial.println(F("[FUEL] Trip reset"));
}

// ═══════════════════════════════════════════════
//  DTC
// ═══════════════════════════════════════════════
#define MAX_DTCS 8
struct DTCEntry { char code[7]; };
DTCEntry      dtcs[MAX_DTCS];
int           dtcCount     = 0;
bool          dtcActive    = false;
int           dtcShowIdx   = 0;
unsigned long dtcShowStart = 0;
bool          dtcPending   = false;

// ═══════════════════════════════════════════════
//  NAVIGATION + BUTTON
// ═══════════════════════════════════════════════
int           currentPage    = 0;
#define NUM_PAGES 8
unsigned long btnPressStart  = 0;
bool          btnWasDown     = false;
bool          btnLongHandled = false;
#define HOLD_RESET_MS 4000UL

// ═══════════════════════════════════════════════
//  OVERHEAT
// ═══════════════════════════════════════════════
bool          overheatActive     = false;
unsigned long overheatStart      = 0;
bool          overheatSndPlaying = false;
#define OVERHEAT_SHOW_MS  10000UL
#define OVERHEAT_PAUSE_MS 10000UL

// ═══════════════════════════════════════════════
//  COLD START
// ═══════════════════════════════════════════════
#define COLD_TEMP_C        60       // below this = cold engine
#define COLD_RPM_LIMIT     2000     // rpm threshold for cold warning
#define COLD_OVERLAY_MS    7000UL   // initial cold overlay duration
#define COLD_RPM_OVERLAY_MS 4000UL  // high-rpm-while-cold overlay duration
#define COLD_RPM_COOLDOWN_MS 5000UL // cooldown between repeated rpm warnings

bool          coldStartShown    = false;  // initial 7s overlay done flag
bool          coldOverlayActive = false;  // initial cold overlay showing
unsigned long coldOverlayStart  = 0;

bool          coldRpmWarning    = false;  // high-rpm-while-cold overlay showing
unsigned long coldRpmStart      = 0;
unsigned long coldRpmCooldown   = 0;      // cooldown timer start
bool          coldRpmInCooldown = false;

// ═══════════════════════════════════════════════
//  ENGINE RESTART SOUND
// ═══════════════════════════════════════════════
bool          prevEngineOn      = false;  // tracks transitions off→on
bool          firstBoot         = true;   // suppress sound on very first boot

// ═══════════════════════════════════════════════
//  HANDBRAKE WARNING
// ═══════════════════════════════════════════════
bool          hbrakeWarning      = false;
bool          hbrakeSndPlaying   = false;

// ═══════════════════════════════════════════════
//  AUDIO  –  Hardware Timer ISR (Timer 1)
//  Timer fires at exact sample rate — zero jitter.
//  No FreeRTOS task, no ets_delay_us, no scheduling overhead.
//  ISR runs on whichever core handles the interrupt (Core 1 by default
//  on ESP32 Arduino) but executes in microseconds so loop() is unaffected.
//  ESP32 APB clock = 80 MHz. Prescaler 80 → 1 tick = 1 µs.
//  Alarm = 1000000 / sampleRate  (e.g. 22050 Hz → alarm = 45 µs)
// ═══════════════════════════════════════════════
// ═══════════════════════════════════════════════
//  BUZZER — KXG1203C active buzzer via NPN transistor
//  GPIO26 → 1kΩ → BC547/2N2222 Base
//  Collector → Buzzer (-) → Buzzer (+) → 5V
//  1N4148 across buzzer for back-EMF protection
//  Patterns defined in buzzer.h
// ═══════════════════════════════════════════════

// ── Player state ─────────────────────────────────
const BuzzStep *buzzPattern  = nullptr;
int             buzzStep     = 0;
bool            buzzLoop     = false;
bool            buzzOn       = false;
unsigned long   buzzNextMs   = 0;

void buzzerInit() {
  pinMode(PIN_AUDIO, OUTPUT);
  digitalWrite(PIN_AUDIO, LOW);
}

void buzzerStop() {
  buzzPattern = nullptr;
  buzzStep    = 0;
  buzzOn      = false;
  digitalWrite(PIN_AUDIO, LOW);
}

void buzzerPlay(const BuzzStep *pattern, bool loop) {
  buzzerStop();
  buzzPattern = pattern;
  buzzLoop    = loop;
  buzzStep    = 0;
  buzzOn      = true;
  buzzNextMs  = millis() + pattern[0].on_ms;
  digitalWrite(PIN_AUDIO, HIGH);
}

// Call every loop() — advances pattern non-blocking
void buzzerTick() {
  if (!buzzPattern) return;
  unsigned long now = millis();
  if (now < buzzNextMs) return;

  if (buzzOn) {
    // Finished ON phase — go to OFF
    digitalWrite(PIN_AUDIO, LOW);
    buzzOn = false;
    uint16_t offMs = buzzPattern[buzzStep].off_ms;
    if (offMs == 0) {
      // No off phase — move to next step immediately
      buzzStep++;
      if (buzzPattern[buzzStep].on_ms == 0) {
        // End of pattern
        if (buzzLoop) { buzzStep = 0; }
        else          { buzzerStop(); return; }
      }
      buzzOn     = true;
      buzzNextMs = now + buzzPattern[buzzStep].on_ms;
      digitalWrite(PIN_AUDIO, HIGH);
    } else {
      buzzNextMs = now + offMs;
    }
  } else {
    // Finished OFF phase — move to next step
    buzzStep++;
    if (buzzPattern[buzzStep].on_ms == 0) {
      // End of pattern
      if (buzzLoop) { buzzStep = 0; }
      else          { buzzerStop(); return; }
    }
    buzzOn     = true;
    buzzNextMs = now + buzzPattern[buzzStep].on_ms;
    digitalWrite(PIN_AUDIO, HIGH);
  }
}

// Legacy wrappers so existing call sites work unchanged
void audioStop() { buzzerStop(); }
void audioInit() { buzzerInit(); }

// ═══════════════════════════════════════════════
//  CAN HELPERS
// ═══════════════════════════════════════════════
void obdRequest(byte pid) {
  byte buf[8] = { 0x02, 0x01, pid, 0, 0, 0, 0, 0 };
  CAN.sendMsgBuf(OBD_REQ_ID, 0, 8, buf);
}
void dtcRequest() {
  byte buf[8] = { 0x01, 0x03, 0, 0, 0, 0, 0, 0 };
  CAN.sendMsgBuf(OBD_REQ_ID, 0, 8, buf);
}
void decodeDTC(byte b1, byte b2, char *out) {
  const char t[] = "PCBU";
  snprintf(out, 7, "%c%01X%02X%02X",
           t[(b1>>6)&0x03], (b1>>4)&0x03, b1&0x0F, b2);
}

// ═══════════════════════════════════════════════
//  OBD POLL TICK
// ═══════════════════════════════════════════════
const byte pidList[] = {
  PID_RPM, PID_SPEED, PID_COOLANT_TEMP, PID_IAT,
  PID_MAP, PID_ENGINE_LOAD, PID_STFT, PID_LTFT, PID_VOLTAGE
};
const int pidCount = sizeof(pidList);
int       pidIdx   = 0;
unsigned long lastPidMs = 0;
unsigned long lastDtcMs = 0;
#define PID_INTERVAL_MS 60UL
#define DTC_INTERVAL_MS 5000UL

void canTick() {
  unsigned long now = millis();
  if (now - lastPidMs >= PID_INTERVAL_MS) {
    lastPidMs = now;
    obdRequest(pidList[pidIdx]);
    pidIdx = (pidIdx + 1) % pidCount;
  }
  if (now - lastDtcMs >= DTC_INTERVAL_MS) {
    lastDtcMs = now;
    dtcRequest();
  }
  if (digitalRead(PIN_MCP_INT) != LOW) {
    if (now - lastCanResponseMs > ENGINE_TIMEOUT_MS && engineOn) {
      engineOn  = false;
      obd.rpm   = 0;
      obd.speed = 0;
      Serial.println(F("[ENGINE] Off"));
    }
    return;
  }

  unsigned long rxId; byte len = 0; byte rxBuf[8];
  CAN.readMsgBuf(&rxId, &len, rxBuf);
  if ((rxId & 0x7FF) != OBD_RSP_ID || len < 4) return;
  lastCanResponseMs = now;

  byte mode = rxBuf[1], pid = rxBuf[2];
  byte A = rxBuf[3], B = (len>=5) ? rxBuf[4] : 0;

  if (mode == 0x41) {
    obd.valid = true;
    switch (pid) {
      case PID_RPM:
        obd.rpm = ((A*256)+B)/4;
        engineOn = (obd.rpm > 400);
        break;
      case PID_SPEED:        obd.speed       = A;                         break;
      case PID_COOLANT_TEMP: obd.coolantTemp = (int)A - 40;              break;
      case PID_IAT:          obd.iat         = (int)A - 40;              break;
      case PID_MAP:          obd.map_kpa     = A;                         break;
      case PID_ENGINE_LOAD:  obd.engineLoad  = A*100.0f/255.0f;         break;
      case PID_STFT:         obd.stft        = (A-128)*100.0f/128.0f;   break;
      case PID_LTFT:         obd.ltft        = (A-128)*100.0f/128.0f;   break;
      case PID_VOLTAGE:      obd.voltage     = ((A*256)+B)/1000.0f;     break;
    }
  } else if (mode == 0x43) {
    int newC = 0;
    for (int i=2; i+1<(int)len && dtcCount<MAX_DTCS; i+=2) {
      if (!rxBuf[i] && !rxBuf[i+1]) continue;
      char code[7]; decodeDTC(rxBuf[i],rxBuf[i+1],code);
      bool ex=false;
      for(int j=0;j<dtcCount;j++) if(!strcmp(dtcs[j].code,code)){ex=true;break;}
      if(!ex){ strncpy(dtcs[dtcCount++].code,code,6); newC++; }
    }
    if (newC>0) dtcPending=true;
  }
}

// ═══════════════════════════════════════════════
//  FUEL TICK
// ═══════════════════════════════════════════════
void fuelTick() {
  static unsigned long lastCalc = 0;
  static float         odoAccum = 0.0f;   // accumulate fractional km for odo
  unsigned long now = millis();
  if (now - lastCalc < 500) return;
  float dt = (now - lastCalc) / 1000.0f;
  lastCalc = now;

  if (!engineOn || obd.rpm < 400) {
    instKmL = instLph = 0.0f;
    return;
  }

  float MAP_Pa     = obd.map_kpa * 1000.0f;
  float T_K        = obd.iat + 273.0f;
  float airMass_gs = (MAP_Pa * VD_M3 * VE * (float)obd.rpm)
                     / (R_AIR * T_K * 2.0f * 60.0f) * 1000.0f;

  float effAFR = AFR_STOICH * (1.0f + (obd.stft + obd.ltft) / 100.0f);
  if (effAFR < 8.0f) effAFR = 8.0f;

  // Live AFR for gauge
  afrLive = effAFR;

  float fuelGs  = airMass_gs / effAFR;
  float fuelLph = (fuelGs * 3600.0f) / FUEL_DENSITY;
  instLph = fuelLph;
  instKmL = (obd.speed > 2 && fuelLph > 0.01f)
            ? (float)obd.speed / fuelLph : 0.0f;

  tripFuelL  += (fuelLph / 3600.0f) * dt;
  float distInc = (float)obd.speed / 3600.0f * dt;
  tripDistKm += distInc;

  // Odometer accumulation
  odoAccum += distInc;
  if (odoAccum >= 1.0f) {
    odoKm   += (unsigned long)odoAccum;
    odoAccum -= (unsigned long)odoAccum;
  }

  // Average — blend default → real
  if (tripFuelL > MIN_FUEL_FOR_AVG) {
    avgKmL = tripDistKm / tripFuelL;
  } else if (tripFuelL > 0.01f) {
    float w = tripFuelL / MIN_FUEL_FOR_AVG;
    avgKmL = 12.0f*(1.0f-w) + (tripDistKm/tripFuelL)*w;
  }

  // NVS save
  if (now - lastNvsSave > NVS_SAVE_MS) {
    lastNvsSave = now;
    saveTrip();
  }
}

// ═══════════════════════════════════════════════
//  DRAW HELPERS
// ═══════════════════════════════════════════════
void drawLabel(int x, int y, const char *s) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(x, y);
  display.print(s);
}

void drawBar(int x, int y, int w, int h, float pct) {
  pct = constrain(pct, 0.0f, 100.0f);
  display.drawRect(x, y, w, h, SSD1306_WHITE);
  int fill = (int)(pct/100.0f*(w-2));
  if (fill>0) display.fillRect(x+1,y+1,fill,h-2,SSD1306_WHITE);
}

void drawHLine(int y) {
  display.drawFastHLine(0, y, SCREEN_W, SSD1306_WHITE);
}

// Arc helper: draw arc between angles a1..a2 (radians), centre cx,cy, radius r
void drawArc(int cx, int cy, int r, float a1, float a2, int steps) {
  float px = cx + r*cos(a1), py = cy - r*sin(a1);
  for (int i=1; i<=steps; i++) {
    float a  = a1 + (a2-a1)*i/steps;
    float nx = cx + r*cos(a), ny = cy - r*sin(a);
    display.drawLine((int)px,(int)py,(int)nx,(int)ny, SSD1306_WHITE);
    px=nx; py=ny;
  }
}

// Page indicator dots — bottom right, 8 dots, 7px spacing
// x centres: 17,24,31,38,45,52,59,66 from right edge
// Use bottom-left so they don't fight with right-side content
// Place at y=61, x=2..58 (left side, 8 dots × 7px = 56px)
void drawPageDots() {
  // removed — no dots
}

// Battery icon used on voltmeter page
void drawBatteryIcon(int x, int y) {
  display.drawRect(x, y, 28, 14, SSD1306_WHITE);
  display.fillRect(x+28, y+4, 4, 6, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(x+3,  y+3); display.print(F("+"));
  display.setCursor(x+17, y+3); display.print(F("-"));
}

// ═══════════════════════════════════════════════
//  PAGE 0  —  SPEEDOMETER  (gauge1 style)
//  "Km/h" top-left size1, big centred value size4
//  size4 char = 24×32px
//  3-digit max "199": 3×24=72px, centred x=(128-72)/2=28
//  y centred: (64-32)/2+4=20
// ═══════════════════════════════════════════════
void drawPage0() {
  drawLabel(0, 0, "Km/h");

  String spd = String(obd.speed);
  int tw = spd.length() * 24;
  int x  = (SCREEN_W - tw) / 2;
  int y  = (SCREEN_H - 32) / 2 + 4;
  display.setTextSize(4);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(x, y);
  display.print(spd);
}

// ═══════════════════════════════════════════════
//  PAGE 1  —  TACHOMETER  (gauge2 style)
//  "RPM" top-centre size1
//  value centred size3 y=16
//  bar  x=4,y=52,w=120,h=10
// ═══════════════════════════════════════════════
void drawPage1() {
  // "RPM" centred
  int labelX = (SCREEN_W - 3*6) / 2;
  drawLabel(labelX, 0, "RPM");

  String rStr = String(obd.rpm);
  int tw = rStr.length() * 18;
  display.setTextSize(3);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor((SCREEN_W-tw)/2, 16);
  display.print(rStr);

  // Bar
  display.drawRect(4, 52, 120, 10, SSD1306_WHITE);
  int fillW = map(constrain(obd.rpm,0,8000), 0, 8000, 0, 118);
  if (fillW>0) display.fillRect(5, 53, fillW, 8, SSD1306_WHITE);

  // Redline marker at 6500 rpm
  int redX = 5 + map(6500, 0, 8000, 0, 118);
  display.drawFastVLine(redX, 52, 10, SSD1306_WHITE);
}

// ═══════════════════════════════════════════════
//  PAGE 2  —  ODOMETER + TRIP  (gauge3 style)
//  ODO: 6-digit size2 right-aligned, label "ODO" top-left size1
//  Divider y=33
//  TRIP: label "TRIP" bottom-left size1, value size2 right-aligned
//  Trip = tripDistKm (our accumulated distance)
// ═══════════════════════════════════════════════
void drawPage2() {
  // ODO
  drawLabel(0, 2, "ODO");
  char odoStr[8];
  snprintf(odoStr, sizeof(odoStr), "%06lu", odoKm);
  int odoW = strlen(odoStr)*12;
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(SCREEN_W - odoW, 0);
  display.print(odoStr);

  drawLabel(SCREEN_W - 18, 18, "km");

  drawHLine(33);

  // TRIP
  drawLabel(0, 36, "TRIP");
  char tripStr[8];
  snprintf(tripStr, sizeof(tripStr), "%06.1f", tripDistKm);
  int tripW = strlen(tripStr)*12;
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(SCREEN_W - tripW, 36);
  display.print(tripStr);

  drawLabel(SCREEN_W - 18, 54, "km");
}

// ═══════════════════════════════════════════════
//  PAGE 3  —  VOLTMETER  (gauge4 style)
//  "Voltmeter" label top-left, battery icon top-right
//  "Alto K6A VXT" sub-label
//  Large centred voltage reading
// ═══════════════════════════════════════════════
void drawPage3() {
  drawLabel(0, 0, "Voltmeter");
  drawBatteryIcon(SCREEN_W - 34, 0);

  drawLabel(0, 12, "Alto K6A VXT");

  // Status
  const char *status;
  if      (obd.voltage >= 13.8f) status = "CHARGING";
  else if (obd.voltage <  11.8f) status = "LOW BATT";
  else                            status = "NORMAL  ";
  drawLabel(0, 22, status);

  // Large voltage value centred
  char val[8];
  snprintf(val, sizeof(val), "%.2f", obd.voltage);
  String valStr = String(val);
  int valW = valStr.length() * 18;  // size3
  int vW   = 12;                    // "V" size2
  int totalW = valW + vW;
  int startX = (SCREEN_W - totalW) / 2;
  display.setTextSize(3);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(startX, 34);
  display.print(valStr);
  display.setTextSize(2);
  display.setCursor(startX + valW, 40);
  display.print(F("V"));
}

// ═══════════════════════════════════════════════
//  PAGE 4  —  COOLANT TEMP  (gauge5 arc-needle style)
//  Geometry verified — no clipping:
//   Arc top   = CY-R1 = 58-50 = 8px
//   Arc left  = CX-R1 = 64-50 = 14px  ✓
//   Arc right = CX+R1 = 64+50 = 114px ✓
//   Hub bottom= CY+HUB_R = 58+5 = 63px ✓
//  Ticks at 0,25,50,75,100 → labels INSIDE arc at R2-8
//  Overheat threshold tick at 100°C (right end of arc)
// ═══════════════════════════════════════════════
#define CLT_CX     64
#define CLT_CY     58
#define CLT_R1     50
#define CLT_R2     46
#define CLT_HUB     5
#define CLT_NEEDLE 44
#define CLT_MIN     0
#define CLT_MAX   120   // show up to 120°C

float cltAngle(float t) {
  float n = constrain(t, CLT_MIN, CLT_MAX) / (float)CLT_MAX;
  return PI - n*PI;
}

void drawPage4() {
  // ── Simple text-style coolant temp gauge ──
  // Flashes entire display when at or above overheat threshold

  bool hot = (obd.coolantTemp >= OVERHEAT_C);

  // Flash: invert display every 500ms when overheating
  if (hot) {
    bool flashOn = (millis() / 500) % 2 == 0;
    if (flashOn) {
      display.fillRect(0, 0, SCREEN_W, SCREEN_H, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
  } else {
    display.setTextColor(SSD1306_WHITE);
  }

  // Label
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(F("COOLANT TEMP"));

  drawHLine(10);

  // Big centred temperature
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", obd.coolantTemp);
  String ts = String(buf);
  int tw = ts.length() * 24;   // size4
  display.setTextSize(4);
  display.setCursor((SCREEN_W - tw) / 2, 14);
  display.print(ts);

  // Degree + C, size2, right of number baseline
  display.setTextSize(2);
  display.setCursor((SCREEN_W + tw) / 2 + 2, 22);
  display.print(F("\xF7""C"));

  // Status bar — proportional fill: 0°C = empty, 120°C = full
  float pct = constrain((float)obd.coolantTemp / 120.0f * 100.0f, 0, 100);
  display.setTextColor(SSD1306_WHITE);  // bar always white
  drawBar(0, 48, 128, 8, pct);

  // Range labels under bar
  display.setTextColor(hot ? SSD1306_BLACK : SSD1306_WHITE);
  drawLabel(0,   57, "0");
  drawLabel(55,  57, "60");
  drawLabel(108, 57, "120");

  // Overheat warning text replaces range labels when hot
  if (hot) {
    display.fillRect(0, 57, 128, 7, SSD1306_BLACK);
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(22, 57);
    display.print(F("!! OVERHEAT !!"));
  }
}

// ═══════════════════════════════════════════════
//  PAGE 5  —  AFR GAUGE  (arc needle, LEAN / RICH)
//  Range: 10.0 (rich) … 18.0 (lean)
//  Stoich 14.7 is centre of arc
//  Geometry same as coolant gauge:
//   CX=64, CY=58, R1=50, R2=46, hub=5, needle=44
//  Arc spans PI (left=lean, right=rich) — REVERSED from coolant
//   10.0 (rich) = right = angle 0
//   18.0 (lean) = left  = angle PI
//  Ticks at 10,12,14.7,16,18
//  "LEAN" right side, "RICH" left side — outside arc
//  Numeric AFR centred below arc
//  Needle clamp: keeps 3px from arc ends so it never clips the border
// ═══════════════════════════════════════════════
#define AFR_CX     64
#define AFR_CY     58
#define AFR_R1     50
#define AFR_R2     46
#define AFR_HUB     5
#define AFR_NEEDLE 44
#define AFR_MIN   10.0f
#define AFR_MAX   18.0f

// AFR→angle: 10 (rich)=angle PI (left), 18 (lean)=angle 0 (right)
// So higher AFR (leaner) → needle moves right
float afrAngle(float afr) {
  float n = constrain(afr, AFR_MIN, AFR_MAX);
  n = (n - AFR_MIN) / (AFR_MAX - AFR_MIN);   // 0.0=rich, 1.0=lean
  // lean (1.0) → angle 0 (right), rich (0.0) → angle PI (left)
  float ang = PI - n*PI;
  // Clamp 5° from each end so needle never clips arc border
  ang = constrain(ang, 0.087f, PI-0.087f);
  return ang;
}

void drawPage5() {
  // ── Simple AFR gauge ──
  // Large centred AFR number, RICH/LEAN status, horizontal bar

  drawLabel(0, 0, "AIR FUEL RATIO");
  drawHLine(10);

  // Big centred AFR value
  char afrStr[8];
  snprintf(afrStr, sizeof(afrStr), "%.2f", afrLive);
  String aStr = String(afrStr);
  int tw = aStr.length() * 18;   // size3
  display.setTextSize(3);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor((SCREEN_W - tw) / 2, 13);
  display.print(aStr);

  // Status: RICH / LEAN / STOICH
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  const char *status;
  if      (afrLive < 14.2f) status = "RICH";
  else if (afrLive > 15.2f) status = "LEAN";
  else                       status = "STOICH";
  int sw = strlen(status) * 6;
  display.setCursor((SCREEN_W - sw) / 2, 40);
  display.print(status);

  // Horizontal bar: 10.0=left(rich), 18.0=right(lean), stoich marked
  // Bar: x=0, y=50, w=128, h=8
  // Map AFR 10..18 → 0..126 fill
  float barPct = (afrLive - 10.0f) / 8.0f * 100.0f;
  barPct = constrain(barPct, 0, 100);
  display.drawRect(0, 50, 128, 8, SSD1306_WHITE);
  int fill = (int)(barPct / 100.0f * 126.0f);
  if (fill > 0) display.fillRect(1, 51, fill, 6, SSD1306_WHITE);

  // Stoich marker line at 14.7 → (14.7-10)/8 * 126 = 74px
  int stoichX = 1 + (int)((14.7f - 10.0f) / 8.0f * 126.0f);
  display.drawFastVLine(stoichX, 49, 10, SSD1306_WHITE);

  // End labels
  drawLabel(0,   58, "RICH");
  drawLabel(100, 58, "LEAN");
}

// ═══════════════════════════════════════════════
//  PAGE 6  —  LONG-TERM FUEL AVERAGE (dedicated)
// ═══════════════════════════════════════════════
void drawPage6() {
  drawLabel(0, 0, "TRIP FUEL AVG");
  drawHLine(10);

  // Big avg value
  char buf[10];
  snprintf(buf, sizeof(buf), "%.1f", avgKmL);
  String avgStr = String(buf);
  int tw = avgStr.length() * 24;
  display.setTextSize(4);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor((SCREEN_W - tw) / 2, 14);
  display.print(avgStr);

  drawLabel((SCREEN_W - 4*6) / 2, 47, "km/L");

  // Just trip distance — clean, no fuel volume clutter
  char dist[12];
  snprintf(dist, sizeof(dist), "Trip: %.1f km", tripDistKm);
  int dw = strlen(dist) * 6;
  drawLabel((SCREEN_W - dw) / 2, 56, dist);
}

// ═══════════════════════════════════════════════
//  PAGE 7  —  INSTANT FUEL (dedicated)
// ═══════════════════════════════════════════════
void drawPage7() {
  if (engineOn && obd.rpm > 400) {
    if (obd.speed > 2 && instKmL > 0) {
      drawLabel(0, 0, "INSTANT km/L");
      drawHLine(10);
      char buf[10];
      snprintf(buf, sizeof(buf), "%.1f", instKmL);
      String valStr = String(buf);
      int tw = valStr.length() * 24;
      display.setTextSize(4);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor((SCREEN_W-tw)/2, 14);
      display.print(valStr);
      drawLabel((SCREEN_W-4*6)/2, 47, "km/L");
    } else {
      // Idle — show L/h
      drawLabel(0, 0, "IDLE FUEL USE");
      drawHLine(10);
      char buf[10];
      snprintf(buf, sizeof(buf), "%.2f", instLph);
      String valStr = String(buf);
      int tw = valStr.length() * 24;
      display.setTextSize(4);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor((SCREEN_W-tw)/2, 14);
      display.print(valStr);
      drawLabel((SCREEN_W-3*6)/2, 47, "L/h");
    }
  } else {
    drawLabel(0, 0, "INSTANT FUEL");
    drawHLine(10);
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 28);
    display.print(F("ENGINE OFF"));
  }

  // Hold hint — bottom
  drawLabel(0, 55, "HOLD 4s=RESET");
}

// ═══════════════════════════════════════════════
//  OVERLAYS
// ═══════════════════════════════════════════════
// ─────────────────────────────────────────────
//  Cold start overlay — initial 7s warning
// ─────────────────────────────────────────────
// Shared overlay — bold inverted bar, centred, auto-sizes to fit 128px
// Full screen black overlay — white bold text centred, sized to fill screen
// Each overlay clears the display completely then draws its own content
void drawFullOverlay(const char *line1, const char *line2 = nullptr) {
  display.clearDisplay();  // black screen

  if (line2 == nullptr) {
    // Single line — use size3 (18px wide, 24px tall)
    // Max chars at size3: 128/18 = 7. If longer, drop to size2 (12px).
    int len  = strlen(line1);
    int sz   = (len * 18 <= 122) ? 3 : 2;
    int charW = (sz == 3) ? 18 : 12;
    int charH = (sz == 3) ? 24 : 16;
    int tw   = len * charW;
    int x    = (128 - tw) / 2;
    int y    = (64  - charH) / 2;
    display.setTextSize(sz);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(x, y);
    display.print(line1);
  } else {
    // Two lines — size2 each (12px wide, 16px tall), 4px gap between
    int len1 = strlen(line1);
    int len2 = strlen(line2);
    // If either line too wide for size2, drop both to size1
    int sz    = ((len1 * 12 <= 122) && (len2 * 12 <= 122)) ? 2 : 1;
    int charW = (sz == 2) ? 12 : 6;
    int charH = (sz == 2) ? 16 : 8;
    int gap   = 6;
    int totalH = charH * 2 + gap;
    int y1    = (64 - totalH) / 2;
    int y2    = y1 + charH + gap;
    display.setTextSize(sz);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor((128 - len1 * charW) / 2, y1);
    display.print(line1);
    display.setCursor((128 - len2 * charW) / 2, y2);
    display.print(line2);
  }
}

void drawColdStartOverlay() { drawFullOverlay("Engine", "Cold");       }
void drawColdRpmOverlay()   { drawFullOverlay("Engine", "Cold");       }
void drawHandbrakeOverlay() { drawFullOverlay("Hand", "Brake");        }
void drawOverheatOverlay()  { drawFullOverlay("Engine", "Overheat");   }

void drawDTCOverlay(int idx) {
  display.clearDisplay();
  display.fillRect(0, 0, 128, 13, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(7, 3);
  display.print(F("!! FAULT DETECTED !!"));
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(3);
  display.setCursor(19, 18);
  display.print(dtcs[idx].code);
  drawHLine(50);
  display.setTextSize(1);
  char buf[22];
  snprintf(buf, sizeof(buf), "Code %d/%d  5s each", idx+1, dtcCount);
  display.setCursor(0, 55);
  display.print(buf);
  display.display();
}

// ═══════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println(F("=== LORD ALTO DASHBOARD v5 ==="));

  pinMode(PIN_BTN,     INPUT_PULLUP);
  pinMode(PIN_MCP_INT, INPUT);
#if HANDBRAKE_ACTIVE_HIGH
  pinMode(PIN_HANDBRAKE, INPUT);          // external divider pulls it — no internal pull needed
#else
  pinMode(PIN_HANDBRAKE, INPUT_PULLUP);   // GND switch — internal pull-up
#endif
  // Audio — buzzer pattern player
  audioInit();
  Serial.println(F("[AUDIO] Buzzer ready on GPIO26"));

  // OLED
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("[OLED] FAILED"));
    for(;;) delay(100);
  }
  display.setTextWrap(false);
  Serial.println(F("[OLED] OK"));

  // Load trip + odo from NVS
  loadTrip();

  // ── CAN init ────────────────────────────────
  Serial.println(F("[CAN] Init..."));
  SPI.begin(18, 19, 23, PIN_MCP_CS);
  if (CAN.begin(MCP_ANY, CAN_500KBPS, MCP_8MHZ) == CAN_OK)
    Serial.println(F("[CAN] OK"));
  else
    Serial.println(F("[CAN] FAILED — check wiring/crystal"));
  CAN.setMode(MCP_NORMAL);
  Serial.println(F("[CAN] Normal mode set"));

  // ── Suzuki logo 3s ───────────────────────────
  display.clearDisplay();
  display.drawBitmap(0, 0, suzuki_logo_bits, 128, 64, SSD1306_WHITE);
  display.display();
  Serial.println(F("[BOOT] Suzuki logo"));
  { unsigned long t = millis(); while (millis() - t < 3000) { buzzerTick(); delay(1); } }

  // ── Lord Alto logo + boot pattern ────────────
  display.clearDisplay();
  display.drawBitmap(0, 0, lord_alto_logo_bits, 128, 64, SSD1306_WHITE);
  display.display();
  buzzerPlay(PAT_BOOT, false);
  Serial.println(F("[BOOT] Lord Alto logo + buzzer"));
  { unsigned long t = millis(); while (millis() - t < 3000) { buzzerTick(); delay(1); } }

  // ── Straight to gauges ───────────────────────
  lastPidMs = lastDtcMs = lastNvsSave = lastCanResponseMs = millis();
  Serial.println(F("[LORD ALTO] Running — 8 pages"));
  Serial.println(F("[CAN] Waiting for ECU responses..."));
}

// ═══════════════════════════════════════════════
//  MAIN LOOP  (Core 1)
// ═══════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  canTick();
  fuelTick();
  buzzerTick();

  // ── Button ──────────────────────────────────
  bool btnDown = (digitalRead(PIN_BTN) == LOW);
  if (btnDown) {
    if (!btnWasDown) { btnPressStart=now; btnLongHandled=false; }
    btnWasDown = true;
    if (!btnLongHandled && (now-btnPressStart >= HOLD_RESET_MS)) {
      btnLongHandled = true;
      // Reset available on fuel pages (6 and 7)
      if (currentPage==6 || currentPage==7) {
        resetTrip();
        display.invertDisplay(true); delay(150); display.invertDisplay(false);
      }
    }
  } else {
    if (btnWasDown && !btnLongHandled) {
      currentPage = (currentPage+1) % NUM_PAGES;
      Serial.print(F("[BTN] Page ")); Serial.println(currentPage);
    }
    btnWasDown=false; btnLongHandled=false;
  }

  // ── Engine restart sound ─────────────────────
  // Play bootup sound every time engine turns back on after being off
  // firstBoot flag suppresses it on the very first startup
  if (engineOn && !prevEngineOn) {
    if (!firstBoot) {
      buzzerPlay(PAT_ENGINE_ON, false);
      Serial.println(F("[ENGINE] Restart — buzzer"));
    }
    firstBoot   = false;
    prevEngineOn = true;
  }
  if (!engineOn && prevEngineOn) {
    prevEngineOn = false;
    Serial.println(F("[ENGINE] Off"));
  }

  // ── Cold start overlay (initial 7s on first cold boot) ───────────────
  // Shows once per engine start when temp < 60°C
  // Plays overheat sound once as the alert chime
  if (engineOn && !coldStartShown && obd.coolantTemp < COLD_TEMP_C) {
    coldStartShown    = true;
    coldOverlayActive = true;
    coldOverlayStart  = now;
    buzzerPlay(PAT_COLD, false);
    Serial.println(F("[COLD] Initial cold start overlay"));
  }
  // Reset so it can fire again next engine start
  if (!engineOn) {
    coldStartShown = false;
    if (!engineOn) buzzerStop();
  }
  // Expire after 7s
  if (coldOverlayActive && (now - coldOverlayStart >= COLD_OVERLAY_MS)) {
    coldOverlayActive = false;
    Serial.println(F("[COLD] Initial overlay done"));
  }

  // ── Cold RPM warning (repeating) ─────────────
  // Fires when engine is cold (<60°C) AND rpm goes above 2000
  // Shows 4s overlay with beep, then 5s cooldown, repeats
  if (coldRpmInCooldown && (now - coldRpmCooldown >= COLD_RPM_COOLDOWN_MS)) {
    coldRpmInCooldown = false;
  }
  if (coldRpmWarning && (now - coldRpmStart >= COLD_RPM_OVERLAY_MS)) {
    coldRpmWarning    = false;
    coldRpmInCooldown = true;
    coldRpmCooldown   = now;
    Serial.println(F("[COLD] RPM warning done, cooldown"));
  }
  if (!coldRpmWarning && !coldRpmInCooldown && engineOn
      && obd.coolantTemp < COLD_TEMP_C && obd.rpm > COLD_RPM_LIMIT) {
    coldRpmWarning = true;
    coldRpmStart   = now;
    buzzerPlay(PAT_COLD_RPM, false);
    Serial.println(F("[COLD] High RPM while cold warning"));
  }

  // ── Handbrake warning ───────────────────────
  // Reads brake switch, triggers popup + looping chime when moving
  {
#if HANDBRAKE_ACTIVE_HIGH
    bool brakeUp = (digitalRead(PIN_HANDBRAKE) == HIGH);
#else
    bool brakeUp = (digitalRead(PIN_HANDBRAKE) == LOW);
#endif
    bool shouldWarn = brakeUp && (obd.speed > 0);

    if (shouldWarn && !hbrakeWarning) {
      hbrakeWarning    = true;
      hbrakeSndPlaying = false;
      Serial.println(F("[HANDBRAKE] Warning on"));
    }
    if (!shouldWarn && hbrakeWarning) {
      hbrakeWarning = false;
      if (hbrakeSndPlaying && !overheatActive) {
        audioStop();
      }
      hbrakeSndPlaying = false;
      Serial.println(F("[HANDBRAKE] Cleared"));
    }
    // Start chime if not already playing and overheat isn't using audio
    if (hbrakeWarning && !hbrakeSndPlaying && !overheatActive) {
      buzzerPlay(PAT_HANDBRAKE, true);
      hbrakeSndPlaying = true;
    }
    // Stop chime if overheat takes priority
    if (hbrakeSndPlaying && overheatActive) {
      audioStop();
      hbrakeSndPlaying = false;
    }
  }

  // ── DTC overlay ─────────────────────────────
  if (dtcPending) {
    dtcPending=false; dtcActive=true; dtcShowIdx=0; dtcShowStart=now;
    Serial.print(F("[DTC] ")); Serial.print(dtcCount); Serial.println(F(" code(s)"));
    if (!overheatActive)
      buzzerPlay(PAT_DTC, false);
  }
  if (dtcActive) {
    if (now-dtcShowStart >= 5000UL) {
      if (++dtcShowIdx>=dtcCount) { dtcActive=false; Serial.println(F("[DTC] Done")); }
      else dtcShowStart=now;
    }
    if (dtcActive) { drawDTCOverlay(dtcShowIdx); return; }
  }

  // ── Overheat ────────────────────────────────
  bool isHot = obd.valid && (obd.coolantTemp >= OVERHEAT_C);
  if (isHot && !overheatActive) {
    overheatActive=true; overheatStart=now; overheatSndPlaying=false;
    Serial.println(F("[OVERHEAT] Active!"));
  }
  if (!isHot && overheatActive) {
    overheatActive=false;
    if (overheatSndPlaying) { audioStop(); overheatSndPlaying=false; }
    Serial.println(F("[OVERHEAT] Cleared"));
  }

  bool showOverheat = false;
  if (overheatActive) {
    unsigned long cycle = (now-overheatStart)%(OVERHEAT_SHOW_MS+OVERHEAT_PAUSE_MS);
    showOverheat = (cycle < OVERHEAT_SHOW_MS);
    if (showOverheat && !overheatSndPlaying) {
      buzzerPlay(PAT_OVERHEAT, true);
      overheatSndPlaying=true;
    }
    if (!showOverheat && overheatSndPlaying) { audioStop(); overheatSndPlaying=false; }
  }

  // ── Render ──────────────────────────────────
  display.clearDisplay();
  switch(currentPage) {
    case 0: drawPage0(); break;
    case 1: drawPage1(); break;
    case 2: drawPage2(); break;
    case 3: drawPage3(); break;
    case 4: drawPage4(); break;
    case 5: drawPage5(); break;
    case 6: drawPage6(); break;
    case 7: drawPage7(); break;
  }
  drawPageDots();
  if (coldOverlayActive)  drawColdStartOverlay();
  if (coldRpmWarning)     drawColdRpmOverlay();
  if (hbrakeWarning)      drawHandbrakeOverlay();
  if (showOverheat)       drawOverheatOverlay();
  display.display();
}
