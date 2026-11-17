#pragma once
#include <Arduino.h>

// Realtime voice session with Qwen (DashScope Realtime API over WebSocket).
//
//   mic (16 kHz, after AEC) --> input_audio_buffer.append (base64 PCM16)
//   response.audio.delta   --> playback ring (PCM16 at the server's output rate)
//   response.function_call_arguments.done --> tool request handed to the app
//
// The WebSocket runs in its own FreeRTOS task; the audio task pushes/pops the
// two rings; the Arduino loop() consumes UI/tool events via poll().
namespace VoiceSession {

enum State { VS_OFF = 0, VS_CONNECTING, VS_READY, VS_ERROR };

struct Event {
  enum Kind { NONE = 0, CONNECTED, FAILED, CLOSED, USER_SPEECH_START, USER_SPEECH_STOP,
              TOOL_CALL, RESPONSE_DONE } kind = NONE;
  String text;        // FAILED: reason
  String toolName;    // TOOL_CALL
  String toolArgs;    // TOOL_CALL (JSON string)
  String callId;      // TOOL_CALL
};

struct Config {
  String host;          // dashscope.aliyuncs.com  or  <WorkspaceId>.cn-beijing.maas.aliyuncs.com
  String model;         // qwen-audio-3.1-realtime-flash
  String apiKey;
  String voice;         // empty = server default
  String instructions;  // system prompt
  String greeting;      // instructions for the opening line (empty = no greeting)
  uint32_t outRate = 24000;   // expected PCM rate of response.audio.delta
  String toolsJson;     // JSON array of tool definitions
};

bool begin(const Config &cfg);   // connect + configure the session (async)
void end();                      // close the socket, drop audio

// Called from the Arduino loop: next pending event (false = none).
bool poll(Event &ev);

// Tool result back to the model, followed by response.create.
void sendToolResult(const String &callId, const String &outputJson);
// Ask the model to speak a given instruction (e.g. the greeting).
void requestResponse(const String &instructions);
// Drop everything buffered for playback (barge-in).
void flushPlayback();

// ---- audio-task side ----
void pushMic(const int16_t *pcm16k, int samples);          // after AEC
int  popPlay(int16_t *out, int samples);                   // PCM at outRate; returns samples copied (0 = silence)
bool playbackActive();                                      // ring has data / preroll satisfied
uint32_t playbackRate();

// activity timestamp (millis) for the idle timer
uint32_t lastUserSpeechMs();

// Reply caption. The ws task keeps it in a fixed buffer; the UI polls it at its
// own rate (4x/s) instead of paying one String per transcript delta.
bool takeTranscriptDirty();                 // true once per change
void copyTranscript(char *dst, size_t cap); // NUL-terminated, may be empty

// diagnostics
struct Stats { uint32_t txChunks, rxAudioBytes, events, playUnderruns, playDrops, bytesIn, micDrops; int playDepthMs; };
Stats stats();

}  // namespace VoiceSession
