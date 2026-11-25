/*
  Word clock - Raspberry Pi Pico 2 W (RP2350), 8x8 WS2812B.  Arduino-Pico core by Earle Philhower.
  Port of the ESP32 word clock firmware v5.2: rainbow wheel, per-pixel shimmer, slot-machine spin, themes,
  word-change transitions, hourly flourish, seconds marker / weather on the * cell, night mode,
  POSIX time zone with automatic DST, web settings page, WiFi setup access point, OTA updates.

  Core:    https://github.com/earlephilhower/arduino-pico  (Boards Manager: "Raspberry Pi Pico/RP2040/RP2350")
  Board:   "Raspberry Pi Pico 2 W"
  Flash:   "4MB (Sketch: 3MB, FS: 1MB)"  - OTA needs some filesystem space to stage the new firmware
  Library: "Adafruit NeoPixel" (Library Manager), version 1.12.4 or newer for RP2350.

  Differences from the ESP32 version (see README_PICO.md):
   * Settings are kept in the emulated EEPROM (a small Preferences shim below keeps the rest of the code identical).
   * The weather lookup is a short blocking call between frames (no background task), and can be compiled out.
   * The Pico's WiFi chip cannot be an access point and a station at once: setup mode is an access point only.
*/
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <sys/time.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <SimpleMDNS.h>                      // ArduinoOTA uses this one internally; LEAmDNS would clash with it
#include <ArduinoOTA.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>
#include <map>
#include "clocklogic.h"

#ifndef LED_PIN
#define LED_PIN 27              // GPIO number (GP2) of the panel DIN; any free GPIO works
#endif
#ifndef ENABLE_WEATHER
#define ENABLE_WEATHER 1       // set to 0 to compile the weather lookup out
#endif
#define AP_RETRY_MS (10UL * 60UL * 1000UL)   // in setup mode with a saved network, reboot this often to try it again

using namespace wc;

// ----------------------------------------------------------------------------- settings
struct Settings {
  String ssid, pass, host = "wordclock";
  String tz = "GMT0BST,M3.5.0/1,M10.5.0";
  String ntp = "pool.ntp.org";
  int grid = 1;               // 0 = andy, 1 = gurgle
  int layout = 0;             // 0 = zigzag, 1 = progressive
  int rotate = 0;
  bool flipX = false, flipY = false;
  float bright = 12.5f;       // % of full brightness (12.5 == luma contrast 32)
  float huePeriod = 20.0f, shimmerPeriod = 5.0f;
  float spinSec = 6.0f;                 // length of the slot-machine spin
  int trans = 1;                        // 0 none, 1 spin, 2 fade, 3 sparkle, 4 ripple, 5 shuffle (fade/sparkle/ripple)
  float transSec = 1.5f;                // length of fade / sparkle / ripple
  float ghost = 0.0f;                   // unlit letters glow at this % of the lit brightness (0 = off)
  int theme = 0;                        // see TH_* in clocklogic.h
  String color = "#ff6a00";             // used by the Solid theme
  int flourish = 0;                     // 0 off, 1 on the hour, 2 hour and half hour
  bool nightRed = false;                // night: shift to red
  int weather = 0;                      // 0 off, 1 tint by conditions, 2 tint by temperature
  float lat = 51.36f, lon = 1.10f;       // for the weather lookup
  bool nightOn = true;
  String nightStart = "22:00", nightEnd = "08:00";
  int nightMode = 0;          // 0 = dim, 1 = off
  float dimPct = 25.0f;
  float fadeMin = 2.0f;
  bool nightSpin = false;
  int star = 0;               // seconds marker on the * cell: 0 off, 1 blink, 2 soft pulse, 3 quarter-minute flashes
};

Settings S;

// Minimal stand-in for the ESP32 Preferences class, stored as key/value pairs in the emulated EEPROM (flash).
// Same calls as before, so loadSettings()/saveSettings() are unchanged.
class Preferences {
  static const int SZ = 2048;
  std::map<String, String> kv;
  bool rw = false;
 public:
  bool begin(const char*, bool readOnly) {
    rw = !readOnly; kv.clear();
    EEPROM.begin(SZ);
    if (EEPROM.read(0) == 'W' && EEPROM.read(1) == 'C' && EEPROM.read(2) == 1) {
      int len = EEPROM.read(3) | (EEPROM.read(4) << 8);
      if (len > SZ - 5) len = 0;
      String k, v; bool inKey = true;
      for (int i = 0; i < len; i++) {
        char c = (char)EEPROM.read(5 + i);
        if (c) { (inKey ? k : v) += String(c); continue; }
        if (inKey) inKey = false; else { kv[k] = v; k = ""; v = ""; inKey = true; }
      }
    }
    return true;
  }
  void end() {
    if (rw) {
      int pos = 5;
      for (auto& p : kv) {
        if (pos + (int)p.first.length() + (int)p.second.length() + 2 > SZ) break;   // never overflow
        for (unsigned i = 0; i < p.first.length(); i++) EEPROM.write(pos++, p.first[i]);
        EEPROM.write(pos++, 0);
        for (unsigned i = 0; i < p.second.length(); i++) EEPROM.write(pos++, p.second[i]);
        EEPROM.write(pos++, 0);
      }
      EEPROM.write(0, 'W'); EEPROM.write(1, 'C'); EEPROM.write(2, 1);
      EEPROM.write(3, (pos - 5) & 255); EEPROM.write(4, (pos - 5) >> 8);
      EEPROM.commit();
    }
    EEPROM.end();
    rw = false;
  }
  String getString(const char* k, const String& d) { auto it = kv.find(k); return it == kv.end() ? d : it->second; }
  int getInt(const char* k, int d) { auto it = kv.find(k); return it == kv.end() ? d : it->second.toInt(); }
  bool getBool(const char* k, bool d) { auto it = kv.find(k); return it == kv.end() ? d : it->second.toInt() != 0; }
  float getFloat(const char* k, float d) { auto it = kv.find(k); return it == kv.end() ? d : it->second.toFloat(); }
  void putString(const char* k, const String& v) { kv[k] = v; }
  void putInt(const char* k, int v) { kv[k] = String(v); }
  void putBool(const char* k, bool v) { kv[k] = v ? "1" : "0"; }
  void putFloat(const char* k, float v) { kv[k] = String(v, 6); }
};
Preferences prefs;

