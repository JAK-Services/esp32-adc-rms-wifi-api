// Implements a minimal DNS responder for captive portal support on the ESP32 SoftAP.
// Replies to all A queries with the SoftAP IP so clients resolve any hostname locally.
// Runs a lightweight UDP task that supports start/stop without external dependencies.

#include "dns_captive.h"

#include <string.h>
#include <stdbool.h>

#include "esp_log.h"

#include "app_config.h"
#include "lwip/ip4_addr.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *gTag = "DNS_CAPTIVE";

static TaskHandle_t gsDnsTaskHandle = NULL;
static int giDnsSocket = -1;
static volatile bool gbStopRequested = false;

static void DnsCaptive_Task(void *pvArg);
static esp_err_t DnsCaptive_OpenSocket(void);
static void DnsCaptive_CloseSocket(void);


static esp_err_t DnsCaptive_OpenSocket(void)
{
    // Opens and binds a UDP socket to port 53 on the SoftAP IP
    // Sets a short receive timeout so stop requests are handled quickly
    // Returns ESP_OK when the socket is ready for queries

    int iSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (iSock < 0) {
        return ESP_FAIL;
    }

    struct sockaddr_in sBindAddr = {0};
    sBindAddr.sin_family = AF_INET;
    sBindAddr.sin_port = htons(53);
    sBindAddr.sin_addr.s_addr = inet_addr(PROV_AP_IP_ADDR);

    if (bind(iSock, (struct sockaddr *)&sBindAddr, sizeof(sBindAddr)) != 0) {
        (void)close(iSock);
        return ESP_FAIL;
    }

    struct timeval sTimeout = {0};
    sTimeout.tv_sec = 1;
    sTimeout.tv_usec = 0;
    (void)setsockopt(iSock, SOL_SOCKET, SO_RCVTIMEO, &sTimeout, sizeof(sTimeout));

    giDnsSocket = iSock;
    return ESP_OK;
}


static void DnsCaptive_CloseSocket(void)
{
    // Closes the UDP socket used by the DNS responder
    // Resets the socket handle so restart is possible
    // Ignores errors to keep shutdown resilient

    if (giDnsSocket >= 0) {
        (void)close(giDnsSocket);
        giDnsSocket = -1;
    }
}


