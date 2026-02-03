// Shared HTTP server owner used by API and provisioning endpoints.
// Starts a single ESP-IDF httpd instance for all network interfaces.
// Allows modules to register routes without owning server lifetime.

#include "web_srv.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "app_config.h"

#include <string.h>

static const char *gTag = "WEB_SRV";
static httpd_handle_t gsHttpServer = NULL;

static esp_err_t WebSrv_SendRedirect(httpd_req_t *psReq, const char *psLocation);
static esp_err_t WebSrv_HandleCaptive(httpd_req_t *psReq);


static esp_err_t WebSrv_HandleRoot(httpd_req_t *psReq)
{
    // Redirects browsers to the provisioning page during first-boot AP setup
    // Helps phones open the configuration page when they detect a captive portal
    // Keeps the root URL simple and consistent across devices

    return WebSrv_SendRedirect(psReq, "/provision");
}


static esp_err_t WebSrv_SendRedirect(httpd_req_t *psReq, const char *psLocation)
{
    // Sends a small HTTP redirect response to guide users to the provisioning UI
    // Sets the Location header and a short cache policy for portal compatibility
    // Returns ESP_OK when the redirect is sent successfully

    // Add redirect headers expected by captive portal checkers
    (void)httpd_resp_set_status(psReq, "302 Found");
    (void)httpd_resp_set_hdr(psReq, "Location", psLocation);
    (void)httpd_resp_set_hdr(psReq, "Cache-Control", "no-store, no-cache, must-revalidate");
    (void)httpd_resp_set_hdr(psReq, "Pragma", "no-cache");

    // Send an empty body to keep responses fast and compatible
    return httpd_resp_send(psReq, NULL, 0);
}


static esp_err_t WebSrv_HandleCaptive(httpd_req_t *psReq)
{
    // Handles OS captive portal probe URLs and redirects them to provisioning
    // Improves the chance that phones show the "Sign in to Wi-Fi" prompt
    // Keeps fallback manual browsing working if the prompt does not appear

    return WebSrv_SendRedirect(psReq, "/provision");
}


esp_err_t WebSrv_Start(void)
{
    // Starts the shared HTTP server instance for the application
    // Registers a root page and leaves additional endpoints to other modules
    // Returns ESP_OK when the server is running

    if (gsHttpServer != NULL) {
        return ESP_OK;
    }

    httpd_config_t sConfig = HTTPD_DEFAULT_CONFIG();
    sConfig.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t eResult = httpd_start(&gsHttpServer, &sConfig);
    if (eResult != ESP_OK) {
        ESP_LOGE(gTag, "httpd_start failed: %s", esp_err_to_name(eResult));
        gsHttpServer = NULL;
        return eResult;
    }

    // Register root redirect for captive portal behavior
    httpd_uri_t sRootUri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = WebSrv_HandleRoot,
        .user_ctx = NULL,
    };
    (void)httpd_register_uri_handler(gsHttpServer, &sRootUri);

    httpd_uri_t sCaptiveUri = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = WebSrv_HandleCaptive,
        .user_ctx = NULL,
    };
    (void)httpd_register_uri_handler(gsHttpServer, &sCaptiveUri);

    httpd_uri_t sCaptiveAppleUri = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = WebSrv_HandleCaptive,
        .user_ctx = NULL,
    };
    (void)httpd_register_uri_handler(gsHttpServer, &sCaptiveAppleUri);

    httpd_uri_t sCaptiveWindowsUri = {
        .uri = "/ncsi.txt",
        .method = HTTP_GET,
        .handler = WebSrv_HandleCaptive,
        .user_ctx = NULL,
    };
    (void)httpd_register_uri_handler(gsHttpServer, &sCaptiveWindowsUri);

    httpd_uri_t sCaptiveWindowsTestUri = {
        .uri = "/connecttest.txt",
        .method = HTTP_GET,
        .handler = WebSrv_HandleCaptive,
        .user_ctx = NULL,
    };
    (void)httpd_register_uri_handler(gsHttpServer, &sCaptiveWindowsTestUri);

    return ESP_OK;
}


httpd_handle_t WebSrv_GetHandle(void)
{
    // Returns the shared HTTP server handle for route registration
    // Allows modules to register endpoints without starting servers
    // Returns NULL if WebSrv_Start has not been called

    return gsHttpServer;
}
