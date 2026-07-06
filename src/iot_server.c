#include "pico_arc452a21/iot_server.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cyw43.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "pico/cyw43_arch.h"

#define HTTP_PORT 80
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define HTTP_REQUEST_MAX 1536
#define HTTP_HEADER_MAX 256
#define HTTP_BODY_MAX 1536
#define FORM_VALUE_MAX 128
#define WIFI_CONNECT_TIMEOUT_MS 8000
#define AP_SSID "Pico-ARC452A21"
#define AP_PASSWORD "arc452a21"

typedef struct {
    struct tcp_pcb *pcb;
    char request[HTTP_REQUEST_MAX];
    size_t request_len;
    char header[HTTP_HEADER_MAX];
    char body[HTTP_BODY_MAX];
    const char *body_ptr;
    size_t header_len;
    size_t body_len;
    size_t header_sent;
    size_t body_sent;
} http_conn_t;

static iot_server_config_t s_config;
static struct tcp_pcb *s_http_listener;
static struct udp_pcb *s_dhcp_pcb;
static http_conn_t s_conn;
static bool s_connected;
static bool s_ap_mode;
static char s_connected_ssid[APP_WIFI_SSID_MAX_LEN + 1];
static char s_ip_address[16] = "0.0.0.0";

static const char s_control_page[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Pico-ARC452A21</title><style>"
    "body{font:15px system-ui,-apple-system,Segoe UI,sans-serif;margin:0;background:#f7f8fa;color:#171b22}"
    "main{max-width:720px;margin:auto;padding:18px}header{display:flex;align-items:flex-start;justify-content:space-between;gap:16px;margin-bottom:12px}"
    "h1{font-size:22px;margin:0}.brand{display:grid;gap:2px}.projectLink{font-size:12px;color:#46515f}a{color:#1769e0;text-decoration:none}section{background:white;border:1px solid #d9dee5;border-radius:8px;padding:14px}"
    ".summary{display:flex;align-items:end;justify-content:space-between;gap:12px;margin-bottom:14px}.temp{font-size:42px;font-weight:700}.meta{color:#46515f;margin-top:3px}"
    ".powerBox{display:grid;gap:8px;justify-items:end}.powerSeg{justify-content:flex-end}.controls{display:grid;gap:12px;margin-top:14px}.controlRow{display:grid;grid-template-columns:110px 1fr;gap:10px;align-items:center}"
    ".controlLabel{color:#46515f;font-weight:600}.seg{display:flex;flex-wrap:wrap;gap:6px}.seg button{background:#eef2f7;color:#1c2633}.seg button.active{background:#1769e0;color:white}"
    "button{font:inherit;padding:10px 12px;border:0;border-radius:6px;background:#1769e0;color:white}.stepper{display:grid;grid-template-columns:52px 1fr 52px;gap:8px}.stepper button{font-size:22px;padding:9px 0}.stepper output{display:grid;place-items:center;border:1px solid #c6ccd4;border-radius:6px;font-size:20px;font-weight:700;min-height:44px}"
    "#status{min-height:22px;color:#263241}#tempField.hidden{display:none}@media(max-width:520px){.controlRow{grid-template-columns:1fr;gap:6px}.controlRow .seg button{flex:1 1 auto}}"
    "</style></head><body><main><header><div class=brand><h1 id=hostTitle>Pico-ARC452A21</h1><a class=projectLink href='https://github.com/sohampatwardhan/ESP-ARC452A21'>ESP-ARC452A21</a></div><a href=/settings>Settings</a></header>"
    "<section><div class=summary><div><div class=temp id=summaryTemp>--</div><div class=meta id=summaryMeta>Last sent settings</div></div><div class=powerBox><div id=powerState class=meta>--</div><div class='seg powerSeg' id=powerSeg><button data-v=on>On</button><button data-v=off>Off</button></div></div></div>"
    "<div class=controls><div class=controlRow id=tempField><div class=controlLabel>Temperature</div><div class=stepper><button onclick=tempStep(-1)>-</button><output id=temp>72 &deg;F</output><button onclick=tempStep(1)>+</button></div></div>"
    "<div class=controlRow><div class=controlLabel>Mode</div><div class=seg id=mode><button data-v=auto>Auto</button><button data-v=cool>Cool</button><button data-v=heat>Heat</button><button data-v=dry>Dry</button><button data-v=fan>Fan</button></div></div>"
    "<div class=controlRow><div class=controlLabel>Fan Speed</div><div class=seg id=fan><button data-v=1>1</button><button data-v=2>2</button><button data-v=3>3</button><button data-v=4>4</button><button data-v=5>5</button><button data-v=auto>Auto</button><button data-v=night>Night</button></div></div>"
    "<div class=controlRow><div class=controlLabel>Swing</div><div class=seg><button id=hswing data-toggle=hswing>Horizontal</button><button id=vswing data-toggle=vswing>Vertical</button></div></div>"
    "<div class=controlRow><div class=controlLabel>Quiet Mode</div><div class=seg id=quiet><button data-v=on>On</button><button data-v=off>Off</button></div></div>"
    "<div class=controlRow><div class=controlLabel>Sensor</div><div class=seg id=sensor><button data-sensor=comfort>Comfort</button><button data-sensor=eye>Intelligent Eye</button></div></div></div><p id=status></p></section></main><script>"
    "const $=id=>document.getElementById(id);let unit='f',ready=0,timer=0,updatedAge=0,updatedAt=0,state={mode:'cool',vswing:'off',hswing:'off',quiet:'off',sensor:'off'};const tempModes={auto:1,cool:1,heat:1},deg=String.fromCharCode(176);"
    "fetch('/wifi').then(r=>r.json()).then(j=>{if(j.hostname){$('hostTitle').textContent=j.hostname;document.title=j.hostname}}).catch(()=>{});"
    "async function cmd(c){$('status').textContent='Sending...';let r=await fetch('/command',{method:'POST',body:c});let j=await r.json().catch(()=>({ok:false,error:'bad response'}));show(j)};"
    "function setSeg(id,v){state[id]=v;document.querySelectorAll('#'+id+' button').forEach(b=>b.classList.toggle('active',b.dataset.v==v))};function setToggle(id,on){state[id]=on?'on':'off';$(id).classList.toggle('active',on)};"
    "function setSensor(v){state.sensor=v;$('sensor').querySelector('[data-sensor=comfort]').classList.toggle('active',v=='comfort'||v=='both');$('sensor').querySelector('[data-sensor=eye]').classList.toggle('active',v=='eye'||v=='both')};"
    "function sensorValue(){let c=$('sensor').querySelector('[data-sensor=comfort]').classList.contains('active'),e=$('sensor').querySelector('[data-sensor=eye]').classList.contains('active');return c&&e?'both':c?'comfort':e?'eye':'off'};"
    "function setPower(v){$('powerState').textContent=v;document.querySelectorAll('#powerSeg button').forEach(b=>b.classList.toggle('active',b.dataset.v==v))};function updateAge(){if(!updatedAt)return;let s=updatedAge+Math.floor((Date.now()-updatedAt)/1000),m=Math.floor(s/60),h=Math.floor(m/60);$('status').textContent='Last updated '+(s<60?'just now':m<60?m+' min ago':h+' hr ago')};"
    "function queue(c){if(!ready)return;clearTimeout(timer);timer=setTimeout(()=>cmd(c),350)};['vswing','hswing'].forEach(id=>$(id).onclick=()=>{let on=state[id]!='on';setToggle(id,on);queue(id+' '+(on?'on':'off'))});"
    "$('mode').onclick=e=>{if(e.target.dataset.v){setSeg('mode',e.target.dataset.v);modeChanged();queue('mode '+e.target.dataset.v)}};$('fan').onclick=e=>{if(e.target.dataset.v){setSeg('fan',e.target.dataset.v);queue('fan '+e.target.dataset.v)}};$('quiet').onclick=e=>{if(e.target.dataset.v){setSeg('quiet',e.target.dataset.v);queue('quiet '+e.target.dataset.v)}};"
    "$('sensor').onclick=e=>{if(e.target.dataset.sensor){e.target.classList.toggle('active');let v=sensorValue();state.sensor=v;queue('sensor '+v)}};$('powerSeg').onclick=e=>{if(e.target.dataset.v)power(e.target.dataset.v)};function modeChanged(){$('tempField').classList.toggle('hidden',!tempModes[state.mode])};"
    "function tempBounds(){return unit=='c'?[16,32]:[60,90]};function tempText(v){return v+' '+deg+unit.toUpperCase()};function tempStep(d){let b=tempBounds(),v=Math.max(b[0],Math.min(b[1],(parseInt($('temp').textContent)||b[0])+d));$('temp').textContent=tempText(v);queue('temp '+v+' '+unit)};"
    "function show(j){if(!j.ok){$('status').textContent=j.error||'failed'}else if(j.updated_age_s!==undefined){updatedAge=Number(j.updated_age_s)||0;updatedAt=Date.now();updateAge()}if(!j.state)return;let s=j.state;unit=s.unit.toLowerCase();$('temp').textContent=tempText(s.temperature);setSeg('mode',s.mode);setSeg('fan',s.fan);setToggle('vswing',s.vswing);setToggle('hswing',s.hswing);setSeg('quiet',s.quiet?'on':'off');setSensor(s.sensor);$('summaryTemp').textContent=tempModes[s.mode]?s.temperature+' '+deg+s.unit:s.mode;$('summaryMeta').textContent=s.mode+' - fan '+s.fan;setPower(s.power);modeChanged()};"
    "async function power(p){await cmd(p+(tempModes[state.mode]?' '+parseInt($('temp').textContent)+' '+unit:''))};async function refreshStatus(){let j=await fetch('/send?cmd=status').then(r=>r.json());show(j)};setInterval(updateAge,30000);setInterval(()=>refreshStatus().catch(()=>{}),15000);refreshStatus().then(()=>{ready=1}).catch(()=>{ready=1});"
    "</script></body></html>";

static const char s_settings_page[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>Settings</title><style>"
    "body{font:15px system-ui,-apple-system,Segoe UI,sans-serif;margin:0;background:#f7f8fa;color:#171b22}main{max-width:760px;margin:auto;padding:18px}header{display:flex;align-items:center;justify-content:space-between;margin-bottom:12px}h1{font-size:22px;margin:0}a{color:#1769e0;text-decoration:none}section{background:white;border:1px solid #d9dee5;border-radius:8px;padding:14px;margin:12px 0}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}.stack{grid-template-columns:1fr}label{display:grid;gap:5px;color:#46515f;font-size:13px;min-width:0}select,input{font:inherit;width:100%;box-sizing:border-box;padding:10px;border:1px solid #c6ccd4;border-radius:6px;background:white}button{font:inherit;padding:10px 12px;border:0;border-radius:6px;background:#1769e0;color:white}.secondary{background:#3d4856}.row{display:flex;flex-wrap:wrap;gap:8px;margin-top:10px}.slot{display:flex;align-items:center;justify-content:space-between;gap:8px;padding:8px 0;border-top:1px solid #edf0f3}.slot:first-child{border-top:0}.metric{background:#f7f8fa;border:1px solid #edf0f3;border-radius:6px;padding:10px}.metric b{display:block;margin-top:4px}#status,.inlineStatus{min-height:22px;color:#263241;margin:8px 0 0}</style></head><body><main><header><h1>Settings</h1><a href=/>AC Controls</a></header>"
    "<section><h2>Display</h2><div class=grid><label>Temperature Unit<select id=unitSetting><option value=fahrenheit>&deg;F</option><option value=celsius>&deg;C</option></select></label></div><div class=row><button type=button onclick=saveUnit()>Save Unit</button></div><p class=inlineStatus id=displayStatus></p></section>"
    "<section><h2>IR Blaster</h2><div class=grid><label>Polarity<select id=polarity><option value=normal>Normal</option><option value=invert>Invert</option></select></label><label>Timing<select id=timing><option value=nominal>Nominal</option><option value=captured>Captured</option></select></label><label>Repeat<input id=repeat type=number min=1 max=10 value=1></label><label>Gap (ms)<input id=gap type=number min=0 max=1000 value=80></label></div><div class=row><button type=button onclick=saveIr()>Apply IR Settings</button></div><p class=inlineStatus id=irStatus></p></section>"
    "<section><h2>Wi-Fi</h2><div id=wifiStatus></div><div class='grid stack'><label>Hostname<input id=hostname maxlength=32 required></label></div><div class=row><button onclick=saveHostname()>Save Hostname</button></div><h3>Saved Networks</h3><div id=wifiSlots></div><h3>Add Network</h3><div class='grid stack'><label>SSID<input id=ssid maxlength=32 required></label><label>Password<input id=password maxlength=64 type=password></label></div><div class=row><button onclick=saveWifi()>Save Wi-Fi</button><button class=secondary onclick=\"$('ssid').value='';$('password').value=''\">Clear</button></div></section>"
    "<section><h2>HomeKit</h2><div id=homekitDetails></div></section><section><h2>MQTT</h2><div id=mqttDetails></div></section><section><h2>Device</h2><button class=secondary onclick=rebootDevice()>Reboot</button><p class=inlineStatus id=deviceStatus></p></section><p id=status></p></main><script>"
    "const $=id=>document.getElementById(id);async function post(path,body,type='application/x-www-form-urlencoded'){let r=await fetch(path,{method:'POST',headers:{'Content-Type':type},body});let t=await r.text();let j;try{j=JSON.parse(t)}catch(e){j={ok:r.ok,message:t}}if(!r.ok)j.ok=false;return j}function msg(id,j){$(id).textContent=j.ok?(j.message||'ok'):(j.error||j.message||'failed')}function esc(s){return String(s||'').replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]))}"
    "async function saveUnit(){msg('displayStatus',await post('/command','unit '+$('unitSetting').value,'text/plain'))}async function saveIr(){msg('irStatus',await post('/settings/ir','polarity='+encodeURIComponent($('polarity').value)+'&timing='+encodeURIComponent($('timing').value)+'&repeat='+encodeURIComponent($('repeat').value)+'&gap='+encodeURIComponent($('gap').value)))}async function rebootDevice(){msg('deviceStatus',await post('/command','reboot','text/plain'))}"
    "async function saveHostname(){msg('status',await post('/wifi','action=hostname&hostname='+encodeURIComponent($('hostname').value)));loadWifi()}async function saveWifi(){msg('status',await post('/wifi','ssid='+encodeURIComponent($('ssid').value)+'&password='+encodeURIComponent($('password').value)));loadWifi()}async function delWifi(i){msg('status',await post('/wifi','action=delete&slot='+encodeURIComponent(i)));loadWifi()}"
    "async function loadWifi(){let j=await fetch('/wifi').then(r=>r.json()).catch(()=>({slots:[]}));$('hostname').value=j.hostname||'';$('wifiStatus').innerHTML='<div class=grid><div class=metric>Network<b>'+(j.connected?esc(j.ssid):'Not connected')+'</b></div><div class=metric>IP<b>'+esc(j.ip||'--')+'</b></div><div class=metric>Mode<b>'+esc(j.mode||'--')+'</b></div></div>';$('wifiSlots').innerHTML=(j.slots||[]).length?j.slots.map((x,i)=>'<div class=slot><span>'+esc(x.ssid)+'</span><span><button onclick=\"delWifi('+i+')\">Delete</button></span></div>').join(''):'<div class=slot>No saved networks</div>'}"
    "async function loadIntegrations(){let h=await fetch('/homekit').then(r=>r.json()).catch(()=>({available:false}));$('homekitDetails').innerHTML='<div class=metric>Status<b>'+(h.available?'Available':'Not compiled')+'</b></div>';let m=await fetch('/mqtt').then(r=>r.json()).catch(()=>({enabled:false}));$('mqttDetails').innerHTML='<div class=metric>Status<b>'+(m.enabled?'Enabled':'Not configured')+'</b></div>'}"
    "fetch('/send?cmd=status').then(r=>r.json()).then(j=>{if(j.state)$('unitSetting').value=j.state.unit=='C'?'celsius':'fahrenheit';if(j.ir){$('polarity').value=j.ir.polarity||'normal';$('timing').value=j.ir.timing||'nominal';$('repeat').value=j.ir.repeat||1;$('gap').value=j.ir.gap_ms!==undefined?j.ir.gap_ms:80}});loadWifi();loadIntegrations();"
    "</script></body></html>";

static void safe_copy(char *dst, const char *src, size_t size)
{
    if (size == 0) return;
    snprintf(dst, size, "%s", src ? src : "");
}

static bool has_text(const char *s)
{
    return s && s[0] != '\0';
}

static void json_escape(const char *src, char *dst, size_t size)
{
    size_t o = 0;
    if (size == 0) return;
    for (size_t i = 0; src && src[i] && o + 2 < size; ++i) {
        char c = src[i];
        if ((c == '"' || c == '\\') && o + 2 < size) {
            dst[o++] = '\\';
            dst[o++] = c;
        } else if ((unsigned char)c >= 0x20) {
            dst[o++] = c;
        }
    }
    dst[o] = '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    char *out = s;
    for (char *in = s; *in; ++in) {
        if (*in == '+') {
            *out++ = ' ';
        } else if (*in == '%' && isxdigit((unsigned char)in[1]) && isxdigit((unsigned char)in[2])) {
            *out++ = (char)((hex_value(in[1]) << 4) | hex_value(in[2]));
            in += 2;
        } else {
            *out++ = *in;
        }
    }
    *out = '\0';
}

static bool param_value(const char *form, const char *key, char *out, size_t out_size)
{
    size_t key_len = strlen(key);
    const char *p = form;
    while (p && *p) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            p += key_len + 1;
            size_t n = 0;
            while (p[n] && p[n] != '&' && n + 1 < out_size) {
                out[n] = p[n];
                ++n;
            }
            out[n] = '\0';
            url_decode(out);
            return true;
        }
        p = strchr(p, '&');
        if (p) ++p;
    }
    if (out_size) out[0] = '\0';
    return false;
}