void loadSettings() {
  prefs.begin("wc", true);
  S.ssid = prefs.getString("ssid", S.ssid);       S.pass = prefs.getString("pass", S.pass);
  S.host = prefs.getString("host", S.host);       S.tz = prefs.getString("tz", S.tz);
  S.ntp = prefs.getString("ntp", S.ntp);
  S.grid = prefs.getInt("grid", S.grid);          S.layout = prefs.getInt("layout", S.layout);
  S.rotate = prefs.getInt("rotate", S.rotate);
  S.flipX = prefs.getBool("flipx", S.flipX);      S.flipY = prefs.getBool("flipy", S.flipY);
  S.bright = prefs.getFloat("bright", S.bright);
  S.huePeriod = prefs.getFloat("hue", S.huePeriod);
  S.shimmerPeriod = prefs.getFloat("shim", S.shimmerPeriod);
  S.spinSec = prefs.getFloat("spinsec", S.spinSec);
  S.trans = prefs.getInt("trans", -1);
  if (S.trans < 0) S.trans = prefs.getBool("spin", true) ? 1 : 0;   // settings saved by the older firmware
  S.transSec = prefs.getFloat("tsec", S.transSec);  S.ghost = prefs.getFloat("ghost", S.ghost);
  S.theme = prefs.getInt("theme", S.theme);         S.color = prefs.getString("color", S.color);
  S.flourish = prefs.getInt("flour", S.flourish);   S.nightRed = prefs.getBool("nred", S.nightRed);
  S.weather = prefs.getInt("wx", S.weather);
  S.lat = prefs.getFloat("lat", S.lat);             S.lon = prefs.getFloat("lon", S.lon);
  S.nightOn = prefs.getBool("non", S.nightOn);
  S.nightStart = prefs.getString("nstart", S.nightStart);
  S.nightEnd = prefs.getString("nend", S.nightEnd);
  S.nightMode = prefs.getInt("nmode", S.nightMode);
  S.dimPct = prefs.getFloat("ndim", S.dimPct);    S.fadeMin = prefs.getFloat("nfade", S.fadeMin);
  S.nightSpin = prefs.getBool("nspin", S.nightSpin);
  S.star = prefs.getInt("star", S.star);
  prefs.end();
}

void saveSettings() {
  prefs.begin("wc", false);
  prefs.putString("ssid", S.ssid);  prefs.putString("pass", S.pass);
  prefs.putString("host", S.host);  prefs.putString("tz", S.tz);   prefs.putString("ntp", S.ntp);
  prefs.putInt("grid", S.grid);     prefs.putInt("layout", S.layout); prefs.putInt("rotate", S.rotate);
  prefs.putBool("flipx", S.flipX);  prefs.putBool("flipy", S.flipY);
  prefs.putFloat("bright", S.bright);
  prefs.putFloat("hue", S.huePeriod); prefs.putFloat("shim", S.shimmerPeriod);
  prefs.putFloat("spinsec", S.spinSec);
  prefs.putInt("trans", S.trans);   prefs.putFloat("tsec", S.transSec);  prefs.putFloat("ghost", S.ghost);
  prefs.putInt("theme", S.theme);   prefs.putString("color", S.color);
  prefs.putInt("flour", S.flourish); prefs.putBool("nred", S.nightRed);
  prefs.putInt("wx", S.weather);
  prefs.putFloat("lat", S.lat);     prefs.putFloat("lon", S.lon);
  prefs.putBool("non", S.nightOn);
  prefs.putString("nstart", S.nightStart); prefs.putString("nend", S.nightEnd);
  prefs.putInt("nmode", S.nightMode); prefs.putFloat("ndim", S.dimPct); prefs.putFloat("nfade", S.fadeMin);
  prefs.putBool("nspin", S.nightSpin);
  prefs.putInt("star", S.star);
  prefs.end();
}

