# Word clock firmware - ESP32-C3 (Arduino C++)

## Build and flash
Arduino IDE: install the "esp32" board package (Espressif) and the library "Adafruit NeoPixel".
Open `wordclock/wordclock.ino` (keep all four files in that folder). Board: **ESP32C3 Dev Module**, **USB CDC On Boot: Enabled**.
Upload. If the port doesn't show up, hold BOOT while plugging the SuperMini in.

PlatformIO: board `esp32-c3-devkitm-1`, `lib_deps = adafruit/Adafruit NeoPixel`, build flag `-DARDUINO_USB_CDC_ON_BOOT=1` (see `platformio.ini`).

Wiring: 5 V and GND to the panel, GPIO3 -> DIN (change `LED_PIN` at the top of the sketch). Common ground.
If the LEDs flicker, use a 3.3 V -> 5 V level shifter.

## First start
1. No WiFi saved: the top-left LED breathes **orange** and the clock creates the open network **WordClock-Setup**.
   Join it from a phone; the settings page opens (or browse to 192.168.4.1). Enter your WiFi and Save. It restarts.
2. Blue breathing dot = connecting / waiting for the time. Once NTP answers the clock appears (with the spin).
3. Afterwards open **http://wordclock.local/** (or the IP) to change anything. Settings are stored on the chip.
   If the saved WiFi can't be reached the clock falls back to the setup network and keeps retrying in the background.

## Settings page
* **Time zone**: POSIX string, DST automatic (presets in the drop-down, e.g. UK `GMT0BST,M3.5.0/1,M10.5.0`).
* **Panel**: letter plate (Andy / Gurgle - the supplied front_frame.stl is Gurgle), wiring (zigzag / progressive), rotate, flips.
  Press **Test pattern**: one pixel should run left to right, row by row from the top. Fix with Wiring / Rotate / Flip, Save, repeat.
  Changes apply immediately, no restart.
* **Look**: day brightness %, colour theme (Rainbow, Solid colour with a picker, Warm white, Fire, Ocean, Aurora, By word group, Time of day), rainbow and shimmer cycle times, ghost letters, word-change transition (None, Slot-machine spin, Cross-fade, Sparkle, Ripple, Shuffle) and its length, spin length, hourly flourish.
* **Night mode**: from/until (can cross midnight), Dim or Off, dim level %, fade minutes, transitions at night, and Night colours (Same or Red only).
* **Weather**: optional - the * cell shows the conditions or the temperature (Gurgle plate; it glows steadily if the seconds marker is off, otherwise the marker takes the weather colour), or tint all the words by conditions or by temperature (api.open-meteo.com, no account, checked every 15 minutes; set your latitude and longitude). Off by default, so nothing goes to the internet unless you turn it on.

## Updating over WiFi
Arduino IDE: Tools > Port > the "wordclock" network port. No password is set, so only use this on a trusted network.

## Files
`wordclock.ino` (hardware, web page), `clocklogic.h` (words, night, mapping, spin - no hardware calls), `grids.inc` (letter tables).

## Notes
* Tested: the logic header was compiled and run on a PC and matches the earlier Python version exactly (every minute of the day on both grids, the night curve, all 32 wiring/rotation/flip combinations; DST handling is the C library's). The sketch itself was syntax-checked only; it has not been run on a C3.
* `LOW_TX_POWER` (default on) lowers WiFi power; several SuperMini boards can't hold a connection at full power. Set to 0 if you have a good board and want the range.
* Dim levels below about 20 % of day brightness make the colours band. Dark LEDs still draw about 64 mA.
* Update the POSIX string if your country changes its DST rules.

## Seconds marker
On the Gurgle plate the * cell is not part of any word and can mark the seconds (web page, Look section): off; blink once per second; soft pulse once per second; or 1-4 quick flashes every second (1 for 0-14 s, 2 for 15-29 s, 3 for 30-44 s, 4 for 45-59 s). It follows the rainbow colour and night dimming. The Andy plate has no spare cell, so the option does nothing there.

## Effects
* **Ghost letters** (%): unlit letters glow faintly, as a percentage of the lit brightness (try 2-5). At low brightness the LEDs only have a few steps, so the lowest visible level is the dimmest step.
* **Transitions**: slot-machine spin (the original), cross-fade, sparkle (letters pop off and on at random with a white flash), ripple (a ring spreads from the centre), or shuffle between fade, sparkle and ripple. Settings saved by the older firmware keep working: "spin on" becomes the spin transition.
* **Hourly flourish**: a rainbow wave sweeps across the whole grid for four seconds on the hour (and half hour if chosen). Not played during night mode unless "Transitions at night" is on.
* **Night colours = Red only**: during night mode the colours slide to red as the clock dims.
* **Weather tint** runs in a background task so the animation never stalls. If the lookup fails it retries after a minute.