static size_t wifi_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < APP_WIFI_CREDENTIAL_SLOTS; ++i) {
        if (has_text(s_config.snapshot->wifi[i].ssid)) ++count;
    }
    return count;
}

static void update_ip_string(void)
{
    if (s_connected) {
        const ip4_addr_t *addr = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
        snprintf(s_ip_address, sizeof(s_ip_address), "%s", ip4addr_ntoa(addr));
    } else if (s_ap_mode) {
        safe_copy(s_ip_address, "192.168.4.1", sizeof(s_ip_address));
    } else {
        safe_copy(s_ip_address, "0.0.0.0", sizeof(s_ip_address));
    }
}

static int connect_saved_wifi(void)
{
    cyw43_arch_enable_sta_mode();
    for (size_t i = 0; i < APP_WIFI_CREDENTIAL_SLOTS; ++i) {
        app_wifi_credential_t *cred = &s_config.snapshot->wifi[i];
        if (!has_text(cred->ssid)) continue;
        printf("Trying Wi-Fi SSID \"%s\"\n", cred->ssid);
        int auth = has_text(cred->password) ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN;
        int err = cyw43_arch_wifi_connect_timeout_ms(cred->ssid,
                                                     has_text(cred->password) ? cred->password : NULL,
                                                     auth,
                                                     WIFI_CONNECT_TIMEOUT_MS);
        if (err == 0) {
            s_connected = true;
            s_ap_mode = false;
            safe_copy(s_connected_ssid, cred->ssid, sizeof(s_connected_ssid));
            update_ip_string();
            printf("Wi-Fi connected: %s at %s\n", s_connected_ssid, s_ip_address);
            return 0;
        }
        printf("Wi-Fi SSID \"%s\" failed: %d\n", cred->ssid, err);
    }
    return -1;
}

