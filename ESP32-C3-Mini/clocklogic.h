// Hardware-independent clock logic (no Arduino calls) so it can be tested on a PC.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

namespace wc {

struct Word { uint8_t n; uint8_t p[8]; };      // p = y*8 + x
#include "grids.inc"

enum { W_FIVE = 0, W_TEN, W_QUARTER, W_TWENTY, W_HALF, W_PAST, W_TO, W_HOUR1 /* 7..18 */, W_COUNT = 19 };

struct WordSet {
  const Word* w[4];
  uint8_t n;
  bool operator==(const WordSet& o) const {
    if (n != o.n) return false;
    for (uint8_t i = 0; i < n; i++) if (w[i] != o.w[i]) return false;
    return true;
  }
  bool operator!=(const WordSet& o) const { return !(*this == o); }
  uint8_t pixels() const { uint8_t c = 0; for (uint8_t i = 0; i < n; i++) c += w[i]->n; return c; }
};

// Same rounding rules as Word-New-Luma V4.
inline WordSet wordsForTime(int grid, int hour, int minute) {
  const Word* g = GRID_DATA[grid ? 1 : 0];
  WordSet s; s.n = 0;
  int a = -1, b = -1, c = -1;
  if      (minute >=  3 && minute <=  7) { a = W_FIVE;                    b = W_PAST; }
  else if (minute >=  8 && minute <= 12) { a = W_TEN;                     b = W_PAST; }
  else if (minute >= 13 && minute <= 17) { a = W_QUARTER;                 b = W_PAST; }
  else if (minute >= 18 && minute <= 22) { a = W_TWENTY;                  b = W_PAST; }
  else if (minute >= 23 && minute <= 27) { a = W_TWENTY; b = W_FIVE;      c = W_PAST; }
  else if (minute >= 28 && minute <= 32) { a = W_HALF;                    b = W_PAST; }
  else if (minute >= 33 && minute <= 37) { a = W_TWENTY; b = W_FIVE;      c = W_TO; }
  else if (minute >= 38 && minute <= 42) { a = W_TWENTY;                  b = W_TO; }
  else if (minute >= 43 && minute <= 47) { a = W_QUARTER;                 b = W_TO; }
  else if (minute >= 48 && minute <= 52) { a = W_TEN;                     b = W_TO; }
  else if (minute >= 53 && minute <= 57) { a = W_FIVE;                    b = W_TO; }
  if (a >= 0) s.w[s.n++] = &g[a];
  if (b >= 0) s.w[s.n++] = &g[b];
  if (c >= 0) s.w[s.n++] = &g[c];
  if (minute > 32) hour++;
  hour %= 12; if (hour == 0) hour = 12;
  s.w[s.n++] = &g[W_HOUR1 + hour - 1];
  return s;
}

inline void wheel(int pos, uint8_t& r, uint8_t& g, uint8_t& b) {
  pos &= 255;
  if (pos < 85)       { r = pos * 3;        g = 255 - pos * 3; b = 0; }
  else if (pos < 170) { pos -= 85;  r = 255 - pos * 3; g = 0;        b = pos * 3; }
  else                { pos -= 170; r = 0;        g = pos * 3;       b = 255 - pos * 3; }
}

// ---- panel mapping: logical (x,y) -> LED index -------------------------------
// layout 0 = zigzag (snake), 1 = progressive; rotate in 0/90/180/270.
inline int ledIndex(int x, int y, int layout, int rotate, bool flipX, bool flipY) {
  int px = x, py = y;
  if (flipX) px = 7 - px;
  if (flipY) py = 7 - py;
  if (rotate == 90)       { int t = px; px = 7 - py; py = t; }
  else if (rotate == 180) { px = 7 - px; py = 7 - py; }
  else if (rotate == 270) { int t = px; px = py; py = 7 - t; }
  if (layout == 0 && (py & 1)) return py * 8 + (7 - px);
  return py * 8 + px;
}

// ---- night mode --------------------------------------------------------------
struct Night {
  bool enabled = true;
  int startMin = 22 * 60, endMin = 7 * 60;
  float level = 0.25f;       // 0 = off
  float fadeMin = 2.0f;
  bool spin = false;

