// Bob - a little robot buddy on the Waveshare ESP32-S3-Touch-AMOLED-1.64
// Shows a living face and reacts to Claude Code (via hooks over USB serial).
//
// Serial protocol (USB CDC, one command per line):
//   working [label]    new prompt, Claude is busy -> focused orange eyes, "reading"
//   tool [label]       a tool finished (keep-alive; ignored right after "done")
//   done [label]       task finished         -> happy green eyes, stays until tapped
//   attention [label]  Claude needs you      -> big amber eyes, wiggle
//   idle | sleep | wake | hello
//   music play [artist - title] | music stop   (from host/bob_music.py)
//   bright <0-255>     display brightness
//   demo               cycle through all moods
//   stats              frame timing
//   ping               -> "bob ok <version>"

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "font_bob.h"

#define BOB_VERSION "0.6.1"

#ifndef BOB_LCD_CS
#define BOB_LCD_CS 9
#endif

// ---------- hardware ----------
static const int PIN_SCK = 10, PIN_D0 = 11, PIN_D1 = 12, PIN_D2 = 13, PIN_D3 = 14, PIN_RST = 21;
static const int PIN_SDA = 47, PIN_SCL = 48;
static const uint8_t TOUCH_ADDR = 0x38;  // FT3168

static const int PW = 280, PH = 456;  // physical panel (portrait)
static const int W = 456, H = 280;    // logical canvas (landscape)

Arduino_DataBus *bus = new Arduino_ESP32QSPI(BOB_LCD_CS, PIN_SCK, PIN_D0, PIN_D1, PIN_D2, PIN_D3);
Arduino_CO5300 *panel = new Arduino_CO5300(bus, PIN_RST, 0, PW, PH, 20, 0, 0, 0);
Arduino_Canvas *canvas = new Arduino_Canvas(PW, PH, panel);
static uint16_t *fb = nullptr;     // physical framebuffer (PSRAM)
static uint16_t *txbuf = nullptr;  // internal chunk buffer for partial flushes
static const int TX_ROWS = 24;

// ---------- small math ----------
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static float frand(float a, float b) { return a + (b - a) * (esp_random() / 4294967295.0f); }
static float approach(float cur, float tgt, float speed, float dt) { return cur + (tgt - cur) * (1.0f - expf(-speed * dt)); }
static float easeInOut(float t) { t = clampf(t, 0, 1); return t * t * (3 - 2 * t); }

// damped spring: natural, slightly overshooting motion
struct Spring {
  float x = 0, v = 0, target = 0;
  float k = 90, zeta = 0.85f;
  void set(float val) { x = target = val; v = 0; }
  void step(float dt) {
    float c = 2.0f * zeta * sqrtf(k);
    for (int i = 0; i < 2; i++) {
      float h = dt * 0.5f;
      v += (k * (target - x) - c * v) * h;
      x += v * h;
    }
  }
};

// ---------- framebuffer (logical coords -> physical portrait buffer) ----------
static inline int fbIndex(int x, int y) { return x * PW + (PW - 1 - y); }

struct Box {
  int x0 = W, y0 = H, x1 = -1, y1 = -1;
  void add(int ax0, int ay0, int ax1, int ay1) {
    x0 = min(x0, ax0); y0 = min(y0, ay0); x1 = max(x1, ax1); y1 = max(y1, ay1);
  }
  void add(const Box &b) { if (b.valid()) add(b.x0, b.y0, b.x1, b.y1); }
  bool valid() const { return x1 >= x0 && y1 >= y0; }
  void clip() { x0 = max(x0, 0); y0 = max(y0, 0); x1 = min(x1, W - 1); y1 = min(y1, H - 1); }
};
static Box drawn, prevDrawn;

struct RGB { float r, g, b; };
static inline uint16_t pack(float r, float g, float b) {
  int ir = (int)clampf(r, 0, 255), ig = (int)clampf(g, 0, 255), ib = (int)clampf(b, 0, 255);
  return ((ir & 0xF8) << 8) | ((ig & 0xFC) << 3) | (ib >> 3);
}
static inline void blendPixel(int x, int y, const RGB &c, float a) {
  if (a <= 0.004f || x < 0 || y < 0 || x >= W || y >= H) return;
  uint16_t &p = fb[fbIndex(x, y)];
  if (a >= 0.996f || p == 0) { p = pack(c.r * min(a, 1.0f), c.g * min(a, 1.0f), c.b * min(a, 1.0f)); return; }
  float br = ((p >> 11) & 0x1F) * (255.0f / 31), bg = ((p >> 5) & 0x3F) * (255.0f / 63), bb = (p & 0x1F) * (255.0f / 31);
  p = pack(lerpf(br, c.r, a), lerpf(bg, c.g, a), lerpf(bb, c.b, a));
}