static void dhcp_reply(struct udp_pcb *pcb, const uint8_t *request, uint16_t len, uint8_t msg_type)
{
    if (len < 240) return;
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 548, PBUF_RAM);
    if (!p) return;
    uint8_t *r = (uint8_t *)p->payload;
    memset(r, 0, 548);
    r[0] = 2;
    r[1] = 1;
    r[2] = 6;
    r[3] = 0;
    memcpy(&r[4], &request[4], 4);
    r[16] = 192; r[17] = 168; r[18] = 4; r[19] = 2;
    r[20] = 192; r[21] = 168; r[22] = 4; r[23] = 1;
    memcpy(&r[28], &request[28], 16);
    r[236] = 99; r[237] = 130; r[238] = 83; r[239] = 99;
    size_t o = 240;
    uint8_t opts[] = {
        53, 1, msg_type,
        54, 4, 192, 168, 4, 1,
        51, 4, 0, 0, 14, 16,
        1, 4, 255, 255, 255, 0,
        3, 4, 192, 168, 4, 1,
        6, 4, 192, 168, 4, 1,
        255,
    };
    memcpy(&r[o], opts, sizeof(opts));
    ip_addr_t dest;
    IP4_ADDR(ip_2_ip4(&dest), 255, 255, 255, 255);
    udp_sendto(pcb, p, &dest, DHCP_CLIENT_PORT);
    pbuf_free(p);
}