static void DnsCaptive_Task(void *pvArg)
{
    // Handles DNS queries by replying with an A record to the SoftAP IP
    // Answers all hostnames to simplify captive portal redirection on clients
    // Runs until a stop request is issued via DnsCaptive_Stop

    (void)pvArg;

    // Open UDP socket for port 53 queries
    if (DnsCaptive_OpenSocket() != ESP_OK) {
        ESP_LOGE(gTag, "Failed to open DNS socket");
        gsDnsTaskHandle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Parse provisioning AP IP once for answer payload reuse
    ip4_addr_t sApIpAddr = {0};
    uint8_t auApIpBytes[4] = {0, 0, 0, 0};

    if (ip4addr_aton(PROV_AP_IP_ADDR, &sApIpAddr) != 0) {
        auApIpBytes[0] = ip4_addr1(&sApIpAddr);
        auApIpBytes[1] = ip4_addr2(&sApIpAddr);
        auApIpBytes[2] = ip4_addr3(&sApIpAddr);
        auApIpBytes[3] = ip4_addr4(&sApIpAddr);
    }

    // Process queries until stop is requested
    while (!gbStopRequested) {
        uint8_t auRxBuffer[512] = {0};
        struct sockaddr_in sClientAddr = {0};
        socklen_t stClientAddrLen = sizeof(sClientAddr);

        int iReceived = recvfrom(giDnsSocket,
                                 auRxBuffer,
                                 sizeof(auRxBuffer),
                                 0,
                                 (struct sockaddr *)&sClientAddr,
                                 &stClientAddrLen);
        if (iReceived <= 0) {
            continue;
        }

        // Validate header and a single-question query packet
        if (iReceived < 12) {
            continue;
        }

        uint16_t usQdCount = (uint16_t)((auRxBuffer[4] << 8) | auRxBuffer[5]);
        if (usQdCount == 0) {
            continue;
        }

        // Find end of QNAME to locate QTYPE/QCLASS
        size_t stOffset = 12;
        while (stOffset < (size_t)iReceived && auRxBuffer[stOffset] != 0) {
            stOffset += (size_t)auRxBuffer[stOffset] + 1;
        }
        if (stOffset + 5 >= (size_t)iReceived) {
            continue;
        }
        stOffset += 1;

        // Check for A/IN queries, otherwise still respond to improve portal behavior
        uint16_t usQType = (uint16_t)((auRxBuffer[stOffset] << 8) | auRxBuffer[stOffset + 1]);
        uint16_t usQClass = (uint16_t)((auRxBuffer[stOffset + 2] << 8) | auRxBuffer[stOffset + 3]);
        (void)usQType;
        (void)usQClass;

        // Build DNS response using the received question as-is
        uint8_t auTxBuffer[512] = {0};
        if ((size_t)iReceived > sizeof(auTxBuffer)) {
            continue;
        }

        // Copy query into demonstrating transaction ID and question section
        (void)memcpy(auTxBuffer, auRxBuffer, (size_t)iReceived);

        // Set response flags and counts: standard response, 1 answer
        auTxBuffer[2] = 0x81;
        auTxBuffer[3] = 0x80;
        auTxBuffer[6] = 0x00;
        auTxBuffer[7] = 0x01;
        auTxBuffer[8] = 0x00;
        auTxBuffer[9] = 0x00;
        auTxBuffer[10] = 0x00;
        auTxBuffer[11] = 0x00;

        size_t stAnswerOffset = (size_t)iReceived;

        // Append answer: name pointer to question at 0x000c
        if (stAnswerOffset + 16 > sizeof(auTxBuffer)) {
            continue;
        }

        auTxBuffer[stAnswerOffset + 0] = 0xC0;
        auTxBuffer[stAnswerOffset + 1] = 0x0C;
        auTxBuffer[stAnswerOffset + 2] = 0x00;
        auTxBuffer[stAnswerOffset + 3] = 0x01;
        auTxBuffer[stAnswerOffset + 4] = 0x00;
        auTxBuffer[stAnswerOffset + 5] = 0x01;

        // TTL = 0 seconds to avoid caching beyond provisioning mode
        auTxBuffer[stAnswerOffset + 6] = 0x00;
        auTxBuffer[stAnswerOffset + 7] = 0x00;
        auTxBuffer[stAnswerOffset + 8] = 0x00;
        auTxBuffer[stAnswerOffset + 9] = 0x00;

        // RDLENGTH = 4 bytes for IPv4
        auTxBuffer[stAnswerOffset + 10] = 0x00;
        auTxBuffer[stAnswerOffset + 11] = 0x04;

        // RDATA = PROV_AP_IP_ADDR
        auTxBuffer[stAnswerOffset + 12] = auApIpBytes[0];
        auTxBuffer[stAnswerOffset + 13] = auApIpBytes[1];
        auTxBuffer[stAnswerOffset + 14] = auApIpBytes[2];
        auTxBuffer[stAnswerOffset + 15] = auApIpBytes[3];

        int iToSend = (int)(stAnswerOffset + 16);

        // Send response packet back to the requester
        (void)sendto(giDnsSocket,
                     auTxBuffer,
                     (size_t)iToSend,
                     0,
                     (struct sockaddr *)&sClientAddr,
                     stClientAddrLen);
    }

    // Shutdown DNS socket and task on stop
    DnsCaptive_CloseSocket();
    gsDnsTaskHandle = NULL;
    vTaskDelete(NULL);
}


esp_err_t DnsCaptive_Start(void)
{
    // Starts the DNS captive responder task if it is not already running
    // Leaves the task running until explicitly stopped by DnsCaptive_Stop
    // Returns ESP_OK when the task is running or already active

    // Avoid restarting when already running
    if (gsDnsTaskHandle != NULL) {
        return ESP_OK;
    }

    // Clear previous stop request and launch the responder task
    gbStopRequested = false;

    BaseType_t eTaskOk = xTaskCreate(DnsCaptive_Task,
                                    "dns_captive",
                                    4096,
                                    NULL,
                                    tskIDLE_PRIORITY + 2,
                                    &gsDnsTaskHandle);
    if (eTaskOk != pdPASS) {
        gsDnsTaskHandle = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(gTag, "DNS captive responder started on %s:53", PROV_AP_IP_ADDR);
    return ESP_OK;
}


esp_err_t DnsCaptive_Stop(void)
{
    // Requests the DNS captive responder to stop and releases its socket
    // Allows Wi-Fi manager to disable captive portal when provisioning ends
    // Returns ESP_OK even if the task was not running

    // Request stop and let the task exit on its own
    gbStopRequested = true;

    // Close socket to unblock recvfrom on some lwIP builds
    DnsCaptive_CloseSocket();

    ESP_LOGI(gTag, "DNS captive responder stop requested");
    return ESP_OK;
}