static void clearBox(const Box &b) {
  if (!b.valid()) return;
  int c0 = PW - 1 - b.y1, c1 = PW - 1 - b.y0;
  for (int x = b.x0; x <= b.x1; x++) memset(&fb[x * PW + c0], 0, (c1 - c0 + 1) * 2);
}

static uint32_t flushPixels = 0;
static float statDrawUs = 0, statFlushUs = 0;
static void flushBox(Box b) {
  if (!b.valid()) return;
  // physical rect: rows = logical x, cols = PW-1-logical y. Keep even alignment for the panel.
  int r0 = b.x0 & ~1, r1 = min(b.x1 | 1, PH - 1);
  int c0 = (PW - 1 - b.y1) & ~1, c1 = min((PW - 1 - b.y0) | 1, PW - 1);
  int w = c1 - c0 + 1;
  flushPixels = 0;
  for (int r = r0; r <= r1; r += TX_ROWS) {
    int rows = min(TX_ROWS, r1 - r + 1);
    for (int i = 0; i < rows; i++) memcpy(&txbuf[i * w], &fb[(r + i) * PW + c0], w * 2);
    panel->draw16bitRGBBitmap(c0, r, txbuf, w, rows);
    flushPixels += w * rows;
  }
}

// ---------- SDF shapes (anti-aliased) ----------
static inline float sdRoundBox(float px, float py, float hw, float hh, float r) {
  float qx = fabsf(px) - hw + r, qy = fabsf(py) - hh + r;
  float ox = max(qx, 0.0f), oy = max(qy, 0.0f);
  return sqrtf(ox * ox + oy * oy) + min(max(qx, qy), 0.0f) - r;
}

static void drawCircle(float cx, float cy, float rad, const RGB &col, float alpha);

struct EyeShape {
  float cx, cy, w, h, r;
  float lidTop;     // 0..1 covered from top
  float lidSlant;   // lowers the inner corner (focused look)
  float lidBottom;  // 0..1 covered from bottom (squint)
  float happy;      // 0..1 smile cut -> upside-down U
  float pupil;      // pupil radius as fraction of the smaller eye half-size (0 = none)
  float pgx, pgy;   // gaze, moves the pupil inside the eye
  bool left;
};

static void drawEye(const EyeShape &e, const RGB &col) {
  float hw = max(e.w * 0.5f, 2.0f), hh = max(e.h * 0.5f, 1.5f);
  float r = min(e.r, min(hw, hh));
  int x0 = max(0, (int)floorf(e.cx - hw - 1)), x1 = min(W - 1, (int)ceilf(e.cx + hw + 1));
  int y0 = max(0, (int)floorf(e.cy - hh - 1)), y1 = min(H - 1, (int)ceilf(e.cy + hh + 1));
  if (x1 < x0 || y1 < y0) return;
  drawn.add(x0, y0, x1, y1);

  float top = e.cy - hh, bottom = e.cy + hh;
  float lidY = top + e.lidTop * e.h;
  float slope = (e.left ? 1.0f : -1.0f) * e.lidSlant * 0.55f;
  float slopeN = 1.0f / sqrtf(1 + slope * slope);
  bool useLid = e.lidTop > 0.001f || e.lidSlant > 0.001f;
  float lidBY = bottom - e.lidBottom * e.h;
  float R = e.w * 0.9f;
  float hcy = bottom + R - e.happy * e.h * 0.62f;

  uint16_t solid = pack(col.r, col.g, col.b);
  // Per column (= contiguous memory): find the exact inside span analytically, fill it
  // solid, and evaluate the anti-aliased SDF only in a thin band at the edges.
  auto sdfAt = [&](float px, float py) {
    float sd = sdRoundBox(px - e.cx, py - e.cy, hw, hh, r);
    if (useLid) sd = max(sd, (lidY + slope * (px - e.cx) - py) * slopeN);
    if (e.lidBottom > 0.001f) sd = max(sd, py - lidBY);
    if (e.happy > 0.001f) {
      float dx = px - e.cx, dy = py - hcy;
      sd = max(sd, R - sqrtf(dx * dx + dy * dy));
    }
    return sd;
  };
  auto aaRange = [&](int x, float px, int ya, int yb) {
    for (int y = max(ya, y0); y <= min(yb, y1); y++) {
      float a = clampf(0.5f - sdfAt(px, y + 0.5f), 0, 1);
      if (a > 0) blendPixel(x, y, col, a);
    }
  };
  for (int x = x0; x <= x1; x++) {
    float px = x + 0.5f;
    float dxa = fabsf(px - e.cx);
    if (dxa > hw + 0.5f) continue;
    if (dxa > hw - 1.5f) { aaRange(x, px, y0, y1); continue; }  // side edge: full AA
    float half = hh;
    if (dxa > hw - r) { float u = dxa - (hw - r); half = (hh - r) + sqrtf(max(r * r - u * u, 0.0f)); }
    float yT = e.cy - half, yB = e.cy + half;
    if (useLid) yT = max(yT, lidY + slope * (px - e.cx));
    if (e.lidBottom > 0.001f) yB = min(yB, lidBY);
    if (e.happy > 0.001f) {
      float dx = px - e.cx;
      if (fabsf(dx) < R) yB = min(yB, hcy - sqrtf(R * R - dx * dx));
    }
    if (yB < yT - 2) continue;
    int ys = (int)ceilf(yT + 1.5f), ye = (int)floorf(yB - 2.5f);
    ys = max(ys, y0); ye = min(ye, y1);
    if (ye < ys) { aaRange(x, px, (int)floorf(yT) - 2, (int)ceilf(yB) + 2); continue; }
    aaRange(x, px, (int)floorf(yT) - 2, ys - 1);
    aaRange(x, px, ye + 1, (int)ceilf(yB) + 2);
    uint16_t *dst = &fb[fbIndex(x, ye)];  // y increasing -> index decreasing
    for (int n = ye - ys; n >= 0; n--) *dst++ = solid;
  }

  // pupil: a black disc, faded out for happy eyes and very flat (blinking/sleeping) eyes
  float vis = clampf(1.6f - e.happy * 2.2f, 0, 1) * clampf((hh - 10) / 20, 0, 1);
  if (e.pupil > 0.01f && vis > 0.01f) {
    float pr = e.pupil * min(hw, hh);
    float px = e.cx + e.pgx * (hw - pr - 7) * 0.65f;
    float py = e.cy + e.pgy * (hh - pr - 7) * 0.65f;
    py = max(py, lidY + pr * 0.4f);  // keep it under a lowered lid
    drawCircle(px, py, pr, {0, 0, 0}, vis);
    drawCircle(px + pr * 0.38f, py - pr * 0.4f, max(pr * 0.24f, 2.0f), col, vis);  // catchlight
  }
}

