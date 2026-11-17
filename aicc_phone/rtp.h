#pragma once
#include <Arduino.h>
#include <NetworkUdp.h>
#include "config.h"

// Minimal RTP session for G.711 (PCMU=0 / PCMA=8), 20 ms frames.
class RtpSession {
 public:
  bool begin(uint16_t localPort);
  void end();

  // Remote media description (from SDP). `pt` is the payload type we send.
  void setRemote(IPAddress ip, uint16_t port, uint8_t pt, int dtmfPt);
  void clearRemote();

  // Send one 20 ms frame of 8 kHz PCM (FRAME_8K samples).
  void sendFrame(const int16_t *pcm);
  // Read all pending datagrams into the jitter buffer.
  void pump();
  // Pop one decoded frame (FRAME_8K samples). Returns false when nothing to play.
  bool popFrame(int16_t *pcm);

  uint32_t lastRxMs() const { return _lastRxMs; }
  // diagnostics (reset by statsReset)
  uint32_t statUnderruns() const { return _underruns; }
  uint32_t statDrops() const { return _drops; }
  uint32_t statRx() const { return _rxWindow; }
  uint32_t statSeqGaps() const { return _seqGaps; }
  uint32_t statTx() const { return _txWindow; }
  uint32_t statTxFail() const { return _txFail; }
  int jbDepth() const { return _jbCount; }
  void statsReset() { _underruns = _drops = _rxWindow = _seqGaps = _txWindow = _txFail = 0; }

 private:
  NetworkUDP _udp;
  uint16_t _localPort = 0;
  IPAddress _remoteIp;
  uint16_t _remotePort = 0;
  uint8_t _pt = 0;
  int _dtmfPt = -1;
  bool _latched = false;

  uint16_t _seq = 0;
  uint32_t _ts = 0;
  uint32_t _ssrc = 0;
  uint32_t _lastRxMs = 0;
  uint32_t _rxPackets = 0, _txPackets = 0;

  // jitter buffer: ring of raw G.711 frames (20 ms each)
  static const int JB_SLOTS = 20;      // 400 ms capacity
  static const int JB_PREFILL = 4;     // start playing after 80 ms
  static const int JB_MAX = 14;        // drop oldest beyond 280 ms
  uint32_t _underruns = 0, _drops = 0, _rxWindow = 0, _seqGaps = 0;
  uint32_t _txWindow = 0, _txFail = 0;   // window counters: frames accepted / not sent
  uint16_t _lastSeq = 0;
  bool _haveSeq = false;
  struct Slot { uint8_t data[FRAME_8K]; uint8_t pt; bool used; };
  Slot _jb[JB_SLOTS];
  int _jbHead = 0, _jbTail = 0, _jbCount = 0;
  bool _playing = false;
  uint8_t _lastFrame[FRAME_8K];
  uint8_t _lastPt = 0;
  bool _haveLast = false;
  int _plcCount = 0;

  void jbPush(const uint8_t *data, uint8_t pt);
  bool jbPop(uint8_t *data, uint8_t &pt);
};
