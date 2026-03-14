# Changelog

All notable changes to the ESP32 OBD2 Dashboard project.

---

## [v2.0] — Buzzer Version (Current)

### Changed
- **Replaced entire DAC audio system with KXG1203C active buzzer**
  - Old system: ESP32 DAC2 (GPIO25) → PAM8403 amplifier → speaker
  - New system: ESP32 GPIO26 → BC547 transistor → 5V buzzer
  - Reason: DAC audio suffered from persistent radio-frequency noise caused by ESP32 WiFi/Bluetooth clock coupling into GPIO25. Multiple hardware fixes (capacitors, resistors, grounding) could not fully eliminate it.
- Buzzer patterns replace audio files — no more `sounds.h` dependency
- Removed `driver/dac.h`, `esp_wifi.h`, `esp_bt.h` includes
- Boot sequence simplified — logos play once, straight to gauges, no ECU-wait loop

### Added
- `buzzer.h` — pattern definitions as `BuzzStep` struct arrays
- Non-blocking buzzer pattern player (`buzzerTick()` called every loop)
- Distinct patterns per event: boot chime, engine restart, overheat, handbrake, cold start, cold RPM, DTC fault
- BC547 transistor driver circuit for buzzer (ESP32 GPIO max 12mA, buzzer needs 30mA+)

### Fixed
- Cold start warning now plays once and stops (was looping)
- Boot audio no longer plays as one long beep (blocking `delay()` replaced with `buzzerTick()` loop)
- Handbrake logic polarity corrected (ACTIVE_HIGH 0 for GND switch + 1N4148 diode)
- Overlay text auto-scales to fit 128px screen width without clipping
- Full screen overlays replace small banner overlays

---

## [v1.1] — DAC Audio Fixes

### Fixed
- Boot skip bug: if car ECU was already running when ESP32 powered on, logo sequence was skipped in milliseconds. Fixed by completing both logos fully before checking for ECU response.
- Slow-motion audio: replaced FreeRTOS task + `ets_delay_us()` with hardware Timer 1 ISR for sample-accurate playback
- DAC noise: added `dac_output_disable()` between sounds to cut amplifier noise floor
- Killed WiFi and Bluetooth radio clocks (`esp_wifi_stop()`, `esp_bt_controller_disable()`) to reduce DAC coupling noise

### Changed
- Audio files reconverted from original WAVs — previous `sounds.h` had MP3/WAV headers embedded in raw PCM data causing gibberish playback
- Added 20ms fade in/out on all audio samples to eliminate click/pop at start and end

---

## [v1.0] — Initial Release (Speaker Version)

### Hardware
- ESP32 WROOM-32
- MCP2515 CAN module (modified for 3.3V ESP32 logic compatibility)
- SSD1306 0.96" OLED 128×64 I2C
- HW-104 PAM8403 amplifier module
- Dual USB 12V→5V step-down power module

### Features
- 8 gauge pages: Speedometer, Tachometer, Odometer+Trip, Voltmeter, Coolant Temp, AFR, Long-term Fuel, Instant Fuel
- Boot sequence: Suzuki logo (3s) → Lord Alto logo + audio (3s) → gauges
- Warning overlays: Overheat, Handbrake, Engine Cold, Cold RPM, DTC fault codes
- Odometer persistence via ESP32 NVS (survives power cycles)
- Engine off detection via CAN timeout (3 seconds)
- Fuel calculations: MAP-based MAF estimation, STFT+LTFT blending for AFR
- Hardware timer ISR for audio playback (replaces FreeRTOS task)

### Known Issues in v1.0
- DAC audio noise from WiFi/BT clock coupling — hardware attempts to fix partially successful
- Cold start warning had no warm-engine check — would trigger even if engine was already warm
- Fuel average needs per-engine calibration

---

## Notes on the MCP2515 Modification

The stock MCP2515 module uses 5V logic. Connecting it directly to the ESP32 (3.3V GPIO) risks damaging the microcontroller. The modification involves running the MCP2515 chip at 3.3V while keeping the CAN transceiver at 5V for bus compatibility. This modification was done manually and is visible in the project photos.