static void drawCircle(float cx, float cy, float rad, const RGB &col, float alpha) {
  int x0 = max(0, (int)(cx - rad - 1)), x1 = min(W - 1, (int)(cx + rad + 1));
  int y0 = max(0, (int)(cy - rad - 1)), y1 = min(H - 1, (int)(cy + rad + 1));
  drawn.add(x0, y0, x1, y1);
  for (int x = x0; x <= x1; x++)
    for (int y = y0; y <= y1; y++) {
      float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
      blendPixel(x, y, col, alpha * clampf(rad + 0.5f - sqrtf(dx * dx + dy * dy), 0, 1));
    }
}

static void drawHeart(float cx, float cy, float s, const RGB &col) {
  int x0 = max(0, (int)(cx - s * 1.2f)), x1 = min(W - 1, (int)(cx + s * 1.2f));
  int y0 = max(0, (int)(cy - s * 1.1f)), y1 = min(H - 1, (int)(cy + s * 1.2f));
  drawn.add(x0, y0, x1, y1);
  for (int x = x0; x <= x1; x++)
    for (int y = y0; y <= y1; y++) {
      // heart SDF (Inigo Quilez), y up
      float px = fabsf((x + 0.5f - cx) / s), py = -(y + 0.5f - cy) / s + 0.55f;
      float d;
      if (py + px > 1.0f) {
        float dx = px - 0.25f, dy = py - 0.75f;
        d = sqrtf(dx * dx + dy * dy) - 0.35355f;
      } else {
        float ay = py - 1.0f;
        float t = max(px + py, 0.0f) * 0.5f;
        float bx = px - t, by = py - t;
        d = sqrtf(min(px * px + ay * ay, bx * bx + by * by)) * (px - py > 0 ? 1 : -1);
      }
      blendPixel(x, y, col, clampf(0.5f - d * s, 0, 1));
    }
}

