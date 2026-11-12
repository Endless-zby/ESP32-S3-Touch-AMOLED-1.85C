#include "face_model.h"
#include <math.h>

// 字段顺序: open, width, topLid, topLidAng, botLid, tilt, browY, browX, browAng, browBend, browLen, browThick
#define P(...) EyeParams{ __VA_ARGS__ }
static const Expression EXPRS[EX_COUNT] = {
  { "neutral",
    P(1.00f, 1.00f, 0.00f,  0.00f, 0.00f,  0.00f, 112.f,  0.f,  0.04f, 0.42f, 104.f, 7.5f),
    P(1.00f, 1.00f, 0.00f,  0.00f, 0.00f,  0.00f, 112.f,  0.f,  0.04f, 0.42f, 104.f, 7.5f), 0.f, 0.f },
  { "happy",
    P(0.92f, 1.04f, 0.00f,  0.00f, 0.55f,  0.00f, 122.f,  2.f,  0.10f, 0.70f, 104.f, 7.5f),
    P(0.92f, 1.04f, 0.00f,  0.00f, 0.55f,  0.00f, 122.f,  2.f,  0.10f, 0.70f, 104.f, 7.5f), 0.f, -0.10f },
  { "sad",
    P(0.95f, 1.00f, 0.34f, -0.28f, 0.00f, -0.05f, 100.f,  8.f,  0.42f, 0.30f,  96.f, 7.0f),
    P(0.95f, 1.00f, 0.34f, -0.28f, 0.00f, -0.05f, 100.f,  8.f,  0.42f, 0.30f,  96.f, 7.0f), 0.f, 0.35f },
  { "angry",
    P(0.90f, 1.02f, 0.42f,  0.38f, 0.00f,  0.00f,  86.f,  6.f, -0.48f, 0.15f, 104.f, 8.5f),
    P(0.90f, 1.02f, 0.42f,  0.38f, 0.00f,  0.00f,  86.f,  6.f, -0.48f, 0.15f, 104.f, 8.5f), 0.f, 0.f },
  { "surprised",
    P(1.24f, 1.06f, 0.00f,  0.00f, 0.00f,  0.00f, 134.f,  0.f,  0.02f, 0.80f, 108.f, 7.0f),
    P(1.24f, 1.06f, 0.00f,  0.00f, 0.00f,  0.00f, 134.f,  0.f,  0.02f, 0.80f, 108.f, 7.0f), 0.f, -0.05f },
  { "sleepy",
    P(0.40f, 1.02f, 0.48f,  0.00f, 0.00f,  0.00f,  92.f,  0.f, -0.04f, 0.25f, 104.f, 7.0f),
    P(0.40f, 1.02f, 0.48f,  0.00f, 0.00f,  0.00f,  92.f,  0.f, -0.04f, 0.25f, 104.f, 7.0f), 0.f, 0.30f },
  { "suspicious",
    P(0.55f, 1.00f, 0.30f,  0.10f, 0.00f,  0.00f,  88.f,  4.f, -0.22f, 0.12f, 100.f, 7.5f),
    P(1.00f, 1.00f, 0.00f,  0.00f, 0.00f,  0.00f, 130.f,  0.f,  0.30f, 0.65f, 104.f, 7.0f), 0.30f, 0.f },
  { "attentive",   // 用户在说话：眼睛微睁、眉毛上扬
    P(1.10f, 1.03f, 0.00f,  0.00f, 0.00f,  0.00f, 124.f,  0.f,  0.08f, 0.60f, 106.f, 7.2f),
    P(1.10f, 1.03f, 0.00f,  0.00f, 0.00f,  0.00f, 124.f,  0.f,  0.08f, 0.60f, 106.f, 7.2f), 0.f, -0.08f },
  { "thinking",    // 思考：视线上飘，一侧挑眉，眼睛略眯
    P(0.80f, 1.00f, 0.16f,  0.06f, 0.00f,  0.00f, 104.f,  2.f, -0.06f, 0.30f, 102.f, 7.2f),
    P(0.86f, 1.00f, 0.00f,  0.00f, 0.00f,  0.00f, 128.f,  0.f,  0.26f, 0.62f, 104.f, 7.0f), 0.45f, -0.55f },
  { "connecting",  // 半睁、眼睑压低、眉毛平放，视线偏下
    P(0.78f, 1.00f, 0.30f,  0.00f, 0.00f,  0.00f, 100.f,  0.f,  0.00f, 0.22f, 100.f, 7.0f),
    P(0.78f, 1.00f, 0.30f,  0.00f, 0.00f,  0.00f, 100.f,  0.f,  0.00f, 0.22f, 100.f, 7.0f), 0.f, 0.40f },
  { "listening",   // 放松待命：眼睛稍高一点，眉毛微扬微弯
    P(1.04f, 1.00f, 0.00f,  0.00f, 0.00f,  0.00f, 118.f,  0.f,  0.10f, 0.50f, 104.f, 7.5f),
    P(1.04f, 1.00f, 0.00f,  0.00f, 0.00f,  0.00f, 118.f,  0.f,  0.10f, 0.50f, 104.f, 7.5f), 0.f, -0.05f },
  { "user_speaking", // 专注：睁大、眉毛高高抬起且更弯，内眼角略高（凑近感）
    P(1.18f, 1.04f, 0.00f,  0.00f, 0.00f,  0.03f, 136.f,  3.f,  0.14f, 0.78f, 108.f, 7.2f),
    P(1.18f, 1.04f, 0.00f,  0.00f, 0.00f,  0.03f, 136.f,  3.f,  0.14f, 0.78f, 108.f, 7.2f), 0.f, -0.12f },
  { "speaking",    // 回复：笑眼（下眼睑弧形上提）、眉毛舒展
    P(0.96f, 1.04f, 0.00f,  0.00f, 0.48f,  0.00f, 120.f,  1.f,  0.06f, 0.62f, 104.f, 7.5f),
    P(0.96f, 1.04f, 0.00f,  0.00f, 0.48f,  0.00f, 120.f,  1.f,  0.06f, 0.62f, 104.f, 7.5f), 0.f, -0.06f },
  { "no_speech",   // 疑惑：左眼眯 + 眉压低，右眼睁 + 眉高挑，两眼整体同向歪
    P(0.62f, 1.00f, 0.26f,  0.08f, 0.00f, -0.10f,  94.f,  4.f, -0.18f, 0.16f, 100.f, 7.5f),
    P(1.08f, 1.00f, 0.00f,  0.00f, 0.00f, -0.10f, 134.f,  0.f,  0.34f, 0.70f, 104.f, 7.0f), 0.25f, -0.10f },
  { "error",       // 难过：眉毛外低内高下垂，上眼睑斜压，眼睛变矮
    P(0.70f, 1.00f, 0.40f, -0.32f, 0.00f, -0.06f,  96.f,  9.f,  0.48f, 0.26f,  94.f, 7.0f),
    P(0.70f, 1.00f, 0.40f, -0.32f, 0.00f, -0.06f,  96.f,  9.f,  0.48f, 0.26f,  94.f, 7.0f), 0.f, 0.40f },
  { "setup_wait",    // 好奇张望
    P(1.12f, 1.02f, 0.00f,  0.00f, 0.00f,  0.00f, 128.f,  0.f,  0.06f, 0.66f, 106.f, 7.2f),
    P(1.12f, 1.02f, 0.00f,  0.00f, 0.00f,  0.00f, 128.f,  0.f,  0.06f, 0.66f, 106.f, 7.2f), 0.f, -0.05f },
  { "setup_linked",  // 惊喜
    P(1.22f, 1.06f, 0.00f,  0.00f, 0.22f,  0.00f, 140.f,  0.f,  0.10f, 0.82f, 108.f, 7.0f),
    P(1.22f, 1.06f, 0.00f,  0.00f, 0.22f,  0.00f, 140.f,  0.f,  0.10f, 0.82f, 108.f, 7.0f), 0.f, -0.10f },
  { "setup_connect", // 专注向上
    P(0.84f, 1.00f, 0.10f,  0.00f, 0.00f,  0.00f, 110.f,  0.f,  0.00f, 0.30f, 104.f, 7.4f),
    P(0.84f, 1.00f, 0.10f,  0.00f, 0.00f,  0.00f, 110.f,  0.f,  0.00f, 0.30f, 104.f, 7.4f), 0.f, -0.45f },
  { "setup_done",    // 大笑眼
    P(0.90f, 1.06f, 0.00f,  0.00f, 0.70f,  0.00f, 126.f,  2.f,  0.12f, 0.75f, 106.f, 7.5f),
    P(0.90f, 1.06f, 0.00f,  0.00f, 0.70f,  0.00f, 126.f,  2.f,  0.12f, 0.75f, 106.f, 7.5f), 0.f, -0.08f },
  { "setup_fail",    // 担忧
    P(0.62f, 1.00f, 0.30f, -0.26f, 0.00f, -0.04f, 102.f,  8.f,  0.40f, 0.28f,  96.f, 7.0f),
    P(0.62f, 1.00f, 0.30f, -0.26f, 0.00f, -0.04f, 102.f,  8.f,  0.40f, 0.28f,  96.f, 7.0f), 0.f, 0.25f },
};
#undef P