// ----------------------------------------------------------------------------- state
Adafruit_NeoPixel strip(64, LED_PIN, NEO_GRB + NEO_KHZ800);
WebServer server(80);
DNSServer dns;
Night night;
Spinner spinner;
uint8_t lut[64];                 // logical pixel (y*8+x) -> LED index
WordSet current, prevSet;
WordSet emptySet = { { nullptr, nullptr, nullptr, nullptr }, 0 };
bool haveCurrent = false;
bool transActive = false;
int transKind = TR_FADE;
uint32_t transStart = 0, transSeed = 1;
Col customColor = { 255, 106, 0 };
float wxTemp = 0; int wxCode = 0; bool wxValid = false, wxRefresh = false;
bool apMode = false, restartPending = false, selftestRequested = false;
uint32_t lastFrame = 0, frameInterval = 50, restartAt = 0, lastWifiCheck = 0, pulseStep = 0, apSince = 0, staLostSince = 0;

double nowSec() { return time_us_64() / 1e6; }
bool timeValid() { return time(nullptr) > 1700000000; }

int parseHHMM(const String& s, int fallback) {
  int c = s.indexOf(':');
  if (c < 1) return fallback;
  int h = s.substring(0, c).toInt(), m = s.substring(c + 1).toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59) return fallback;
  return h * 60 + m;
}

bool staServices = false;
String ntpUsed;
void startNtp() {                                 // needs the network up; sets the system clock
  if (ntpUsed == S.ntp) return;
  NTP.begin(S.ntp.c_str());
  ntpUsed = S.ntp;
}

void applySettings() {
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++)
      lut[y * 8 + x] = ledIndex(x, y, S.layout, S.rotate, S.flipX, S.flipY);
  night.enabled = S.nightOn;
  night.startMin = parseHHMM(S.nightStart, 22 * 60);
  night.endMin = parseHHMM(S.nightEnd, 7 * 60);
  night.level = S.nightMode == 1 ? 0.0f : S.dimPct / 100.0f;
  night.fadeMin = S.fadeMin;
  night.spin = S.nightSpin;
  spinner.seconds = S.spinSec < 0.5f ? 0.5f : S.spinSec;
  if (S.color.length() == 7 && S.color[0] == '#') {
    long v = strtol(S.color.c_str() + 1, nullptr, 16);
    customColor = { (uint8_t)((v >> 16) & 255), (uint8_t)((v >> 8) & 255), (uint8_t)(v & 255) };
  }
  setenv("TZ", S.tz.c_str(), 1);                 // POSIX TZ: DST handled by the C library
  tzset();
  if (staServices) startNtp();                   // a changed NTP server is picked up straight away
  haveCurrent = false;
}

// ----------------------------------------------------------------------------- seconds marker
// The * cell of the Gurgle plate (row 2, column 2 = pixel 18) is not part of any word, so it is free to act as a seconds marker.
float starLevel = 0.0f;                          // 0..1, updated once per frame
int starPixel() { return S.grid == 1 ? 18 : -1; }   // the Andy plate has no spare cell

void updateStar(int sec, int ms) {
  starLevel = (S.star == 0 && S.weather >= 3 && wxValid) ? 1.0f : 0.0f;   // weather * with no seconds marker: steady glow
  switch (S.star) {
    case 1: starLevel = ms < 500 ? 1.0f : 0.0f; break;                          // blink once per second
    case 2: starLevel = 0.5f * (1.0f + cosf(ms * (6.2831853f / 1000.0f))); break; // soft pulse once per second
    case 3: {                                                                    // 1..4 quick flashes each second: 0-14 s = 1, 15-29 s = 2, 30-44 s = 3, 45-59 s = 4
      int flashes = sec / 15 + 1;
      for (int i = 0; i < flashes; i++) if (ms >= i * 200 && ms < i * 200 + 100) starLevel = 1.0f;
      break;
    }
  }
}

// ----------------------------------------------------------------------------- drawing
struct Ctx {                      // per-frame values shared by all drawing code
  float bright = 0, minutes = 0, tod = 0.5f, nightMix = 0;
  double el = 0;
  bool flourishOn = false; float flourishU = 0;
} cx;

uint8_t buf[64][3];               // the frame being built, logical pixel order (y*8+x)

Col tinted(Col c) {               // weather tint, then night red
  if ((S.weather == 1 || S.weather == 2) && wxValid) c = lerpRGB(c, S.weather == 1 ? weatherColor(wxCode) : tempColor(wxTemp), 0.45f);
  if (S.nightRed && cx.nightMix > 0.001f) c = nightRed(c, cx.nightMix);
  return c;
}

