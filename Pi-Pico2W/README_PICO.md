# Word clock - Raspberry Pi Pico 2 W

Port of the ESP32 word clock firmware v5.1. Same web settings page, themes, transitions, flourish, night mode,
seconds marker and weather on the *.

## Build (Arduino IDE)
1. Boards Manager: install **"Raspberry Pi Pico/RP2040/RP2350"** by Earle F. Philhower, III (add the URL from its README if it is not listed).
2. Library Manager: **Adafruit NeoPixel**, version 1.12.4 or newer.
3. Open `wordclock_pico.ino` (keep `clocklogic.h` and `grids.h` in the same folder).
4. Tools: Board **Raspberry Pi Pico 2 W**, Flash Size **4MB (Sketch: 3MB, FS: 1MB)**, IP stack IPv4 only.
5. First flash: hold BOOTSEL while plugging in, then Upload. Later uploads can go over WiFi (Tools > Port > network port).

PlatformIO: `platformio.ini` is included (`pio run -t upload`).

## Wiring
Panel DIN to **GP2** (change `LED_PIN` at the top of the sketch for another GPIO), 5 V and GND to the panel, and join the Pico's GND to the panel's GND.
The data line is 3.3 V. A 330 ohm resistor in series with it, close to the panel, is a good idea.

## First run
No saved WiFi: the clock opens an access point called **WordClock-Setup**. Join it, a setup page appears (or browse to 192.168.4.1), enter your WiFi and Save.
Afterwards use http://wordclock.local/ or the IP printed on the serial monitor (press any key to print it again).
Then set **Letter plate** (Andy or Gurgle), and use **Test pattern** to set Wiring, Rotate and the flips.

## Differences from the ESP32 version
* **Settings** are saved in the emulated EEPROM instead of Preferences (a small shim in the sketch). Settings from an ESP32 do not carry over.
* **Setup mode is an access point only.** The Pico's WiFi chip cannot be an access point and a station at the same time. If a network is saved but unreachable at boot (router still starting), the clock offers the setup access point and reboots every 10 minutes to try the saved network again (`AP_RETRY_MS`).
* If the router goes away while running, the clock keeps its time and rejoins automatically.
* **Weather** is a short blocking request (a second or two) every 15 minutes, made only when nothing is animating. The display holds its last frame during it. Set `ENABLE_WEATHER` to 0 to remove it (saves about 85 KB).
* OTA needs the filesystem part of the flash layout (step 4); without it the OTA upload will fail.

## Tested
Compiled and linked against arduino-pico 6.1 (mDNS uses SimpleMDNS, which ArduinoOTA also uses; LEAmDNS clashes with it in the Arduino IDE) (Pico 2 W board, with and without weather): about 550 KB of the 3 MB sketch space and 81 KB of RAM.
The settings store was run on a PC. It has NOT been run on a Pico 2 W: WiFi, NTP, OTA, the setup access point and LED timing are untested on hardware.

## v5.2 additions
* Serial: the sketch waits up to 3 s at start for the USB serial monitor, and prints an `alive ...` status line every 30 s (WiFi up/down, IP, time set or not).