void FaceModel::begin() {
  const float *l = (const float *)&EXPRS[_expr].L;
  const float *r = (const float *)&EXPRS[_expr].R;
  for (int i = 0; i < EYE_PARAM_COUNT; i++) { _pL[i].snap(l[i]); _pR[i].snap(r[i]); }
  _gazeX.snap(0); _gazeY.snap(0); _closed.snap(0);
  _blinkTimer = frand(1.5f, 3.f);
  _idleTimer = frand(1.f, 2.5f);
  _twitchTimer = frand(4.f, 9.f);
  buildShapes(0.f);
}

void FaceModel::setExpression(int id) { if (id >= 0 && id < EX_COUNT) _expr = id; }
const char *FaceModel::expressionName() const { return EXPRS[_expr].name; }

void FaceModel::setGazeOverride(bool on, float gx, float gy) {
  _gazeOverride = on;
  _ovX = constrain(gx, -1.f, 1.f);
  _ovY = constrain(gy, -1.f, 1.f);
}

void FaceModel::update(float dt) {
  if (dt > 0.05f) dt = 0.05f;
  _t += dt;
  const Expression &E = EXPRS[_expr];
  const float *tl = (const float *)&E.L;
  const float *tr = (const float *)&E.R;
  for (int i = 0; i < EYE_PARAM_COUNT; i++) {
    bool isBrow = i >= 6;
    float omega = isBrow ? 8.5f : 10.f;
    float zeta = isBrow ? 0.72f : 0.95f;
    _pL[i].update(tl[i], omega, zeta, dt);
    _pR[i].update(tr[i], omega, zeta, dt);
  }
  float gx, gy;
  if (_gazeOverride) { gx = _ovX; gy = _ovY; }
  else {
    _idleTimer -= dt;
    if (_idleTimer <= 0.f) {
      if (frand(0.f, 1.f) < 0.45f) { _idleGazeX = 0; _idleGazeY = 0; }
      else { _idleGazeX = frand(-0.6f, 0.6f); _idleGazeY = frand(-0.45f, 0.45f); }
      _idleTimer = frand(1.2f, 3.5f);
    }
    float wander = 1.f;
    if (_expr == EX_THINKING) wander = 0.3f;
    else if (_expr == EX_USER_SPEAKING) wander = 0.15f;
    else if (_expr == EX_CONNECTING) wander = 1.3f;
    else if (_expr == EX_SETUP_WAIT) wander = 1.6f;
    else if (_expr == EX_SETUP_CONNECT) wander = 0.4f;
    gx = _idleGazeX * wander + E.gazeX;
    gy = _idleGazeY * wander + E.gazeY;
  }
  float gOmega = _gazeOverride ? 16.f : 7.f;
  _gazeX.update(constrain(gx, -1.f, 1.f), gOmega, 0.78f, dt);
  _gazeY.update(constrain(gy, -1.f, 1.f), gOmega, 0.78f, dt);

  float blinkClose = 0.f;
  if (_blinkPhase < 0.f) {
    _blinkTimer -= dt;
    if (_blinkTimer <= 0.f) { _blinkPhase = 0.f; _blinkDur = frand(0.20f, 0.28f); }
  } else {
    _blinkPhase += dt / _blinkDur;
    if (_blinkPhase >= 1.f) {
      _blinkPhase = -1.f;
      if (!_doubleBlink && frand(0.f, 1.f) < 0.18f) { _doubleBlink = true; _blinkTimer = 0.18f; }
      else { _doubleBlink = false; _blinkTimer = frand(2.2f, 5.5f); }
    } else {
      float p = _blinkPhase;
      float s = p < 0.38f ? p / 0.38f : 1.f - (p - 0.38f) / 0.62f;
      blinkClose = s * s * (3.f - 2.f * s);
    }
  }
  _closed.update(_forceClosed ? 1.f : 0.f, 9.f, 1.f, dt);
  blinkClose = max(blinkClose, _closed.v);

  _twitchTimer -= dt;
  if (_twitchTimer <= 0.f && !_gazeOverride) { _browTwitch = 1.f; _twitchTimer = frand(5.f, 11.f); }
  _browTwitch -= dt * 3.2f;
  if (_browTwitch < 0.f) _browTwitch = 0.f;

  buildShapes(blinkClose);
}

