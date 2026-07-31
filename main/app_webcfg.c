#include "app_webcfg.h"
#include "app_config.h"
#include "app_dbg.h"
#include "app_ota.h"
#include "app_wifi.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_webcfg";
static httpd_handle_t s_server;

static const char INDEX_HTML[] =
"<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>claudius Config</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:system-ui,sans-serif;background:#0d1117;color:#c9d1d9;padding:20px;max-width:480px;margin:0 auto}"
"h1{font-size:1.2rem;margin-bottom:1.2rem;color:#58a6ff}"
"h2{font-size:.9rem;color:#8b949e;margin:1.2rem 0 .6rem;text-transform:uppercase;letter-spacing:.05em}"
"label{display:block;font-size:.85rem;margin-bottom:.2rem;color:#8b949e}"
"input[type=text],input[type=password]{width:100%;padding:.45rem .6rem;background:#161b22;border:1px solid #30363d;border-radius:6px;color:#c9d1d9;font-size:.9rem;margin-bottom:.7rem}"
"input:focus{outline:none;border-color:#58a6ff}"
".net-row{display:flex;gap:.4rem;margin-bottom:.5rem}"
".net-row input{margin:0;flex:1}"
".net-row button{background:#21262d;border:1px solid #30363d;color:#c9d1d9;border-radius:6px;padding:0 .6rem;cursor:pointer}"
"#add-net{background:none;border:1px dashed #30363d;color:#58a6ff;padding:.4rem .8rem;border-radius:6px;cursor:pointer;font-size:.85rem;width:100%;margin-top:.2rem}"
"button[type=submit]{background:#238636;border:none;color:#fff;padding:.55rem 1.2rem;border-radius:6px;cursor:pointer;font-size:.9rem;margin-top:.8rem}"
".chk{display:flex;align-items:center;gap:.5rem;color:#c9d1d9;font-size:.85rem;margin-bottom:.4rem;cursor:pointer}"
".range-row{display:flex;align-items:center;gap:.6rem;margin-bottom:.7rem}"
".range-row input[type=range]{flex:1;accent-color:#58a6ff}"
".range-row span{min-width:2.6rem;text-align:right;font-size:.85rem;color:#c9d1d9}"
"#fw{display:flex;gap:.5rem;align-items:center;margin-bottom:.4rem}"
"#fw input[type=file]{flex:1;color:#8b949e;font-size:.8rem}"
"#fw button{background:#21262d;border:1px solid #30363d;color:#c9d1d9;border-radius:6px;padding:.45rem .8rem;cursor:pointer}"
".hint{color:#8b949e;font-size:.75rem;margin-bottom:.6rem}"
"#msg{margin-top:.8rem;font-size:.85rem;color:#3fb950;min-height:1.2rem}"
"a.ota{display:inline-block;margin-top:1.2rem;color:#58a6ff;font-size:.85rem;text-decoration:none}"
"</style></head><body>"
"<h1>claudius</h1>"
"<form id=\"cfg\">"
"<h2>Device</h2>"
"<label>Device name (mDNS hostname)</label>"
"<input type=\"text\" id=\"deviceName\" placeholder=\"claudius\" maxlength=\"32\">"
"<label>Companion name</label>"
"<input type=\"text\" id=\"companionName\" placeholder=\"e.g. my-laptop\" maxlength=\"64\">"
"<label>Companion host (bypasses mDNS)</label>"
"<input type=\"text\" id=\"companionHost\" placeholder=\"e.g. 192.168.1.100\" maxlength=\"64\">"
"<label>Companion secret</label>"
"<input type=\"password\" id=\"companionSecret\" placeholder=\"(leave blank to keep)\">"
"<label class=\"chk\"><input type=\"checkbox\" id=\"clearSecret\"> Clear secret</label>"
"<h2>Display</h2>"
"<label>Brightness</label>"
"<div class=\"range-row\"><input type=\"range\" id=\"brightness\" min=\"1\" max=\"100\" value=\"25\">"
"<span id=\"brightnessVal\">25%</span></div>"
"<label class=\"chk\"><input type=\"checkbox\" id=\"sleepOnDisconnect\"> Sleep after 10 min disconnected</label>"
"<label class=\"chk\"><input type=\"checkbox\" id=\"sleepOnIdle\"> Sleep after 1 h idle</label>"
"<h2>WiFi networks (up to 3)</h2>"
"<div id=\"wifi-list\"></div>"
"<button type=\"button\" id=\"add-net\">+ Add network</button><br>"
"<button type=\"submit\">Save &amp; apply</button><div id=\"msg\"></div>"
"</form>"
"<h2>Firmware update</h2>"
"<form id=\"fw\" method=\"POST\" action=\"/api/ota\" enctype=\"multipart/form-data\">"
"<input type=\"file\" name=\"firmware\" accept=\".bin\" required>"
"<button type=\"submit\">Upload &amp; flash</button></form>"
"<div class=\"hint\">Device reboots after a successful upload.</div>"
"<a class=\"ota\" href=\"/debug\">Debug log →</a>"
"<script>"
"const list=document.getElementById('wifi-list');"
"const bright=document.getElementById('brightness');"
"const brightVal=document.getElementById('brightnessVal');"
"function setBright(v){bright.value=v;brightVal.textContent=v+'%';}"
"bright.oninput=()=>setBright(bright.value);"
"function esc(s){return s.replace(/&/g,'&amp;').replace(/\"/g,'&quot;').replace(/</g,'&lt;');}"
"function addNet(ssid='',pw=''){"
"  const row=document.createElement('div');row.className='net-row';"
"  row.innerHTML='<input type=\"text\" placeholder=\"SSID\" value=\"'+esc(ssid)+'\">'+"
"    '<input type=\"password\" placeholder=\"'+(pw?'(saved)':'Password')+'\">'+"
"    '<button type=\"button\">✕</button>';"
"  row.querySelector('button').onclick=()=>row.remove();list.appendChild(row);"
"}"
"fetch('/api/config').then(r=>r.json()).then(cfg=>{"
"  deviceName.value=cfg.deviceName||'';companionName.value=cfg.companionName||'';"
"  companionHost.value=cfg.companionHost||'';"
"  setBright(Math.min(100,Math.max(1,cfg.brightness|0||25)));"
"  sleepOnDisconnect.checked=cfg.sleepOnDisconnect!==false;"
"  sleepOnIdle.checked=cfg.sleepOnIdle!==false;"
"  companionSecret.placeholder=cfg.hasSecret?'(set — leave blank to keep)':'leave blank';"
"  (cfg.wifi||[]).forEach(n=>addNet(n.ssid,''));if(!list.children.length)addNet();"
"}).catch(()=>addNet());"
"document.getElementById('add-net').onclick=()=>{if(list.children.length<3)addNet();};"
"document.getElementById('cfg').onsubmit=async e=>{"
"  e.preventDefault();const msg=document.getElementById('msg');"
"  const wifi=[...list.querySelectorAll('.net-row')].map(r=>{"
"    const [s,p]=r.querySelectorAll('input');return{ssid:s.value.trim(),password:p.value};"
"  }).filter(n=>n.ssid);"
"  const body={"
"    deviceName:deviceName.value.trim(),companionName:companionName.value.trim(),"
"    companionHost:companionHost.value.trim(),"
"    companionSecret:clearSecret.checked?'':companionSecret.value,"
"    clearSecret:clearSecret.checked,"
"    brightness:+brightness.value,"
"    sleepOnDisconnect:sleepOnDisconnect.checked,sleepOnIdle:sleepOnIdle.checked,wifi"
"  };"
"  try{const res=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});"
"  msg.style.color=res.ok?'#3fb950':'#f85149';msg.textContent=res.ok?await res.text():'Error';"
"  }catch{msg.style.color='#f85149';msg.textContent='Connection error.';}"
"};"
"</script></body></html>";