  static int mod1440(float v) { float r = fmodf(v, 1440.0f); return (int)(r < 0 ? r + 1440 : r); }
  bool active() const { return enabled && startMin != endMin; }
  bool isNight(float minutes) const {
    if (!active()) return false;
    float since = fmodf(minutes - startMin + 1440.0f, 1440.0f);
    float len = (float)((endMin - startMin + 1440) % 1440);
    return since < len;
  }
  float factor(float minutes) const {
    if (!active()) return 1.0f;
    float fade = fadeMin < 0.01f ? 0.01f : fadeMin;
    float since = fmodf(minutes - startMin + 1440.0f, 1440.0f);
    float len = (float)((endMin - startMin + 1440) % 1440);
    if (since < len) { float k = since / fade; if (k > 1) k = 1; return 1.0f - (1.0f - level) * k; }
    float sinceEnd = fmodf(minutes - endMin + 1440.0f, 1440.0f);
    if (sinceEnd < fade) return level + (1.0f - level) * (sinceEnd / fade);
    return 1.0f;
  }
};

// ---- slot-machine spin (non-blocking version of V4 slot_spin) ----------------
struct Spinner {
  bool running = false;
  float seconds = 6.0f, finalHold = 0.5f;
  int minFrames = 2, maxFrames = 20;
  int grid = 0;
  WordSet target, show;
  uint32_t startMs = 0, holdMs = 0;
  bool holding = false;
  int frame = 0, lastSwap = 0;

  static void shuffle(const Word** a, int n) {
    for (int i = n - 1; i > 0; i--) { int j = rand() % (i + 1); const Word* t = a[i]; a[i] = a[j]; a[j] = t; }
  }
  WordSet randomSet(int count) const {
    const Word* pool[W_COUNT];
    for (int i = 0; i < W_COUNT; i++) pool[i] = &GRID_DATA[grid ? 1 : 0][i];
    shuffle(pool, W_COUNT);
    WordSet s; s.n = 0;
    if (count < 1) count = 1;
    if (count > 4) count = 4;
    for (int i = 0; i < count; i++) s.w[s.n++] = pool[i];
    return s;
  }
  void begin(const WordSet& tgt, int g, uint32_t nowMs) {
    target = tgt; grid = g; startMs = nowMs; frame = lastSwap = 0; holding = false;
    show = randomSet(tgt.n); running = true;
  }
  // Call once per displayed frame. Returns the set to draw; running turns false once target is shown.
  const WordSet& step(uint32_t nowMs) {
    if (!running) return target;
    float el = (nowMs - startMs) / 1000.0f;
    float progress = el / seconds; if (progress > 1) progress = 1;
    float inv = 1.0f - progress;
    float ease = 1.0f - inv * inv * inv;
    int interval = (int)(minFrames + ease * (maxFrames - minFrames));
    if (progress >= 1.0f) {
      if (!holding) { holding = true; holdMs = nowMs; }
      else if ((nowMs - holdMs) / 1000.0f >= finalHold) { running = false; return target; }
    } else if (frame - lastSwap >= interval) {
      show = randomSet(target.n); lastSwap = frame;
    }
    frame++;
    return show;
  }
};

// ======================================================================= effects (all pure maths, tested on a PC)
struct Col { uint8_t r, g, b; };

inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline float smooth01(float v) { v = clamp01(v); return v * v * (3 - 2 * v); }
inline uint8_t u8(float v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5f)); }
inline Col lerpRGB(Col a, Col b, float t) {
  t = clamp01(t);
  return { u8(a.r + (b.r - a.r) * t), u8(a.g + (b.g - a.g) * t), u8(a.b + (b.b - a.b) * t) };
}
inline Col grad3(Col a, Col b, Col c, float t) { t = clamp01(t); return t < 0.5f ? lerpRGB(a, b, t * 2) : lerpRGB(b, c, (t - 0.5f) * 2); }
inline uint32_t hash32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
inline float hash01(uint32_t x) { return (hash32(x) & 0xFFFFFF) / 16777216.0f; }

// colour themes
enum { TH_RAINBOW = 0, TH_SOLID, TH_WARM, TH_FIRE, TH_OCEAN, TH_AURORA, TH_GROUPS, TH_TIMEOFDAY, TH_COUNT };
// group: 0 = minute word, 1 = past/to, 2 = hour.  wave 0..1 = slow per-pixel shimmer.  wheelPos = classic rainbow position.
// tod 0 = warm (night/evening) .. 1 = cool daylight.
inline int wordGroup(int wordIndex) { return wordIndex <= W_HALF ? 0 : (wordIndex <= W_TO ? 1 : 2); }
inline Col themeColor(int theme, float wave, int wheelPos, Col custom, int group, float tod) {
  uint8_t r, g, b;
  switch (theme) {
    case TH_SOLID:  return lerpRGB({ u8(custom.r * 0.8f), u8(custom.g * 0.8f), u8(custom.b * 0.8f) }, custom, wave);
    case TH_WARM:   return lerpRGB({ 255, 140, 40 }, { 255, 190, 100 }, wave);
    case TH_FIRE:   return grad3({ 210, 15, 0 }, { 255, 90, 0 }, { 255, 200, 30 }, wave);
    case TH_OCEAN:  return grad3({ 0, 40, 220 }, { 0, 140, 230 }, { 0, 230, 190 }, wave);
    case TH_AURORA: return grad3({ 0, 230, 80 }, { 0, 200, 210 }, { 160, 60, 230 }, wave);
    case TH_GROUPS: {
      static const Col gc[3] = { { 0, 190, 255 }, { 255, 50, 150 }, { 255, 170, 0 } };
      return lerpRGB({ u8(gc[group].r * 0.75f), u8(gc[group].g * 0.75f), u8(gc[group].b * 0.75f) }, gc[group], wave);
    }
    case TH_TIMEOFDAY: return lerpRGB({ 255, 120, 30 }, { 175, 215, 255 }, tod);
    default: wheel(wheelPos, r, g, b); return { r, g, b };
  }
}
// colour temperature through the day: warm in the small hours and evening, coolest around 13:00
inline float todFromMinutes(float minutes) {
  float v = 0.5f - 0.5f * cosf((minutes - 60.0f) / 1440.0f * 6.2831853f);   // 0 at 01:00, 1 at 13:00
  return smooth01(v);
}

