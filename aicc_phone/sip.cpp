// Minimal SIP user agent (UDP) for FreeSWITCH / aicc.
#include "sip.h"
#include <WiFi.h>
#include <MD5Builder.h>
#include <esp_random.h>

#define SIP_T1 500
#define SIP_UA "AICC-ESP32-Phone/1.0"
#define SIP_ALLOW "INVITE, ACK, CANCEL, BYE, OPTIONS, INFO, NOTIFY, UPDATE"
#define REG_EXPIRES 300

// ===========================================================================
// SipMsg
// ===========================================================================
static bool hdrEq(const String &a, const char *b) { return a.equalsIgnoreCase(b); }

String SipMsg::get(const char *name) const {
  for (int i = 0; i < nh; i++) if (hdrEq(hname[i], name)) return hval[i];
  return String();
}

int SipMsg::cseqNum() const {
  String c = get("CSeq");
  return c.toInt();
}

String SipMsg::cseqMethod() const {
  String c = get("CSeq");
  int sp = c.indexOf(' ');
  if (sp < 0) return String();
  String m = c.substring(sp + 1);
  m.trim();
  return m;
}

static String paramOf(const String &h, const char *param) {
  // find ";param=" (case-insensitive) outside <> would be ideal; good enough here
  String lower = h;
  lower.toLowerCase();
  String key = String(";") + param + "=";
  int p = lower.indexOf(key);
  if (p < 0) return String();
  int s = p + key.length();
  int e = s;
  while (e < (int)h.length() && h[e] != ';' && h[e] != ',' && h[e] != '>' && h[e] != ' ' && h[e] != '\r') e++;
  String v = h.substring(s, e);
  v.trim();
  if (v.startsWith("\"") && v.endsWith("\"") && v.length() >= 2) v = v.substring(1, v.length() - 1);
  return v;
}

String SipMsg::fromTag() const { return paramOf(get("From"), "tag"); }
String SipMsg::toTag() const { return paramOf(get("To"), "tag"); }

static String expandCompact(const String &n) {
  if (n.length() != 1) return n;
  switch (tolower(n[0])) {
    case 'v': return "Via";
    case 'f': return "From";
    case 't': return "To";
    case 'i': return "Call-ID";
    case 'm': return "Contact";
    case 'l': return "Content-Length";
    case 'c': return "Content-Type";
    case 'k': return "Supported";
    case 'e': return "Content-Encoding";
    case 's': return "Subject";
    default: return n;
  }
}

bool SipClient::parseMsg(const char *buf, int len, SipMsg &m) {
  String s(buf, (unsigned int)len);
  int hdrEnd = s.indexOf("\r\n\r\n");
  String head = hdrEnd >= 0 ? s.substring(0, hdrEnd) : s;
  m.body = hdrEnd >= 0 ? s.substring(hdrEnd + 4) : String();
  int lineEnd = head.indexOf("\r\n");
  String first = lineEnd >= 0 ? head.substring(0, lineEnd) : head;
  first.trim();
  if (first.length() == 0) return false;
  if (first.startsWith("SIP/2.0 ")) {
    m.isRequest = false;
    m.status = first.substring(8, 11).toInt();
    m.reason = first.substring(12);
    m.reason.trim();
  } else {
    m.isRequest = true;
    int sp1 = first.indexOf(' ');
    int sp2 = first.indexOf(' ', sp1 + 1);
    if (sp1 < 0 || sp2 < 0) return false;
    m.method = first.substring(0, sp1);
    m.uri = first.substring(sp1 + 1, sp2);
  }
  m.nh = 0;
  int pos = lineEnd >= 0 ? lineEnd + 2 : head.length();
  while (pos < (int)head.length() && m.nh < SipMsg::MAXH) {
    int e = head.indexOf("\r\n", pos);
    if (e < 0) e = head.length();
    String line = head.substring(pos, e);
    pos = e + 2;
    // header folding (rare): continuation lines start with whitespace
    while (pos < (int)head.length() && (head[pos] == ' ' || head[pos] == '\t')) {
      int e2 = head.indexOf("\r\n", pos);
      if (e2 < 0) e2 = head.length();
      line += " " + head.substring(pos, e2);
      pos = e2 + 2;
    }
    int c = line.indexOf(':');
    if (c <= 0) continue;
    String n = line.substring(0, c);
    n.trim();
    String v = line.substring(c + 1);
    v.trim();
    m.hname[m.nh] = expandCompact(n);
    m.hval[m.nh] = v;
    m.nh++;
  }
  int cl = m.get("Content-Length").toInt();
  if (m.get("Content-Length").length() && cl < (int)m.body.length()) m.body = m.body.substring(0, cl);
  return true;
}

// ===========================================================================
// Small helpers
// ===========================================================================
static String hex32(uint32_t v) {
  char b[9];
  snprintf(b, sizeof(b), "%08lx", (unsigned long)v);
  return String(b);
}

static String md5(const String &s) {
  MD5Builder m;
  m.begin();
  m.add(s);
  m.calculate();
  return m.toString();
}