// ---------- text ----------
static uint32_t utf8Next(const char *&s) {
  uint8_t c = *s++;
  if (c < 0x80) return c;
  if ((c & 0xE0) == 0xC0 && *s) return ((c & 0x1F) << 6) | (*s++ & 0x3F);
  while ((*s & 0xC0) == 0x80) s++;
  return '?';
}
static const BobGlyph *findGlyph(const BobFont &f, uint32_t cp) {
  for (int i = 0; i < f.count; i++) if (f.glyphs[i].cp == cp) return &f.glyphs[i];
  return cp == '?' ? nullptr : findGlyph(f, '?');
}
static int textWidth(const BobFont &f, const String &s) {
  int w = 0; const char *p = s.c_str();
  while (*p) { const BobGlyph *g = findGlyph(f, utf8Next(p)); if (g) w += g->adv; }
  return w;
}
static int clipX0 = 0, clipX1 = W - 1;  // horizontal clip for text
static void drawText(const BobFont &f, const String &s, int x, int baseline, const RGB &col, float alpha) {
  if (alpha <= 0.01f) return;
  const char *p = s.c_str();
  while (*p) {
    const BobGlyph *g = findGlyph(f, utf8Next(p));
    if (!g) continue;
    if (g->w && g->h) {
      int gx = x + g->xoff, gy = baseline + g->yoff;
      if (gx + g->w < clipX0 || gx > clipX1) { x += g->adv; continue; }
      drawn.add(max(gx, clipX0), max(gy, 0), min(gx + g->w, clipX1), min(gy + g->h, H - 1));
      for (int i = 0; i < g->w; i++)
        for (int j = 0; j < g->h; j++) {
          int idx = j * g->w + i;
          uint8_t b = f.data[g->off + idx / 2];
          uint8_t a4 = (idx & 1) ? (b & 0x0F) : (b >> 4);
          int px = gx + i;
          if (a4 && px >= clipX0 && px <= clipX1) blendPixel(px, gy + j, col, alpha * a4 / 15.0f);
        }
    }
    x += g->adv;
  }
}
// ---------- face model ----------
struct Look {
  float w, h, r, gap;
  float lidTop, lidSlant, lidBottom, happy;
  RGB col;
};
static const RGB WHITE = {235, 240, 245};
static const Look L_IDLE      = {92, 112, 40, 152, 0.00f, 0.00f, 0.00f, 0.12f, WHITE};
static const Look L_DROWSY    = {96,  62, 28, 152, 0.00f, 0.00f, 0.00f, 0, {200, 205, 210}};
static const Look L_WORKING   = {98,  96, 38, 152, 0.00f, 0.00f, 0.00f, 0.20f, {245, 135,  85}};
static const Look L_DONE      = {104, 106, 36, 158, 0.00f, 0.00f, 0.00f, 1, WHITE};
static const Look L_DONE_WAIT = {100, 106, 34, 156, 0.00f, 0.00f, 0.00f, 1, WHITE};  // stays happy until tapped
static const Look L_ATTENTION = {108, 126, 44, 160, 0.00f, 0.00f, 0.00f, 0, {255, 185,  45}};
static const Look L_SLEEP     = {84,   9, 30, 150, 0.00f, 0.00f, 0.00f, 0, {110, 112, 118}};
static const Look L_LOVE      = {104, 106, 36, 158, 0.00f, 0.00f, 0.00f, 1, WHITE};
static const Look L_HELLO     = {100, 108, 34, 156, 0.00f, 0.00f, 0.00f, 1, WHITE};

enum Mood { IDLE, WORKING, DONE, DONE_WAIT, ATTENTION, SLEEP, LOVE, HELLO };
static const char *MOOD_NAMES[] = {"idle", "working", "done", "done_wait", "attention", "sleep", "love", "hello"};

// music (from the Mac's Spotify watcher)
static bool musicOn = false;
static String musicTrack = "";
static float musicAlpha = 0, musicScroll = 0;
static uint32_t musicLast = 0;
static const uint32_t MUSIC_TIMEOUT_MS = 90UL * 1000;  // watcher gone -> stop

static Mood mood = IDLE, moodAfterLove = IDLE;
static uint32_t moodSince = 0, lastActivity = 0;
static String label = "";

static Spring sW, sH, sR, sGap, sLidTop, sLidSlant, sLidBottom, sHappy;
static Spring gazeX, gazeY;
static Spring eyeDiff;  // + makes the right eye bigger (curious)
static Spring smile;    // extra smile (idle "smiles at you")
static Spring sPupil;   // pupil size
static Spring *SHAPE_SPRINGS[] = {&sW, &sH, &sR, &sGap, &sLidTop, &sLidSlant, &sLidBottom, &sHappy, &eyeDiff, &smile, &sPupil};
static RGB col = L_IDLE.col, colTarget = L_IDLE.col;

static float blinkOpen = 1;
static uint32_t nextBlink = 0, blinkStart = 0;
static bool blinking = false, blinkDouble = false;

static uint32_t nextAction = 0, nextMicro = 0, effectUntil = 0;
static float microX = 0, microY = 0;
static int lookAroundStep = -1;

static uint8_t brightness = 200;
static bool screenOn = true, dimmed = false;

static const uint32_t DROWSY_AFTER_MS = 150UL * 1000;
static const uint32_t SLEEP_AFTER_MS = 180UL * 1000;
static const uint32_t SCREEN_OFF_AFTER_MS = 20UL * 60 * 1000;
static const uint32_t DONE_HAPPY_MS = 8000;
static const uint32_t WORKING_TIMEOUT_MS = 15UL * 60 * 1000;
static const uint32_t LOVE_MS = 1800;
static const uint32_t HELLO_MS = 3200;

static void applyLook(const Look &l) {
  sW.target = l.w; sH.target = l.h; sR.target = l.r; sGap.target = l.gap;
  sLidTop.target = l.lidTop; sLidSlant.target = l.lidSlant; sLidBottom.target = l.lidBottom; sHappy.target = l.happy;
  colTarget = l.col;
}