// full-scale colours (0-255) for every pixel of a word set; in[p] marks the pixels that belong to it
void colorize(const WordSet& ws, Col* col, bool* in) {
  for (int p = 0; p < 64; p++) in[p] = false;
  int hue = (int)(cx.el / S.huePeriod * 256.0) & 255;
  int shim = (int)(cx.el / S.shimmerPeriod * 256.0) & 255;
  int idx = 0;
  for (uint8_t i = 0; i < ws.n; i++) {
    int group = wordGroup((int)(ws.w[i] - GRID_DATA[S.grid ? 1 : 0]));
    for (uint8_t k = 0; k < ws.w[i]->n; k++) {
      int p = ws.w[i]->p[k];
      float wave = 0.5f + 0.5f * sinf(6.2831853f * ((float)(cx.el / S.shimmerPeriod) + idx * 0.13f));
      col[p] = tinted(themeColor(S.theme, wave, hue + idx * 8 + shim, customColor, group, cx.tod));
      in[p] = true;
      idx++;
    }
  }
}

void put(int p, float r, float g, float b) { buf[p][0] = u8(r); buf[p][1] = u8(g); buf[p][2] = u8(b); }

// overlays, then out to the strip
void present() {
  if (cx.bright > 0.001f) {
    if (cx.flourishOn)                                       // hourly flourish: rainbow wave over the whole grid
      for (int p = 0; p < 64; p++) {
        float g = flourishGlow(p, cx.flourishU);
        if (g > 0.001f) {
          uint8_t r, gg, b;
          wheel(p * 6 + (int)(cx.flourishU * 256), r, gg, b);
          float k = g * cx.bright * 1.4f;
          put(p, buf[p][0] + r * k, buf[p][1] + gg * k, buf[p][2] + b * k);
        }
      }
    int sp = starPixel();
    if (S.ghost > 0.01f) {                                   // unlit letters glow faintly
      uint8_t v = u8(fmaxf(1.0f, cx.bright * 255.0f * S.ghost / 100.0f));
      Col gc = tinted({ 255, 255, 255 });
      for (int p = 0; p < 64; p++)
        if (p != sp && !buf[p][0] && !buf[p][1] && !buf[p][2])
          put(p, gc.r * v / 255.0f, gc.g * v / 255.0f, gc.b * v / 255.0f);
    }
    if ((S.star || S.weather >= 3) && sp >= 0 && starLevel > 0.001f) {   // seconds marker / weather on the * cell
      Col c;
      if (S.weather >= 3 && wxValid) c = S.weather == 3 ? weatherColor(wxCode) : tempColor(wxTemp);
      else { uint8_t r, g, b; wheel((int)(cx.el / S.huePeriod * 256.0) + (int)(cx.el / S.shimmerPeriod * 256.0), r, g, b); c = { r, g, b }; }
      if (S.nightRed && cx.nightMix > 0.001f) c = nightRed(c, cx.nightMix);
      float l = cx.bright * starLevel;
      put(sp, c.r * l, c.g * l, c.b * l);
    }
  }
  for (int p = 0; p < 64; p++) strip.setPixelColor(lut[p], buf[p][0], buf[p][1], buf[p][2]);
  strip.show();
}

void drawSet(const WordSet& ws) {
  memset(buf, 0, sizeof buf);
  if (cx.bright > 0.001f) {
    Col col[64]; bool in[64];
    colorize(ws, col, in);
    for (int p = 0; p < 64; p++) if (in[p]) put(p, col[p].r * cx.bright, col[p].g * cx.bright, col[p].b * cx.bright);
  }
  present();
}

void drawTransition(const WordSet& a, const WordSet& b, int kind, float u) {
  memset(buf, 0, sizeof buf);
  if (cx.bright > 0.001f) {
    Col ca[64], cb[64]; bool ia[64], ib[64];
    colorize(a, ca, ia); colorize(b, cb, ib);
    for (int p = 0; p < 64; p++) {
      PixW w = transitionWeights(kind, p, u, transSeed);
      float r = 0, g = 0, bl = 0, glow = 0;
      if (ia[p] && ib[p]) { r = cb[p].r; g = cb[p].g; bl = cb[p].b; }          // lit before and after: stays put
      else {
        if (ia[p]) { r += ca[p].r * w.wOld; g += ca[p].g * w.wOld; bl += ca[p].b * w.wOld; glow = w.glowOff; }
        if (ib[p]) { r += cb[p].r * w.wNew; g += cb[p].g * w.wNew; bl += cb[p].b * w.wNew; glow = w.glowOn; }
      }
      if (kind == TR_RIPPLE) glow = w.glowOn;                                  // the ring runs over every cell
      else if (kind != TR_SPARKLE) glow = 0;
      float add = glow * cx.bright * 255.0f * 1.6f;
      put(p, r * cx.bright + add, g * cx.bright + add, bl * cx.bright + add);
    }
  }
  present();
}

void pulse(uint8_t r, uint8_t g, uint8_t b) {     // breathing dot, top-left
  float level = fabsf((float)(pulseStep++ % 40) - 20.0f) / 20.0f;
  strip.clear();
  strip.setPixelColor(lut[0], (uint8_t)(r * level) + 1, (uint8_t)(g * level) + 1, (uint8_t)(b * level) + 1);
  strip.show();
}

