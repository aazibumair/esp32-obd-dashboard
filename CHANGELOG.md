# Changelog

All notable changes to the ESP32 OBD2 Dashboard project.

---

## [v3.0] — Wired CAN + Buzzer (Current Stable)

### Why we switched
DAC audio on GPIO25 suffered from persistent RF noise caused by the ESP32 WiFi/Bluetooth clock coupling into the DAC output. Multiple hardware fixes were attempted — coupling capacitors, resistor dividers, grounding improvements, dual USB power separation — but noise could not be fully eliminated. Switched to an active buzzer which has zero noise issues and is simpler to wire.

### Changed
- **Replaced entire DAC audio system with KXG1203C active buzzer**
  - Old: ESP32 DAC2 (GPIO25) → PAM8403 amplifier → speaker
  - New: ESP32 GPIO26 → 1kΩ resistor → BC547 NPN transistor → KXG1203C buzzer
  - Reason: DAC noise from ESP32 WiFi/BT clock coupling was unresolvable in hardware
- Buzzer pattern player replaces audio file playback — no more `sounds.h` dependency
- Boot sequence simplified — logos play once with no ECU-wait loop
- All warning overlays redesigned as full-screen black with large centred white text

### Added
- `buzzer.h` — non-blocking pattern player using `BuzzStep` struct arrays
- 7 distinct buzzer patterns: boot chime, engine restart, overheat, handbrake, cold start, cold RPM, DTC fault
- **Handbrake warning** — GPIO34 input, triggers overlay + looping buzzer when speed > 0
- **Cold start warning** — shows "Engine Cold" overlay for 7s on first start below 60°C
- **Cold RPM warning** — warns when RPM > 2000 while coolant < 60°C, 4s overlay, 5s cooldown, repeats
- **Engine restart chime** — plays boot pattern every time engine turns back on after being off
- MCP2515 3.3V modification documentation added to README

### Fixed
- Boot skip bug — if ECU was already running when ESP32 powered on, logos were skipped in milliseconds
- Handbrake logic polarity corrected for GND switch with 1N4148 diode wiring
- Warning overlays no longer clip text at screen edges — auto-scales between size2 and size1

### Known Issues
- Long-term fuel average needs per-engine calibration — MAP-based MAF estimation tuned for K6A
- Cold start warning triggers even on warm restart if ECU briefly reports temp below threshold
- Handbrake warning does not work in reverse — OBD2 speed PID (0x0D) always returns positive value

### Hardware
- ESP32 WROOM-32
- MCP2515 CAN module (modified for 3.3V)
- SSD1306 0.96" OLED I2C
- KXG1203C active buzzer 5V
- BC547 NPN transistor + 1kΩ resistor + 1N4148 diode
- Dual USB 12V→5V step-down module

---

## [v2.0] — Wired CAN + DAC Speaker

### Why we switched from v1
Bluetooth ELM327 communication was too slow and unreliable for a real-time dashboard. Each PID read took 300-800ms meaning all gauges were running on stale data. Direct CAN communication via MCP2515 brought this down to ~60ms per full PID cycle — a 10x improvement. Also eliminated all Bluetooth pairing and reconnection issues permanently.

### Changed
- **Replaced ELM327 Bluetooth with direct MCP2515 CAN bus**
  - Old: ESP32 Bluetooth → ELM327 adapter → OBD port
  - New: ESP32 SPI → MCP2515 → CAN bus → OBD port directly
  - PID read time: 300-800ms → ~60ms
- OBD communication rewritten from AT command parsing to raw CAN frame handling
- Audio system upgraded from FreeRTOS task with `ets_delay_us` to hardware Timer 1 ISR
  - FreeRTOS task caused slow-motion audio playback due to scheduling jitter
  - Hardware timer fires at exact sample intervals with no jitter