static void startBlink(bool dbl = false) { blinking = true; blinkStart = millis(); blinkDouble = dbl; }

static void setMood(Mood m, const String &lbl = "") {
  if (m != LOVE) label = lbl;
  mood = m;
  moodSince = millis();
  lastActivity = millis();
  if (!screenOn) prevDrawn.add(0, 0, W - 1, H - 1);
  screenOn = true;
  dimmed = false;
  panel->setBrightness(brightness);
  eyeDiff.target = 0; smile.target = 0; lookAroundStep = -1;
  nextAction = millis() + 300;
  static const float PUPIL[] = {0.42f, 0.36f, 0.42f, 0.42f, 0.28f, 0.42f, 0.5f, 0.42f};  // per Mood
  sPupil.target = PUPIL[m];
  switch (m) {
    case IDLE: applyLook(L_IDLE); break;
    case WORKING: applyLook(L_WORKING); nextAction = 0; break;
    case DONE: applyLook(L_DONE); gazeX.target = 0; gazeY.target = -0.1f; break;
    case DONE_WAIT: applyLook(L_DONE_WAIT); break;
    case ATTENTION: applyLook(L_ATTENTION); gazeX.target = 0; gazeY.target = 0; break;
    case SLEEP: applyLook(L_SLEEP); gazeX.target = 0; gazeY.target = 0.35f; break;
    case LOVE: applyLook(L_LOVE); gazeX.target = 0; gazeY.target = -0.1f; break;
    case HELLO: applyLook(L_HELLO); gazeX.target = 0; gazeY.target = 0; break;
  }
  Serial.printf("bob mood %s %s\n", MOOD_NAMES[m], label.c_str());
}

// ---------- touch ----------
static bool touchOk = false;
static bool readTouch() {
  if (!touchOk) return false;
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(TOUCH_ADDR, (uint8_t)1) != 1) return false;
  uint8_t n = Wire.read() & 0x0F;
  return n > 0 && n < 3;
}

static void onTap() {
  lastActivity = millis();
  if (!screenOn || mood == SLEEP) { setMood(IDLE); startBlink(true); return; }
  moodAfterLove = mood == WORKING ? WORKING : IDLE;
  setMood(LOVE);
}

// ---------- behaviour ----------
static void idleBehaviour(uint32_t now) {
  if (lookAroundStep >= 0) {
    static const float seq[][2] = {{-0.95f, -0.1f}, {0.95f, -0.1f}, {0, 0}};
    gazeX.target = seq[lookAroundStep][0]; gazeY.target = seq[lookAroundStep][1];
    lookAroundStep++;
    nextAction = now + (lookAroundStep >= 3 ? 1800 : 750);
    if (lookAroundStep >= 3) lookAroundStep = -1;
    return;
  }
  if (now > effectUntil) { eyeDiff.target = 0; smile.target = 0; }
  float r = frand(0, 1);
  if (r < 0.52f) {          // glance somewhere
    gazeX.target = frand(-1, 1); gazeY.target = frand(-0.75f, 0.75f);
    nextAction = now + (uint32_t)frand(900, 3600);
  } else if (r < 0.74f) {   // look at you
    gazeX.target = 0; gazeY.target = 0;
    nextAction = now + (uint32_t)frand(1500, 4500);
  } else if (r < 0.84f) {   // look around
    lookAroundStep = 0; nextAction = now;
  } else if (r < 0.93f) {   // curious: one eye bigger, peek to that side
    float side = frand(0, 1) < 0.5f ? -1 : 1;
    gazeX.target = 0.6f * side; gazeY.target = -0.2f;
    eyeDiff.target = 0.16f * side; effectUntil = now + 1600;
    nextAction = now + 1700;
  } else {                  // look at you and smile
    smile.target = 0.55f; effectUntil = now + 1800;
    gazeX.target = 0; gazeY.target = 0;
    nextAction = now + 2000;
  }
}