static String extractUri(const String &hdr) {
  int a = hdr.indexOf('<');
  int b = hdr.indexOf('>');
  if (a >= 0 && b > a) return hdr.substring(a + 1, b);
  int sc = hdr.indexOf(';');
  String u = sc >= 0 ? hdr.substring(0, sc) : hdr;
  u.trim();
  return u;
}

String SipClient::uriHost(const String &uriIn, uint16_t &port) {
  String uri = uriIn;
  int c = uri.indexOf(':');
  if (c >= 0 && (uri.startsWith("sip:") || uri.startsWith("sips:"))) uri = uri.substring(c + 1);
  int at = uri.indexOf('@');
  if (at >= 0) uri = uri.substring(at + 1);
  int end = uri.length();
  for (int i = 0; i < (int)uri.length(); i++) {
    if (uri[i] == ';' || uri[i] == '?' || uri[i] == '>') { end = i; break; }
  }
  uri = uri.substring(0, end);
  port = 5060;
  int pc = uri.indexOf(':');
  if (pc >= 0) {
    port = uri.substring(pc + 1).toInt();
    uri = uri.substring(0, pc);
  }
  return uri;
}

String SipClient::displayNameOf(const String &hdr) {
  int a = hdr.indexOf('<');
  if (a <= 0) return String();
  String d = hdr.substring(0, a);
  d.trim();
  if (d.startsWith("\"")) d = d.substring(1);
  if (d.endsWith("\"")) d = d.substring(0, d.length() - 1);
  return d;
}

String SipClient::userOf(const String &hdr) {
  String u = extractUri(hdr);
  int c = u.indexOf(':');
  if (c >= 0) u = u.substring(c + 1);
  int at = u.indexOf('@');
  if (at >= 0) u = u.substring(0, at);
  int sc = u.indexOf(';');
  if (sc >= 0) u = u.substring(0, sc);
  return u;
}

bool SipClient::parseSdp(const String &body, SipMedia &out) {
  out.port = 0;
  out.dtmfPt = -1;
  String ip;
  int pos = 0;
  bool haveAudio = false;
  int chosen = -1;
  while (pos < (int)body.length()) {
    int e = body.indexOf('\n', pos);
    if (e < 0) e = body.length();
    String line = body.substring(pos, e);
    line.trim();
    pos = e + 1;
    if (line.startsWith("c=IN IP4 ")) {
      if (ip.length() == 0 || haveAudio) ip = line.substring(9);
    } else if (line.startsWith("m=audio ")) {
      haveAudio = true;
      String rest = line.substring(8);
      int sp = rest.indexOf(' ');
      out.port = rest.substring(0, sp).toInt();
      // payload list after "RTP/AVP"
      int p = rest.indexOf("RTP/AVP");
      if (p >= 0) {
        String pts = rest.substring(p + 7);
        pts.trim();
        int i = 0;
        while (i < (int)pts.length()) {
          int sp2 = pts.indexOf(' ', i);
          if (sp2 < 0) sp2 = pts.length();
          int pt = pts.substring(i, sp2).toInt();
          if (chosen < 0 && (pt == 0 || pt == 8)) chosen = pt;
          i = sp2 + 1;
        }
      }
    } else if (line.startsWith("a=rtpmap:")) {
      String l = line;
      l.toLowerCase();
      if (l.indexOf("telephone-event") > 0) out.dtmfPt = line.substring(9).toInt();
    } else if (line.startsWith("m=") && haveAudio) {
      break;  // other media after audio: stop
    }
  }
  ip.trim();
  if (!haveAudio || out.port == 0 || chosen < 0 || ip.length() == 0) return false;
  if (!out.ip.fromString(ip)) return false;
  out.pt = (uint8_t)chosen;
  return true;
}

// ===========================================================================
// Setup
// ===========================================================================
bool SipClient::begin(const SipConfig &cfg) {
  end();
  _cfg = cfg;
  _localIp = WiFi.localIP().toString();
  _serverResolved = false;
  _pubKnown = false;
  _call = SIP_CALL_IDLE;
  _dlg = Dialog();
  _reg = cfg.doRegister ? SIP_REG_TRYING : SIP_REG_DISABLED;
  _regCallId = newCallId();
  _regCseq = 1;
  _regAuthTried = false;
  _lastStatus = "";
  if (!_udp.begin(_cfg.localPort)) {
    Serial.println("[SIP] udp bind failed");
    return false;
  }
  _running = true;
  resolveServer();
  uint32_t now = millis();
  _nextOptionsAt = now + 200;          // discover public address right away
  _nextRegisterAt = now + 1500;
  _nextKeepaliveAt = now + 20000;
  Serial.printf("[SIP] started on %s:%u -> %s:%u register=%d\n", _localIp.c_str(), _cfg.localPort,
                _cfg.server.c_str(), _cfg.port, _cfg.doRegister);
  return true;
}

