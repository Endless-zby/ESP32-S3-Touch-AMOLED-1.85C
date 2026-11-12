#include "face_render.h"
#include <math.h>
#include <string.h>
#include <esp_heap_caps.h>

#define AI __attribute__((always_inline)) static inline

// ---------- math helpers (same shape math as the reference renderer) ----------
AI float fsqrt(float x) {
  if (x <= 0.f) return 0.f;
  union { float f; uint32_t i; } u;
  u.f = x;
  u.i = 0x5f375a86 - (u.i >> 1);
  float y = u.f;
  y = y * (1.5f - 0.5f * x * y * y);
  y = y * (1.5f - 0.5f * x * y * y);
  return x * y;
}
AI float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
AI float fmaxf_(float a, float b) { return a > b ? a : b; }
AI float fminf_(float a, float b) { return a < b ? a : b; }
AI float smax(float a, float b, float k, float invK) {
  float h = clamp01(0.5f + 0.5f * (a - b) * invK);
  return (b + (a - b) * h) + k * h * (1.f - h);
}
AI float sdRoundRect(float qx, float qy, float hw, float hh, float r) {
  float ax = fabsf(qx) - (hw - r);
  float ay = fabsf(qy) - (hh - r);
  float mx = ax > 0.f ? ax : 0.f;
  float my = ay > 0.f ? ay : 0.f;
  float outside = (mx > 0.f && my > 0.f) ? fsqrt(mx * mx + my * my) : (mx + my);
  float inside = fminf_(fmaxf_(ax, ay), 0.f);
  return outside + inside - r;
}
AI void blendRow(uint8_t *row, int x, int v) {
  int o = row[x];
  row[x] = (uint8_t)(o + v - ((o * v + 127) / 255));
}

bool FaceRenderer::begin(uint16_t *buf, int w, int h) {
  if (!buf || w <= 0 || h <= 0) return false;
  _buf = buf;
  _w = w;
  _h = h;
  _scale = (float)w / 466.f;
  _edge = EDGE_SOFT * _scale;
  _glow = GLOW_RADIUS * _scale;
  if (_edge < 0.6f) _edge = 0.6f;
  if (_glow < 3.f) _glow = 3.f;
  _invEdge2 = 1.f / (2.f * _edge);
  _invGlow = 1.f / _glow;
  _row = (uint8_t *)malloc(w > 512 ? w : 512);
  if (!_row) return false;
  for (int i = 0; i < 256; i++) {
    float v = powf(i / 255.f, GAMMA_ENC);
    uint8_t g = (uint8_t)(255.f * v + 0.5f);
    _lut[i] = (uint16_t)(((g & 0xF8) << 8) | ((g & 0xFC) << 3) | (g >> 3));
  }
  resetDirty();
  return true;
}

void FaceRenderer::resetDirty() {
  for (int i = 0; i < FACE_SHAPE_COUNT; i++) _prev[i] = {0, 0, 0, 0};
}

