#!/usr/bin/env python3
"""Probe SIP REGISTER against a server over UDP with digest auth.

usage: sip_register_probe.py <server> <port> <user> <password> [expires] [domain] [authuser]
Sends REGISTER, answers the 401/407 challenge, prints every response.
"""
import hashlib, random, socket, sys, time, re

def md5(s): return hashlib.md5(s.encode()).hexdigest()

def main():
    if len(sys.argv) < 5:
        print(__doc__); sys.exit(2)
    server, port, user, pw = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    expires = int(sys.argv[5]) if len(sys.argv) > 5 else 60
    domain = sys.argv[6] if len(sys.argv) > 6 else server
    authuser = sys.argv[7] if len(sys.argv) > 7 else user
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(4)
    s.bind(("0.0.0.0", 0))
    lport = s.getsockname()[1]
    # learn the local IP used to reach the server
    t = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); t.connect((server, port)); lip = t.getsockname()[0]; t.close()
    callid = "%08x@%s" % (random.getrandbits(32), lip)
    tag = "%08x" % random.getrandbits(32)
    cseq = 1

    def build(auth=None):
        branch = "z9hG4bK%08x" % random.getrandbits(32)
        m = (f"REGISTER sip:{domain} SIP/2.0\r\n"
             f"Via: SIP/2.0/UDP {lip}:{lport};branch={branch};rport\r\n"
             f"Max-Forwards: 70\r\n"
             f"From: <sip:{user}@{domain}>;tag={tag}\r\n"
             f"To: <sip:{user}@{domain}>\r\n"
             f"Call-ID: {callid}\r\n"
             f"CSeq: {cseq} REGISTER\r\n"
             f"Contact: <sip:{user}@{lip}:{lport}>\r\n"
             f"Expires: {expires}\r\n"
             f"User-Agent: aicc-probe/0.1\r\n")
        if auth: m += auth + "\r\n"
        m += "Content-Length: 0\r\n\r\n"
        return m

    def send_recv(msg):
        s.sendto(msg.encode(), (server, port))
        print(f">> {msg.splitlines()[0]}  ({len(msg)} B)")
        try:
            data, addr = s.recvfrom(4096)
        except socket.timeout:
            print("!! no response within 4 s"); return None
        txt = data.decode(errors="replace")
        print(f"<< {txt.splitlines()[0]}  from {addr[0]}:{addr[1]}")
        return txt

    resp = send_recv(build())
    if not resp: return 1
    code = int(resp.split()[1])
    if code in (401, 407):
        hdr = "WWW-Authenticate" if code == 401 else "Proxy-Authenticate"
        line = next((l for l in resp.splitlines() if l.lower().startswith(hdr.lower())), "")
        realm = re.search(r'realm="([^"]*)"', line).group(1)
        nonce = re.search(r'nonce="([^"]*)"', line).group(1)
        qop = re.search(r'qop="?([^",]*)', line)
        algo = re.search(r'algorithm=([A-Za-z0-9-]+)', line)
        print(f"   challenge: realm={realm} qop={qop.group(1) if qop else '-'} algo={algo.group(1) if algo else 'MD5'}")
        uri = f"sip:{domain}"
        ha1 = md5(f"{authuser}:{realm}:{pw}")
        ha2 = md5(f"REGISTER:{uri}")
        if qop:
            nc, cnonce = "00000001", "%08x" % random.getrandbits(32)
            resp_d = md5(f"{ha1}:{nonce}:{nc}:{cnonce}:auth:{ha2}")
            a = (f'Digest username="{authuser}", realm="{realm}", nonce="{nonce}", uri="{uri}", '
                 f'response="{resp_d}", algorithm=MD5, qop=auth, nc={nc}, cnonce="{cnonce}"')
        else:
            resp_d = md5(f"{ha1}:{nonce}:{ha2}")
            a = f'Digest username="{authuser}", realm="{realm}", nonce="{nonce}", uri="{uri}", response="{resp_d}", algorithm=MD5'
        cseq += 1
        auth_hdr = ("Authorization: " if code == 401 else "Proxy-Authorization: ") + a
        resp = send_recv(build(auth_hdr))
        if not resp: return 1
        code = int(resp.split()[1])
        for l in resp.splitlines():
            if l.lower().startswith(("contact", "expires", "via", "warning", "reason")): print("   " + l)
    if code == 200:
        print("REGISTER OK")
        # unregister so the test leaves no stale binding
        if expires:
            time.sleep(0.5)
        return 0
    print(f"REGISTER failed: {code}")
    return 1

if __name__ == "__main__":
    sys.exit(main())
