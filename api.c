// Hosts the embedded HTTP server and registers API endpoints.
// Serves a lightweight dashboard page for humans on any browser.
// Exposes the server handle so other modules can add their own handlers.

#include "api.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_http_server.h"

#include "adc.h"
#include "wifi_mgr.h"
#include "proto.h"
#include "app_config.h"

static const char *gTag = "API";
static httpd_handle_t gsHttpServer = NULL;

static esp_err_t Api_HandleRoot(httpd_req_t *psReq)
{
    // Serves a simple dashboard HTML page
    // Displays current RMS values and update timestamp
    // Provides easy links to JSON endpoints

    // Build a responsive single-page UI
    const char *sHtml =
        "<!doctype html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ADC Node</title>"
        "<style>"
        "html,body{height:100%;margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;}"
        "body{background:radial-gradient(circle at 30% 10%,#172033,#0b0f16);color:#e9edf5;}"
        ".wrap{max-width:760px;margin:0 auto;padding:24px 16px;}"
        "h1{margin:6px 0 18px;font-size:clamp(22px,4vw,34px);letter-spacing:.2px;}"
        ".card{background:rgba(13,18,28,.75);border:1px solid rgba(255,255,255,.08);"
        "border-radius:16px;padding:18px 18px;box-shadow:0 12px 40px rgba(0,0,0,.35);}"
        ".grid{display:grid;grid-template-columns:1fr 1fr;gap:18px;}"
        ".k{opacity:.75;font-size:clamp(12px,2.2vw,14px);text-transform:uppercase;letter-spacing:.12em;}"
        ".v{margin-top:6px;font-size:clamp(26px,6vw,42px);font-weight:700;}"
        ".u{margin-top:10px;opacity:.8;font-size:clamp(12px,2.4vw,14px);}"
        "a{color:#b7d3ff;text-decoration:none;}a:hover{text-decoration:underline;}"
        "code{background:rgba(255,255,255,.06);padding:2px 6px;border-radius:8px;}"
        "@media (max-width:520px){.grid{grid-template-columns:1fr;}}"
        "</style></head><body><div class='wrap'>"
        "<h1>ADC Node</h1>"
        "<div class='card'><div class='grid'>"
        "<div><div class='k'>RMS A</div><div id='rmsa' class='v'>-</div></div>"
        "<div><div class='k'>RMS B</div><div id='rmsb' class='v'>-</div></div>"
        "</div><div id='upd' class='u'>Updated: -</div></div>"
        "<div style='height:16px'></div>"
        "<div class='card'>"
        "<div class='k'>API</div><div class='u'>"
        "<a href='/api/rms'><code>/api/rms</code></a> &nbsp;"
        "<a href='/api/status'><code>/api/status</code></a> &nbsp;"
        "<a href='/provision'><code>/provision</code></a>"
        "</div></div>"
        "</div>"
        "<script>"
        "async function upd(){try{const r=await fetch('/api/rms',{cache:'no-store'});"
        "if(!r.ok) return; const j=await r.json();"
        "document.getElementById('rmsa').textContent=(j.rmsA ? j.rmsA : 0).toFixed(3)+' V';"
        "document.getElementById('rmsb').textContent=(j.rmsB ? j.rmsB : 0).toFixed(3)+' V';"
        "document.getElementById('upd').textContent='Updated: '+(new Date()).toLocaleTimeString();"

        "}catch(e){}} upd(); setInterval(upd,1000);"
        "</script></body></html>";

    // Send HTML response
    httpd_resp_set_type(psReq, "text/html");
    httpd_resp_send(psReq, sHtml, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t Api_HandleStatus(httpd_req_t *psReq)
{
    // Serves device status JSON including Wi-Fi manager state
    // Keeps response small to reduce latency and heap fragmentation
    // Allows mobile and desktop clients to show connection state

    // Build JSON payload
    char sJson[128];
    (void)Proto_BuildStatusJson(sJson, sizeof(sJson), WifiMgr_GetState());

    // Send JSON response
    httpd_resp_set_type(psReq, "application/json");
    httpd_resp_send(psReq, sJson, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


static esp_err_t Api_HandleStaIp(httpd_req_t *psReq)
{
    // Serves the current STA IPv4 address (if any) as JSON.
    // Supports the provisioning UI in showing router-assigned addressing.
    // Disables caching so mobile browsers always fetch fresh state.

    // Read cached IP from Wi-Fi manager
    char sStaIp[16] = {0};
    bool bHasIp = WifiMgr_GetStaIp(sStaIp, sizeof(sStaIp));

    // Build JSON payload
    char sJson[64];
    if (bHasIp) {
        (void)snprintf(sJson, sizeof(sJson), "{\"sta_ip\":\"%s\"}", sStaIp);
    } else {
        (void)snprintf(sJson, sizeof(sJson), "{\"sta_ip\":\"\"}");
    }

    // Send JSON response
    httpd_resp_set_type(psReq, "application/json");
    httpd_resp_set_hdr(psReq, "Cache-Control", "no-store");
    httpd_resp_send(psReq, sJson, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t Api_HandleIps(httpd_req_t *psReq)
{
    // Serves the provisioning IP status page on the AP interface.
    // Polls the cached STA DHCP IP and turns it into a clickable link.
    // Keeps refresh on this page to avoid resubmitting provisioning forms.

    // Build HTML response
    const char *sHtml =
        "<!doctype html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Device IP</title>"
        "<style>"
        "body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;"
        "background:#0b0f14;color:#e9eef6;display:flex;min-height:100vh;align-items:center;"
        "justify-content:center;padding:24px}"
        ".card{width:min(520px,100%);background:#121a24;border:1px solid #1f2b3a;"
        "border-radius:18px;box-shadow:0 12px 30px rgba(0,0,0,.35);padding:22px}"
        "h1{font-size:clamp(20px,4.5vw,28px);margin:0 0 10px}"
        ".muted{color:#a9b4c2;font-size:clamp(13px,3.4vw,14px);line-height:1.35}"
        "a{color:#7dd3fc;text-decoration:none}a:hover{text-decoration:underline}"
        ".pill{display:inline-block;padding:6px 10px;border-radius:999px;"
        "border:1px solid #2a3a50;background:#0f1620;font-size:13px}"
        "small{display:block;margin-top:14px;color:#9fb0c6;line-height:1.35}"
        "</style></head><body><div class='card'>"
        "<h1>WiFi saved</h1>"
        "<div class='muted'>Select your <b>home router WiFi</b> for the link below to work.</div>"
        "<div style='height:14px'></div>"
        "<div class='muted'>Device IP on your router: <span class='pill'><a id='ipLink' href='#'>detecting...</a></span></div>"
        "<small>If your phone disconnects from this AP during setup, reconnect and refresh this page.</small>"
        "<script>"
        "async function poll(){"
        " try{"
        "  const r=await fetch('/api/sta_ip?t='+Date.now(),{cache:'no-store'});"
        "  if(!r.ok) return;"
        "  const j=await r.json();"
        "  const a=document.getElementById('ipLink');"
        "  if(j.sta_ip){a.textContent=j.sta_ip; a.href='http://'+j.sta_ip+'/';}"
        "  else{a.textContent='detecting...'; a.href='#';}"
        " }catch(e){}"
        "}"
        "poll();"
        "setInterval(poll,5000);"
        "</script>"
        "</div></body></html>";

    // Send HTML response
    httpd_resp_set_type(psReq, "text/html; charset=utf-8");
    httpd_resp_set_hdr(psReq, "Cache-Control", "no-store");
    httpd_resp_send(psReq, sHtml, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


static esp_err_t Api_HandleRms(httpd_req_t *psReq)
{
    // Serves latest RMS measurement JSON from ADC module cache
    // Avoids blocking by returning cached values immediately
    // Allows clients to poll periodically without impacting sampling

    // Fetch latest ADC result
    adc_result_t sResult;
    bool bHas = Adc_GetLatest(&sResult);

    // Build JSON payload
    char sJson[256];
    (void)Proto_BuildRmsJson(sJson, sizeof(sJson), &sResult, bHas);

    // Send JSON response
    httpd_resp_set_type(psReq, "application/json");
    httpd_resp_send(psReq, sJson, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t Api_HandleCmd(httpd_req_t *psReq)
{
    // Accepts simple commands for future extension
    // Currently supports "measureNow" command to trigger ADC measurement
    // Responds with status JSON to confirm command acceptance

    // Read body into buffer
    char sBody[128];
    int iLen = httpd_req_recv(psReq, sBody, sizeof(sBody) - 1);
    if (iLen < 0) {
        httpd_resp_send_err(psReq, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_OK;
    }
    sBody[iLen] = '\0';

    // Trigger measurement if requested
    if (strstr(sBody, "measureNow") != NULL) {
        (void)Adc_MeasureNow();
    }

    // Reply with status
    char sJson[128];
    (void)Proto_BuildStatusJson(sJson, sizeof(sJson), WifiMgr_GetState());
    httpd_resp_set_type(psReq, "application/json");
    httpd_resp_send(psReq, sJson, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t Api_Start(void)
{
    // Starts HTTP API server for status, RMS readings, and commands
    // Registers endpoints that work in browser on mobile and desktop
    // Keeps implementation minimal to expand later with WebSocket streaming

    // Configure HTTP server
    httpd_config_t sCfg = HTTPD_DEFAULT_CONFIG();
    sCfg.server_port = iHttpServerPort;

    // Start server
    esp_err_t eErr = httpd_start(&gsHttpServer, &sCfg);
    if (eErr != ESP_OK) {
        ESP_LOGE(gTag, "httpd_start failed: %s", esp_err_to_name(eErr));
        return eErr;
    }

    // Register /api/status
    httpd_uri_t sStatusUri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = Api_HandleStatus,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(gsHttpServer, &sStatusUri));

    
    // Register /api/sta_ip
    httpd_uri_t sStaIpUri = {
        .uri = "/api/sta_ip",
        .method = HTTP_GET,
        .handler = Api_HandleStaIp,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(gsHttpServer, &sStaIpUri));

// Register /api/ips
httpd_uri_t sIpsUri = {
    .uri = "/api/ips",
    .method = HTTP_GET,
    .handler = Api_HandleIps,
    .user_ctx = NULL
};
ESP_ERROR_CHECK(httpd_register_uri_handler(gsHttpServer, &sIpsUri));

    // Register dashboard page
    httpd_uri_t sRootUri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = Api_HandleRoot,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(gsHttpServer, &sRootUri));

    // Register /api/rms
    httpd_uri_t sRmsUri = {
        .uri = "/api/rms",
        .method = HTTP_GET,
        .handler = Api_HandleRms,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(gsHttpServer, &sRmsUri));

    // Register /api/cmd
    httpd_uri_t sCmdUri = {
        .uri = "/api/cmd",
        .method = HTTP_POST,
        .handler = Api_HandleCmd,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(gsHttpServer, &sCmdUri));

    ESP_LOGI(gTag, "API started on port %d", iHttpServerPort);
    return ESP_OK;
}

httpd_handle_t Api_GetHttpServer(void)
{
    // Provides access to the shared HTTP server handle
    // Allows other modules to register their own URI handlers
    // Avoids creating multiple servers that compete for the same port

    return gsHttpServer;
}