static const char DEBUG_HTML[] =
"<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>claudius debug</title>"
"<style>body{font-family:monospace;background:#0d1117;color:#3fb950;padding:12px}"
"h1{font-size:.85rem;color:#58a6ff}#log{white-space:pre-wrap;font-size:.78rem;line-height:1.5}"
".err{color:#f85149}</style></head><body>"
"<h1>claudius debug — <span id=\"st\">…</span></h1><div id=\"log\"></div>"
"<script>let seq=0,el=log,st=document.getElementById('st');"
"function poll(){fetch('/api/debug/log?from='+seq).then(r=>r.json()).then(d=>{"
"st.textContent='live';(d.lines||[]).forEach(l=>{let div=document.createElement('div');div.textContent=l;el.appendChild(div);});"
"seq=d.seq;}).catch(()=>{st.textContent='disconnected';st.className='err';})"
".finally(()=>setTimeout(poll,1000));}poll();</script></body></html>";

static void reboot_soon(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static esp_err_t send_html(httpd_req_t *req, const char *html)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t root_get(httpd_req_t *req)
{
    return send_html(req, INDEX_HTML);
}

static esp_err_t debug_get(httpd_req_t *req)
{
    return send_html(req, DEBUG_HTML);
}

static esp_err_t api_debug_log(httpd_req_t *req)
{
    uint16_t from = 0;
    char query[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(query, "from", val, sizeof(val)) == ESP_OK) {
            from = (uint16_t)atoi(val);
        }
    }

    char lines[APP_DBG_LINES][APP_DBG_LINE_LEN];
    uint16_t seq = 0, used_from = 0;
    int n = app_dbg_copy_lines(from, lines, APP_DBG_LINES, &seq, &used_from);

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddNumberToObject(doc, "seq", seq);
    cJSON *arr = cJSON_AddArrayToObject(doc, "lines");
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(lines[i]));
    char *out = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{}");
    free(out);
    return ESP_OK;
}

