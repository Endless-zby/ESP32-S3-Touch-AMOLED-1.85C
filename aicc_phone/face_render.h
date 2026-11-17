#pragma once
#include <Arduino.h>

// Appearance (measured in the 466 px reference space; scaled to the canvas).
#define EDGE_SOFT   1.1f   // anti-alias transition half width (px)
#define GLOW_RADIUS 7.0f   // outer glow radius (px)
#define GLOW_ALPHA  0.22f  // outer glow strength (0..1)
#define GAMMA_ENC   (1.0f / 1.7f)
#define FACE_SHAPE_COUNT 4

struct EyeShape {
  float cx, cy;
  float hw, hh;
  float r;
  float ang;
  float topLid;
  float topLidK;
  float botLid;
};

struct BrowShape {
  float cx, cy;
  float ang;
  float len;
  float bend;
  float thick;
};

struct IRect {
  int x0, y0, x1, y1;
  bool empty() const { return x1 <= x0 || y1 <= y0; }
};

// Renders the eye/brow shapes straight into an RGB565 canvas buffer
// (LV_COLOR_FORMAT_RGB565, native byte order - LVGL swaps on flush). Only the
// shape bounding boxes are re-rendered each frame and reported back as dirty
// rectangles, so the caller can invalidate exactly those areas.
class FaceRenderer {
 public:
  bool begin(uint16_t *buf, int w, int h);
  // Render one frame; fills out[] with the rectangles that changed.
  // Returns the number of rectangles (0..maxOut).
  int render(const EyeShape eyes[2], const BrowShape brows[2], IRect out[], int maxOut);
  void resetDirty();    // forget previous shapes (after a full clear / screen show)
  uint32_t lastPixels() const { return _lastPixels; }

 private:
  uint16_t *_buf = nullptr;
  int _w = 0, _h = 0;
  float _scale = 1.f;
  float _edge = EDGE_SOFT, _glow = GLOW_RADIUS;
  float _invEdge2 = 1.f, _invGlow = 1.f;
  uint16_t _lut[256];
  uint8_t *_row = nullptr;
  uint32_t _lastPixels = 0;
  IRect _prev[FACE_SHAPE_COUNT];

  EyeShape scEye(const EyeShape &e) const;
  BrowShape scBrow(const BrowShape &b) const;
  IRect eyeBBox(const EyeShape &e) const;
  IRect browBBox(const BrowShape &b) const;
  void shadeEyeRow(const EyeShape &e, int y, int x0, int x1);
  void shadeBrowRow(const BrowShape &b, int y, int x0, int x1);
  void renderRegion(const IRect &reg, const EyeShape eyes[2], const BrowShape brows[2], const IRect bb[FACE_SHAPE_COUNT]);
};