void runSelftest() {                              // one pixel, reading order
  for (int p = 0; p < 64; p++) {
    strip.clear();
    strip.setPixelColor(lut[p], 40, 40, 40);
    strip.show();
    delay(80);
    server.handleClient();
  }
  strip.clear();
  strip.show();
}

// ----------------------------------------------------------------------------- web page
String esc(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;"; else o += c;
  }
  return o;
}
String sel(const char* name, int cur, const char* const* labels, const int* values, int n) {
  String o = "<select name='" + String(name) + "'>";
  for (int i = 0; i < n; i++)
    o += "<option value='" + String(values[i]) + "'" + (values[i] == cur ? " selected" : "") + ">" + labels[i] + "</option>";
  return o + "</select>";
}
String row(const char* label, const String& input, const char* hint = "") {
  return "<label><span>" + String(label) + "</span>" + input + "</label>" + (*hint ? "<small>" + String(hint) + "</small>" : "");
}
String num(const char* name, float v, const char* step = "any") {
  return "<input type='number' step='" + String(step) + "' name='" + name + "' value='" + String(v, 2) + "'>";
}
String txt(const char* name, const String& v) { return "<input name='" + String(name) + "' value='" + esc(v) + "'>"; }
String chk(const char* name, bool v) { return "<input type='checkbox' name='" + String(name) + "'" + (v ? " checked" : "") + ">"; }