static void dhcp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                      const ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)addr; (void)port;
    if (!p) return;
    uint8_t buf[548] = {};
    uint16_t len = p->tot_len < sizeof(buf) ? p->tot_len : sizeof(buf);
    pbuf_copy_partial(p, buf, len, 0);
    pbuf_free(p);
    if (len < 244 || buf[236] != 99 || buf[237] != 130 || buf[238] != 83 || buf[239] != 99) return;
    uint8_t type = 0;
    for (uint16_t i = 240; i + 1 < len && buf[i] != 255;) {
        uint8_t opt = buf[i++];
        if (opt == 0) continue;
        uint8_t opt_len = buf[i++];
        if (i + opt_len > len) break;
        if (opt == 53 && opt_len == 1) type = buf[i];
        i += opt_len;
    }
    if (type == 1) dhcp_reply(pcb, buf, len, 2);
    if (type == 3) dhcp_reply(pcb, buf, len, 5);
}

static void start_dhcp_server(void)
{
    s_dhcp_pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (!s_dhcp_pcb) return;
    ip_addr_t any = IPADDR4_INIT(IPADDR_ANY);
    udp_bind(s_dhcp_pcb, &any, DHCP_SERVER_PORT);
    udp_recv(s_dhcp_pcb, dhcp_recv, NULL);
}

