#pragma once
#include <Arduino.h>
#include <NetworkUdp.h>
#include <functional>

enum SipCallState {
  SIP_CALL_IDLE = 0,
  SIP_CALL_INVITING,   // INVITE sent, no provisional answer yet
  SIP_CALL_RINGING,    // 18x received
  SIP_CALL_INCOMING,   // remote INVITE, ringing locally
  SIP_CALL_CONNECTED,
  SIP_CALL_ENDING,     // BYE / CANCEL sent, waiting for final answer
};

enum SipRegState { SIP_REG_DISABLED = 0, SIP_REG_TRYING, SIP_REG_OK, SIP_REG_FAILED };

struct SipConfig {
  String server;        // IP or host name
  uint16_t port = 5060;
  String user;          // local user part / auth user
  String password;
  String displayName;
  bool doRegister = false;
  uint16_t localPort = 5060;
  uint16_t rtpPort = 20000;
};

struct SipMedia {
  IPAddress ip;
  uint16_t port = 0;
  uint8_t pt = 0;       // payload type to send (0 = PCMU, 8 = PCMA)
  int dtmfPt = -1;
};

struct SipMsg {
  bool isRequest = false;
  String method, uri;
  int status = 0;
  String reason;
  static const int MAXH = 48;
  String hname[MAXH];
  String hval[MAXH];
  int nh = 0;
  String body;

  String get(const char *name) const;
  String callId() const { return get("Call-ID"); }
  int cseqNum() const;
  String cseqMethod() const;
  String fromTag() const;
  String toTag() const;
};

class SipClient {
 public:
  bool begin(const SipConfig &cfg);
  void end();
  void loop();

  bool call(const String &number);  // dials sip:number@server:port
  void hangup();                    // CANCEL / BYE / reject depending on state
  void answer();
  void reject();

  // OPTIONS probe + UDP NAT keepalive. Turned off while the device sleeps: it is
  // not registered to FreeSWITCH, so there is nothing to keep alive. Enabling it
  // refreshes the public address immediately (wake, before a call is dialed).
  void setHeartbeat(bool on);
  bool heartbeat() const { return _hbOn; }

  bool isIdle() const { return _call == SIP_CALL_IDLE; }
  SipCallState callState() const { return _call; }
  SipRegState regState() const { return _reg; }
  const String &remoteParty() const { return _remoteParty; }
  const String &lastStatus() const { return _lastStatus; }

  std::function<void(SipCallState)> onCallState;
  std::function<void(SipRegState)> onRegState;
  std::function<void(const SipMedia &)> onMediaStart;
  std::function<void()> onMediaStop;

 private:
  struct Txn {
    bool active = false;
    String method;
    String msg;
    IPAddress ip;
    uint16_t port = 0;
    uint32_t sentAt = 0;
    uint32_t interval = 0;
    uint32_t startedAt = 0;
    uint32_t maxLife = 0;
    String callId;
    uint32_t cseq = 0;
  };

  struct Dialog {
    bool active = false;
    bool outgoing = false;
    String callId, localTag, remoteTag;
    String fromHdr, toHdr;       // as used in requests we send (without remote tag for To)
    String requestUri;           // target for in-dialog requests
    IPAddress targetIp;
    uint16_t targetPort = 0;
    uint32_t localCseq = 1;
    uint32_t remoteCseq = 0;
    String inviteBranch;
    uint32_t inviteCseq = 0;
    bool authTried = false;
    bool provisional = false;
    SipMedia remoteMedia;
    bool mediaStarted = false;
    SipMsg inviteReq;             // incoming INVITE (for 487 / 200 retransmit)
    uint32_t startedAt = 0;
  };

  SipConfig _cfg;
  NetworkUDP _udp;
  bool _running = false;
  String _localIp;
  IPAddress _serverIp;
  bool _serverResolved = false;
  IPAddress _pubIp;
  uint16_t _pubPort = 0;
  bool _pubKnown = false;

  SipCallState _call = SIP_CALL_IDLE;
  SipRegState _reg = SIP_REG_DISABLED;
  String _remoteParty, _lastStatus;
  Dialog _dlg;

  Txn _txInvite, _txMisc, _txReg, _txOpt, _tx200;
  uint32_t _endingSince = 0;

  String _regCallId;
  uint32_t _regCseq = 1;
  bool _regAuthTried = false;
  uint32_t _nextRegisterAt = 0;
  uint32_t _nextOptionsAt = 0;
  uint32_t _nextKeepaliveAt = 0;
  bool _hbOn = true;
  String _optCallId;

  char _rx[2048];

  // helpers
  bool resolveServer();
  String contactHost() const;
  uint16_t contactPort() const;
  String newBranch() const;
  String newTag() const;
  String newCallId() const;
  String viaHeader(const String &branch) const;
  String contactHeader() const;
  String buildSdp() const;
  String buildRequest(const String &method, const String &reqUri, const String &from, const String &to,
                      const String &callId, uint32_t cseq, const String &branch, const String &extra,
                      const String &body) const;
  String buildResponse(const SipMsg &req, int code, const char *reason, const String &extra,
                       const String &body, const String &toTag) const;
  void sendRaw(const String &msg, IPAddress ip, uint16_t port);
  void startTxn(Txn &t, const String &method, const String &msg, IPAddress ip, uint16_t port,
                const String &callId, uint32_t cseq, bool invite);
  void serviceTxn(Txn &t, uint32_t now);
  void handlePacket(const char *data, int len, IPAddress from, uint16_t fromPort);
  void handleRequest(SipMsg &m, IPAddress from, uint16_t fromPort);
  void handleResponse(SipMsg &m, IPAddress from, uint16_t fromPort);
  void handleInviteResponse(SipMsg &m);
  void sendAck(const SipMsg &resp, bool to2xx);
  void sendInvite(const String &authHeader);
  void sendRegister(const String &authHeader);
  void sendOptionsProbe();
  void sendBye();
  void sendCancel();
  void endCall(const char *status);
  void setCallState(SipCallState s);
  void setRegState(SipRegState s);
  void learnPublicAddress(const SipMsg &resp);
  String authHeaderFor(const SipMsg &resp, const String &method, const String &uri, bool &isProxy);
  static bool parseSdp(const String &body, SipMedia &out);
  static bool parseMsg(const char *buf, int len, SipMsg &m);
  static String uriHost(const String &uri, uint16_t &port);
  static String displayNameOf(const String &hdr);
  static String userOf(const String &hdr);
};
