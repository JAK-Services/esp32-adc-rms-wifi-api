// Implements the Wi-Fi provisioning web form and credential persistence.
// Registers endpoints on the shared HTTP server used for UI and APIs.
// Keeps provisioning logic independent from Wi-Fi driver setup and STA retries.

#include "wifi_prov.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "storage.h"
#include "wifi_mgr.h"

static const char *gTag = "WIFI_PROV";


static void WifiProv_UrlDecode(const char *sSrc, char *sDst, size_t iDstSize)
{
    // Decodes application/x-www-form-urlencoded fields
    // Converts percent-escaped bytes and plus signs
    // Produces a NUL terminated string within bounds

    size_t iSrcIndex = 0;
    size_t iDstIndex = 0;

    // Convert each input character safely
    while ((sSrc[iSrcIndex] != '\0') && (iDstIndex + 1 < iDstSize)) {

        if (sSrc[iSrcIndex] == '+') {
            sDst[iDstIndex++] = ' ';
            iSrcIndex++;
            continue;
        }

        if ((sSrc[iSrcIndex] == '%') &&
            (sSrc[iSrcIndex + 1] != '\0') &&
            (sSrc[iSrcIndex + 2] != '\0')) {

            // Decode a %HH sequence
            char sHex[3];
            sHex[0] = sSrc[iSrcIndex + 1];
            sHex[1] = sSrc[iSrcIndex + 2];
            sHex[2] = '\0';

            long liByteVal = strtol(sHex, NULL, 16);
            sDst[iDstIndex++] = (char)liByteVal;
            iSrcIndex += 3;
            continue;
        }

        // Copy a regular character
        sDst[iDstIndex++] = sSrc[iSrcIndex++];
    }

    // NUL terminate the output string
    sDst[iDstIndex] = '\0';
}


static void WifiProv_ExtractFormField(const char *sBody, const char *sKey,
                                      char *sOutVal, size_t iOutSize)
{
    // Extracts a single key=value field from an HTTP form body
    // Performs URL decoding into the provided output buffer
    // Returns an empty string if the field is not found

    sOutVal[0] = '\0';

    // Locate the key in the form body
    const char *sFound = strstr(sBody, sKey);
    if (sFound == NULL) {
        return;
    }

    // Ensure it is a proper key match (start or preceded by &)
    if ((sFound != sBody) && (*(sFound - 1) != '&')) {
        return;
    }

    // Ensure it is followed by '='
    const char *sEq = sFound + strlen(sKey);
    if (*sEq != '=') {
        return;
    }

    // Copy up to next '&' or end
    const char *sValStart = sEq + 1;
    const char *sValEnd = strchr(sValStart, '&');
    size_t iRawLen = (sValEnd == NULL) ? strlen(sValStart) : (size_t)(sValEnd - sValStart);

    // Decode the raw value into output
    char sTemp[128];
    size_t iCopyLen = (iRawLen < sizeof(sTemp) - 1) ? iRawLen : (sizeof(sTemp) - 1);
    memcpy(sTemp, sValStart, iCopyLen);
    sTemp[iCopyLen] = '\0';

    WifiProv_UrlDecode(sTemp, sOutVal, iOutSize);
}