void handleRoot() {
  static const char* gl[] = {"Andy (QUARTER)", "Gurgle (FIFTEEN)"}; static const int gv[] = {0, 1};
  static const char* ll[] = {"Zigzag / snake", "Progressive"};       static const int lv[] = {0, 1};
  static const char* rl[] = {"0", "90", "180", "270"};               static const int rv[] = {0, 90, 180, 270};
  static const char* nl[] = {"Dim", "Off"};                          static const int nv[] = {0, 1};
  String h = F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<title>Word clock</title><style>body{font-family:system-ui,sans-serif;max-width:30rem;margin:0 auto;padding:1rem;background:#111;color:#eee}"
               "h1{font-size:1.3rem}h2{font-size:1rem;margin:1.4rem 0 .3rem;color:#8cf}label{display:flex;justify-content:space-between;align-items:center;gap:1rem;margin:.4rem 0}"
               "input:not([type=checkbox]),select{width:55%;padding:.4rem;background:#222;color:#eee;border:1px solid #444;border-radius:4px}"
               "small{display:block;color:#999;margin:-.2rem 0 .4rem}button{padding:.6rem 1rem;margin:.4rem .4rem 0 0;border:0;border-radius:4px;background:#37c;color:#fff;font-size:1rem}"
               "button.g{background:#444}.st{background:#222;padding:.6rem;border-radius:4px;font-size:.9rem}</style></head><body><h1>Word clock</h1>");
  h += "<small>Firmware v5.2 (effects + weather) - built " __DATE__ " " __TIME__ "</small>";
  time_t n = time(nullptr);
  struct tm t; localtime_r(&n, &t);
  char buf[48];
  if (timeValid()) strftime(buf, sizeof buf, "%a %d %b %Y %H:%M:%S", &t); else strcpy(buf, "time not set yet");
  h += "<div class='st'>Local time: " + String(buf) + "<br>" + (apMode ? "Setup mode (access point WordClock-Setup)" : "WiFi: " + WiFi.SSID() + "  " + WiFi.localIP().toString()) + "</div>";
  h += "<form method='post' action='/save'>";
  h += "<h2>WiFi</h2>" + row("Network name", txt("ssid", S.ssid));
  h += row("Password", "<input type='password' name='pass' placeholder='(unchanged)'>");
  h += row("Hostname", txt("host", S.host), "Reach the clock at http://hostname.local/ (changing WiFi or hostname restarts it)");
  h += "<h2>Time</h2>" + row("POSIX time zone", txt("tz", S.tz));
  h += F("<label><span>Preset</span><select onchange=\"if(this.value)document.getElementsByName('tz')[0].value=this.value\"><option value=''>choose...</option>"
         "<option value='GMT0BST,M3.5.0/1,M10.5.0'>UK</option><option value='CET-1CEST,M3.5.0,M10.5.0/3'>Central Europe</option>"
         "<option value='EET-2EEST,M3.5.0/3,M10.5.0/4'>Eastern Europe</option><option value='EST5EDT,M3.2.0,M11.1.0'>US Eastern</option>"
         "<option value='CST6CDT,M3.2.0,M11.1.0'>US Central</option><option value='MST7MDT,M3.2.0,M11.1.0'>US Mountain</option>"
         "<option value='PST8PDT,M3.2.0,M11.1.0'>US Pacific</option><option value='AEST-10AEDT,M10.1.0,M4.1.0/3'>Sydney</option>"
         "<option value='NZST-12NZDT,M9.5.0,M4.1.0/3'>Auckland</option><option value='IST-5:30'>India</option><option value='UTC0'>UTC</option></select></label>");
  h += row("NTP server", txt("ntp", S.ntp));
  h += "<h2>Panel</h2>" + row("Letter plate", sel("grid", S.grid, gl, gv, 2));
  h += row("Wiring", sel("layout", S.layout, ll, lv, 2));
  h += row("Rotate", sel("rotate", S.rotate, rl, rv, 4));
  h += row("Flip left-right", chk("flipx", S.flipX)) + row("Flip up-down", chk("flipy", S.flipY));
  static const char* thl[] = {"Rainbow", "Solid colour", "Warm white", "Fire", "Ocean", "Aurora", "By word group", "Time of day (warm to cool)"};
  static const int thv[] = {0, 1, 2, 3, 4, 5, 6, 7};
  static const char* trl[] = {"None", "Slot-machine spin", "Cross-fade", "Sparkle", "Ripple from the centre", "Shuffle (fade / sparkle / ripple)"};
  static const int trv[] = {0, 1, 2, 3, 4, 5};
  static const char* fll[] = {"Off", "On the hour", "On the hour and half hour"};  static const int flv[] = {0, 1, 2};
  static const char* ncl[] = {"Same colours", "Red only"};                        static const int ncv[] = {0, 1};
  static const char* wxl[] = {"Off", "The * shows conditions (sun, cloud, rain...)", "The * shows temperature", "Tint all words by conditions", "Tint all words by temperature"};  static const int wxv[] = {0, 3, 4, 1, 2};
  h += "<h2>Look</h2>" + row("Brightness %", num("bright", S.bright), "Day brightness, 0-100");
  h += row("Colour theme", sel("theme", S.theme, thl, thv, 8));
  h += row("Solid colour", "<input type='color' name='color' value='" + S.color + "'>", "Used by the Solid colour theme");
  h += row("Rainbow cycle (s)", num("hue", S.huePeriod)) + row("Shimmer cycle (s)", num("shim", S.shimmerPeriod));
  h += row("Ghost letters %", num("ghost", S.ghost), "Unlit letters glow faintly, as a % of the lit brightness. 0 = off, try 2-5");
  h += row("Word change", sel("trans", S.trans, trl, trv, 6)) + row("Fade / sparkle / ripple length (s)", num("tsec", S.transSec));
  h += row("Spin length (s)", num("spinsec", S.spinSec));
  h += row("Hourly flourish", sel("flour", S.flourish, fll, flv, 3), "A rainbow wave across the whole grid");
  static const char* sl[] = {"Off", "Blink once per second", "Soft pulse once per second", "Quick flashes (1-4 by quarter minute)"}; static const int sv[] = {0, 1, 2, 3};
  h += row("Seconds marker (*)", sel("star", S.star, sl, sv, 4), "Uses the * cell on the Gurgle plate; the Andy plate has none");
  h += "<h2>Night mode</h2>" + row("Enabled", chk("non", S.nightOn));
  h += row("From", "<input type='time' name='nstart' value='" + S.nightStart + "'>") + row("Until", "<input type='time' name='nend' value='" + S.nightEnd + "'>");
  h += row("Mode", sel("nmode", S.nightMode, nl, nv, 2)) + row("Dim level %", num("ndim", S.dimPct), "Percent of day brightness");
  h += row("Fade (minutes)", num("nfade", S.fadeMin)) + row("Transitions at night", chk("nspin", S.nightSpin));
  h += row("Night colours", sel("nred", S.nightRed ? 1 : 0, ncl, ncv, 2), "Red only is easier on night vision");
  h += "<h2>Weather</h2>" + row("Weather tint", sel("wx", S.weather, wxl, wxv, 5), "Needs internet; uses api.open-meteo.com (no account), checked every 15 minutes. The * options need the Gurgle plate; if the seconds marker is off the * just glows steadily");
  h += row("Latitude", num("lat", S.lat, "0.0001")) + row("Longitude", num("lon", S.lon, "0.0001"));
  h += F("<button>Save</button><button class='g' formaction='/selftest' formmethod='post'>Test pattern</button></form>"
         "<small>Test pattern lights one pixel at a time in reading order. If it snakes or starts in the wrong corner, change Wiring / Rotate / Flip and save.</small></body></html>");
  server.send(200, "text/html", h);
}