void FaceModel::buildShapes(float blinkClose) {
  float gx = _gazeX.v, gy = _gazeY.v;
  float offX = gx * GAZE_MAX_X, offY = gy * GAZE_MAX_Y;
  float breathe = 1.f + 0.015f * sinf(_t * 1.4f);
  float bob = 2.f * sinf(_t * 0.9f);
  float twitch = sinf(_browTwitch * 3.14159f) * 7.f;
  // 说话时轻微的节奏起伏
  float talk = _talking ? 0.04f * sinf(_t * 9.f) + 0.03f * sinf(_t * 13.7f) : 0.f;

  for (int side = 0; side < 2; side++) {
    const Spring *p = side == 0 ? _pL : _pR;
    float inner = side == 0 ? 1.f : -1.f;
    float baseX = SCREEN_CX - inner * EYE_SEP;
    float open = p[0].v, width = p[1].v, topLid = p[2].v, topLidAng = p[3].v;
    float botLid = p[4].v, tilt = p[5].v;
    float browY = p[6].v, browX = p[7].v, browAng = p[8].v, browBend = p[9].v;
    float browLen = p[10].v, browThick = p[11].v;
    float persp = 1.f - 0.10f * fabsf(gx);

    EyeShape &e = _eyes[side];
    e.cx = baseX + offX;
    e.cy = EYE_BASE_Y + offY + bob;
    e.hw = EYE_HW * width * persp;
    float hh = EYE_HH * (open + talk) * breathe;
    hh *= (1.f - blinkClose * 0.96f);
    if (hh < 2.5f) hh = 2.5f;
    e.hh = hh;
    e.r = EYE_RADIUS;
    e.ang = -inner * tilt;
    e.topLid = topLid * (1.f - blinkClose);
    e.topLidK = inner * tanf(topLidAng);
    e.botLid = botLid * (1.f - blinkClose);

    BrowShape &b = _brows[side];
    float browLift = -gy * 10.f + twitch;
    b.cx = baseX + inner * browX + offX * 0.55f;
    b.cy = EYE_BASE_Y + bob + offY * 0.35f - browY - browLift;
    b.ang = -inner * browAng + gx * 0.08f;
    b.len = browLen;
    b.bend = browBend;
    b.thick = browThick;
  }
}