static void updateBehaviour(float dt, uint32_t now) {
  uint32_t inMood = now - moodSince;

  if (mood == HELLO && inMood > HELLO_MS) setMood(IDLE);
  if (mood == DONE && inMood > DONE_HAPPY_MS) { String l = label; setMood(DONE_WAIT, l); }
  if (mood == LOVE && inMood > LOVE_MS) {
    if (moodAfterLove == WORKING) { String l = label; setMood(WORKING, l); } else setMood(IDLE);
  }
  if (mood == WORKING && now - lastActivity > WORKING_TIMEOUT_MS) setMood(IDLE);
  bool drowsy = mood == IDLE && now - lastActivity > DROWSY_AFTER_MS;
  if (mood == IDLE) applyLook(drowsy ? L_DROWSY : L_IDLE);
  if (mood == IDLE && now - lastActivity > SLEEP_AFTER_MS) setMood(SLEEP);
  if (musicOn && now - musicLast > MUSIC_TIMEOUT_MS) musicOn = false;
  if (musicOn && (mood == IDLE || mood == SLEEP)) {
    lastActivity = now;  // music playing = you're around: stay awake
    if (mood == SLEEP) setMood(IDLE);
  }
  if (mood == SLEEP && screenOn && inMood > SCREEN_OFF_AFTER_MS) { screenOn = false; panel->setBrightness(0); }
  if ((mood == DONE_WAIT || mood == ATTENTION) && inMood > 10UL * 60 * 1000 && !dimmed) {
    dimmed = true; panel->setBrightness(60);
  }

  // gaze targets
  if (now >= nextAction) {
    switch (mood) {
      case WORKING: {
        static int line = 0; static bool right = false;
        if (!right) { gazeX.target = -0.6f; gazeY.target = -0.4f + line * 0.27f; nextAction = now + 380; }
        else { gazeX.target = 0.6f; nextAction = now + (uint32_t)frand(1100, 1600); line = (line + 1) % 4; }
        right = !right;
        break;
      }
      case ATTENTION:
        gazeX.target = frand(-0.08f, 0.08f); gazeY.target = frand(-0.08f, 0.04f);
        nextAction = now + (uint32_t)frand(700, 1500);
        break;
      case IDLE: case DONE_WAIT: idleBehaviour(now); break;
      default: nextAction = now + 1000;
    }
  }
  if (now >= nextMicro) {  // micro saccades keep the eyes alive while fixating
    float amp = (mood == SLEEP) ? 0 : 0.035f;
    microX = frand(-amp, amp); microY = frand(-amp, amp);
    nextMicro = now + (uint32_t)frand(250, 700);
  }

  // blinking: fast close, short hold, eased open
  if (mood != SLEEP && !blinking && now >= nextBlink) startBlink(frand(0, 1) < 0.15f);
  if (blinking) {
    float t = (now - blinkStart) / 1000.0f;
    const float CLOSE = 0.07f, HOLD = 0.03f, OPEN = 0.13f, ONE = CLOSE + HOLD + OPEN, GAP = 0.06f;
    float total = blinkDouble ? ONE * 2 + GAP : ONE;
    float p = t;
    if (blinkDouble && t > ONE) p = (t < ONE + GAP) ? ONE : t - ONE - GAP;
    if (p < CLOSE) blinkOpen = 1 - easeInOut(p / CLOSE) * 0.95f;
    else if (p < CLOSE + HOLD) blinkOpen = 0.05f;
    else if (p < ONE) blinkOpen = 0.05f + 0.95f * easeInOut((p - CLOSE - HOLD) / OPEN);
    else blinkOpen = 1;
    if (t >= total) {
      blinking = false; blinkOpen = 1;
      nextBlink = now + (uint32_t)(drowsy ? frand(1200, 2500) : frand(2000, 6000));
    }
  }

  sH.k = mood == SLEEP ? 6.0f : 90.0f;
  sH.zeta = mood == SLEEP ? 1.0f : 0.85f;
  musicScroll += 30.0f * dt;
  musicAlpha = approach(musicAlpha, musicOn && musicTrack.length() ? 1 : 0, 4, dt);
  for (Spring *s : SHAPE_SPRINGS) s->step(dt);
  gazeX.step(dt); gazeY.step(dt);
  float cs = mood == SLEEP ? 1.2f : 5.0f;
  col.r = approach(col.r, colTarget.r, cs, dt);
  col.g = approach(col.g, colTarget.g, cs, dt);
  col.b = approach(col.b, colTarget.b, cs, dt);
}

// ---------- now playing ----------
static void drawNowPlaying(float t) {
  if (musicAlpha < 0.02f) return;
  const RGB c = {150, 152, 158};
  const int base = H - 18, gapPx = 60;
  const int boxX0 = 40, boxX1 = W - 40;  // text area
  int tw = textWidth(FONT_SMALL, musicTrack);
  int avail = boxX1 - boxX0;
  float a = musicAlpha * 0.9f;
  if (tw <= avail) {
    clipX0 = 0; clipX1 = W - 1;
    drawText(FONT_SMALL, musicTrack, (W - tw) / 2, base, c, a);
  } else {
    clipX0 = boxX0; clipX1 = boxX1;
    int off = (int)musicScroll % (tw + gapPx);
    drawText(FONT_SMALL, musicTrack, boxX0 - off, base, c, a);
    drawText(FONT_SMALL, musicTrack, boxX0 - off + tw + gapPx, base, c, a);
    clipX0 = 0; clipX1 = W - 1;
  }
}