void SipClient::end() {
  if (_running) {
    _udp.stop();
    _running = false;
  }
  _txInvite = Txn(); _txMisc = Txn(); _txReg = Txn(); _txOpt = Txn(); _tx200 = Txn();
}

bool SipClient::resolveServer() {
  if (_serverResolved) return true;
  if (_serverIp.fromString(_cfg.server)) { _serverResolved = true; return true; }
  if (WiFi.hostByName(_cfg.server.c_str(), _serverIp) == 1) { _serverResolved = true; return true; }
  return false;
}

String SipClient::contactHost() const { return _pubKnown ? _pubIp.toString() : _localIp; }
uint16_t SipClient::contactPort() const { return _pubKnown ? _pubPort : _cfg.localPort; }
String SipClient::newBranch() const { return "z9hG4bK" + hex32(esp_random()) + hex32(esp_random()); }
String SipClient::newTag() const { return hex32(esp_random()); }
String SipClient::newCallId() const { return hex32(esp_random()) + hex32(esp_random()) + "@" + _localIp; }
String SipClient::publicAddress() const { return _pubKnown ? _pubIp.toString() + ":" + String(_pubPort) : String("?"); }

String SipClient::viaHeader(const String &branch) const {
  return "Via: SIP/2.0/UDP " + _localIp + ":" + String(_cfg.localPort) + ";rport;branch=" + branch + "\r\n";
}

String SipClient::contactHeader() const {
  return "Contact: <sip:" + _cfg.user + "@" + contactHost() + ":" + String(contactPort()) + ">\r\n";
}

String SipClient::buildSdp() const {
  String s;
  s.reserve(320);
  s += "v=0\r\n";
  s += "o=- " + String((unsigned long)(esp_random() & 0x7fffffff)) + " 1 IN IP4 " + _localIp + "\r\n";
  s += "s=AICC Phone\r\n";
  s += "c=IN IP4 " + _localIp + "\r\n";
  s += "t=0 0\r\n";
  s += "m=audio " + String(_cfg.rtpPort) + " RTP/AVP 0 8 101\r\n";
  s += "a=rtpmap:0 PCMU/8000\r\n";
  s += "a=rtpmap:8 PCMA/8000\r\n";
  s += "a=rtpmap:101 telephone-event/8000\r\n";
  s += "a=fmtp:101 0-16\r\n";
  s += "a=ptime:20\r\n";
  s += "a=sendrecv\r\n";
  return s;
}

String SipClient::buildRequest(const String &method, const String &reqUri, const String &from, const String &to,
                               const String &callId, uint32_t cseq, const String &branch, const String &extra,
                               const String &body) const {
  String r;
  r.reserve(900 + body.length());
  r += method + " " + reqUri + " SIP/2.0\r\n";
  r += viaHeader(branch);
  r += "Max-Forwards: 70\r\n";
  r += "From: " + from + "\r\n";
  r += "To: " + to + "\r\n";
  r += "Call-ID: " + callId + "\r\n";
  r += "CSeq: " + String(cseq) + " " + method + "\r\n";
  if (method != "ACK" && method != "CANCEL" && method != "BYE") r += contactHeader();
  r += "User-Agent: " SIP_UA "\r\n";
  r += "Allow: " SIP_ALLOW "\r\n";
  r += extra;
  if (body.length()) r += "Content-Type: application/sdp\r\n";
  r += "Content-Length: " + String(body.length()) + "\r\n\r\n";
  r += body;
  return r;
}

String SipClient::buildResponse(const SipMsg &req, int code, const char *reason, const String &extra,
                                const String &body, const String &toTag) const {
  String r;
  r.reserve(700 + body.length());
  r += "SIP/2.0 " + String(code) + " " + reason + "\r\n";
  for (int i = 0; i < req.nh; i++) {
    if (hdrEq(req.hname[i], "Via")) r += "Via: " + req.hval[i] + "\r\n";
  }
  for (int i = 0; i < req.nh; i++) {
    if (hdrEq(req.hname[i], "Record-Route")) r += "Record-Route: " + req.hval[i] + "\r\n";
  }
  r += "From: " + req.get("From") + "\r\n";
  String to = req.get("To");
  if (toTag.length() && req.toTag().length() == 0) to += ";tag=" + toTag;
  r += "To: " + to + "\r\n";
  r += "Call-ID: " + req.get("Call-ID") + "\r\n";
  r += "CSeq: " + req.get("CSeq") + "\r\n";
  if (code < 300 && (req.method == "INVITE" || req.method == "OPTIONS" || req.method == "UPDATE")) r += contactHeader();
  r += "User-Agent: " SIP_UA "\r\n";
  r += "Allow: " SIP_ALLOW "\r\n";
  r += extra;
  if (body.length()) r += "Content-Type: application/sdp\r\n";
  r += "Content-Length: " + String(body.length()) + "\r\n\r\n";
  r += body;
  return r;
}

