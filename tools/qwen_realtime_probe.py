#!/usr/bin/env python3
"""Minimal DashScope Realtime API probe (no third-party deps): raw WebSocket over TLS.
Verifies session config (smart_turn_v2, tools), a greeting, and streams 16 kHz WAVs as user turns.
Usage: rt_test.py <host> <model> <keyfile> <wav16k> [wav16k...]"""
import sys, ssl, socket, json, base64, os, struct, time, wave

host, model, keyfile = sys.argv[1], sys.argv[2], sys.argv[3]
wavs = sys.argv[4:]
key = open(keyfile).read().strip()

ctx = ssl.create_default_context()
s = ctx.wrap_socket(socket.create_connection((host, 443), timeout=20), server_hostname=host)
path = f"/api-ws/v1/realtime?model={model}"
wskey = base64.b64encode(os.urandom(16)).decode()
s.send((f"GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {wskey}\r\nSec-WebSocket-Version: 13\r\nAuthorization: Bearer {key}\r\n\r\n").encode())
buf = b""
while b"\r\n\r\n" not in buf:
    d = s.recv(4096)
    if not d: break
    buf += d
head, _, rest = buf.partition(b"\r\n\r\n")
print("HANDSHAKE:", head.split(b"\r\n")[0].decode())
if b" 101 " not in head.split(b"\r\n")[0]:
    print(head.decode(errors="replace")); print(rest.decode(errors="replace")); sys.exit(1)
s.settimeout(0.5)
rx = bytearray(rest)