static void start_ap(void)
{
    printf("Starting setup AP SSID %s password %s\n", AP_SSID, AP_PASSWORD);
    cyw43_arch_disable_sta_mode();
    cyw43_arch_enable_ap_mode(AP_SSID, AP_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
    s_ap_mode = true;
    s_connected = false;
    s_connected_ssid[0] = '\0';
    update_ip_string();
    start_dhcp_server();
}

static void http_close(http_conn_t *conn)
{
    if (conn->pcb) {
        tcp_arg(conn->pcb, NULL);
        tcp_recv(conn->pcb, NULL);
        tcp_sent(conn->pcb, NULL);
        tcp_poll(conn->pcb, NULL, 0);
        tcp_close(conn->pcb);
    }
    memset(conn, 0, sizeof(*conn));
}

static err_t http_send_next(http_conn_t *conn)
{
    bool queued = false;
    while (conn->header_sent < conn->header_len) {
        size_t left = conn->header_len - conn->header_sent;
        u16_t chunk = (u16_t)(left > tcp_sndbuf(conn->pcb) ? tcp_sndbuf(conn->pcb) : left);
        if (chunk == 0) {
            if (queued) tcp_output(conn->pcb);
            return ERR_OK;
        }
        err_t err = tcp_write(conn->pcb, conn->header + conn->header_sent, chunk, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK) return err;
        queued = true;
        conn->header_sent += chunk;
    }
    while (conn->body_sent < conn->body_len) {
        size_t left = conn->body_len - conn->body_sent;
        u16_t snd = tcp_sndbuf(conn->pcb);
        u16_t chunk = (u16_t)(left > snd ? snd : left);
        if (chunk > 1024) chunk = 1024;
        if (chunk == 0) {
            if (queued) tcp_output(conn->pcb);
            return ERR_OK;
        }
        err_t err = tcp_write(conn->pcb, conn->body_ptr + conn->body_sent, chunk, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK) return err;
        queued = true;
        conn->body_sent += chunk;
    }
    if (queued) tcp_output(conn->pcb);
    if (conn->body_sent >= conn->body_len) {
        http_close(conn);
    }
    return ERR_OK;
}

static err_t http_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    (void)tpcb; (void)len;
    return http_send_next((http_conn_t *)arg);
}