void SipClient::sendRaw(const String &msg, IPAddress ip, uint16_t port) {
  if (!_running) return;
  if (_udp.beginPacket(ip, port)) {
    _udp.write((const uint8_t *)msg.c_str(), msg.length());
    _udp.endPacket();
  }
  int e = msg.indexOf("\r\n");
  Serial.printf("[SIP] >> %s (%d bytes)\n", msg.substring(0, e > 0 ? e : 40).c_str(), msg.length());
}

void SipClient::startTxn(Txn &t, const String &method, const String &msg, IPAddress ip, uint16_t port,
                         const String &callId, uint32_t cseq, bool invite) {
  t.active = true;
  t.method = method;
  t.msg = msg;
  t.ip = ip;
  t.port = port;
  t.callId = callId;
  t.cseq = cseq;
  t.sentAt = t.startedAt = millis();
  t.interval = SIP_T1;
  t.maxLife = invite ? 32000 : 16000;
  sendRaw(msg, ip, port);
}

void SipClient::serviceTxn(Txn &t, uint32_t now) {
  if (!t.active) return;
  if (now - t.startedAt > t.maxLife) {
    t.active = false;
    Serial.printf("[SIP] %s transaction timeout\n", t.method.c_str());
    if (&t == &_txInvite) {
      endCall("Timeout");
    } else if (&t == &_txReg) {
      setRegState(SIP_REG_FAILED);
      _nextRegisterAt = now + 30000;
    } else if (&t == &_txMisc) {
      if (_call == SIP_CALL_ENDING) endCall("Ended");
    } else if (&t == &_tx200) {
      // no ACK ever arrived: tear the call down
      if (_call == SIP_CALL_CONNECTED) { sendBye(); }
    }
    return;
  }
  if (now - t.sentAt >= t.interval) {
    t.sentAt = now;
    if (t.interval < 4000) t.interval *= 2;
    sendRaw(t.msg, t.ip, t.port);
  }
}

void SipClient::setCallState(SipCallState s) {
  if (_call == s) return;
  _call = s;
  Serial.printf("[SIP] call state -> %d\n", (int)s);
  if (onCallState) onCallState(s);
}

void SipClient::setRegState(SipRegState s) {
  if (_reg == s) return;
  _reg = s;
  if (onRegState) onRegState(s);
}

void SipClient::endCall(const char *status) {
  if (status) _lastStatus = status;
  bool hadMedia = _dlg.mediaStarted;
  _dlg = Dialog();
  _txInvite.active = false;
  _txMisc.active = false;
  _tx200.active = false;
  if (hadMedia && onMediaStop) onMediaStop();
  setCallState(SIP_CALL_IDLE);
}

// ===========================================================================
// Authentication
// ===========================================================================
struct Challenge {
  String realm, nonce, opaque, qop, algorithm;
};

static String chParam(const String &h, const char *key) {
  String lower = h;
  lower.toLowerCase();
  String k = String(key) + "=";
  int p = -1, from = 0;
  // ensure we match the full token (preceded by space, comma or start)
  while (true) {
    p = lower.indexOf(k, from);
    if (p < 0) return String();
    if (p == 0 || lower[p - 1] == ' ' || lower[p - 1] == ',') break;
    from = p + 1;
  }
  int s = p + k.length();
  if (s < (int)h.length() && h[s] == '"') {
    int e = h.indexOf('"', s + 1);
    return e > s ? h.substring(s + 1, e) : String();
  }
  int e = s;
  while (e < (int)h.length() && h[e] != ',' && h[e] != ' ') e++;
  return h.substring(s, e);
}

String SipClient::authHeaderFor(const SipMsg &resp, const String &method, const String &uri, bool &isProxy) {
  String h = resp.get("WWW-Authenticate");
  isProxy = false;
  if (h.length() == 0) { h = resp.get("Proxy-Authenticate"); isProxy = true; }
  if (h.length() == 0) return String();
  Challenge c;
  c.realm = chParam(h, "realm");
  c.nonce = chParam(h, "nonce");
  c.opaque = chParam(h, "opaque");
  c.qop = chParam(h, "qop");
  c.algorithm = chParam(h, "algorithm");
  if (c.nonce.length() == 0) return String();
  bool useQop = c.qop.indexOf("auth") >= 0;
  String ha1 = md5(_cfg.user + ":" + c.realm + ":" + _cfg.password);
  String ha2 = md5(method + ":" + uri);
  String nc = "00000001";
  String cnonce = hex32(esp_random());
  String response = useQop ? md5(ha1 + ":" + c.nonce + ":" + nc + ":" + cnonce + ":auth:" + ha2)
                           : md5(ha1 + ":" + c.nonce + ":" + ha2);
  String a = String(isProxy ? "Proxy-Authorization" : "Authorization") + ": Digest username=\"" + _cfg.user +
             "\", realm=\"" + c.realm + "\", nonce=\"" + c.nonce + "\", uri=\"" + uri + "\", response=\"" +
             response + "\", algorithm=MD5";
  if (useQop) a += ", qop=auth, nc=" + nc + ", cnonce=\"" + cnonce + "\"";
  if (c.opaque.length()) a += ", opaque=\"" + c.opaque + "\"";
  a += "\r\n";
  return a;
}

