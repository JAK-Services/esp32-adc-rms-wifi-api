// Hosts the embedded HTTP server and registers API endpoints.
// Serves a lightweight dashboard page for humans on any browser.
// Exposes the server handle so other modules can add their own handlers.

#include "api.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"

#include "adc.h"
#include "wifi_mgr.h"
#include "proto.h"
#include "app_config.h"

static const char *gTag = "API";
static httpd_handle_t gsHttpServer = NULL;



static esp_err_t Api_HandleRoot(httpd_req_t *psReq)
{
    // Serves a responsive dashboard page with RMS values and waveform plot
    // Plots DC-removed samples in volts so both channels oscillate around 0 V
    // Uses DPR-aware canvas rendering for readable text on phones

    // Build a responsive single-page UI
    const char *sHtml =
        "<!doctype html><html><head>"
        "<meta name='viewport' contenttext/html; charset=utf-8' content='width=device-width,initial-scale=1'>"
        "<meta charset='utf-8'>"
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
        ".row{display:flex;gap:12px;flex-wrap:wrap;align-items:center;justify-content:space-between;}"
        ".btn{appearance:none;border:1px solid rgba(255,255,255,.14);background:rgba(255,255,255,.06);"
        "color:#e9edf5;border-radius:12px;padding:10px 12px;font-weight:600;cursor:pointer;}"
        ".btn:active{transform:translateY(1px);}"
        "a{color:#b7d3ff;text-decoration:none;}a:hover{text-decoration:underline;}"
        "code{background:rgba(255,255,255,.06);padding:2px 6px;border-radius:8px;}"
        ".chartWrap{margin-top:12px;height:clamp(220px,35vh,360px);}"
        "canvas{width:100%;height:100%;display:block;border-radius:14px;"
        "background:rgba(8,12,18,.55);border:1px solid rgba(255,255,255,.08);}"
        "</style></head><body><div class='wrap'>"
        "<h1>ADC Node</h1>"

        "<div class='card'><div class='grid'>"
        "<div><div class='k'>RMS A</div><div id='rmsa' class='v'>-</div></div>"
        "<div><div class='k'>RMS B</div><div id='rmsb' class='v'>-</div></div>"
        "</div><div id='upd' class='u'>Updated: -</div></div>"

        "<div style='height:16px'></div>"

        "<div class='card'>"
        "<div class='row'>"
        "<div>"
        "<div class='k'>Last ADC Capture (AC)</div>"
        "<div class='u' id='waveInfo'>-</div>"
        "</div>"
        "<button class='btn' id='btnWave' type='button'>Refresh</button>"
        "</div>"
        "<div class='chartWrap'><canvas id='waveCanvas' aria-label='Waveform plot' role='img'></canvas></div>"
        "</div>"

        "<div style='height:16px'></div>"

        "<div class='card'>"
        "<div class='k'>API</div><div class='u'>"
        "<a href='/api/rms'><code>/api/rms</code></a> &nbsp;"
        "<a href='/api/samples'><code>/api/samples</code></a> &nbsp;"
        "<a href='/api/status'><code>/api/status</code></a> &nbsp;"
        "<a href='/provision'><code>/provision</code></a>"
        "</div></div>"

        "</div>"
        "<script>"
        "const sIdRmsA=document.getElementById('rmsa');"
        "const sIdRmsB=document.getElementById('rmsb');"
        "const sIdUpd=document.getElementById('upd');"
        "const sIdWaveInfo=document.getElementById('waveInfo');"
        "const sCanvas=document.getElementById('waveCanvas');"
        "const sBtnWave=document.getElementById('btnWave');"

        "function Clamp(dVal,dMin,dMax){"
        "  if(dVal<dMin)return dMin;"
        "  if(dVal>dMax)return dMax;"
        "  return dVal;"
        "}"

        "function GetCanvasDpr(){"
        "  const dCssWidth=Math.max(1,sCanvas.clientWidth);"
        "  return sCanvas.width/dCssWidth;"
        "}"

        "function ResizeCanvasToDisplay(){"
        "  const dDpr=window.devicePixelRatio||1;"
        "  const iCssWidth=Math.max(1,Math.floor(sCanvas.clientWidth));"
        "  const iCssHeight=Math.max(1,Math.floor(sCanvas.clientHeight));"
        "  const iNewWidth=Math.floor(iCssWidth*dDpr);"
        "  const iNewHeight=Math.floor(iCssHeight*dDpr);"
        "  if(sCanvas.width!==iNewWidth||sCanvas.height!==iNewHeight){"
        "    sCanvas.width=iNewWidth; sCanvas.height=iNewHeight;"
        "  }"
        "}"

        "function DrawWaveformVolts(sContext,afVoltsA,afVoltsB){"
        "  const iWidth=sCanvas.width, iHeight=sCanvas.height;"
        "  sContext.clearRect(0,0,iWidth,iHeight);"

        "  const dDpr=GetCanvasDpr();"
        "  const bIsMobile=window.matchMedia('(max-width:520px)').matches;"
        "  const dFontCss=bIsMobile?14:12;"
        "  const dFontPx=Math.round(dFontCss*dDpr);"
        "  const dLineThin=Math.max(1,Math.round(1*dDpr));"
        "  const dLineBold=Math.max(1,Math.round(2*dDpr));"

        "  const iPadLeft=Math.round(iWidth*0.14);"
        "  const iPadRight=Math.round(iWidth*0.04);"
        "  const iPadTop=Math.round(iHeight*0.10);"
        "  const iPadBottom=Math.round(iHeight*0.20);"
        "  const iPlotLeft=iPadLeft, iPlotRight=iWidth-iPadRight;"
        "  const iPlotTop=iPadTop, iPlotBottom=iHeight-iPadBottom;"
        "  const iPlotWidth=Math.max(1,iPlotRight-iPlotLeft);"
        "  const iPlotHeight=Math.max(1,iPlotBottom-iPlotTop);"

        "  let dMin=Number.POSITIVE_INFINITY;"
        "  let dMax=Number.NEGATIVE_INFINITY;"
        "  for(let iIndex=0;iIndex<afVoltsA.length;iIndex++){"
        "    const dValA=afVoltsA[iIndex];"
        "    const dValB=afVoltsB[iIndex];"
        "    if(dValA<dMin)dMin=dValA; if(dValA>dMax)dMax=dValA;"
        "    if(dValB<dMin)dMin=dValB; if(dValB>dMax)dMax=dValB;"
        "  }"
        "  if(!isFinite(dMin)||!isFinite(dMax)){return;}"
        "  if(dMax===dMin){dMax=dMin+0.001;}"
        "  const dRange=dMax-dMin;"
        "  const dPad=Math.max(0.002,dRange*0.10);"
        "  let dScaleMin=dMin-dPad;"
        "  let dScaleMax=dMax+dPad;"
        "  if(dScaleMin>0.0)dScaleMin=0.0-dPad;"
        "  if(dScaleMax<0.0)dScaleMax=0.0+dPad;"
        "  const dScaleRange=dScaleMax-dScaleMin;"

        "  sContext.save();"
        "  sContext.fillStyle='rgba(255,255,255,.04)';"
        "  sContext.fillRect(iPlotLeft,iPlotTop,iPlotWidth,iPlotHeight);"

        "  sContext.strokeStyle='rgba(255,255,255,.10)';"
        "  sContext.lineWidth=dLineThin;"
        "  const iGridX=5, iGridY=4;"
        "  for(let iG=0;iG<=iGridX;iG++){"
        "    const dX=iPlotLeft+(iPlotWidth*iG/iGridX);"
        "    sContext.beginPath(); sContext.moveTo(dX,iPlotTop); sContext.lineTo(dX,iPlotBottom); sContext.stroke();"
        "  }"
        "  for(let iG=0;iG<=iGridY;iG++){"
        "    const dY=iPlotTop+(iPlotHeight*iG/iGridY);"
        "    sContext.beginPath(); sContext.moveTo(iPlotLeft,dY); sContext.lineTo(iPlotRight,dY); sContext.stroke();"
        "  }"

        "  sContext.strokeStyle='rgba(255,255,255,.22)';"
        "  sContext.lineWidth=dLineThin;"
        "  sContext.beginPath();"
        "  sContext.moveTo(iPlotLeft,iPlotTop);"
        "  sContext.lineTo(iPlotLeft,iPlotBottom);"
        "  sContext.lineTo(iPlotRight,iPlotBottom);"
        "  sContext.stroke();"

        "  function MapX(iIndex,iCount){"
        "    if(iCount<=1)return iPlotLeft;"
        "    return iPlotLeft+(iPlotWidth*iIndex/(iCount-1));"
        "  }"
        "  function MapY(dVal){"
        "    return iPlotTop + (iPlotHeight*(1-((dVal-dScaleMin)/dScaleRange)));"
        "  }"

        "  const dZeroY=MapY(0.0);"
        "  sContext.strokeStyle='rgba(255,255,255,.30)';"
        "  sContext.lineWidth=dLineThin;"
        "  sContext.beginPath(); sContext.moveTo(iPlotLeft,dZeroY); sContext.lineTo(iPlotRight,dZeroY); sContext.stroke();"

        "  sContext.fillStyle='rgba(233,237,245,.80)';"
        "  sContext.font=dFontPx+'px system-ui,-apple-system,Segoe UI,Roboto,sans-serif';"
        "  sContext.textAlign='right'; sContext.textBaseline='middle';"

        "  const dTopVal=dScaleMax;"
        "  const dBotVal=dScaleMin;"
        "  const dTopY=iPlotTop;"
        "  const dBotY=iPlotBottom;"
        "  sContext.fillText(dTopVal.toFixed(3), iPlotLeft-10, dTopY);"
        "  sContext.fillText(dBotVal.toFixed(3), iPlotLeft-10, dBotY);"

        "  const dMinLabelSeparation=Math.max(14*dDpr, dFontPx*1.25);"
        "  if(Math.abs(dZeroY-dTopY)>dMinLabelSeparation && Math.abs(dZeroY-dBotY)>dMinLabelSeparation){"
        "    sContext.fillText('0.000', iPlotLeft-10, dZeroY);"
        "  }"

        "  sContext.textAlign='center'; sContext.textBaseline='top';"
        "  sContext.fillText('sample index', iPlotLeft+iPlotWidth/2, iPlotBottom+10*dDpr);"

        "  sContext.save();"
        "  sContext.translate(iPlotLeft-80*dDpr, iPlotTop+iPlotHeight/2);"
        "  sContext.rotate(-Math.PI/2);"
        "  sContext.textAlign='center'; sContext.textBaseline='top';"
        "  sContext.fillText('volts', 0, 0);"
        "  sContext.restore();"

        "  function DrawSeries(afSeries,sStroke){"
        "    sContext.strokeStyle=sStroke;"
        "    sContext.lineWidth=dLineBold;"
        "    sContext.beginPath();"
        "    for(let iIndex=0;iIndex<afSeries.length;iIndex++){"
        "      const dX=MapX(iIndex,afSeries.length);"
        "      const dY=MapY(afSeries[iIndex]);"
        "      if(iIndex===0)sContext.moveTo(dX,dY); else sContext.lineTo(dX,dY);"
        "    }"
        "    sContext.stroke();"
        "  }"

        "  DrawSeries(afVoltsA,'rgba(120,200,255,.95)');"
        "  DrawSeries(afVoltsB,'rgba(255,165,90,.95)');"

        "  sContext.textAlign='left'; sContext.textBaseline='middle';"
        "  const dLegendX=iPlotLeft+10*dDpr;"
        "  const dLegendY=iPlotTop+16*dDpr;"
        "  sContext.fillStyle='rgba(120,200,255,.95)'; sContext.fillRect(dLegendX,dLegendY-7*dDpr,12*dDpr,3*dDpr);"
        "  sContext.fillStyle='rgba(233,237,245,.82)'; sContext.fillText('Ch A', dLegendX+18*dDpr, dLegendY-6*dDpr);"
        "  sContext.fillStyle='rgba(255,165,90,.95)'; sContext.fillRect(dLegendX+64*dDpr,dLegendY-7*dDpr,12*dDpr,3*dDpr);"
        "  sContext.fillStyle='rgba(233,237,245,.82)'; sContext.fillText('Ch B', dLegendX+82*dDpr, dLegendY-6*dDpr);"
        "  sContext.restore();"
        "}"

        "async function FetchJson(sUrl){"
        "  const sResp=await fetch(sUrl,{cache:'no-store'});"
        "  if(!sResp.ok){throw new Error('HTTP '+sResp.status);}"
        "  return await sResp.json();"
        "}"

        "function FormatAgeSeconds(dAgeSec){"
        "  if(!isFinite(dAgeSec)){return '-';}"
        "  if(dAgeSec<0.0)dAgeSec=0.0;"
        "  if(dAgeSec<1.0)return (dAgeSec*1000.0).toFixed(0)+' ms ago';"
        "  return dAgeSec.toFixed(2)+' s ago';"
        "}"

        "async function UpdateRms(){"
        "  const sRms=await FetchJson('/api/rms');"
        "  if(!sRms||!sRms.hasValue){return;}"
        "  sIdRmsA.textContent=(sRms.rmsA?sRms.rmsA:0).toFixed(3)+' V';"
        "  sIdRmsB.textContent=(sRms.rmsB?sRms.rmsB:0).toFixed(3)+' V';"
        "  sIdUpd.textContent='Updated: '+(new Date()).toLocaleTimeString();"
        "}"

        "async function UpdateWaveform(){"
        "  ResizeCanvasToDisplay();"
        "  const sSamples=await FetchJson('/api/samples');"
        "  if(!sSamples||!sSamples.hasValue){sIdWaveInfo.textContent='No capture yet';return;}"
        "  const iCount=sSamples.samples||0;"
        "  const dAgeSec=(sSamples.serverNowUs && sSamples.timestampUs) ? ((sSamples.serverNowUs-sSamples.timestampUs)/1000000.0) : NaN;"
        "  sIdWaveInfo.innerHTML='Samples: '+iCount+' &middot; Units: V (AC) &middot; '+FormatAgeSeconds(dAgeSec);"
        "  const afVoltsA=sSamples.chA.map(iMilliVolts=>iMilliVolts/1000.0);"
        "  const afVoltsB=sSamples.chB.map(iMilliVolts=>iMilliVolts/1000.0);"
        "  const sContext=sCanvas.getContext('2d');"
        "  DrawWaveformVolts(sContext, afVoltsA, afVoltsB);"
        "}"

        "async function Tick(){"
        "  try{await UpdateRms();}catch(eVal){}"
        "  try{await UpdateWaveform();}catch(eVal){}"
        "}"

        "sBtnWave.addEventListener('click',()=>{UpdateWaveform();});"
        "window.addEventListener('resize',()=>{UpdateWaveform();});"
        "Tick();"
        "setInterval(Tick,1000);"
        "</script></body></html>";

    // Send HTML response
    httpd_resp_set_type(psReq, "text/html");
    httpd_resp_send(psReq, sHtml, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}



static esp_err_t Api_HandleStatus(httpd_req_t *psReq)
{
    // Serves JSON for current Wi-Fi manager state
    // Keeps output small so it is easy to parse on any client
    // Uses Proto serializer to keep formatting consistent

    // Build JSON into buffer
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
    // Backwards compatible with v1 provisioning page which expects {"sta_ip":"x"}.
    // Also keeps v2 fields {"hasValue":true,"ip":"x"} for newer clients.

    // Read cached IP from Wi-Fi manager
    char sIp[32] = {0};
    bool bHas = WifiMgr_GetStaIp(sIp, sizeof(sIp));

    // Build JSON payload
    char sJson[128];
    if (bHas) {
        (void)snprintf(sJson, sizeof(sJson),
                       "{\"hasValue\":true,\"ip\":\"%s\",\"sta_ip\":\"%s\"}",
                       sIp, sIp);
    } else {
        (void)snprintf(sJson, sizeof(sJson),
                       "{\"hasValue\":false,\"ip\":\"\",\"sta_ip\":\"\"}");
    }

    // Send JSON response (no-cache so browsers see updates)
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
    // Allows clients to poll periodically for updated results

    // Get latest result
    adc_result_t sResult;
    bool bHas = Adc_GetLatest(&sResult);

    // Build JSON
    char sJson[256];
    (void)Proto_BuildRmsJson(sJson, sizeof(sJson), &sResult, bHas);

    // Send JSON response
    httpd_resp_set_type(psReq, "application/json");
    httpd_resp_send(psReq, sJson, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}



static esp_err_t Api_HandleSamples(httpd_req_t *psReq)
{
    // Serves the last cached AC waveform window as signed millivolts
    // Adds server-side time so UI can show "age" without epoch-time confusion
    // Uses chunked responses to keep peak RAM usage low on the device

    int16_t aiChannelA_mV[iSamples_PerCh];
    int16_t aiChannelB_mV[iSamples_PerCh];
    int iSamplesReturned = 0;
    int64_t liTimestampUs = 0;
    adc_atten_t eAttenChannelA = ADC_ATTEN_DB_12;
    adc_atten_t eAttenChannelB = ADC_ATTEN_DB_12;

    // Read the last cached capture window
    bool bHasValue = Adc_GetLastSamplesMilliVolts(aiChannelA_mV, aiChannelB_mV, iSamples_PerCh,
                                                  &iSamplesReturned, &liTimestampUs,
                                                  &eAttenChannelA, &eAttenChannelB);

    httpd_resp_set_type(psReq, "application/json");

    // Return quickly if no samples are available yet
    if (!bHasValue) {
        httpd_resp_sendstr(psReq, "{\"hasValue\":false}");
        return ESP_OK;
    }

    // Capture current device time for age computation
    int64_t liServerNowUs = esp_timer_get_time();

    // Send JSON header metadata
    httpd_resp_sendstr_chunk(psReq, "{");
    httpd_resp_sendstr_chunk(psReq, "\"hasValue\":true,");

    char sHeader[256];
    snprintf(sHeader, sizeof(sHeader),
             "\"timestampUs\":%" PRId64 ",\"serverNowUs\":%" PRId64 ",\"samples\":%d,\"units\":\"mV\",",
             liTimestampUs, liServerNowUs, iSamplesReturned);
    httpd_resp_sendstr_chunk(psReq, sHeader);

    // Serialize channel A samples (signed mV)
    httpd_resp_sendstr_chunk(psReq, "\"chA\":[");
    for (int iIndex = 0; iIndex < iSamplesReturned; iIndex++) {

        char sNumber[20];
        snprintf(sNumber, sizeof(sNumber), "%d%s",
                 (int)aiChannelA_mV[iIndex], (iIndex == iSamplesReturned - 1) ? "" : ",");
        httpd_resp_sendstr_chunk(psReq, sNumber);
    }
    httpd_resp_sendstr_chunk(psReq, "],");

    // Serialize channel B samples (signed mV)
    httpd_resp_sendstr_chunk(psReq, "\"chB\":[");
    for (int iIndex = 0; iIndex < iSamplesReturned; iIndex++) {

        char sNumber[20];
        snprintf(sNumber, sizeof(sNumber), "%d%s",
                 (int)aiChannelB_mV[iIndex], (iIndex == iSamplesReturned - 1) ? "" : ",");
        httpd_resp_sendstr_chunk(psReq, sNumber);
    }
    httpd_resp_sendstr_chunk(psReq, "]");

    // Close the JSON object
    httpd_resp_sendstr_chunk(psReq, "}");
    httpd_resp_sendstr_chunk(psReq, NULL);

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
    // Increases handler slots so provisioning pages can register without abort

    // Configure HTTP server
    httpd_config_t sCfg = HTTPD_DEFAULT_CONFIG();
    sCfg.server_port = iHttpServerPort;

    // Increase handler slots for API + provisioning pages
    sCfg.max_uri_handlers = 16;

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

    // Register /api/samples
    httpd_uri_t sSamplesUri = {
        .uri = "/api/samples",
        .method = HTTP_GET,
        .handler = Api_HandleSamples,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(gsHttpServer, &sSamplesUri));

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
    // Returns HTTP server handle for other modules to register endpoints
    // Keeps a single server instance shared across the application
    // Avoids port conflicts by centralizing server ownership

    return gsHttpServer;
}