void handleSave() {
  String oldSsid = S.ssid, oldPass = S.pass, oldHost = S.host;
  auto a = [&](const char* k) { return server.arg(k); };
  if (server.hasArg("ssid")) S.ssid = a("ssid");
  if (a("pass").length()) S.pass = a("pass");
  if (a("host").length()) S.host = a("host");
  if (a("tz").length()) S.tz = a("tz");
  if (a("ntp").length()) S.ntp = a("ntp");
  S.grid = a("grid").toInt() ? 1 : 0;
  S.layout = a("layout").toInt() ? 1 : 0;
  int r = a("rotate").toInt(); S.rotate = (r == 90 || r == 180 || r == 270) ? r : 0;
  S.flipX = server.hasArg("flipx"); S.flipY = server.hasArg("flipy");
  S.bright = constrain(a("bright").toFloat(), 0.0f, 100.0f);
  S.huePeriod = max(a("hue").toFloat(), 1.0f);
  S.shimmerPeriod = max(a("shim").toFloat(), 0.5f);
  S.spinSec = max(a("spinsec").toFloat(), 0.5f);
  S.trans = constrain(a("trans").toInt(), 0, 5);
  S.transSec = constrain(a("tsec").toFloat(), 0.3f, 10.0f);
  S.ghost = constrain(a("ghost").toFloat(), 0.0f, 20.0f);
  S.theme = constrain(a("theme").toInt(), 0, (int)TH_COUNT - 1);
  if (a("color").length() == 7 && a("color")[0] == '#') S.color = a("color");
  S.flourish = constrain(a("flour").toInt(), 0, 2);
  S.nightRed = a("nred").toInt() == 1;
  S.weather = constrain(a("wx").toInt(), 0, 4);
  S.lat = constrain(a("lat").toFloat(), -90.0f, 90.0f);
  S.lon = constrain(a("lon").toFloat(), -180.0f, 180.0f);
  if (S.weather) wxRefresh = true;                         // new settings: look the weather up again soon
  S.nightOn = server.hasArg("non");
  if (a("nstart").length()) S.nightStart = a("nstart");
  if (a("nend").length()) S.nightEnd = a("nend");
  S.nightMode = a("nmode").toInt() ? 1 : 0;
  S.dimPct = constrain(a("ndim").toFloat(), 0.0f, 100.0f);
  S.fadeMin = max(a("nfade").toFloat(), 0.0f);
  S.nightSpin = server.hasArg("nspin");
  S.star = constrain(a("star").toInt(), 0, 3);
  saveSettings();
  if (S.ssid != oldSsid || S.pass != oldPass || S.host != oldHost) {
    server.send(200, "text/html", "<meta name='viewport' content='width=device-width'><body style='font-family:sans-serif'><p>Saved. Restarting - reconnect in about 20 seconds.</p></body>");
    restartPending = true; restartAt = millis() + 1000;
    return;
  }
  applySettings();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSelftest() {
  selftestRequested = true;
  server.sendHeader("Location", "/");
  server.send(303);
}

// ----------------------------------------------------------------------------- network
void startAccessPoint() {                                  // the Pico's WiFi chip is either an access point or a station, not both
  WiFi.softAP("WordClock-Setup");
  dns.start(53, "*", WiFi.softAPIP());
  apMode = true;
  apSince = millis();
}

void printIp() {
  Serial.print("WiFi connected to \""); Serial.print(WiFi.SSID());
  Serial.print("\", IP address: "); Serial.print(WiFi.localIP());
  Serial.print("  (http://"); Serial.print(S.host); Serial.println(".local/)");
}

void startStaServices() {
  if (staServices) return;
  staServices = true;
  MDNS.begin(S.host.c_str());
  MDNS.addService("http", "tcp", 80);
  ArduinoOTA.setHostname(S.host.c_str());
  ArduinoOTA.begin();
  startNtp();
  printIp();                                               // once per boot, when the DHCP lease is obtained
}

// ----------------------------------------------------------------------------- weather
// One short blocking request (about a second or two) between frames, every 15 minutes. Compile out with ENABLE_WEATHER 0.
#if ENABLE_WEATHER
bool fetchWeather() {
  WiFiClientSecure client;
  client.setInsecure();                                    // public data, no secrets sent
  HTTPClient http;
  http.setTimeout(8000);
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(S.lat, 4) + "&longitude=" + String(S.lon, 4) +
               "&current=temperature_2m,weather_code";
  if (!http.begin(client, url)) return false;
  bool ok = false;
  if (http.GET() == 200) {
    String body = http.getString();
    int c = body.indexOf("\"current\":{");
    int t = c >= 0 ? body.indexOf("\"temperature_2m\":", c) : -1;
    int w = c >= 0 ? body.indexOf("\"weather_code\":", c) : -1;
    if (t >= 0 && w >= 0) {
      wxTemp = body.substring(t + 17).toFloat();
      wxCode = body.substring(w + 15).toInt();
      wxValid = true;
      ok = true;
    }
  }
  http.end();
  Serial.printf("Weather: %s  %.1f C, code %d\n", ok ? "ok" : "failed", (double)wxTemp, (int)wxCode);
  return ok;
}

uint32_t weatherNextAt = 0;
void serviceWeather() {                                    // called from loop() when nothing is animating
  if (S.weather && WiFi.status() == WL_CONNECTED && (wxRefresh || (int32_t)(millis() - weatherNextAt) >= 0)) {
    wxRefresh = false;
    weatherNextAt = millis() + (fetchWeather() ? 15UL * 60UL * 1000UL : 60UL * 1000UL);
  }
}
#else
void serviceWeather() {}
#endif

void setup() {
  Serial.begin(115200);
  { uint32_t w0 = millis(); while (!Serial && millis() - w0 < 3000) delay(10); }   // give the USB serial monitor a moment to attach
  Serial.println("Word clock firmware v5.2 (effects + weather), built " __DATE__ " " __TIME__);
  strip.begin();
  strip.show();
  loadSettings();
  applySettings();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/selftest", HTTP_POST, handleSelftest);
  server.onNotFound([]() {                                 // captive portal: send everything to the page
    server.sendHeader("Location", "http://" + (apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "/");
    server.send(302);
  });
  server.begin();

  WiFi.setHostname(S.host.c_str());
  if (S.ssid.length()) {
    WiFi.begin(S.ssid.c_str(), S.pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { pulse(0, 0, 60); delay(50); }
  }
  if (WiFi.status() == WL_CONNECTED) startStaServices();
  else startAccessPoint();
}

// ----------------------------------------------------------------------------- loop
void loop() {
  server.handleClient();
  static uint32_t lastBeat = 0;                            // status line every 30 s so the serial monitor always shows something
  if (millis() - lastBeat > 30000) {
    lastBeat = millis();
    Serial.printf("alive %lus  wifi=%s  ip=%s  time=%s\n", (unsigned long)(millis() / 1000), WiFi.status() == WL_CONNECTED ? "up" : "down",
                  WiFi.localIP().toString().c_str(), timeValid() ? "set" : "not set");
  }
  if (Serial.available()) {                                // press any key in the serial monitor to repeat the IP
    while (Serial.available()) Serial.read();
    if (WiFi.status() == WL_CONNECTED) printIp(); else Serial.println("WiFi not connected");
  }
  if (staServices) { ArduinoOTA.handle(); MDNS.update(); }
  if (apMode) dns.processNextRequest();
  if (restartPending && (int32_t)(millis() - restartAt) > 0) rp2040.restart();

  uint32_t ms = millis();
  if (apMode && S.ssid.length() && ms - apSince > AP_RETRY_MS) rp2040.restart();   // setup mode with a saved network: try it again
  if (staServices && ms - lastWifiCheck > 5000) {          // router rebooted? join again
    lastWifiCheck = ms;
    if (WiFi.status() == WL_CONNECTED) staLostSince = 0;
    else if (!staLostSince) staLostSince = ms ? ms : 1;
    else if (ms - staLostSince > 30000) { WiFi.begin(S.ssid.c_str(), S.pass.c_str()); staLostSince = ms; }
  }
  if (ms - lastFrame < frameInterval) return;
  lastFrame = ms;
  frameInterval = S.star == 3 ? 20 : 50;                    // finer timing for the quick flashes

  if (selftestRequested) { selftestRequested = false; runSelftest(); return; }
  if (!timeValid()) { apMode ? pulse(80, 30, 0) : pulse(0, 0, 60); return; }   // orange = setup mode, blue = waiting

  struct timeval tv; gettimeofday(&tv, nullptr);
  time_t n = tv.tv_sec;
  struct tm t; localtime_r(&n, &t);
  updateStar(t.tm_sec, tv.tv_usec / 1000);
  float minutes = t.tm_hour * 60 + t.tm_min + t.tm_sec / 60.0f;
  float bright = S.bright / 100.0f * night.factor(minutes);
  bool quiet = night.isNight(minutes) && !night.spin;
  WordSet ns = wordsForTime(S.grid, t.tm_hour, t.tm_min);

  cx.bright = bright; cx.minutes = minutes; cx.el = nowSec(); cx.tod = todFromMinutes(minutes);
  cx.nightMix = 0;
  if (S.nightRed && night.active()) {
    float span = 1.0f - night.level;
    if (span > 0.01f) cx.nightMix = clamp01((1.0f - night.factor(minutes)) / span);
  }
  cx.flourishOn = false;
  if (S.flourish && !quiet && bright > 0.001f && (t.tm_min == 0 || (S.flourish == 2 && t.tm_min == 30))) {
    float sec = t.tm_sec + tv.tv_usec / 1e6f;
    if (sec < 4.0f) { cx.flourishOn = true; cx.flourishU = sec / 4.0f; }
  }

  if (!haveCurrent || ns != current) {
    WordSet old = current; bool had = haveCurrent;
    current = ns;
    haveCurrent = true;
    if (S.trans && !quiet && bright > 0.001f) {
      int kind = S.trans;
      if (kind == TR_SHUFFLE) kind = TR_FADE + (int)(rp2040.hwrand32() % 3);        // fade, sparkle or ripple
      if (kind == TR_SPIN) {
        if (!spinner.running) spinner.begin(ns, S.grid, ms);
      } else {
        prevSet = had ? old : emptySet;
        transKind = kind; transStart = ms; transSeed = rp2040.hwrand32(); transActive = true;
      }
    }
  }

  if (spinner.running) {
    const WordSet& show = spinner.step(ms);
    drawSet(spinner.running ? show : current);
  } else if (transActive) {
    float u = (ms - transStart) / (S.transSec * 1000.0f);
    if (u >= 1.0f) { transActive = false; drawSet(current); }
    else drawTransition(prevSet, current, transKind, u);
  } else {
    drawSet(current);
    if (bright <= 0.001f) frameInterval = 500;             // display off: no need for 20 fps
    serviceWeather();                                      // brief pause every 15 minutes, only when nothing is animating
  }
}