// ===========================================================================
// Outgoing call
// ===========================================================================
bool SipClient::call(const String &number) {
  if (!_running || _call != SIP_CALL_IDLE) return false;
  if (!resolveServer()) { _lastStatus = "DNS failed"; return false; }
  _dlg = Dialog();
  _dlg.active = true;
  _dlg.outgoing = true;
  _dlg.callId = newCallId();
  _dlg.localTag = newTag();
  _dlg.localCseq = 1;
  _dlg.requestUri = "sip:" + number + "@" + _cfg.server + ":" + String(_cfg.port);
  _dlg.fromHdr = "\"" + _cfg.displayName + "\" <sip:" + _cfg.user + "@" + _cfg.server + ">;tag=" + _dlg.localTag;
  _dlg.toHdr = "<" + _dlg.requestUri + ">";
  _dlg.targetIp = _serverIp;
  _dlg.targetPort = _cfg.port;
  _dlg.startedAt = millis();
  _remoteParty = number;
  _lastStatus = "";
  sendInvite("");
  setCallState(SIP_CALL_INVITING);
  return true;
}

void SipClient::sendInvite(const String &authHeader) {
  _dlg.inviteBranch = newBranch();
  _dlg.inviteCseq = _dlg.localCseq++;
  String body = buildSdp();
  String msg = buildRequest("INVITE", _dlg.requestUri, _dlg.fromHdr, _dlg.toHdr, _dlg.callId, _dlg.inviteCseq,
                            _dlg.inviteBranch, authHeader + "Supported: replaces\r\n", body);
  _dlg.provisional = false;
  startTxn(_txInvite, "INVITE", msg, _dlg.targetIp, _dlg.targetPort, _dlg.callId, _dlg.inviteCseq, true);
}

void SipClient::sendAck(const SipMsg &resp, bool to2xx) {
  String to = resp.get("To");
  String branch = to2xx ? newBranch() : _dlg.inviteBranch;
  String reqUri = _dlg.requestUri;
  IPAddress ip = _dlg.targetIp;
  uint16_t port = _dlg.targetPort;
  if (to2xx) {
    String contact = resp.get("Contact");
    if (contact.length()) {
      String curi = extractUri(contact);
      uint16_t p;
      String h = uriHost(curi, p);
      IPAddress cip;
      if (cip.fromString(h)) { ip = cip; port = p; reqUri = curi; }
    }
  }
  String msg = buildRequest("ACK", reqUri, _dlg.fromHdr, to, _dlg.callId, _dlg.inviteCseq, branch, "", "");
  sendRaw(msg, ip, port);
}

void SipClient::sendCancel() {
  String msg = buildRequest("CANCEL", _dlg.requestUri, _dlg.fromHdr, _dlg.toHdr, _dlg.callId, _dlg.inviteCseq,
                            _dlg.inviteBranch, "", "");
  startTxn(_txMisc, "CANCEL", msg, _dlg.targetIp, _dlg.targetPort, _dlg.callId, _dlg.inviteCseq, false);
}

void SipClient::sendBye() {
  String to = _dlg.toHdr;
  if (_dlg.remoteTag.length() && to.indexOf("tag=") < 0) to += ";tag=" + _dlg.remoteTag;
  uint32_t cseq = _dlg.localCseq++;
  String msg = buildRequest("BYE", _dlg.requestUri, _dlg.fromHdr, to, _dlg.callId, cseq, newBranch(), "", "");
  _txInvite.active = false;
  _tx200.active = false;
  startTxn(_txMisc, "BYE", msg, _dlg.targetIp, _dlg.targetPort, _dlg.callId, cseq, false);
  if (_dlg.mediaStarted && onMediaStop) onMediaStop();
  _dlg.mediaStarted = false;
  _endingSince = millis();
  setCallState(SIP_CALL_ENDING);
}

void SipClient::hangup() {
  switch (_call) {
    case SIP_CALL_INVITING:
    case SIP_CALL_RINGING:
      _lastStatus = "Cancelled";
      sendCancel();
      _endingSince = millis();
      if (_dlg.mediaStarted && onMediaStop) onMediaStop();
      _dlg.mediaStarted = false;
      setCallState(SIP_CALL_ENDING);
      break;
    case SIP_CALL_INCOMING:
      reject();
      break;
    case SIP_CALL_CONNECTED:
      _lastStatus = "Ended";
      sendBye();
      break;
    default: break;
  }
}

void SipClient::answer() {
  if (_call != SIP_CALL_INCOMING) return;
  String body = buildSdp();
  String msg = buildResponse(_dlg.inviteReq, 200, "OK", "", body, _dlg.localTag);
  startTxn(_tx200, "200", msg, _dlg.targetIp, _dlg.targetPort, _dlg.callId, _dlg.inviteCseq, false);
  _tx200.maxLife = 10000;
  if (!_dlg.mediaStarted && onMediaStart) { _dlg.mediaStarted = true; onMediaStart(_dlg.remoteMedia); }
  _lastStatus = "";
  setCallState(SIP_CALL_CONNECTED);
}