// night: shift colours towards red (keeps night vision). mix 0..1
inline Col nightRed(Col c, float mix) {
  uint8_t m = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
  return lerpRGB(c, { m, u8(m * 0.12f), 0 }, mix);
}

// weather (WMO codes from Open-Meteo)
inline Col weatherColor(int code) {          // saturated, clearly different hues so it reads on a single pixel
  if (code == 0) return { 255, 140, 0 };                                  // clear: orange
  if (code <= 2) return { 255, 230, 30 };                                 // mainly clear / partly cloudy: yellow
  if (code == 3) return { 0, 210, 170 };                                  // overcast: teal
  if (code == 45 || code == 48) return { 170, 170, 170 };                 // fog: grey
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return { 0, 50, 255 };   // drizzle / rain: deep blue
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return { 190, 255, 255 };  // snow: ice white
  if (code >= 95) return { 210, 0, 255 };                                 // thunderstorm: magenta
  return { 255, 255, 255 };
}
inline Col tempColor(float t) {            // violet <=0, blue 5, cyan 10, green 15, yellow 20, orange 25, red >=30 (degrees C)
  static const float k[7] = { 0, 5, 10, 15, 20, 25, 30 };
  static const Col c[7] = { { 110, 50, 255 }, { 0, 90, 255 }, { 0, 220, 220 }, { 0, 255, 70 }, { 255, 230, 0 }, { 255, 100, 0 }, { 255, 0, 0 } };
  if (t <= k[0]) return c[0];
  for (int i = 1; i < 7; i++) if (t <= k[i]) return lerpRGB(c[i - 1], c[i], (t - k[i - 1]) / (k[i] - k[i - 1]));
  return c[6];
}

// word-change transitions. u runs 0..1. Per pixel: how much of the old and new word set shows, plus a white sparkle.
enum { TR_NONE = 0, TR_SPIN, TR_FADE, TR_SPARKLE, TR_RIPPLE, TR_SHUFFLE };
struct PixW { float wOld, wNew, glowOff, glowOn; };
inline PixW transitionWeights(int kind, int p, float u, uint32_t seed) {
  PixW w = { 0, 1, 0, 0 };
  u = clamp01(u);
  if (kind == TR_FADE) {
    float s = smooth01(u); w.wOld = 1 - s; w.wNew = s;
  } else if (kind == TR_SPARKLE) {
    float offU = 0.05f + 0.40f * hash01(seed + p * 2654435761u);
    float onU  = 0.50f + 0.45f * hash01(seed * 7919u + p * 40503u + 77);
    w.wOld = u < offU ? 1.0f : 0.0f;
    w.wNew = u >= onU ? 1.0f : 0.0f;
    w.glowOff = clamp01(1.0f - fabsf(u - offU) / 0.08f);
    w.glowOn  = clamp01(1.0f - fabsf(u - onU) / 0.10f);
  } else if (kind == TR_RIPPLE) {
    float dx = (p % 8) - 3.5f, dy = (p / 8) - 3.5f;
    float d = sqrtf(dx * dx + dy * dy) / 4.95f;                 // 0 centre .. 1 corner
    float front = u * 1.35f;
    float s = clamp01((front - d) / 0.15f);
    w.wOld = 1 - s; w.wNew = s;
    w.glowOn = w.glowOff = clamp01(1.0f - fabsf(front - d) / 0.2f) * 0.8f;
  }
  return w;
}

// hourly / half-hourly flourish: a bright diagonal wave over the whole grid. u runs 0..1. returns 0..1
inline float flourishGlow(int p, float u) {
  float d = ((p % 8) + (p / 8)) / 14.0f;
  float front = u * 1.5f - 0.1f;
  return clamp01(1.0f - fabsf(front - d) / 0.22f);
}

}  // namespace wc