bool FaceRenderer::beginRing() {
  if (!_buf) return false;
  _ringMask = (uint8_t *)heap_caps_malloc((size_t)_w * _h, MALLOC_CAP_SPIRAM);
  if (!_ringMask) return false;
  const float cx = _w * 0.5f, cy = _h * 0.5f;
  const float outer = _w * 0.5f - 1.0f;               // hug the glass edge
  const float width = _w * (2.0f / 46.99f);           // 2 mm on a 1.85" panel
  const float inner = outer - width;
  uint8_t *m = _ringMask;
  for (int y = 0; y < _h; y++) {
    for (int x = 0; x < _w; x++, m++) {
      const float dx = (x + 0.5f) - cx, dy = (y + 0.5f) - cy;
      const float r = sqrtf(dx * dx + dy * dy);
      float a = (r - inner) / width;
      if (a <= 0.f) { *m = 0; continue; }
      if (a > 1.f) a = 1.f;
      *m = (uint8_t)(255.f * a * a * (3.f - 2.f * a));
    }
  }
  for (int i = 0; i < 256; i++) {
    const float v = i / 255.f;
    const uint8_t r = (uint8_t)(139.f * v), g = (uint8_t)(92.f * v), b = (uint8_t)(246.f * v);
    _ringLut[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }
  return true;
}

void FaceRenderer::ringDraw(uint8_t level) {
  if (!_ringMask || !_buf) return;
  const uint8_t *m = _ringMask;
  uint16_t *p = _buf;
  const int n = _w * _h;
  for (int i = 0; i < n; i++, m++, p++) {
    const uint8_t a = *m;
    if (!a) continue;
    *p = _ringLut[(uint16_t)((uint16_t)a * level / 255u)];
  }
}

void FaceRenderer::clearAll() {
  if (!_buf) return;
  memset(_buf, 0, (size_t)_w * _h * sizeof(uint16_t));
  resetDirty();
}

EyeShape FaceRenderer::scEye(const EyeShape &e) const {
  EyeShape o = e;
  o.cx *= _scale; o.cy *= _scale;
  o.hw *= _scale; o.hh *= _scale; o.r *= _scale;
  return o;   // angles / lid parameters are normalised already
}

BrowShape FaceRenderer::scBrow(const BrowShape &b) const {
  BrowShape o = b;
  o.cx *= _scale; o.cy *= _scale;
  o.len *= _scale; o.thick *= _scale;
  return o;
}

// ---------- bounding boxes ----------
static IRect unionRect(const IRect &a, const IRect &b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  return { min(a.x0, b.x0), min(a.y0, b.y0), max(a.x1, b.x1), max(a.y1, b.y1) };
}
static bool intersects(const IRect &a, const IRect &b) {
  return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

IRect FaceRenderer::eyeBBox(const EyeShape &e) const {
  float c = fabsf(cosf(e.ang)), s = fabsf(sinf(e.ang));
  float ex = c * e.hw + s * e.hh + _glow + 2.f * _scale;
  float ey = s * e.hw + c * e.hh + _glow + 2.f * _scale;
  return { (int)floorf(e.cx - ex), (int)floorf(e.cy - ey), (int)ceilf(e.cx + ex) + 1, (int)ceilf(e.cy + ey) + 1 };
}

IRect FaceRenderer::browBBox(const BrowShape &b) const {
  float bend = b.bend < 0.02f ? 0.02f : b.bend;
  float ra = (b.len * 0.5f) / bend;
  float ex = ra * sinf(bend), ey = ra - ra * cosf(bend);
  float px[3] = { 0.f, -ex, ex }, py[3] = { 0.f, ey, ey };
  float c = cosf(b.ang), s = sinf(b.ang);
  float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
  for (int i = 0; i < 3; i++) {
    float X = b.cx + px[i] * c - py[i] * s;
    float Y = b.cy + px[i] * s + py[i] * c;
    minx = fminf_(minx, X); maxx = fmaxf_(maxx, X);
    miny = fminf_(miny, Y); maxy = fmaxf_(maxy, Y);
  }
  float m = b.thick + _glow + 4.f * _scale;
  return { (int)floorf(minx - m), (int)floorf(miny - m), (int)ceilf(maxx + m) + 1, (int)ceilf(maxy + m) + 1 };
}

// ---------- per row shading ----------
void FaceRenderer::shadeEyeRow(const EyeShape &e, int y, int x0, int x1) {
  float c = cosf(e.ang), s = sinf(e.ang);
  float px = (x0 + 0.5f) - e.cx, py = (y + 0.5f) - e.cy;
  const float qx0 = px * c + py * s;
  const float qy0 = -px * s + py * c;
  float r = fminf_(e.r, fminf_(e.hw, e.hh));
  if (r < 0.5f) r = 0.5f;
  const bool useTop = e.topLid > 0.002f;
  const bool useBot = e.botLid > 0.002f;
  const float lidY = -e.hh + e.topLid * 2.f * e.hh;
  const float lidC = 1.f / fsqrt(1.f + e.topLidK * e.topLidK);
  const float Rb = e.hw * 2.6f;
  const float bcy = e.hh + Rb - e.botLid * 2.f * e.hh;
  const float kTop = 6.f * _scale, invKTop = 1.f / kTop;
  const float kBot = 9.f * _scale, invKBot = 1.f / kBot;
  uint8_t *row = _row;
  int x = x0;
  while (x < x1) {
    float t = (float)(x - x0);
    float qx = qx0 + t * c;
    float qy = qy0 - t * s;
    float d = sdRoundRect(qx, qy, e.hw, e.hh, r);
    if (d < _glow) {
      if (useTop) { float dl = (lidY + e.topLidK * qx - qy) * lidC; d = smax(d, dl, kTop, invKTop); }
      if (useBot) { float dx = qx, dy = qy - bcy; float db = Rb - fsqrt(dx * dx + dy * dy); d = smax(d, db, kBot, invKBot); }
    }
    if (d <= -_edge) {
      int n = 1 + (int)((-d - _edge) * 0.9f);
      int xe = min(x1, x + n);
      memset(row + x, 255, xe - x);
      x = xe;
    } else if (d >= _glow) {
      x += 1 + (int)((d - _glow) * 0.9f);
    } else {
      float core;
      if (d >= _edge) core = 0.f;
      else { float t2 = 0.5f - d * _invEdge2; core = t2 * t2 * (3.f - 2.f * t2); }
      float g = d <= 0.f ? 1.f : (1.f - d * _invGlow);
      g = g * g * GLOW_ALPHA;
      int v = (int)((core + (1.f - core) * g) * 255.f + 0.5f);
      blendRow(row, x, v);
      x++;
    }
  }
}

void FaceRenderer::shadeBrowRow(const BrowShape &b, int y, int x0, int x1) {
  float c = cosf(b.ang), s = sinf(b.ang);
  float bend = b.bend < 0.02f ? 0.02f : (b.bend > 1.4f ? 1.4f : b.bend);
  const float ra = (b.len * 0.5f) / bend;
  const float scx = sinf(bend), scy = cosf(bend);
  const float thick = b.thick;
  uint8_t *row = _row;
  float px = (x0 + 0.5f) - b.cx, py = (y + 0.5f) - b.cy;
  const float qx0 = px * c + py * s;
  const float qy0 = -px * s + py * c;
  int x = x0;
  while (x < x1) {
    float t = (float)(x - x0);
    float qx = qx0 + t * c;
    float qy = qy0 - t * s;
    float ax = fabsf(qx);
    float ay = ra - qy;
    float d;
    if (scy * ax > scx * ay) { float dx = ax - scx * ra, dy = ay - scy * ra; d = fsqrt(dx * dx + dy * dy); }
    else d = fabsf(fsqrt(ax * ax + ay * ay) - ra);
    d -= thick;
    if (d <= -_edge) {
      int n = 1 + (int)((-d - _edge) * 0.9f);
      int xe = min(x1, x + n);
      memset(row + x, 255, xe - x);
      x = xe;
    } else if (d >= _glow) {
      x += 1 + (int)((d - _glow) * 0.9f);
    } else {
      float core;
      if (d >= _edge) core = 0.f;
      else { float t2 = 0.5f - d * _invEdge2; core = t2 * t2 * (3.f - 2.f * t2); }
      float g = d <= 0.f ? 1.f : (1.f - d * _invGlow);
      g = g * g * GLOW_ALPHA;
      int v = (int)((core + (1.f - core) * g) * 255.f + 0.5f);
      blendRow(row, x, v);
      x++;
    }
  }
}

void FaceRenderer::renderRegion(const IRect &reg, const EyeShape eyes[2], const BrowShape brows[2], const IRect bb[FACE_SHAPE_COUNT]) {
  const int W = reg.x1 - reg.x0;
  if (W <= 0) return;
  // The shade functions index _row with absolute x coordinates, so start the
  // clear/read window at _row + reg.x0 (same convention as the reference code).
  uint8_t *row = _row + reg.x0;
  for (int y = reg.y0; y < reg.y1; y++) {
    memset(row, 0, W);
    for (int i = 0; i < FACE_SHAPE_COUNT; i++) {
      if (y < bb[i].y0 || y >= bb[i].y1) continue;
      int x0 = max(reg.x0, bb[i].x0), x1 = min(reg.x1, bb[i].x1);
      if (x1 <= x0) continue;
      if (i < 2) shadeEyeRow(eyes[i], y, x0, x1);
      else shadeBrowRow(brows[i - 2], y, x0, x1);
    }
    uint16_t *dst = _buf + (size_t)y * _w + reg.x0;
    for (int x = 0; x < W; x++) dst[x] = _lut[row[x]];
  }
}

int FaceRenderer::render(const EyeShape eyesIn[2], const BrowShape browsIn[2], IRect out[], int maxOut) {
  if (!_buf || !_row) return 0;
  uint32_t t0 = micros();
  EyeShape eyes[2] = { scEye(eyesIn[0]), scEye(eyesIn[1]) };
  BrowShape brows[2] = { scBrow(browsIn[0]), scBrow(browsIn[1]) };
  IRect bb[FACE_SHAPE_COUNT];
  bb[0] = eyeBBox(eyes[0]);
  bb[1] = eyeBBox(eyes[1]);
  bb[2] = browBBox(brows[0]);
  bb[3] = browBBox(brows[1]);
  IRect regs[FACE_SHAPE_COUNT];
  int nreg = 0;
  for (int i = 0; i < FACE_SHAPE_COUNT; i++) {
    IRect r = unionRect(_prev[i], bb[i]);
    _prev[i] = bb[i];
    r.x0 = max(0, r.x0 & ~1);
    r.y0 = max(0, r.y0 & ~1);
    r.x1 = min(_w, (r.x1 + 1) & ~1);
    r.y1 = min(_h, (r.y1 + 1) & ~1);
    if (!r.empty()) regs[nreg++] = r;
  }
  bool merged = true;
  while (merged) {
    merged = false;
    for (int i = 0; i < nreg && !merged; i++)
      for (int j = i + 1; j < nreg; j++)
        if (intersects(regs[i], regs[j])) { regs[i] = unionRect(regs[i], regs[j]); regs[j] = regs[--nreg]; merged = true; break; }
  }
  _lastPixels = 0;
  int n = 0;
  for (int i = 0; i < nreg; i++) {
    // shade functions take absolute x; our row buffer is relative to reg.x0, so
    // render with the row pointer offset - see renderRegion: it memsets row at 0
    // but shading writes with absolute x. Keep regs aligned by shifting the row
    // pointer when shading.
    renderRegion(regs[i], eyes, brows, bb);
    if (n < maxOut) out[n++] = regs[i];
    _lastPixels += (uint32_t)(regs[i].x1 - regs[i].x0) * (regs[i].y1 - regs[i].y0);
  }
  (void)t0;
  return n;
}