void SipClient::reject() {
  if (_call != SIP_CALL_INCOMING) return;
  String msg = buildResponse(_dlg.inviteReq, 486, "Busy Here", "", "", _dlg.localTag);
  sendRaw(msg, _dlg.targetIp, _dlg.targetPort);
  endCall("Rejected");
}

// ===========================================================================
// Registration / OPTIONS probe
// ===========================================================================
void SipClient::sendRegister(const String &authHeader) {
  if (!resolveServer()) return;
  String aor = "sip:" + _cfg.user + "@" + _cfg.server;
  String from = "\"" + _cfg.displayName + "\" <" + aor + ">;tag=" + newTag();
  String to = "<" + aor + ">";
  uint32_t cseq = _regCseq++;
  String msg = buildRequest("REGISTER", "sip:" + _cfg.server, from, to, _regCallId, cseq, newBranch(),
                            authHeader + "Expires: " + String(REG_EXPIRES) + "\r\n", "");
  startTxn(_txReg, "REGISTER", msg, _serverIp, _cfg.port, _regCallId, cseq, false);
  setRegState(SIP_REG_TRYING);
}

void SipClient::sendOptionsProbe() {
  if (!resolveServer()) return;
  _optCallId = newCallId();
  String from = "<sip:" + _cfg.user + "@" + _cfg.server + ">;tag=" + newTag();
  String to = "<sip:" + _cfg.server + ">";
  String msg = buildRequest("OPTIONS", "sip:" + _cfg.server + ":" + String(_cfg.port), from, to, _optCallId, 1,
                            newBranch(), "Accept: application/sdp\r\n", "");
  startTxn(_txOpt, "OPTIONS", msg, _serverIp, _cfg.port, _optCallId, 1, false);
  _txOpt.maxLife = 6000;
}

void SipClient::learnPublicAddress(const SipMsg &resp) {
  String via = resp.get("Via");
  String recv = paramOf(via, "received");
  String rport = paramOf(via, "rport");
  if (recv.length() && rport.length()) {
    IPAddress ip;
    if (ip.fromString(recv)) {
      bool changed = !_pubKnown || ip != _pubIp || _pubPort != (uint16_t)rport.toInt();
      _pubIp = ip;
      _pubPort = rport.toInt();
      _pubKnown = true;
      if (changed) Serial.printf("[SIP] public address %s:%u\n", recv.c_str(), _pubPort);
    }
  }
}

// ===========================================================================
// Main loop
// ===========================================================================
void SipClient::loop() {
  if (!_running) return;
  uint32_t now = millis();

  for (int guard = 0; guard < 8; guard++) {
    int len = _udp.parsePacket();
    if (len <= 0) break;
    if (len > (int)sizeof(_rx) - 1) len = sizeof(_rx) - 1;
    int n = _udp.read(_rx, len);
    if (n <= 0) continue;
    _rx[n] = 0;
    // drain anything larger than our buffer
    while (_udp.available()) _udp.read();
    if (n < 10) continue;  // keepalive / junk
    handlePacket(_rx, n, _udp.remoteIP(), _udp.remotePort());
  }

  serviceTxn(_txInvite, now);
  serviceTxn(_txMisc, now);
  serviceTxn(_txReg, now);
  serviceTxn(_txOpt, now);
  serviceTxn(_tx200, now);

  if (_call == SIP_CALL_ENDING && now - _endingSince > 6000) endCall(NULL);
  if ((_call == SIP_CALL_INVITING || _call == SIP_CALL_RINGING) && now - _dlg.startedAt > 90000) hangup();
  if (_call == SIP_CALL_INCOMING && now - _dlg.startedAt > 60000) {
    String msg = buildResponse(_dlg.inviteReq, 480, "Temporarily Unavailable", "", "", _dlg.localTag);
    sendRaw(msg, _dlg.targetIp, _dlg.targetPort);
    endCall("Missed");
  }

  if (_cfg.doRegister && !_txReg.active && (int32_t)(now - _nextRegisterAt) >= 0) {
    _regAuthTried = false;
    sendRegister("");
    _nextRegisterAt = now + 60000;  // will be rescheduled by the 200 OK
  }
  if (!_txOpt.active && (int32_t)(now - _nextOptionsAt) >= 0) {
    sendOptionsProbe();
    _nextOptionsAt = now + 60000;
  }
  if ((int32_t)(now - _nextKeepaliveAt) >= 0) {
    _nextKeepaliveAt = now + 20000;
    if (_serverResolved && _udp.beginPacket(_serverIp, _cfg.port)) {
      _udp.write((const uint8_t *)"\r\n\r\n", 4);
      _udp.endPacket();
    }
  }
}