def send(obj):
    data = json.dumps(obj, ensure_ascii=False).encode()
    mask = os.urandom(4)
    hdr = bytes([0x81])
    n = len(data)
    if n < 126: hdr += bytes([0x80 | n])
    elif n < 65536: hdr += bytes([0x80 | 126]) + struct.pack(">H", n)
    else: hdr += bytes([0x80 | 127]) + struct.pack(">Q", n)
    s.send(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

frag = b""
def recv_events(timeout):
    """Yield parsed JSON events for up to `timeout` seconds."""
    global rx, frag
    end = time.time() + timeout
    while time.time() < end:
        # parse frames available in rx
        while True:
            if len(rx) < 2: break
            b0, b1 = rx[0], rx[1]
            fin, op = b0 & 0x80, b0 & 0x0F
            ln = b1 & 0x7F; off = 2
            if ln == 126:
                if len(rx) < 4: break
                ln = struct.unpack(">H", rx[2:4])[0]; off = 4
            elif ln == 127:
                if len(rx) < 10: break
                ln = struct.unpack(">Q", rx[2:10])[0]; off = 10
            if len(rx) < off + ln: break
            payload = bytes(rx[off:off+ln]); del rx[:off+ln]
            if op == 0x8: print("CLOSE", payload[:2].hex(), payload[2:].decode(errors="replace")); return
            if op in (0x9, 0xA): continue
            if op in (0x1, 0x0):
                frag += payload
                if fin:
                    msg = frag; frag = b""
                    try: yield json.loads(msg.decode())
                    except Exception as e:
                        print("bad json", e, "len", len(msg)); print(msg.decode(errors="replace")[:1500])
        try:
            d = s.recv(65536)
            if not d: print("socket closed"); return
            rx += d
        except socket.timeout:
            continue

def wait_for(types, timeout, show=True):
    for ev in recv_events(timeout):
        t = ev.get("type")
        if show and t not in ("response.audio.delta", "response.audio_transcript.delta"):
            print("  <<", t, json.dumps({k: v for k, v in ev.items() if k not in ("type", "delta", "session", "response", "item")}, ensure_ascii=False)[:300])
        if t == "error": print("  ERROR:", json.dumps(ev.get("error"), ensure_ascii=False))
        if t in types: return ev
    return None

ev = wait_for(("session.created",), 10)
print("session.created:", bool(ev))
tools = [
 {"type":"function","name":"set_volume","description":"调节扬声器音量","parameters":{"type":"object","properties":{"level":{"type":"integer"},"delta":{"type":"integer"}}}},
 {"type":"function","name":"call_front_desk","description":"拨打酒店前台","parameters":{"type":"object","properties":{"reason":{"type":"string"}}}},
]
send({"type":"session.update","session":{
  "modalities":["text","audio"],
  "instructions":"你是酒店客房语音助手小安，回答简短。客人要调音量就调用 set_volume，要找前台就调用 call_front_desk。",
  "input_audio_format":"pcm16","output_audio_format":"pcm16",
  "turn_detection":{"type":"smart_turn_v2","silence_duration_ms":1000},
  "tools":tools,"tool_choice":"auto"}})
ev = wait_for(("session.updated",), 10)
if ev:
    sess = ev.get("session", {})
    print("session.updated OK. turn_detection=%s voice=%s out_fmt=%s audio=%s" % (sess.get("turn_detection"), sess.get("voice"), sess.get("output_audio_format"), json.dumps(sess.get("audio"), ensure_ascii=False)))
else:
    print("no session.updated"); sys.exit(1)

# greeting: the API refuses response.create on an empty conversation, so seed a user message
send({"type":"conversation.item.create","item":{"type":"message","role":"user","content":[{"type":"input_text","text":"（客人刚唤醒了设备）"}]}})
send({"type":"response.create","response":{"instructions":"请用一句话向客人问好并问需要什么帮助，不超过15个字。"}})
audio_bytes = 0; transcript = ""
t0 = time.time(); first_audio = None
for ev in recv_events(20):
    t = ev.get("type")
    if t == "response.audio.delta":
        n = len(base64.b64decode(ev["delta"])); audio_bytes += n
        if first_audio is None: first_audio = time.time() - t0
    elif t == "response.audio_transcript.delta": transcript += ev.get("delta", "")
    elif t == "response.audio_transcript.done": transcript = ev.get("transcript", transcript)
    elif t == "error": print("  ERROR:", json.dumps(ev.get("error"), ensure_ascii=False))
    elif t == "response.done":
        print("GREETING: %r  audio=%d bytes (%.2fs @24k / %.2fs @16k) first-audio=%.2fs status=%s" % (
            transcript, audio_bytes, audio_bytes/48000, audio_bytes/32000, first_audio or -1, ev.get("response", {}).get("status")))
        break

# stream each wav as user speech, watch for tool calls / replies
for w in wavs:
    pcm = wave.open(w).readframes(10**9)
    print("\nUSER AUDIO:", os.path.basename(w), len(pcm)//32, "ms")
    for i in range(0, len(pcm), 3200):
        send({"type":"input_audio_buffer.append","audio":base64.b64encode(pcm[i:i+3200]).decode()})
        time.sleep(0.1)
    # 3.5 s of silence so the turn detector closes the turn
    for _ in range(35):
        send({"type":"input_audio_buffer.append","audio":base64.b64encode(b"\0"*3200).decode()}); time.sleep(0.1)
    audio_bytes = 0; transcript = ""; tool = None
    for ev in recv_events(25):
        t = ev.get("type")
        if t == "response.audio.delta": audio_bytes += len(base64.b64decode(ev["delta"]))
        elif t == "response.audio_transcript.delta": transcript += ev.get("delta", "")
        elif t == "response.audio_transcript.done": transcript = ev.get("transcript", transcript)
        elif t in ("input_audio_buffer.speech_started","input_audio_buffer.speech_stopped","input_audio_buffer.committed"): print("  <<", t)
        elif t == "response.function_call_arguments.done":
            tool = ev; print("  TOOL CALL:", ev.get("name"), ev.get("arguments"), "call_id", ev.get("call_id"))
            send({"type":"conversation.item.create","item":{"type":"function_call_output","call_id":ev.get("call_id"),"output":json.dumps({"ok":True,"volume":40})}})
            send({"type":"response.create"})
        elif t == "error": print("  ERROR:", json.dumps(ev.get("error"), ensure_ascii=False))
        elif t == "response.done":
            st = ev.get("response", {}).get("status")
            print("  REPLY: %r audio=%d bytes (%.2fs @24k) status=%s" % (transcript, audio_bytes, audio_bytes/48000, st))
            if tool and audio_bytes == 0 and transcript == "":
                tool = None; audio_bytes = 0; continue   # wait for the post-tool reply
            break
s.close()
print("\nDONE")