static void respond(http_conn_t *conn, const char *status, const char *type,
                    const char *body)
{
    conn->body_ptr = body;
    conn->body_len = strlen(body);
    conn->header_len = (size_t)snprintf(conn->header, sizeof(conn->header),
                                        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
                                        status, type, (unsigned)conn->body_len);
    conn->header_sent = 0;
    conn->body_sent = 0;
    http_send_next(conn);
}

static void respond_json(http_conn_t *conn, const char *json)
{
    safe_copy(conn->body, json, sizeof(conn->body));
    respond(conn, "200 OK", "application/json", conn->body);
}

static void wifi_json(char *out, size_t size)
{
    char hostname[APP_HOSTNAME_MAX_LEN * 2] = {};
    char ssid[APP_WIFI_SSID_MAX_LEN * 2] = {};
    json_escape(s_config.snapshot->hostname, hostname, sizeof(hostname));
    json_escape(s_connected_ssid, ssid, sizeof(ssid));
    int rssi = iot_server_rssi();
    size_t o = (size_t)snprintf(out, size,
                                "{\"ok\":true,\"connected\":%s,\"mode\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"hostname\":\"%s\",\"slots\":[",
                                s_connected ? "true" : "false",
                                s_ap_mode ? "ap" : "sta",
                                ssid,
                                rssi,
                                s_ip_address,
                                hostname);
    bool first = true;
    for (size_t i = 0; i < APP_WIFI_CREDENTIAL_SLOTS && o < size; ++i) {
        if (!has_text(s_config.snapshot->wifi[i].ssid)) continue;
        char e[APP_WIFI_SSID_MAX_LEN * 2] = {};
        json_escape(s_config.snapshot->wifi[i].ssid, e, sizeof(e));
        o += (size_t)snprintf(out + o, size - o, "%s{\"slot\":%u,\"ssid\":\"%s\"}",
                              first ? "" : ",", (unsigned)i, e);
        first = false;
    }
    snprintf(out + o, size - o, "]}");
}

