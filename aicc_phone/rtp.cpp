#include "rtp.h"
#include "g711.h"
#include <esp_random.h>
#include <string.h>

bool RtpSession::begin(uint16_t localPort) {
  g711_init();
  _localPort = localPort;
  _ssrc = esp_random();
  _seq = esp_random() & 0xFFFF;
  _ts = esp_random();
  memset(_jb, 0, sizeof(_jb));
  _jbHead = _jbTail = _jbCount = 0;
  _playing = false;
  _haveLast = false;
  return _udp.begin(localPort) != 0;
}

void RtpSession::end() {
  _udp.stop();
  clearRemote();
}

void RtpSession::setRemote(IPAddress ip, uint16_t port, uint8_t pt, int dtmfPt) {
  _remoteIp = ip;
  _remotePort = port;
  _pt = pt;
  _dtmfPt = dtmfPt;
  _latched = false;
  _jbHead = _jbTail = _jbCount = 0;
  _playing = false;
  _haveLast = false;
  _rxPackets = _txPackets = 0;
  _lastRxMs = 0;
  _haveSeq = false;
  statsReset();
}

void RtpSession::clearRemote() {
  _remotePort = 0;
  _latched = false;
  _jbHead = _jbTail = _jbCount = 0;
  _playing = false;
}

void RtpSession::sendFrame(const int16_t *pcm) {
  if (!_remotePort) { _txFail++; return; }   // no media address yet
  uint8_t pkt[12 + FRAME_8K];
  pkt[0] = 0x80;
  pkt[1] = _pt & 0x7F;
  pkt[2] = _seq >> 8; pkt[3] = _seq & 0xFF;
  pkt[4] = _ts >> 24; pkt[5] = _ts >> 16; pkt[6] = _ts >> 8; pkt[7] = _ts;
  pkt[8] = _ssrc >> 24; pkt[9] = _ssrc >> 16; pkt[10] = _ssrc >> 8; pkt[11] = _ssrc;
  g711_encode(pcm, pkt + 12, FRAME_8K, _pt == 8);
  _seq++;
  _ts += FRAME_8K;
  if (_udp.beginPacket(_remoteIp, _remotePort)) {
    _udp.write(pkt, sizeof(pkt));
    _udp.endPacket();
    _txPackets++;
    _txWindow++;
  } else {
    _txFail++;   // socket/write side refused the packet
  }
}

void RtpSession::pump() {
  for (int guard = 0; guard < 16; guard++) {
    int len = _udp.parsePacket();
    if (len <= 0) break;
    uint8_t buf[12 + 4 + 320];
    if (len > (int)sizeof(buf)) len = sizeof(buf);
    int n = _udp.read(buf, len);
    if (n < 12) continue;
    if ((buf[0] & 0xC0) != 0x80) continue;  // not RTP v2
    uint8_t pt = buf[1] & 0x7F;
    int cc = buf[0] & 0x0F;
    int hdr = 12 + cc * 4;
    if (buf[0] & 0x10) {  // extension
      if (n < hdr + 4) continue;
      int extLen = (buf[hdr + 2] << 8) | buf[hdr + 3];
      hdr += 4 + extLen * 4;
    }
    if (n <= hdr) continue;
    int payloadLen = n - hdr;
    if (buf[0] & 0x20) {  // padding
      int pad = buf[n - 1];
      if (pad < payloadLen) payloadLen -= pad;
    }
    _lastRxMs = millis();
    _rxPackets++;
    _rxWindow++;
    uint16_t seq = (buf[2] << 8) | buf[3];
    if (_haveSeq && (uint16_t)(seq - _lastSeq) != 1) _seqGaps++;
    _lastSeq = seq;
    _haveSeq = true;
    // Symmetric RTP: latch onto the first source we hear from (NAT traversal).
    if (!_latched) {
      _remoteIp = _udp.remoteIP();
      _remotePort = _udp.remotePort();
      _latched = true;
    }
    if (pt != 0 && pt != 8) continue;  // telephone-event / CN / unknown -> ignore
    const uint8_t *p = buf + hdr;
    while (payloadLen >= FRAME_8K) {  // handle 20/40/60 ms packets
      jbPush(p, pt);
      p += FRAME_8K;
      payloadLen -= FRAME_8K;
    }
    if (payloadLen > 0 && payloadLen < FRAME_8K) {  // short frame -> pad with last byte
      uint8_t tmp[FRAME_8K];
      memcpy(tmp, p, payloadLen);
      memset(tmp + payloadLen, p[payloadLen - 1], FRAME_8K - payloadLen);
      jbPush(tmp, pt);
    }
  }
}

void RtpSession::jbPush(const uint8_t *data, uint8_t pt) {
  if (_jbCount >= JB_MAX) {  // too much buffered: drop oldest to bound latency
    _jbHead = (_jbHead + 1) % JB_SLOTS;
    _jbCount--;
    _drops++;
  }
  memcpy(_jb[_jbTail].data, data, FRAME_8K);
  _jb[_jbTail].pt = pt;
  _jbTail = (_jbTail + 1) % JB_SLOTS;
  _jbCount++;
}

bool RtpSession::jbPop(uint8_t *data, uint8_t &pt) {
  if (_jbCount == 0) return false;
  memcpy(data, _jb[_jbHead].data, FRAME_8K);
  pt = _jb[_jbHead].pt;
  _jbHead = (_jbHead + 1) % JB_SLOTS;
  _jbCount--;
  return true;
}

bool RtpSession::popFrame(int16_t *pcm) {
  if (!_playing) {
    if (_jbCount >= JB_PREFILL) _playing = true;
    else return false;
  }
  uint8_t raw[FRAME_8K];
  uint8_t pt;
  if (jbPop(raw, pt)) {
    g711_decode(raw, pcm, FRAME_8K, pt == 8);
    memcpy(_lastFrame, raw, FRAME_8K);
    _lastPt = pt;
    _haveLast = true;
    _plcCount = 0;
    return true;
  }
  // underrun: simple packet loss concealment (repeat last frame, fading out)
  _playing = false;
  _underruns++;
  if (_haveLast && _plcCount < 3) {
    g711_decode(_lastFrame, pcm, FRAME_8K, _lastPt == 8);
    int gain = 3 - _plcCount;  // 3/4, 2/4, 1/4
    for (int i = 0; i < FRAME_8K; i++) pcm[i] = (int16_t)((pcm[i] * gain) >> 2);
    _plcCount++;
    return true;
  }
  return false;
}