// ---------- render ----------
static const float FACE_LIFT = 14;  // face + dots sit a bit higher to leave room for the track line
static void render(float t, uint32_t now) {
  uint32_t t0 = micros();
  prevDrawn.clip();
  clearBox(prevDrawn);
  drawn = Box();

  uint32_t inMood = now - moodSince;
  float offX = 0, offY = 0;
  if (mood == DONE || mood == LOVE || mood == HELLO) {
    float decay = mood == DONE ? expf(-inMood / 2500.0f) : 1.0f;
    offY -= fabsf(sinf(t * 7.0f)) * 14.0f * decay;
  }
  if (mood == DONE_WAIT) {
    float ph = fmodf(inMood / 1000.0f, 6.0f);
    if (ph < 0.9f) offY -= fabsf(sinf(ph * PI / 0.45f)) * 10.0f;  // two small hops
  }
  if (mood == ATTENTION) {
    float ph = fmodf(inMood / 1000.0f, 2.2f);
    if (ph < 0.45f) offX = sinf(ph * 48.0f) * 9.0f * (1.0f - ph / 0.45f);
  }
  if (mood == SLEEP) offY += 5.0f * sinf(t * 0.9f);


  float breathe = 1.0f + 0.02f * sinf(t * 1.6f);
  float gx = clampf(gazeX.x + microX, -1.2f, 1.2f), gy = clampf(gazeY.x + microY, -1.2f, 1.2f);
  float speed = sqrtf(gazeX.v * gazeX.v + gazeY.v * gazeY.v);
  float squash = clampf(speed * 0.045f, 0, 0.22f);  // fast motion squashes the eyes a bit

  float cx = W / 2 + gx * 64.0f + offX;
  float cy = H / 2 - FACE_LIFT + gy * 40.0f + offY;
  float persp = gx * 0.07f;  // the eye on the gaze side grows
  float w = sW.x * (1 + squash * 0.5f), h = sH.x * breathe * blinkOpen * (1 - squash);

  for (int i = 0; i < 2; i++) {
    bool left = i == 0;
    float scale = 1 + (left ? -persp : persp) + (left ? -eyeDiff.x : eyeDiff.x);
    EyeShape e;
    e.left = left;
    e.cx = cx + (left ? -1 : 1) * sGap.x / 2;
    e.cy = cy + (1 - blinkOpen) * 6;
    e.w = w * scale; e.h = h * scale; e.r = sR.x * scale;
    e.lidTop = clampf(sLidTop.x, 0, 0.98f);
    e.lidSlant = max(0.0f, sLidSlant.x);
    e.lidBottom = clampf(sLidBottom.x, 0, 0.9f);
    e.happy = clampf(sHappy.x + smile.x, 0, 1.2f);
    e.pupil = sPupil.x;
    e.pgx = gx; e.pgy = gy;
    drawEye(e, col);
  }

  if (mood == WORKING) {
    for (int i = 0; i < 3; i++) {
      float a = 0.35f + 0.65f * (0.5f + 0.5f * sinf(t * 5.0f - i * 0.9f));
      drawCircle(W / 2 - 20 + i * 20, H - 52 - FACE_LIFT, 5, col, a);
    }
  }
  if (mood == SLEEP) {
    for (int i = 0; i < 3; i++) {
      float ph = fmodf(t * 0.3f + i / 3.0f, 1.0f);
      drawText(FONT_SMALL, "z", (int)(cx + sGap.x / 2 + 58 + ph * 36), (int)(cy - 20 - ph * 60), col, sinf(ph * PI) * 0.8f);
    }
  }
  if (mood == LOVE) drawHeart(cx, cy - 92 + offY * 0.3f, 16, col);
  drawNowPlaying(t);


  uint32_t t1 = micros();
  Box flush = drawn;
  flush.add(prevDrawn);
  flush.clip();
  flushBox(flush);
  uint32_t t2 = micros();
  statDrawUs = statDrawUs * 0.95f + (t1 - t0) * 0.05f;
  statFlushUs = statFlushUs * 0.95f + (t2 - t1) * 0.05f;
  prevDrawn = drawn;
}

// ---------- serial ----------
static String sanitize(const String &s) {
  String o;
  for (size_t i = 0; i < s.length() && o.length() < 60; i++) {
    uint8_t c = s[i];
    if (c >= 32 && c != 127) o += (char)c;
  }
  o.trim();
  return o;
}

static bool demoRunning = false;
static uint32_t demoNext = 0;
static int demoStep = 0;
static float statFrame = 0, statWork = 0;