static void save_wifi_form(const char *form, char *json, size_t json_size)
{
    char action[16] = {};
    char slot_value[8] = {};
    char hostname[APP_HOSTNAME_MAX_LEN + 1] = {};
    char ssid[APP_WIFI_SSID_MAX_LEN + 1] = {};
    char password[APP_WIFI_PASSWORD_MAX_LEN + 1] = {};

    param_value(form, "action", action, sizeof(action));
    if (strcmp(action, "hostname") == 0) {
        if (!param_value(form, "hostname", hostname, sizeof(hostname)) || !has_text(hostname)) {
            snprintf(json, json_size, "{\"ok\":false,\"error\":\"hostname is required\"}");
            return;
        }
        safe_copy(s_config.snapshot->hostname, hostname, sizeof(s_config.snapshot->hostname));
        app_state_save(s_config.snapshot);
        snprintf(json, json_size, "{\"ok\":true,\"message\":\"Hostname saved\"}");
        return;
    }
    if (strcmp(action, "delete") == 0) {
        if (!param_value(form, "slot", slot_value, sizeof(slot_value))) {
            snprintf(json, json_size, "{\"ok\":false,\"error\":\"slot is required\"}");
            return;
        }
        long slot = strtol(slot_value, NULL, 10);
        if (slot < 0 || slot >= APP_WIFI_CREDENTIAL_SLOTS) {
            snprintf(json, json_size, "{\"ok\":false,\"error\":\"invalid slot\"}");
            return;
        }
        memset(&s_config.snapshot->wifi[slot], 0, sizeof(s_config.snapshot->wifi[slot]));
        app_state_save(s_config.snapshot);
        snprintf(json, json_size, "{\"ok\":true,\"message\":\"WiFi network deleted\"}");
        return;
    }
    if (!param_value(form, "ssid", ssid, sizeof(ssid)) || !has_text(ssid)) {
        snprintf(json, json_size, "{\"ok\":false,\"error\":\"SSID is required\"}");
        return;
    }
    param_value(form, "password", password, sizeof(password));
    size_t slot = APP_WIFI_CREDENTIAL_SLOTS;
    for (size_t i = 0; i < APP_WIFI_CREDENTIAL_SLOTS; ++i) {
        if (strcmp(s_config.snapshot->wifi[i].ssid, ssid) == 0) {
            slot = i;
            break;
        }
        if (slot == APP_WIFI_CREDENTIAL_SLOTS && !has_text(s_config.snapshot->wifi[i].ssid)) {
            slot = i;
        }
    }
    if (slot == APP_WIFI_CREDENTIAL_SLOTS) slot = APP_WIFI_CREDENTIAL_SLOTS - 1;
    safe_copy(s_config.snapshot->wifi[slot].ssid, ssid, sizeof(s_config.snapshot->wifi[slot].ssid));
    safe_copy(s_config.snapshot->wifi[slot].password, password, sizeof(s_config.snapshot->wifi[slot].password));
    app_state_save(s_config.snapshot);
    snprintf(json, json_size, "{\"ok\":true,\"message\":\"WiFi saved; reboot to connect\"}");
}

static void settings_ir_form(const char *form, char *json, size_t json_size)
{
    char value[FORM_VALUE_MAX] = {};
    char cmd[96] = {};
    char response[HTTP_BODY_MAX] = {};
    if (param_value(form, "polarity", value, sizeof(value))) {
        snprintf(cmd, sizeof(cmd), "polarity %s", value);
        s_config.command_handler(cmd, response, sizeof(response));
    }
    if (param_value(form, "timing", value, sizeof(value))) {
        snprintf(cmd, sizeof(cmd), "timing %s", value);
        s_config.command_handler(cmd, response, sizeof(response));
    }
    char repeat[16] = {};
    char gap[16] = "80";
    if (param_value(form, "repeat", repeat, sizeof(repeat))) {
        param_value(form, "gap", gap, sizeof(gap));
        snprintf(cmd, sizeof(cmd), "repeat %s %s", repeat, gap);
        s_config.command_handler(cmd, response, sizeof(response));
    }
    snprintf(json, json_size, "{\"ok\":true,\"message\":\"IR settings applied\"}");
}

static const char *request_body(const char *request)
{
    const char *p = strstr(request, "\r\n\r\n");
    return p ? p + 4 : "";
}

