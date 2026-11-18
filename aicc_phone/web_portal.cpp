#include "web_portal.h"
#include "config.h"
#include "settings.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

static WebServer s_server(80);
static DNSServer s_dns;
static bool s_apActive = false;
static String s_apSsid;
static bool s_serverStarted = false;

static String s_reqSsid, s_reqPass;
static bool s_haveWifiReq = false;
static bool s_restartReq = false;
static bool s_wifiResetReq = false;
static String s_statusText = "idle";
static String s_statusIp;
static bool s_connected = false;

static const char PAGE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>AICC Phone 配网</title>
<style>
body{font-family:-apple-system,BlinkMacSystemFont,"PingFang SC","Helvetica Neue",Arial,sans-serif;background:#0f1115;color:#e8e8e8;margin:0;padding:16px}
h1{font-size:20px;margin:8px 0 16px}
.card{background:#1b1f27;border-radius:14px;padding:16px;margin-bottom:16px;box-shadow:0 2px 10px rgba(0,0,0,.4)}
.card h2{font-size:16px;margin:0 0 12px;color:#8fd3ff}
label{display:block;font-size:13px;color:#aab;margin:10px 0 4px}
input,select{width:100%;box-sizing:border-box;padding:11px;border-radius:9px;border:1px solid #333a48;background:#0f1115;color:#fff;font-size:16px}
button{width:100%;padding:13px;border:0;border-radius:10px;font-size:16px;font-weight:600;margin-top:14px;color:#fff;background:#1e9e5a}
button.sec{background:#3a4152}button.warn{background:#b23b3b}
.net{display:flex;justify-content:space-between;align-items:center;padding:11px 8px;border-bottom:1px solid #262c38;cursor:pointer}
.net:active{background:#262c38}.net.sel{background:#1f3a2b}
.rssi{font-size:12px;color:#9aa}.lock{font-size:12px;color:#9aa;margin-left:6px}
.muted{font-size:12px;color:#889}
#status{font-size:14px;padding:10px;border-radius:9px;background:#0f1115;margin-top:10px;white-space:pre-wrap}
.row{display:flex;gap:10px}.row>*{flex:1}
</style></head><body>
<h1>AICC Phone 设置</h1>
<div class="card"><h2>1. 选择 Wi-Fi（仅支持 2.4GHz）</h2>
<div id="list" class="muted">正在扫描...</div>
<button class="sec" onclick="scan(true)">重新扫描</button>
<label>Wi-Fi 名称 (SSID)</label><input id="ssid" placeholder="点击上方列表选择，或手动输入">
<label>Wi-Fi 密码</label><input id="pass" type="password" placeholder="密码">
<button onclick="saveWifi()">连接并保存</button>
<div id="status">状态：等待操作</div>
</div>
<div class="card"><h2>2. SIP 设置</h2>
<label>服务器地址</label><input id="sserver">
<div class="row"><div><label>端口</label><input id="sport" type="number"></div><div><label>前台号码（DID）</label><input id="dial"></div></div>
<label>本机号码（SIP 用户名）</label><input id="suser">
<label>密码（仅注册时需要）</label><input id="spass" type="password">
<label>显示名称</label><input id="dname">
<label>注册到服务器</label><select id="sreg"><option value="0">否（直接呼叫）</option><option value="1">是</option></select>
<div class="row"><div><label>音量 (0-100)</label><input id="vol" type="number" min="0" max="100"></div><div><label>麦克风增益 (%)</label><input id="mic" type="number" min="0" max="200"></div></div>
<label>回声消除 (AEC)</label><select id="aec"><option value="1">全双工·低功耗（推荐，可插话打断）</option><option value="3">全双工·高性能（回声抑制最强，可能压住较弱的插话）</option><option value="2">VoIP·高性能</option><option value="0">关闭（半双工）</option></select>
<div class="row"><div><label>麦克风通道 (0-3)</label><input id="mslot" type="number" min="0" max="3"></div><div><label>回声参考 (0-3=硬件通道, 4=软件)</label><input id="rslot" type="number" min="0" max="4"></div></div>
<div class="row"><div><label>软件参考延迟 (ms)</label><input id="rdelay" type="number" min="0" max="400"></div><div><label>AEC 滤波器长度 (1-16)</label><input id="afilt" type="number" min="1" max="16"></div></div>
<div class="row"><div><label>参考通道模拟增益 (0-14)</label><input id="rgain" type="number" min="0" max="14"></div><div><label>参考通道软件增益 (x1-x8)</label><input id="rdig" type="number" min="1" max="8"></div></div>
<div class="row"><div><label>麦克风模拟增益 (0-14, 6=18dB)</label><input id="mpga" type="number" min="0" max="14"></div><div><label>残余回声抑制</label><select id="rsup"><option value="1">开</option><option value="0">关</option></select></div></div>
<label>唤醒灵敏度（唤醒词“你好小安”）</label><select id="wsens"><option value="0">标准</option><option value="1">宽松（更容易唤醒，误唤醒略多）</option><option value="2">严格（更少误唤醒）</option></select>
<button onclick="saveSip()">保存 SIP 设置并重启</button>
</div>
<div class="card"><h2>3. 语音助手（Qwen Realtime，直连）</h2>
<label>DashScope API Key</label><input id="aikey" type="password" placeholder="sk-...">
<div class="row"><div><label>模型</label><input id="aimodel"></div><div><label>音色（留空=默认）</label><input id="aivoice" placeholder="longanqian_v3.1"></div></div>
<label>接入地址（主机名）</label><input id="aihost" placeholder="dashscope.aliyuncs.com 或 <WorkspaceId>.cn-beijing.maas.aliyuncs.com">
<div class="row"><div><label>回复音频采样率</label><select id="airate"><option value="24000">24000</option><option value="16000">16000</option><option value="48000">48000</option><option value="8000">8000</option></select></div><div><label>无人说话多久休眠 (秒)</label><input id="aiidle" type="number" min="20" max="3600"></div></div>
<label><input id="aisearch" type="checkbox" style="width:auto;margin-right:6px"> 联网搜索（让小安在需要时联网查询天气、资讯等，默认开启）</label>
<label>本机附加信息（附加到提示词，例如酒店名、Wi-Fi 名称；酒店政策的正式回答请交给前台客服 AI）</label><input id="aiextra" placeholder="军阅国际酒店，Wi-Fi: Junyue-Guest / 密码 88888888">
<button onclick="saveSip()">保存并重启</button>
</div>
<div class="card"><h2>设备</h2>
<div class="muted" id="info"></div>
<button class="warn" onclick="resetWifi()">清除 Wi-Fi 配置</button>
</div>
<script>
var sel=null;
function $(i){return document.getElementById(i)}
function scan(force){
  $('list').textContent='正在扫描...';
  fetch('/scan'+(force?'?force=1':'')).then(r=>r.json()).then(d=>{
    if(d.scanning){setTimeout(()=>scan(false),1500);return}
    if(!d.networks.length){$('list').textContent='未发现网络，请重新扫描';return}
    var h='';d.networks.forEach(function(n,i){
      h+='<div class="net" onclick="pick('+i+',this)"><span>'+esc(n.ssid)+(n.enc?'<span class="lock">&#128274;</span>':'')+'</span><span class="rssi">'+n.rssi+' dBm</span></div>'});
    $('list').innerHTML=h;window._nets=d.networks;
  }).catch(()=>{$('list').textContent='扫描失败，请重试'});
}
function esc(s){return s.replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
function pick(i,el){document.querySelectorAll('.net').forEach(e=>e.classList.remove('sel'));el.classList.add('sel');$('ssid').value=window._nets[i].ssid;$('pass').focus()}
function saveWifi(){
  var s=$('ssid').value.trim();if(!s){alert('请选择或输入 Wi-Fi 名称');return}
  var b=new URLSearchParams();b.append('ssid',s);b.append('pass',$('pass').value);
  $('status').textContent='状态：正在连接 '+s+' ...';
  fetch('/wifi',{method:'POST',body:b}).then(r=>r.text()).then(()=>poll(0));
}
function poll(n){
  fetch('/status').then(r=>r.json()).then(d=>{
    $('status').textContent='状态：'+d.state+(d.ip?('\nIP: '+d.ip):'');
    if(d.connected){$('status').textContent+='\n配网成功！设备将自动进入电话界面。';return}
    if(n<40)setTimeout(()=>poll(n+1),1500);else $('status').textContent+='\n超时，请检查密码后重试';
  }).catch(()=>{if(n<40)setTimeout(()=>poll(n+1),1500)});
}
function load(){
  fetch('/config').then(r=>r.json()).then(c=>{
    ['sserver','sport','dial','suser','spass','dname','vol','mic','aec','mslot','rslot','rdelay','afilt','rgain','rdig','mpga','rsup','wsens','aikey','aimodel','aivoice','aihost','airate','aiidle','aiextra'].forEach(k=>$(k).value=c[k]);
    $('aisearch').checked=!!c.aisearch;
    $('sreg').value=c.sreg?'1':'0';
    $('info').textContent='固件 '+c.fw+' | 热点 '+c.ap+' | 当前 Wi-Fi: '+(c.wifi||'未配置')+' | IP: '+(c.ip||'-');
  });
}
function saveSip(){
  var b=new URLSearchParams();
  ['sserver','sport','dial','suser','spass','dname','sreg','vol','mic','aec','mslot','rslot','rdelay','afilt','rgain','rdig','mpga','rsup','wsens','aikey','aimodel','aivoice','aihost','airate','aiidle','aiextra'].forEach(k=>b.append(k,$(k).value));
  b.append('aisearch',$('aisearch').checked?'1':'0');
  fetch('/sip',{method:'POST',body:b}).then(r=>r.text()).then(t=>alert(t));
}
function resetWifi(){if(confirm('确定清除 Wi-Fi 配置并重启？'))fetch('/reset',{method:'POST'}).then(()=>alert('已清除，设备重启后会重新开启热点'))}
load();scan(true);
</script></body></html>)HTML";

static bool isCaptiveProbe() {
  String host = s_server.hostHeader();
  if (!s_apActive) return false;
  IPAddress apIp = WiFi.softAPIP();
  return host != apIp.toString();
}

static void redirectToRoot() {
  s_server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
  s_server.send(302, "text/plain", "");
}

static void handleRoot() {
  if (isCaptiveProbe()) { redirectToRoot(); return; }
  s_server.send_P(200, "text/html; charset=utf-8", PAGE_HTML);
}

static String jsonEscape(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if ((uint8_t)c < 0x20) continue;
    else o += c;
  }
  return o;
}

static void handleScan() {
  bool force = s_server.hasArg("force");
  int n = WiFi.scanComplete();
  if (force || n == WIFI_SCAN_FAILED) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true, false, false, 300);
    s_server.send(200, "application/json", "{\"scanning\":true}");
    return;
  }
  if (n == WIFI_SCAN_RUNNING) {
    s_server.send(200, "application/json", "{\"scanning\":true}");
    return;
  }
  // dedupe by SSID, keep strongest, sort by RSSI
  struct Net { String ssid; int rssi; bool enc; };
  static Net nets[40];
  int cnt = 0;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    int rssi = WiFi.RSSI(i);
    bool enc = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    bool dup = false;
    for (int j = 0; j < cnt; j++) {
      if (nets[j].ssid == ssid) { dup = true; if (rssi > nets[j].rssi) { nets[j].rssi = rssi; nets[j].enc = enc; } break; }
    }
    if (!dup && cnt < 40) { nets[cnt].ssid = ssid; nets[cnt].rssi = rssi; nets[cnt].enc = enc; cnt++; }
  }
  for (int i = 1; i < cnt; i++) {
    Net t = nets[i];
    int j = i - 1;
    while (j >= 0 && nets[j].rssi < t.rssi) { nets[j + 1] = nets[j]; j--; }
    nets[j + 1] = t;
  }
  String out = "{\"scanning\":false,\"networks\":[";
  for (int i = 0; i < cnt; i++) {
    if (i) out += ",";
    out += "{\"ssid\":\"" + jsonEscape(nets[i].ssid) + "\",\"rssi\":" + String(nets[i].rssi) + ",\"enc\":" + (nets[i].enc ? "true" : "false") + "}";
  }
  out += "]}";
  s_server.send(200, "application/json", out);
  WiFi.scanDelete();
}

static void handleWifi() {
  if (!s_server.hasArg("ssid")) { s_server.send(400, "text/plain", "missing ssid"); return; }
  s_reqSsid = s_server.arg("ssid");
  s_reqPass = s_server.arg("pass");
  s_haveWifiReq = true;
  s_statusText = "正在连接...";
  s_connected = false;
  s_server.send(200, "text/plain", "ok");
}

static void handleStatus() {
  String out = "{\"state\":\"" + jsonEscape(s_statusText) + "\",\"ip\":\"" + s_statusIp + "\",\"connected\":" + (s_connected ? "true" : "false") + "}";
  s_server.send(200, "application/json", out);
}

static void handleConfig() {
  Settings &s = g_settings;
  String out = "{";
  out += "\"sserver\":\"" + jsonEscape(s.sipServer) + "\",";
  out += "\"sport\":" + String(s.sipPort) + ",";
  out += "\"dial\":\"" + jsonEscape(s.dialNumber) + "\",";
  out += "\"suser\":\"" + jsonEscape(s.sipUser) + "\",";
  out += "\"spass\":\"" + jsonEscape(s.sipPass) + "\",";
  out += "\"dname\":\"" + jsonEscape(s.displayName) + "\",";
  out += "\"sreg\":" + String(s.sipRegister ? 1 : 0) + ",";
  out += "\"vol\":" + String(s.volume) + ",";
  out += "\"mic\":" + String(s.micGain) + ",";
  out += "\"aec\":" + String(s.aecMode) + ",";
  out += "\"mslot\":" + String(s.micSlot) + ",";
  out += "\"rslot\":" + String(s.refSlot) + ",";
  out += "\"rdelay\":" + String(s.refDelayMs) + ",";
  out += "\"afilt\":" + String(s.aecFilter) + ",";
  out += "\"rgain\":" + String(s.refGain) + ",";
  out += "\"rdig\":" + String(s.refDigGain) + ",";
  out += "\"mpga\":" + String(s.micPga) + ",";
  out += "\"rsup\":" + String(s.resSuppress ? 1 : 0) + ",";
  out += "\"wsens\":" + String(s.wakeSens) + ",";
  out += "\"aikey\":\"" + jsonEscape(s.aiKey) + "\",";
  out += "\"aimodel\":\"" + jsonEscape(s.aiModel) + "\",";
  out += "\"aivoice\":\"" + jsonEscape(s.aiVoice) + "\",";
  out += "\"aihost\":\"" + jsonEscape(s.aiHost) + "\",";
  out += "\"airate\":" + String(s.aiOutRate) + ",";
  out += "\"aiidle\":" + String(s.aiIdleSec) + ",";
  out += "\"aisearch\":" + String(s.aiSearch ? 1 : 0) + ",";
  out += "\"aiextra\":\"" + jsonEscape(s.aiExtra) + "\",";
  out += "\"fw\":\"" FW_VERSION "\",";
  out += "\"ap\":\"" + jsonEscape(s_apSsid) + "\",";
  out += "\"wifi\":\"" + jsonEscape(s.wifiSsid) + "\",";
  out += "\"ip\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("")) + "\"";
  out += "}";
  s_server.send(200, "application/json", out);
}

static void handleSip() {
  Settings &s = g_settings;
  if (s_server.hasArg("sserver")) s.sipServer = s_server.arg("sserver");
  if (s_server.hasArg("sport")) s.sipPort = s_server.arg("sport").toInt();
  if (s_server.hasArg("dial")) s.dialNumber = s_server.arg("dial");
  if (s_server.hasArg("suser")) s.sipUser = s_server.arg("suser");
  if (s_server.hasArg("spass")) s.sipPass = s_server.arg("spass");
  if (s_server.hasArg("dname")) s.displayName = s_server.arg("dname");
  if (s_server.hasArg("sreg")) s.sipRegister = s_server.arg("sreg").toInt() != 0;
  if (s_server.hasArg("vol")) s.volume = constrain(s_server.arg("vol").toInt(), 0, 100);
  if (s_server.hasArg("mic")) s.micGain = constrain(s_server.arg("mic").toInt(), 0, 200);
  if (s_server.hasArg("mslot")) s.micSlot = constrain(s_server.arg("mslot").toInt(), 0, 3);
  if (s_server.hasArg("rslot")) s.refSlot = constrain(s_server.arg("rslot").toInt(), 0, 4);
  if (s_server.hasArg("rdelay")) s.refDelayMs = constrain(s_server.arg("rdelay").toInt(), 0, 400);
  if (s_server.hasArg("afilt")) s.aecFilter = constrain(s_server.arg("afilt").toInt(), 1, 16);
  if (s_server.hasArg("rgain")) s.refGain = constrain(s_server.arg("rgain").toInt(), 0, 14);
  if (s_server.hasArg("rdig")) s.refDigGain = constrain(s_server.arg("rdig").toInt(), 1, 8);
  if (s_server.hasArg("mpga")) s.micPga = constrain(s_server.arg("mpga").toInt(), 0, 14);
  if (s_server.hasArg("rsup")) s.resSuppress = s_server.arg("rsup").toInt() != 0;
  if (s_server.hasArg("wsens")) s.wakeSens = constrain(s_server.arg("wsens").toInt(), 0, 2);
  if (s_server.hasArg("aikey")) { s.aiKey = s_server.arg("aikey"); s.aiKey.trim(); }
  if (s_server.hasArg("aimodel")) { s.aiModel = s_server.arg("aimodel"); s.aiModel.trim(); }
  if (s_server.hasArg("aivoice")) { s.aiVoice = s_server.arg("aivoice"); s.aiVoice.trim(); }
  if (s_server.hasArg("aihost")) { s.aiHost = s_server.arg("aihost"); s.aiHost.trim(); s.aiHost.replace("wss://", ""); s.aiHost.replace("https://", ""); int sl = s.aiHost.indexOf('/'); if (sl > 0) s.aiHost = s.aiHost.substring(0, sl); }
  if (s_server.hasArg("airate")) s.aiOutRate = constrain(s_server.arg("airate").toInt(), 8000, 48000);
  if (s_server.hasArg("aiidle")) s.aiIdleSec = constrain(s_server.arg("aiidle").toInt(), 20, 3600);
  if (s_server.hasArg("aiextra")) s.aiExtra = s_server.arg("aiextra");
  if (s_server.hasArg("aisearch")) s.aiSearch = (s_server.arg("aisearch") == "1" || s_server.arg("aisearch") == "true");
  if (s_server.hasArg("aec")) s.aecMode = constrain(s_server.arg("aec").toInt(), 0, 3);
  s.sipServer.trim(); s.dialNumber.trim(); s.sipUser.trim();
  if (s.sipPort == 0) s.sipPort = DEF_SIP_PORT;
  s.save();
  s_server.send(200, "text/plain; charset=utf-8", "已保存，设备正在重启。");
  s_restartReq = true;
}

static void handleReset() {
  s_wifiResetReq = true;
  s_server.send(200, "text/plain", "ok");
}

static void handleNotFound() {
  if (s_apActive) { redirectToRoot(); return; }
  s_server.send(404, "text/plain", "not found");
}

void web_begin() {
  if (s_serverStarted) return;
  s_server.on("/", handleRoot);
  s_server.on("/scan", handleScan);
  s_server.on("/wifi", HTTP_POST, handleWifi);
  s_server.on("/status", handleStatus);
  s_server.on("/config", handleConfig);
  s_server.on("/sip", HTTP_POST, handleSip);
  s_server.on("/reset", HTTP_POST, handleReset);
  // captive portal probes
  s_server.on("/generate_204", redirectToRoot);
  s_server.on("/gen_204", redirectToRoot);
  s_server.on("/hotspot-detect.html", redirectToRoot);
  s_server.on("/library/test/success.html", redirectToRoot);
  s_server.on("/connecttest.txt", redirectToRoot);
  s_server.on("/ncsi.txt", redirectToRoot);
  s_server.on("/fwlink", redirectToRoot);
  s_server.on("/redirect", redirectToRoot);
  s_server.onNotFound(handleNotFound);
  s_server.begin();
  s_serverStarted = true;
}

void web_start_ap() {
  if (s_apActive) return;
  s_apSsid = web_ap_ssid();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(s_apSsid.c_str(), AP_PASSWORD);
  s_dns.setErrorReplyCode(DNSReplyCode::NoError);
  s_dns.start(53, "*", WiFi.softAPIP());
  s_apActive = true;
  web_begin();
  WiFi.scanNetworks(true, false, false, 300);
  Serial.printf("[WEB] AP %s started, http://%s/\n", s_apSsid.c_str(), WiFi.softAPIP().toString().c_str());
}

void web_stop_ap() {
  if (!s_apActive) return;
  s_dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  s_apActive = false;
  Serial.println("[WEB] AP stopped");
}

bool web_ap_active() { return s_apActive; }
String web_ap_ssid() {
  if (s_apSsid.length() == 0) {
    uint64_t mac = ESP.getEfuseMac();   // valid before the WiFi driver is started
    char suffix[8];
    snprintf(suffix, sizeof(suffix), "%02X%02X", (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));
    s_apSsid = String(AP_SSID_PREFIX) + suffix;
  }
  return s_apSsid;
}

void web_loop() {
  if (s_apActive) s_dns.processNextRequest();
  if (s_serverStarted) s_server.handleClient();
}

bool web_take_wifi_request(String &ssid, String &pass) {
  if (!s_haveWifiReq) return false;
  s_haveWifiReq = false;
  ssid = s_reqSsid;
  pass = s_reqPass;
  return true;
}

bool web_take_restart_request() {
  if (!s_restartReq) return false;
  s_restartReq = false;
  return true;
}

bool web_take_wifi_reset_request() {
  if (!s_wifiResetReq) return false;
  s_wifiResetReq = false;
  return true;
}

void web_set_status(const char *stateText, const String &ip, bool connected) {
  s_statusText = stateText;
  s_statusIp = ip;
  s_connected = connected;
}