void SipClient::handlePacket(const char *data, int len, IPAddress from, uint16_t fromPort) {
  SipMsg m;
  if (!parseMsg(data, len, m)) return;
  if (m.isRequest) {
    Serial.printf("[SIP] << %s from %s:%u\n", m.method.c_str(), from.toString().c_str(), fromPort);
    handleRequest(m, from, fromPort);
  } else {
    Serial.printf("[SIP] << %d %s (%s)\n", m.status, m.reason.c_str(), m.cseqMethod().c_str());
    handleResponse(m, from, fromPort);
  }
}

// ---------------------------------------------------------------------------
void SipClient::handleResponse(SipMsg &m, IPAddress from, uint16_t fromPort) {
  String cid = m.callId();
  String cm = m.cseqMethod();
  learnPublicAddress(m);

  if (cid == _optCallId && cm == "OPTIONS") {
    _txOpt.active = false;
    return;
  }

  if (cid == _regCallId && cm == "REGISTER") {
    if (m.status >= 200) _txReg.active = false;
    if (m.status == 200) {
      int exp = m.get("Expires").toInt();
      if (exp <= 0) {
        String c = m.get("Contact");
        String e = paramOf(c, "expires");
        exp = e.length() ? e.toInt() : REG_EXPIRES;
      }
      if (exp < 60) exp = 60;
      _nextRegisterAt = millis() + (uint32_t)exp * 1000 / 2;
      setRegState(SIP_REG_OK);
    } else if ((m.status == 401 || m.status == 407) && !_regAuthTried) {
      _regAuthTried = true;
      bool proxy;
      String a = authHeaderFor(m, "REGISTER", "sip:" + _cfg.server, proxy);
      if (a.length()) sendRegister(a);
      else { setRegState(SIP_REG_FAILED); _nextRegisterAt = millis() + 60000; }
    } else if (m.status >= 300) {
      setRegState(SIP_REG_FAILED);
      _lastStatus = String(m.status) + " " + m.reason;
      _nextRegisterAt = millis() + 60000;
    }
    return;
  }

  if (!_dlg.active || cid != _dlg.callId) return;

  if (cm == "INVITE") { handleInviteResponse(m); return; }

  if (cm == "CANCEL") {
    if (m.status >= 200) _txMisc.active = false;
    // wait for the 487 to the INVITE; endCall happens there or on timeout
    return;
  }
  if (cm == "BYE") {
    if (m.status >= 200) { _txMisc.active = false; endCall(NULL); }
    return;
  }
}

void SipClient::handleInviteResponse(SipMsg &m) {
  if ((uint32_t)m.cseqNum() != _dlg.inviteCseq) return;
  int st = m.status;

  if (st < 200) {
    _dlg.provisional = true;
    _txInvite.active = false;  // provisional answer: stop retransmitting (no-answer guard lives in loop())
    if (st == 100) return;
    if (_call == SIP_CALL_INVITING) setCallState(SIP_CALL_RINGING);
    if (m.body.length() && !_dlg.mediaStarted) {
      SipMedia md;
      if (parseSdp(m.body, md)) {
        _dlg.remoteMedia = md;
        _dlg.mediaStarted = true;
        if (onMediaStart) onMediaStart(md);
      }
    }
    return;
  }

  if (st >= 200 && st < 300) {
    _txInvite.active = false;
    if (_call == SIP_CALL_ENDING) {
      // CANCEL lost the race: accept then terminate properly
      _dlg.remoteTag = m.toTag();
      sendAck(m, true);
      sendBye();
      return;
    }
    if (_call == SIP_CALL_IDLE) return;
    _dlg.remoteTag = m.toTag();
    String contact = m.get("Contact");
    if (contact.length()) {
      String curi = extractUri(contact);
      uint16_t p;
      String h = uriHost(curi, p);
      IPAddress cip;
      if (cip.fromString(h) && h != "0.0.0.0") {
        _dlg.requestUri = curi;
        _dlg.targetIp = cip;
        _dlg.targetPort = p;
      }
    }
    if (m.body.length()) {
      SipMedia md;
      if (parseSdp(m.body, md)) {
        bool changed = !_dlg.mediaStarted || md.ip != _dlg.remoteMedia.ip || md.port != _dlg.remoteMedia.port || md.pt != _dlg.remoteMedia.pt;
        _dlg.remoteMedia = md;
        if (changed && onMediaStart) onMediaStart(md);
        _dlg.mediaStarted = true;
      }
    }
    sendAck(m, true);
    _lastStatus = "";
    setCallState(SIP_CALL_CONNECTED);
    return;
  }

  // final failure
  _txInvite.active = false;
  sendAck(m, false);
  if ((st == 401 || st == 407) && !_dlg.authTried && _call != SIP_CALL_ENDING) {
    _dlg.authTried = true;
    bool proxy;
    String a = authHeaderFor(m, "INVITE", _dlg.requestUri, proxy);
    if (a.length()) { sendInvite(a); return; }
  }
  if (st == 487) { endCall("Cancelled"); return; }
  String why = String(st) + " " + m.reason;
  endCall(why.c_str());
}