static esp_err_t api_config_get(httpd_req_t *req)
{
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "deviceName", g_cfg.device_name);
    cJSON_AddStringToObject(doc, "companionName", g_cfg.companion_name);
    cJSON_AddStringToObject(doc, "companionHost", g_cfg.companion_host);
    cJSON_AddBoolToObject(doc, "hasSecret", g_cfg.companion_secret[0] != '\0');
    cJSON_AddNumberToObject(doc, "brightness", g_cfg.brightness);
    cJSON_AddBoolToObject(doc, "sleepOnDisconnect", g_cfg.sleep_on_disconnect);
    cJSON_AddBoolToObject(doc, "sleepOnIdle", g_cfg.sleep_on_idle);
    cJSON *wifi = cJSON_AddArrayToObject(doc, "wifi");
    for (uint8_t i = 0; i < g_cfg.wifi_count; i++) {
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "ssid", g_cfg.wifi[i].ssid);
        cJSON_AddItemToArray(wifi, n);
    }
    char *out = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{}");
    free(out);
    return ESP_OK;
}

static esp_err_t api_config_post(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_FAIL;
    }
    char *body = calloc(1, total + 1);
    if (!body) return ESP_ERR_NO_MEM;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
            return ESP_FAIL;
        }
        got += r;
    }

    cJSON *doc = cJSON_Parse(body);
    free(body);
    if (!doc) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }

    const cJSON *v;
    if ((v = cJSON_GetObjectItem(doc, "deviceName")) && cJSON_IsString(v)) {
        snprintf(g_cfg.device_name, sizeof(g_cfg.device_name), "%s", v->valuestring);
    }
    if ((v = cJSON_GetObjectItem(doc, "companionName")) && cJSON_IsString(v)) {
        snprintf(g_cfg.companion_name, sizeof(g_cfg.companion_name), "%s", v->valuestring);
    }
    if ((v = cJSON_GetObjectItem(doc, "companionHost")) && cJSON_IsString(v)) {
        snprintf(g_cfg.companion_host, sizeof(g_cfg.companion_host), "%s", v->valuestring);
    }

    bool clear_secret = cJSON_IsTrue(cJSON_GetObjectItem(doc, "clearSecret"));
    v = cJSON_GetObjectItem(doc, "companionSecret");
    if (clear_secret) {
        g_cfg.companion_secret[0] = '\0';
    } else if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
        snprintf(g_cfg.companion_secret, sizeof(g_cfg.companion_secret), "%s", v->valuestring);
    }

    if ((v = cJSON_GetObjectItem(doc, "brightness")) && cJSON_IsNumber(v)) {
        int b = v->valueint;
        if (b < 1) b = 1;
        if (b > 100) b = 100;
        g_cfg.brightness = (uint8_t)b;
    }
    if ((v = cJSON_GetObjectItem(doc, "sleepOnDisconnect"))) {
        g_cfg.sleep_on_disconnect = cJSON_IsTrue(v);
    }
    if ((v = cJSON_GetObjectItem(doc, "sleepOnIdle"))) {
        g_cfg.sleep_on_idle = cJSON_IsTrue(v);
    }

    const cJSON *wifi = cJSON_GetObjectItem(doc, "wifi");
    if (cJSON_IsArray(wifi)) {
        app_wifi_entry_t old[APP_MAX_WIFI_NETWORKS];
        uint8_t old_count = g_cfg.wifi_count;
        memcpy(old, g_cfg.wifi, sizeof(old));

        g_cfg.wifi_count = 0;
        const cJSON *net;
        cJSON_ArrayForEach(net, wifi) {
            if (g_cfg.wifi_count >= APP_MAX_WIFI_NETWORKS) break;
            const cJSON *ssid = cJSON_GetObjectItem(net, "ssid");
            const cJSON *pass = cJSON_GetObjectItem(net, "password");
            if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) continue;
            uint8_t i = g_cfg.wifi_count++;
            snprintf(g_cfg.wifi[i].ssid, sizeof(g_cfg.wifi[i].ssid), "%s", ssid->valuestring);
            g_cfg.wifi[i].password[0] = '\0';
            if (cJSON_IsString(pass) && pass->valuestring && pass->valuestring[0]) {
                snprintf(g_cfg.wifi[i].password, sizeof(g_cfg.wifi[i].password),
                         "%s", pass->valuestring);
            } else {
                /* Blank password → keep previous password for this SSID. */
                for (uint8_t j = 0; j < old_count; j++) {
                    if (strcmp(old[j].ssid, g_cfg.wifi[i].ssid) == 0) {
                        snprintf(g_cfg.wifi[i].password, sizeof(g_cfg.wifi[i].password),
                                 "%s", old[j].password);
                        break;
                    }
                }
            }
        }
    }

    cJSON_Delete(doc);
    app_config_save();
    httpd_resp_sendstr(req, "Saved — rebooting…");
    xTaskCreate(reboot_soon, "cfg_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t api_debug_sleep(httpd_req_t *req)
{
    if (ui_is_sleeping()) {
        ui_wake();
        httpd_resp_sendstr(req, "awake");
    } else {
        ui_sleep_start();
        httpd_resp_sendstr(req, "sleeping");
    }
    return ESP_OK;
}

esp_err_t app_webcfg_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t routes[] = {
        { .uri = "/",              .method = HTTP_GET,  .handler = root_get },
        { .uri = "/debug",         .method = HTTP_GET,  .handler = debug_get },
        { .uri = "/api/config",    .method = HTTP_GET,  .handler = api_config_get },
        { .uri = "/api/config",    .method = HTTP_POST, .handler = api_config_post },
        { .uri = "/api/debug/log", .method = HTTP_GET,  .handler = api_debug_log },
        { .uri = "/api/debug/sleep", .method = HTTP_POST, .handler = api_debug_sleep },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }
    app_ota_register(s_server);
    app_dbg_log("webcfg: http://%s.local/", g_cfg.device_name);
    return ESP_OK;
}
