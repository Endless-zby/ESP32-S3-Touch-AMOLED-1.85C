#pragma once
#include <Arduino.h>
#include "face_render.h"

// Expression model, geometry in the 466 px reference space (scaled by the renderer)
#define SCREEN_CX     233.f
#define SCREEN_CY     233.f
#define EYE_SEP       92.f
#define EYE_BASE_Y    (SCREEN_CY + 4.f)
#define EYE_HW        50.f
#define EYE_HH        66.f
#define EYE_RADIUS    32.f
#define GAZE_MAX_X    34.f
#define GAZE_MAX_Y    26.f

struct EyeParams {
  float open, width, topLid, topLidAng, botLid, tilt;
  float browY, browX, browAng, browBend, browLen, browThick;
};
static const int EYE_PARAM_COUNT = sizeof(EyeParams) / sizeof(float);

struct Expression {
  const char *name;
  EyeParams L, R;
  float gazeX, gazeY;
};

enum ExprId {
  // 通用表情
  EX_NEUTRAL = 0, EX_HAPPY, EX_SAD, EX_ANGRY, EX_SURPRISED, EX_SLEEPY, EX_SUSPICIOUS,
  EX_ATTENTIVE, EX_THINKING,
  // 语音对话各状态的专属表情（切换时经弹簧平滑过渡）
  EX_CONNECTING,     // 连网/连服务：半睁、视线下垂，慢慢四处看
  EX_LISTENING,      // 就绪待命：放松、眉毛微扬、略带期待
  EX_USER_SPEAKING,  // 用户在说：睁大、眉毛抬高、整体上抬"凑近"
  EX_SPEAKING,       // 在回复：笑眼 + 轻微律动
  EX_NO_SPEECH,      // 没听清：歪头、一边挑眉、一边眯眼
  EX_ERROR,          // 出错：下垂的眉毛、眼睛压扁
  // 配网流程
  EX_SETUP_WAIT,     // 等手机连热点：好奇张望，眼睛圆、眉高，视线大幅游移
  EX_SETUP_LINKED,   // 手机已连上：惊喜，眼睛更大、眉更高，微笑眼
  EX_SETUP_CONNECT,  // 正在连 Wi-Fi：专注，眼睛略眯看向上方，眉毛平
  EX_SETUP_DONE,     // 联网成功：大笑眼
  EX_SETUP_FAIL,     // 连接失败：担忧，内眉上提，眼睛半闭
  EX_COUNT
};

struct Spring {
  float v = 0, vel = 0;
  void snap(float t) { v = t; vel = 0; }
  void update(float target, float omega, float zeta, float dt) {
    float a = omega * omega * (target - v) - 2.f * zeta * omega * vel;
    vel += a * dt;
    v += vel * dt;
  }
};

class FaceModel {
public:
  void begin();
  void update(float dt);
  void setExpression(int id);
  int  expression() const { return _expr; }
  const char *expressionName() const;
  void setGazeOverride(bool on, float gx, float gy);
  void setForceClosed(bool on) { _forceClosed = on; }
  void setTalking(bool on) { _talking = on; }   // 说话时眼睛轻微律动
  const EyeShape *eyes() const { return _eyes; }
  const BrowShape *brows() const { return _brows; }

private:
  int _expr = EX_NEUTRAL;
  Spring _pL[EYE_PARAM_COUNT], _pR[EYE_PARAM_COUNT];
  Spring _gazeX, _gazeY, _closed;
  float _t = 0;
  float _blinkTimer = 2.f, _blinkPhase = -1.f, _blinkDur = 0.22f;
  bool _doubleBlink = false;
  bool _gazeOverride = false;
  float _ovX = 0, _ovY = 0;
  float _idleGazeX = 0, _idleGazeY = 0, _idleTimer = 1.5f;
  float _browTwitch = 0, _twitchTimer = 5.f;
  bool _forceClosed = false, _talking = false;
  EyeShape _eyes[2];
  BrowShape _brows[2];
  void buildShapes(float blinkClose);
  static float frand(float a, float b) { return a + (b - a) * (float)random(0, 10001) / 10000.f; }
};