static esp_err_t WifiProv_HandleGet(httpd_req_t *psReq)
{
    // Serves the provisioning HTML form
    // Provides a mobile-friendly layout and password visibility toggle
    // Guides the user through saving credentials and rebooting

    // Responsive and self-contained HTML/CSS/JS
    const char *sHtml =
        "<!doctype html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>WiFi Provision</title>"
        "<style>"
        "body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;"
        "background:#0b0f14;color:#e9eef6;display:flex;min-height:100vh;align-items:center;"
        "justify-content:center;padding:24px}"
        ".card{width:min(520px,100%);background:#121a24;border:1px solid #1f2b3a;"
        "border-radius:18px;box-shadow:0 12px 30px rgba(0,0,0,.35);padding:22px}"
        "h1{font-size:clamp(20px,4.5vw,28px);margin:0 0 10px}"
        ".muted{color:#a9b4c2;font-size:clamp(13px,3.4vw,14px);line-height:1.35}"
        "label{display:block;margin:16px 0 6px;font-size:14px;color:#cfd8e5}"
        "input{width:100%;box-sizing:border-box;padding:14px 12px;border-radius:12px;"
        "border:1px solid #2a3a50;background:#0f1620;color:#e9eef6;font-size:16px}"
        ".row{display:flex;gap:10px;align-items:stretch}"
        ".row input{flex:1}"
        ".btn{border:0;border-radius:12px;padding:14px 14px;font-size:16px;"
        "cursor:pointer;color:#0b0f14;background:#7dd3fc;white-space:nowrap}"
        ".btn2{background:#1f2b3a;color:#e9eef6;border:1px solid #2a3a50}"
        ".actions{display:flex;gap:10px;margin-top:18px}"
        "small{display:block;margin-top:12px;color:#9fb0c6}"
        "</style></head><body><div class='card'>"
        "<h1>Configure WiFi</h1>"
        "<div class='muted'>Enter your router SSID and password. The device will connect in the background after saving.</div>"
        "<form method='POST' action='/provision' autocomplete='off'>"
        "<label for='ssid'>SSID</label>"
        "<input id='ssid' name='ssid' maxlength='32' placeholder='Your WiFi name' required>"
        "<label for='pass'>Password</label>"
        "<div class='row'>"
        "<input id='pass' name='pass' type='password' maxlength='64' placeholder='WiFi password'>"
        "<button class='btn btn2' type='button' onclick='t()' id='tbtn'>Show</button>"
        "</div>"
        "<div class='actions'>"
        "<button class='btn' type='submit'>Save</button>"
        "</div>"
        "<small>Tip: The device will get an IP from your router once connected.</small>"
        "</form>"
        "<script>function t(){const p=document.getElementById('pass');"
        "const b=document.getElementById('tbtn');"
        "if(p.type==='password'){p.type='text';b.textContent='Hide';}"
        "else{p.type='password';b.textContent='Show';}};</script>"
        "</div></body></html>";

    httpd_resp_set_type(psReq, "text/html");
    return httpd_resp_send(psReq, sHtml, HTTPD_RESP_USE_STRLEN);
}


static esp_err_t WifiProv_HandlePost(httpd_req_t *psReq)
{
    // Saves posted Wi-Fi credentials to non-volatile storage.
    // Redirects the browser to an IP status page to avoid form resubmits.
    // Leaves STA connection handling to the background Wi-Fi manager.

    // Read the request body
    int iBodyLen = psReq->content_len;
    if (iBodyLen <= 0 || iBodyLen > 512) {
        return httpd_resp_send_err(psReq, HTTPD_400_BAD_REQUEST, "Bad request");
    }

    char sBody[513];
    int iReceivedLen = httpd_req_recv(psReq, sBody, iBodyLen);
    if (iReceivedLen <= 0) {
        return httpd_resp_send_err(psReq, HTTPD_500_INTERNAL_SERVER_ERROR, "Read failed");
    }
    sBody[iReceivedLen] = '\0';

    // Parse and decode SSID/password
    char sSsid[33];
    char sPass[65];
    WifiProv_ExtractFormField(sBody, "ssid", sSsid, sizeof(sSsid));
    WifiProv_ExtractFormField(sBody, "pass", sPass, sizeof(sPass));
    if (sSsid[0] == '\0') {
        return httpd_resp_send_err(psReq, HTTPD_400_BAD_REQUEST, "SSID required");
    }

    // Store credentials into NVS
    wifi_creds_t sCreds;
    memset(&sCreds, 0, sizeof(sCreds));
    strncpy(sCreds.sSsid, sSsid, sizeof(sCreds.sSsid) - 1);
    strncpy(sCreds.sPassword, sPass, sizeof(sCreds.sPassword) - 1);
    sCreds.bValid = true;

    esp_err_t eSaveErr = Storage_SaveWifiCreds(&sCreds);
    if (eSaveErr != ESP_OK) {
        ESP_LOGE(gTag, "Save creds failed (%s)", esp_err_to_name(eSaveErr));
        return httpd_resp_send_err(psReq, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
    }

    // Redirect to the IP status page
    httpd_resp_set_status(psReq, "303 See Other");
    httpd_resp_set_hdr(psReq, "Location", "/api/ips");
    httpd_resp_set_hdr(psReq, "Cache-Control", "no-store");
    httpd_resp_send(psReq, "", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


esp_err_t WifiProv_RegisterHandlers(httpd_handle_t sHttpServer)
{
    // Registers provisioning URI handlers on an existing HTTP server
    // Enables provisioning from either AP or STA network interfaces
    // Avoids creating a second HTTP server and port conflicts

    if (sHttpServer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Register GET /provision
    httpd_uri_t sProvGetUri = {
        .uri = "/provision",
        .method = HTTP_GET,
        .handler = WifiProv_HandleGet,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(sHttpServer, &sProvGetUri));

    // Register POST /provision
    httpd_uri_t sProvPostUri = {
        .uri = "/provision",
        .method = HTTP_POST,
        .handler = WifiProv_HandlePost,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(sHttpServer, &sProvPostUri));

    ESP_LOGI(gTag, "Provisioning handlers registered");
    return ESP_OK;
}