static void route_request(http_conn_t *conn)
{
    char method[8] = {};
    char target[256] = {};
    sscanf(conn->request, "%7s %255s", method, target);
    char *query = strchr(target, '?');
    if (query) *query++ = '\0';

    if (strcmp(method, "GET") == 0 && strcmp(target, "/") == 0) {
        respond(conn, "200 OK", "text/html; charset=utf-8", s_control_page);
    } else if (strcmp(method, "GET") == 0 && strcmp(target, "/settings") == 0) {
        respond(conn, "200 OK", "text/html; charset=utf-8", s_settings_page);
    } else if (strcmp(method, "GET") == 0 && strcmp(target, "/health") == 0) {
        respond_json(conn, "{\"ok\":true,\"device\":\"pico-arc452a21\"}");
    } else if (strcmp(method, "GET") == 0 && strcmp(target, "/wifi") == 0) {
        wifi_json(conn->body, sizeof(conn->body));
        respond(conn, "200 OK", "application/json", conn->body);
    } else if (strcmp(method, "POST") == 0 && strcmp(target, "/wifi") == 0) {
        save_wifi_form(request_body(conn->request), conn->body, sizeof(conn->body));
        respond(conn, "200 OK", "application/json", conn->body);
    } else if (strcmp(method, "GET") == 0 && strcmp(target, "/send") == 0) {
        char command[96] = "status";
        if (query) param_value(query, "cmd", command, sizeof(command));
        s_config.command_handler(command, conn->body, sizeof(conn->body));
        respond(conn, "200 OK", "application/json", conn->body);
    } else if (strcmp(method, "POST") == 0 && strcmp(target, "/command") == 0) {
        s_config.command_handler(request_body(conn->request), conn->body, sizeof(conn->body));
        respond(conn, "200 OK", "application/json", conn->body);
    } else if (strcmp(method, "POST") == 0 && strcmp(target, "/settings/ir") == 0) {
        settings_ir_form(request_body(conn->request), conn->body, sizeof(conn->body));
        respond(conn, "200 OK", "application/json", conn->body);
    } else if (strcmp(method, "GET") == 0 && strcmp(target, "/homekit") == 0) {
        respond_json(conn, "{\"ok\":true,\"enabled\":false,\"available\":false,\"started\":false,\"pairing\":false,\"pairing_timed_out\":false,\"paired_controller_count\":0,\"connected_controller_count\":0,\"setup_code\":\"\",\"setup_id\":\"\",\"setup_payload\":\"\",\"hap_port\":0}");
    } else if (strcmp(method, "POST") == 0 && strcmp(target, "/homekit") == 0) {
        respond_json(conn, "{\"ok\":false,\"error\":\"HomeKit native module is not compiled yet\"}");
    } else if (strcmp(method, "GET") == 0 && strcmp(target, "/mqtt") == 0) {
        respond_json(conn, "{\"ok\":true,\"enabled\":false,\"broker\":\"\",\"subscribe_topic\":\"pico-arc452a21/command\",\"publish_topic\":\"pico-arc452a21/status\"}");
    } else if (strcmp(method, "POST") == 0 && strcmp(target, "/mqtt") == 0) {
        respond_json(conn, "{\"ok\":false,\"error\":\"MQTT native module is not compiled yet\"}");
    } else {
        respond(conn, "200 OK", "text/html; charset=utf-8", s_settings_page);
    }
}

static bool request_complete(const char *request)
{
    const char *headers_end = strstr(request, "\r\n\r\n");
    if (!headers_end) return false;
    const char *cl = strstr(request, "Content-Length:");
    if (!cl) return true;
    long len = strtol(cl + 15, NULL, 10);
    return (long)(strlen(headers_end + 4)) >= len;
}

static err_t http_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    (void)tpcb;
    http_conn_t *conn = (http_conn_t *)arg;
    if (!p || err != ERR_OK) {
        if (p) pbuf_free(p);
        http_close(conn);
        return ERR_OK;
    }
    tcp_recved(conn->pcb, p->tot_len);
    size_t copy = p->tot_len;
    if (conn->request_len + copy >= sizeof(conn->request)) {
        copy = sizeof(conn->request) - conn->request_len - 1;
    }
    pbuf_copy_partial(p, conn->request + conn->request_len, copy, 0);
    conn->request_len += copy;
    conn->request[conn->request_len] = '\0';
    pbuf_free(p);
    if (request_complete(conn->request)) route_request(conn);
    return ERR_OK;
}

static err_t http_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !newpcb) return ERR_VAL;
    if (s_conn.pcb) http_close(&s_conn);
    memset(&s_conn, 0, sizeof(s_conn));
    s_conn.pcb = newpcb;
    tcp_arg(newpcb, &s_conn);
    tcp_recv(newpcb, http_recv);
    tcp_sent(newpcb, http_sent);
    return ERR_OK;
}

static void start_http_server(void)
{
    s_http_listener = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!s_http_listener) return;
    tcp_bind(s_http_listener, IP_ANY_TYPE, HTTP_PORT);
    s_http_listener = tcp_listen(s_http_listener);
    tcp_accept(s_http_listener, http_accept);
    printf("HTTP server ready on http://%s/\n", s_ip_address);
}

pico_arc_status_t iot_server_start(const iot_server_config_t *config)
{
    if (!config || !config->snapshot || !config->command_handler) return PICO_ARC_ERR_INVALID_ARG;
    s_config = *config;
    if (cyw43_arch_init() != 0) {
        printf("Wi-Fi init failed\n");
        return PICO_ARC_ERR_IO;
    }
    if (wifi_count() == 0 || connect_saved_wifi() != 0) start_ap();
    start_http_server();
    return PICO_ARC_OK;
}

void iot_server_poll(void)
{
    cyw43_arch_poll();
}

bool iot_server_is_connected(void)
{
    return s_connected;
}

const char *iot_server_ip_address(void)
{
    update_ip_string();
    return s_ip_address;
}

const char *iot_server_connected_ssid(void)
{
    return s_connected_ssid;
}

int iot_server_rssi(void)
{
    int32_t rssi = 0;
    if (!s_connected || cyw43_wifi_get_rssi(&cyw43_state, &rssi) != 0) return 0;
    return (int)rssi;
}