// ---------------------------------------------------------------------------
void SipClient::handleRequest(SipMsg &m, IPAddress from, uint16_t fromPort) {
  const String &method = m.method;
  String cid = m.callId();

  if (method == "OPTIONS") {
    sendRaw(buildResponse(m, 200, "OK", "Accept: application/sdp\r\n", "", newTag()), from, fromPort);
    return;
  }

  if (method == "ACK") {
    if (_dlg.active && cid == _dlg.callId) {
      _tx200.active = false;
      _dlg.remoteCseq = m.cseqNum();
    }
    return;
  }

  if (method == "INVITE") {
    if (_dlg.active && cid == _dlg.callId) {
      // re-INVITE inside our dialog (hold / codec update / session refresh)
      _dlg.remoteCseq = m.cseqNum();
      _dlg.inviteReq = m;
      _dlg.inviteCseq = m.cseqNum();
      if (m.body.length()) {
        SipMedia md;
        if (parseSdp(m.body, md)) {
          bool changed = md.ip != _dlg.remoteMedia.ip || md.port != _dlg.remoteMedia.port || md.pt != _dlg.remoteMedia.pt;
          _dlg.remoteMedia = md;
          if (changed && _dlg.mediaStarted && onMediaStart) onMediaStart(md);
        }
      }
      String msg = buildResponse(m, 200, "OK", "", buildSdp(), _dlg.localTag);
      startTxn(_tx200, "200", msg, from, fromPort, cid, m.cseqNum(), false);
      _tx200.maxLife = 10000;
      return;
    }
    if (_call != SIP_CALL_IDLE) {
      sendRaw(buildResponse(m, 486, "Busy Here", "", "", newTag()), from, fromPort);
      return;
    }
    SipMedia md;
    if (!parseSdp(m.body, md)) {
      sendRaw(buildResponse(m, 488, "Not Acceptable Here", "", "", newTag()), from, fromPort);
      return;
    }
    _dlg = Dialog();
    _dlg.active = true;
    _dlg.outgoing = false;
    _dlg.callId = cid;
    _dlg.localTag = newTag();
    _dlg.remoteTag = m.fromTag();
    _dlg.remoteCseq = m.cseqNum();
    _dlg.inviteCseq = m.cseqNum();
    _dlg.inviteReq = m;
    _dlg.localCseq = 1;
    _dlg.remoteMedia = md;
    _dlg.startedAt = millis();
    // In-dialog requests from us go back to where the INVITE came from
    _dlg.targetIp = from;
    _dlg.targetPort = fromPort;
    String contact = m.get("Contact");
    _dlg.requestUri = contact.length() ? extractUri(contact) : extractUri(m.get("From"));
    // From/To for requests we originate in this dialog are swapped
    String toHdr = m.get("To");
    if (m.toTag().length() == 0) toHdr += ";tag=" + _dlg.localTag;
    _dlg.fromHdr = toHdr;
    String fromHdr = m.get("From");
    _dlg.toHdr = fromHdr;  // already carries the remote tag
    String disp = displayNameOf(fromHdr);
    String user = userOf(fromHdr);
    _remoteParty = disp.length() ? disp : user;
    if (disp.length() && user.length() && disp != user) _remoteParty = disp + " (" + user + ")";
    sendRaw(buildResponse(m, 100, "Trying", "", "", ""), from, fromPort);
    sendRaw(buildResponse(m, 180, "Ringing", "", "", _dlg.localTag), from, fromPort);
    _lastStatus = "";
    setCallState(SIP_CALL_INCOMING);
    return;
  }

  if (method == "CANCEL") {
    if (_dlg.active && cid == _dlg.callId && _call == SIP_CALL_INCOMING) {
      sendRaw(buildResponse(m, 200, "OK", "", "", _dlg.localTag), from, fromPort);
      sendRaw(buildResponse(_dlg.inviteReq, 487, "Request Terminated", "", "", _dlg.localTag), from, fromPort);
      endCall("Missed");
    } else {
      sendRaw(buildResponse(m, 481, "Call/Transaction Does Not Exist", "", "", newTag()), from, fromPort);
    }
    return;
  }

  if (method == "BYE") {
    if (_dlg.active && cid == _dlg.callId) {
      sendRaw(buildResponse(m, 200, "OK", "", "", _dlg.localTag), from, fromPort);
      endCall("Remote hung up");
    } else {
      sendRaw(buildResponse(m, 481, "Call/Transaction Does Not Exist", "", "", newTag()), from, fromPort);
    }
    return;
  }

  if (method == "UPDATE") {
    String body = m.body.length() ? buildSdp() : String();
    sendRaw(buildResponse(m, 200, "OK", "", body, _dlg.active ? _dlg.localTag : newTag()), from, fromPort);
    return;
  }

  if (method == "INFO" || method == "NOTIFY" || method == "MESSAGE" || method == "PRACK") {
    sendRaw(buildResponse(m, 200, "OK", "", "", _dlg.active ? _dlg.localTag : newTag()), from, fromPort);
    return;
  }

  sendRaw(buildResponse(m, 501, "Not Implemented", "", "", newTag()), from, fromPort);
}