static void handleCommand(String line) {
  line.trim();
  if (!line.length()) return;
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : sanitize(line.substring(sp + 1));
  cmd.toLowerCase();

  if (cmd == "ping") { Serial.println("bob ok " BOB_VERSION); return; }
  if (cmd == "stats") {
    Serial.printf("bob stats frame=%.1fms (%.0f fps) work=%.1fms draw=%.1fms flush=%.1fms lastflush=%upx mood=%s\n",
                  statFrame, 1000.0f / max(statFrame, 1.0f), statWork, statDrawUs / 1000, statFlushUs / 1000, (unsigned)flushPixels, MOOD_NAMES[mood]);
    return;
  }
  demoRunning = false;
  if (cmd == "working") {
    if (mood == WORKING) { lastActivity = millis(); if (arg.length()) label = arg; return; }
    setMood(WORKING, arg);
  } else if (cmd == "tool") {
    // async hooks can arrive after "done": ignore tool events shortly after it
    uint32_t inMood = millis() - moodSince;
    if (mood == WORKING) { lastActivity = millis(); if (arg.length()) label = arg; return; }
    if ((mood == DONE || mood == DONE_WAIT) && inMood < 8000) return;
    if (mood == LOVE) return;
    setMood(WORKING, arg);
  }
  else if (cmd == "done") setMood(DONE, arg);
  else if (cmd == "attention") setMood(ATTENTION, arg);
  else if (cmd == "idle" || cmd == "wake") setMood(IDLE);
  else if (cmd == "sleep") setMood(SLEEP);
  else if (cmd == "hello") setMood(HELLO);
  else if (cmd == "music") {
    // "music play [artist - title]" | "music stop"
    bool play = arg.startsWith("play");
    bool was = musicOn;
    musicOn = play;
    musicLast = millis();
    if (play) {
      String tr = arg.length() > 5 ? arg.substring(5) : "";
      tr.trim();
      if (tr != musicTrack) { musicTrack = tr; musicScroll = 0; }
      if (!was && mood == SLEEP) setMood(IDLE);
    }
  }
  else if (cmd == "bright") { brightness = (uint8_t)constrain(arg.toInt(), 0, 255); panel->setBrightness(brightness); }
  else if (cmd == "demo") { demoRunning = true; demoStep = 0; demoNext = 0; }
  else Serial.printf("bob err unknown '%s'\n", cmd.c_str());
}

static void pollSerial() {
  static String buf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') { if (buf.length()) handleCommand(buf); buf = ""; }
    else if (buf.length() < 200) buf += c;
  }
}

static void runDemo(uint32_t now) {
  if (!demoRunning || now < demoNext) return;
  switch (demoStep) {
    case 0: setMood(HELLO); demoNext = now + 3500; break;
    case 1: setMood(IDLE); demoNext = now + 9000; break;
    case 2: setMood(WORKING, "mein-projekt"); demoNext = now + 7000; break;
    case 3: setMood(ATTENTION, "mein-projekt"); demoNext = now + 5000; break;
    case 4: setMood(DONE, "mein-projekt"); demoNext = now + 10000; break;
    case 5: moodAfterLove = IDLE; setMood(LOVE); demoNext = now + 2500; break;
    case 6: setMood(SLEEP); demoNext = now + 7000; break;
    default: setMood(IDLE); demoRunning = false; return;
  }
  demoStep++;
}

// ---------- main ----------
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);

  if (!canvas->begin()) Serial.println("bob err display init failed");
  fb = canvas->getFramebuffer();
  txbuf = (uint16_t *)heap_caps_malloc(PW * TX_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  memset(fb, 0, PW * PH * 2);
  canvas->flush();
  panel->setBrightness(brightness);

  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  Wire.beginTransmission(TOUCH_ADDR);
  touchOk = Wire.endTransmission() == 0;

  for (Spring *s : SHAPE_SPRINGS) { s->k = 90; s->zeta = 0.85f; }
  gazeX.k = gazeY.k = 170; gazeX.zeta = gazeY.zeta = 0.72f;
  sW.set(L_IDLE.w); sH.set(L_IDLE.h); sR.set(L_IDLE.r); sGap.set(L_IDLE.gap);
  sH.set(9);  // boot: eyes start as thin lines and open up

  uint32_t now = millis();
  nextBlink = now + 2500;
  setMood(HELLO);
  Serial.printf("bob ready %s touch=%d cs=%d\n", BOB_VERSION, touchOk, BOB_LCD_CS);
}

void loop() {
  static uint32_t last = millis();
  static bool wasTouched = false;
  static uint32_t touchStart = 0;

  uint32_t now = millis();
  float dt = clampf((now - last) / 1000.0f, 0.001f, 0.05f);
  statFrame = statFrame * 0.95f + (now - last) * 0.05f;
  last = now;

  pollSerial();
  runDemo(now);

  bool touched = readTouch();
  if (touched && !wasTouched) touchStart = now;
  if (!touched && wasTouched && now - touchStart < 1500) onTap();
  wasTouched = touched;

  now = millis();  // commands above may have set timestamps later than the old 'now'
  updateBehaviour(dt, now);
  if (screenOn) render(now / 1000.0f, now);

  uint32_t spent = millis() - now;
  statWork = statWork * 0.95f + spent * 0.05f;
  if (spent < 16) delay(16 - spent);
}