### Added
- 8 gauge pages: Speedometer, Tachometer, Odometer+Trip, Voltmeter, Coolant Temp, AFR, Long-term Fuel, Instant Fuel
- Odometer persistence via ESP32 NVS — survives power cycles
- Engine off detection via 3s CAN timeout — freezes fuel calculations when engine stops
- Overheat overlay — fires at 100°C, 10s on / 10s off cycle, looping audio
- DTC fault overlay — full screen, 5s per code, beep on detection
- Fuel calculations: MAP-based MAF estimation, STFT+LTFT blending for live AFR
- Boot logos: Suzuki logo 3s → Lord Alto logo + audio 3s
- `sounds.h` audio conversion pipeline: WAV → 8-bit unsigned PCM → hex arrays
  - Files must be stripped of headers before hex conversion
  - 20ms fade in/out applied to eliminate DAC click/pop
  - Format: 1ch, 8-bit, 22050Hz, centred at value 128

### Known Issues
- DAC audio (GPIO25) picks up RF noise from ESP32 WiFi/Bluetooth clock — audible hiss
- Hardware fixes attempted: coupling capacitors, resistor dividers, grounding, dual USB power — partially successful
- Audio plays slow-motion on first flash until Timer ISR is confirmed working on specific board

### Hardware
- ESP32 WROOM-32
- MCP2515 CAN module (**must be modified for 3.3V logic** — see README)
- SSD1306 0.96" OLED I2C
- HW-104 PAM8403 amplifier module
- Dual USB 12V→5V step-down module

---

## [v1.0] — Bluetooth ELM327 (Legacy)

### Overview
First working version of the dashboard. Uses a knockoff ELM327 Bluetooth OBD adapter to communicate with the car ECU wirelessly. The ESP32 connects to the adapter over Bluetooth Classic, sends AT commands, and parses the text responses to extract PID values.

### Features
- Basic gauge pages: Speed, RPM, Coolant Temp, Voltage, Fuel Economy
- DAC audio via PAM8403 amplifier
- Bluetooth Classic connection with silent auto-reconnect
- ECU ready detection — waits for valid "41" response before showing gauges
- BT restart after 5 consecutive failed connection attempts
- Skips ATZ reset command — required for knockoff ELM327 compatibility

### Known Issues
- **Slow** — 300-800ms per PID read, all gauges run on stale data
- **Connection instability** — often requires 2-3 ignition cycles to establish connection
- **Data freeze** — silent reconnection hides the problem rather than fixing it
- **Knockoff ELM327 compatibility** — genuine adapters work better but cost more
- No handbrake warning, no cold start warning, no DTC overlay
- No odometer persistence

### Hardware
- ESP32 WROOM-32
- ELM327 Bluetooth OBD2 adapter (knockoff)
- SSD1306 0.96" OLED I2C
- HW-104 PAM8403 amplifier module

### Important Note for Users
You must replace the MAC address in the code with your own ELM327 adapter's Bluetooth MAC address. Every adapter has a different MAC. To find yours: pair the ELM327 to your phone and check the Bluetooth device details in your phone settings.

---

## Notes on Audio Conversion (v1.0 and v2.0)

For anyone wanting to use custom audio with the DAC speaker versions:

1. Start with a WAV file — mono, 8-bit, 22050Hz. Convert in Audacity if needed.
2. Do not hexdump the WAV file directly — this embeds the file header into the data and the ESP32 will play it as audio causing gibberish at the start.
3. Open the WAV in a hex editor or use a script to skip the 44-byte header and extract only the raw PCM samples.
4. The raw bytes should be 8-bit unsigned values centred around 128 (silence = 128, max positive = 255, max negative = 0).
5. Add a 20ms fade in and fade out by blending each sample toward 128 at the start and end — this eliminates the click/pop when the DAC transitions.
6. Wrap the byte array in a `const unsigned char name[] PROGMEM = { ... };` declaration with matching `_length` and `_samplerate` constants.
7. A Python conversion script is included in the `tools/` folder of the repo.
