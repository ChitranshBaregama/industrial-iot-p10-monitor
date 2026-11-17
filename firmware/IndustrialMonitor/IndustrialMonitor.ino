
/*
 * Industrial IoT P10 Monitor — ATmega2560 firmware (prototype)
 * Changes:
 *  - HTTP connection (HTTPPARA + HTTPSETUP) is prepared BEFORE EVERY POST.
 *  - 60s wait for HTTPSETUP each time.
 *  - DHT floats formatted with dtostrf (no %f in snprintf) → never '?'.
 *  - All AT commands echoed: ">> ..." (including body and ^Z).
 *  - Fallback URL form if bracketed IPv6 URL is rejected.
 */

#define DEBUG_ENABLE 1
#define USE_DUMMY_DATA 0
#define SCREEN_INTERVAL_MS 4000UL
#define RTC_REFRESH_MS    1000UL
#define DHT_REFRESH_MS    2000UL
#define LOG_INTERVAL_MS   60000UL   // POST every 60 s
#define BOOT_GUARD_MS     6000UL

#include <Wire.h>
#include <RTClib.h>
#include <DHT.h>
#include <DMD2.h>
#include <fonts/SystemFont5x7.h>
#if DEBUG_ENABLE
  #include <stdarg.h>
#endif

// ---------------- GSM/Server ----------------
// Copy config.example.h to config.h and fill in your own deployment values.
// NEVER commit config.h, phone numbers, APNs, or server addresses.
#include "config.h"
#define GSM       Serial2
#define GSM_BAUD  9600

// ---------------- Hardware ----------------
#define RELAY_HEATER 22
#define RELAY_FAN    23
#define BUZZER_PIN   24
#define DHTPIN       4
#define DHTTYPE      DHT22

#define TEMP_HIGH_LIMIT 40.0f
#define HUMI_HIGH_LIMIT 80.0f

// ---------------- Debug helpers ----------------
#if DEBUG_ENABLE
  #define DBG_BEGIN(x) Serial.begin(x)
  #define DBG_PRINT(x) Serial.print(x)
  #define DBG_PRINTLN(x) Serial.println(x)
  #define DBG_WRITE(x) Serial.write(x)
  void dbg_printf(const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.print(buf);
  }
  #define DBG_PRINTF(...) dbg_printf(__VA_ARGS__)
#else
  #define DBG_BEGIN(x)
  #define DBG_PRINT(x)
  #define DBG_PRINTLN(x)
  #define DBG_WRITE(x)
  #define DBG_PRINTF(...)
#endif

// ---- Echo helpers for commands ----
void gsmEchoCmdPrefix() { DBG_PRINT(">> "); }
void gsmCmd(const __FlashStringHelper* p) { gsmEchoCmdPrefix(); Serial.println(p); GSM.println(p); }
void gsmCmd(const char* s)               { gsmEchoCmdPrefix(); DBG_PRINTLN(s);     GSM.println(s); }
void gsmCmdf(const char* fmt, ...) {
  char buf[192];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  gsmCmd(buf);
}
void gsmWriteCtrl(uint8_t b, const char* label) { GSM.write(b); gsmEchoCmdPrefix(); DBG_PRINTLN(label); }
void gsmEchoInfo(const char* s) { gsmEchoCmdPrefix(); DBG_PRINTLN(s); }

// ---------------- Objects/Globals ----------------
RTC_DS3231 rtc;
DHT dht(DHTPIN, DHTTYPE);
SoftDMD dmd(2, 1);
DMD_TextBox timeBox(dmd, 0, 0, 48, 7);
DMD_TextBox dateBox(dmd, 0, 8, 60, 8);

char timeStr[16], dateStr[16];
float temperature = 0.0f, humidity = 0.0f;
float lastGoodTemp = 25.0f, lastGoodHumi = 50.0f;   // defaults until first valid read
bool alarmActive=false, buzzerState=false, systemReady=false;

uint8_t currentScreen=0;
unsigned long lastScreenSwitch=0, lastRTCRefresh=0, lastDHTRead=0, lastLogTime=0, bootTime=0;

#define GSM_LINE_MAX 180
char gsmLineBuf[GSM_LINE_MAX]; uint8_t gsmLineLen=0;

volatile bool smsRespOK=false, smsRespERROR=false, smsRespPROMPT=false;
volatile bool httpRespOK=false, httpRespERROR=false, httpRespPROMPT=false;

// Network readiness flags (refreshed from URCs)
volatile bool cgattAttached = false;      // +CGATT: 1
volatile bool ceregRegistered = false;    // +CEREG: 1 or 5
volatile bool haveIPv6 = false;           // CGPADDR has ':'

// ---------------- SMS Queue ----------------
#define SMS_QUEUE_SIZE 4
#define SMS_MAX_LEN    161
char smsQueue[SMS_QUEUE_SIZE][SMS_MAX_LEN];
uint8_t smsQHead=0, smsQTail=0, smsQCount=0;
bool smsPush(const char *msg) {
  if (smsQCount >= SMS_QUEUE_SIZE) { DBG_PRINTLN(F("[SMS] Queue full – drop")); return false; }
  strncpy(smsQueue[smsQHead], msg, SMS_MAX_LEN - 1);
  smsQueue[smsQHead][SMS_MAX_LEN - 1] = '\0';
  smsQHead = (smsQHead + 1) % SMS_QUEUE_SIZE; smsQCount++; return true;
}

// ---------------- SMS RX state ----------------
bool smsAvailable=false; char smsRxBody[SMS_MAX_LEN]; char smsSenderNum[20];
bool rxWaitingHeader=false, rxWaitingBody=false;

// ---------------- SMS TX FSM (declare early) ----------------
enum SMS_TX_State {
  SMS_IDLE, SMS_SEND_AT, SMS_WAIT_AT, SMS_SET_TEXT, SMS_WAIT_TEXT,
  SMS_SEND_NUM, SMS_WAIT_PROMPT, SMS_SEND_BODY, SMS_WAIT_OK
};
SMS_TX_State smsTxState = SMS_IDLE;

// ---------------- Modem Basic Init FSM ----------------
enum MODEM_INIT_State {
  MODEM_IDLE, MODEM_AT, MODEM_WAIT_AT, MODEM_ECHO_OFF, MODEM_WAIT_ECHO,
  MODEM_CMGF, MODEM_WAIT_CMGF, MODEM_CSCS, MODEM_WAIT_CSCS,
  MODEM_CNMI, MODEM_WAIT_CNMI, MODEM_DONE
};
MODEM_INIT_State modemInitState=MODEM_IDLE; unsigned long modemInitTimer=0; bool modemReady=false;

// ---------------- Network Bring-up FSM (your sequence) ----------------
enum NET_State {
  NET_IDLE, NET_CFUN0, NET_W_CFUN0, NET_CFUN1, NET_W_CFUN1, NET_CPIN, NET_W_CPIN,
  NET_CSQ, NET_W_CSQ, NET_COPS, NET_W_COPS, NET_AT, NET_W_AT, NET_GMR, NET_W_GMR,
  NET_CGDCONT, NET_W_CGDCONT, NET_QCGDCONT, NET_W_QCGDCONT, NET_CGATTQ, NET_W_CGATTQ,
  NET_CEREGQ, NET_W_CEREGQ, NET_CGACT11, NET_W_CGACT11, NET_CGPADDR, NET_W_CGPADDR,
  NET_CGATT1, NET_W_CGATT1, NET_DONE, NET_FAIL
};
NET_State netState=NET_IDLE; unsigned long netTimer=0;

// ---------------- Per-POST HTTP FSM ----------------
enum HTTPv6_State {
  H6_IDLE,
  H6_WAIT_READY,                 // ensure CGATT/CEREG/IPv6 are good; poll if not
  H6_SETPARA_URL1, H6_W_URL1,    // http://[IPv6]/sensor
  H6_SETPARA_URL2, H6_W_URL2,    // fallback: raw literal
  H6_SETPARA_PORT, H6_W_PORT,
  H6_SETUP, H6_W_SETUP,          // 60s allowance
  H6_ACTION, H6_W_PROMPT,
  H6_SEND_BODY, H6_W_RESULT,
  H6_CLOSE, H6_W_CLOSE,
  H6_BACKOFF                      // short backoff if HTTPSETUP error
};
HTTPv6_State h6State=H6_IDLE; unsigned long h6Timer=0;
char h6Json[256]; int h6Len=0; bool h6TriedFallback=false; unsigned long h6BackoffUntil=0;

// ---------------- RTC / Display ----------------
void updateRTC() {
  DateTime now = rtc.now();
  snprintf(timeStr,sizeof(timeStr),"%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  snprintf(dateStr,sizeof(dateStr),"%02d/%02d/%04d", now.day(),  now.month(),  now.year());
}
void showTimeDateFull(){ updateRTC(); timeBox.clear(); dateBox.clear(); timeBox.print(timeStr); dateBox.print(dateStr); }
void updateTimeOnly(){ updateRTC(); timeBox.clear(); timeBox.print(timeStr); }
void showTempHumi(){ char tBuf[16], hBuf[16]; snprintf(tBuf,sizeof(tBuf),"TEMP %dC",(int)lastGoodTemp); snprintf(hBuf,sizeof(hBuf),"HUMI %d%%",(int)lastGoodHumi); timeBox.clear(); dateBox.clear(); timeBox.print(tBuf); dateBox.print(hBuf); }

// ---------------- Utils: safe float string with one decimal ----------------
void fmtFloat1(float v, char* out, size_t n) {
  if (isnan(v) || isinf(v)) { strncpy(out, "0.0", n); out[n-1]='\0'; return; }
  dtostrf(v, 0, 1, out);                     // minimal width, 1 decimal
  // trim any leading spaces that some libc variants may add
  char *p = out; while (*p==' ') p++;
  if (p!=out) memmove(out, p, strlen(p)+1);
}

// ---------------- Sensors ----------------
void updateDHT() {
  if (millis() - lastDHTRead < DHT_REFRESH_MS) return;
  lastDHTRead = millis();
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) { temperature = t; lastGoodTemp = t; }
  if (!isnan(h)) { humidity    = h; lastGoodHumi = h; }
}

// ---------------- Parser ----------------
void parseGsmLine(const char *line) {
  if (strcmp(line, "OK") == 0)      { smsRespOK = true; httpRespOK  = true; }
  if (strstr(line, "ERROR"))        { smsRespERROR = true; httpRespERROR = true; }

  // SMS notifications
  if (strncmp(line, "+CMTI:", 6) == 0) {
    const char *p = strrchr(line, ',');
    if (p) { int idx = atoi(p+1); char cmd[20]; snprintf(cmd,sizeof(cmd),"AT+CMGR=%d",idx); gsmCmd(cmd);
      rxWaitingHeader=true; rxWaitingBody=false; smsSenderNum[0]='\0'; }
  }
  else if (rxWaitingHeader && strncmp(line, "+CMGR:", 6) == 0) {
    rxWaitingHeader=false; rxWaitingBody=true;
    const char *q1=strchr(line,'"'); const char *q2=q1?strchr(q1+1,'"'):nullptr;
    const char *q3=q2?strchr(q2+1,'"'):nullptr; const char *q4=q3?strchr(q3+1,'"'):nullptr;
    if (q3 && q4) { size_t len=(size_t)(q4-q3-1); if (len>=sizeof(smsSenderNum)) len=sizeof(smsSenderNum)-1;
      strncpy(smsSenderNum,q3+1,len); smsSenderNum[len]='\0'; }
  }
  else if (rxWaitingBody && line[0] != '\0') {
    rxWaitingBody=false; strncpy(smsRxBody,line,SMS_MAX_LEN-1); smsRxBody[SMS_MAX_LEN-1]='\0'; smsAvailable=true; gsmCmd("AT+CMGD=1,4");
  }

  // Network readiness flags
  if (strncmp(line, "+CGATT:", 7) == 0) {
    int val = atoi(line + 7);
    cgattAttached = (val == 1);
  }
  if (strncmp(line, "+CEREG:", 7) == 0) {
    const char* p = strchr(line, ',');
    int stat = p ? atoi(p+1) : atoi(line + 7);
    ceregRegistered = (stat == 1 || stat == 5);
  }
  if (strncmp(line, "+CGPADDR:", 9) == 0) {
    const char* comma = strchr(line, ',');
    if (comma) {
      const char* ip = comma + 1;
      haveIPv6 = (strchr(ip, ':') != nullptr);
    }
  }

  // HTTP result
  if (strncmp(line, "+HTTPACTION:", 12) == 0) {
    const char *p = strchr(line, ',');
    if (p) {
      int code = atoi(p + 1);
      if (code == 200) { httpRespOK = true; }
      else             { httpRespERROR = true; }
    }
  }
}

// ---------------- RX pump ----------------
void gsmRxProcess() {
  while (GSM.available()) {
    char c = (char)GSM.read();
    if (DEBUG_ENABLE) DBG_WRITE(c);
    if (c == '>') { smsRespPROMPT = true; httpRespPROMPT = true; continue; }
    if (c == '\n') {
      gsmLineBuf[gsmLineLen] = '\0';
      while (gsmLineLen>0 && (gsmLineBuf[gsmLineLen-1]=='\r' || gsmLineBuf[gsmLineLen-1]==' '))
        gsmLineBuf[--gsmLineLen] = '\0';
      if (gsmLineLen>0) parseGsmLine(gsmLineBuf);
      gsmLineLen=0;
    } else if (c != '\r') {
      if (gsmLineLen < GSM_LINE_MAX-1) gsmLineBuf[gsmLineLen++] = c;
    }
  }
}

// ---------------- Modem init ----------------
void modemInitProcess() {
  if (modemReady) return;
  unsigned long now = millis();
  switch (modemInitState) {
    case MODEM_IDLE: modemInitTimer=now; modemInitState=MODEM_AT; break;
    case MODEM_AT:
      if (now - modemInitTimer >= 1000UL) { gsmCmd("AT"); modemInitTimer=now; modemInitState=MODEM_WAIT_AT; smsRespOK=false; }
      break;
    case MODEM_WAIT_AT:
      if (smsRespOK) { smsRespOK=false; modemInitState=MODEM_ECHO_OFF; }
      else if (now - modemInitTimer > 3000UL) modemInitState=MODEM_AT;
      break;
    case MODEM_ECHO_OFF: gsmCmd("ATE0"); modemInitTimer=now; modemInitState=MODEM_WAIT_ECHO; smsRespOK=false; break;
    case MODEM_WAIT_ECHO:
      if (smsRespOK || now - modemInitTimer > 2000UL) { smsRespOK=false; modemInitState=MODEM_CMGF; }
      break;
    case MODEM_CMGF: gsmCmd("AT+CMGF=1"); modemInitTimer=now; modemInitState=MODEM_WAIT_CMGF; smsRespOK=false; break;
    case MODEM_WAIT_CMGF:
      if (smsRespOK || now - modemInitTimer > 2000UL) { smsRespOK=false; modemInitState=MODEM_CSCS; }
      break;
    case MODEM_CSCS: gsmCmd("AT+CSCS=\"GSM\""); modemInitTimer=now; modemInitState=MODEM_WAIT_CSCS; smsRespOK=false; break;
    case MODEM_WAIT_CSCS:
      if (smsRespOK || now - modemInitTimer > 2000UL) { smsRespOK=false; modemInitState=MODEM_CNMI; }
      break;
    case MODEM_CNMI: gsmCmd("AT+CNMI=1,1,0,0,0"); modemInitTimer=now; modemInitState=MODEM_WAIT_CNMI; smsRespOK=false; break;
    case MODEM_WAIT_CNMI:
      if (smsRespOK || now - modemInitTimer > 2000UL) { smsRespOK=false; modemInitState=MODEM_DONE; }
      break;
    case MODEM_DONE:
      modemReady = true;
      DBG_PRINTLN(F("[MODEM] Init complete"));
      smsPush("SYSTEM READY\nCMDS:\n1 FAN ON\n2 FAN OFF\n3 HEATER ON\n4 HEATER OFF\n5 STATUS\n6 BUZZER OFF");
      break;
  }
}

// ---------------- Network bring-up (your list) ----------------
void netBringupProcess() {
  if (modemReady && netState == NET_IDLE) netState = NET_CFUN0;
  unsigned long now = millis();

  switch (netState) {
    case NET_IDLE: break;
    case NET_CFUN0: gsmCmd("AT+CFUN=0"); httpRespOK=httpRespERROR=false; netTimer=now; netState=NET_W_CFUN0; break;
    case NET_W_CFUN0:
      if (httpRespOK || httpRespERROR || (now - netTimer > 3000)) {
        gsmCmd("AT+CFUN=1"); httpRespOK=httpRespERROR=false; netTimer=now; netState=NET_W_CFUN1;
      } break;
    case NET_W_CFUN1: if (httpRespOK || (now - netTimer > 5000)) netState = NET_CPIN; break;
    case NET_CPIN:    gsmCmd("AT+CPIN?"); netTimer=now; netState=NET_W_CPIN; break;
    case NET_W_CPIN:  if (now - netTimer > 1000) netState = NET_CSQ; break;
    case NET_CSQ:     gsmCmd("AT+CSQ"); netTimer=now; netState=NET_W_CSQ; break;
    case NET_W_CSQ:   if (now - netTimer > 1000) netState = NET_COPS; break;
    case NET_COPS:    gsmCmd("AT+COPS?"); netTimer=now; netState=NET_W_COPS; break;
    case NET_W_COPS:  if (now - netTimer > 2000) netState = NET_AT; break;
    case NET_AT:      gsmCmd("AT"); netTimer=now; netState=NET_W_AT; httpRespOK=false; break;
    case NET_W_AT:    if (httpRespOK || now - netTimer > 2000) netState = NET_GMR; break;
    case NET_GMR:     gsmCmd("AT+GMR"); netTimer=now; netState=NET_W_GMR; break;
    case NET_W_GMR:   if (now - netTimer > 2000) netState = NET_CGDCONT; break;
    case NET_CGDCONT: gsmCmdf("AT+CGDCONT=1,\"%s\",\"%s\"", PDP_TYPE, APN); netTimer=now; netState=NET_W_CGDCONT; httpRespOK=false; break;
    case NET_W_CGDCONT: if (httpRespOK || now - netTimer > 3000) netState = NET_QCGDCONT; break;
    case NET_QCGDCONT: gsmCmd("AT+CGDCONT?"); netTimer=now; netState=NET_W_QCGDCONT; break;
    case NET_W_QCGDCONT: if (now - netTimer > 2000) netState = NET_CGATTQ; break;
    case NET_CGATTQ:  gsmCmd("AT+CGATT?"); netTimer=now; netState=NET_W_CGATTQ; break;
    case NET_W_CGATTQ: if (now - netTimer > 2000) netState = NET_CEREGQ; break;
    case NET_CEREGQ:  gsmCmd("AT+CEREG?"); netTimer=now; netState=NET_W_CEREGQ; break;
    case NET_W_CEREGQ: if (now - netTimer > 2000) netState = NET_CGACT11; break;
    case NET_CGACT11: gsmCmd("AT+CGACT=1,1"); netTimer=now; netState=NET_W_CGACT11; httpRespOK=false; break;
    case NET_W_CGACT11: if (httpRespOK || now - netTimer > 10000) netState = NET_CGPADDR; break;
    case NET_CGPADDR: gsmCmd("AT+CGPADDR=1"); netTimer=now; netState=NET_W_CGPADDR; break;
    case NET_W_CGPADDR: if (now - netTimer > 2000) netState = NET_CGATT1; break;
    case NET_CGATT1:  gsmCmd("AT+CGATT=1"); netTimer=now; netState=NET_W_CGATT1; httpRespOK=false; break;
    case NET_W_CGATT1: if (httpRespOK || now - netTimer > 5000) netState = NET_DONE; break;
    case NET_DONE: break;
    case NET_FAIL: break;
  }
}

// ---------------- Per-POST HTTP FSM ----------------
void httpV6StartOnce(const char* json, int len) {
  if (h6State != H6_IDLE) return;
  strncpy(h6Json, json, sizeof(h6Json)-1); h6Json[sizeof(h6Json)-1]='\0';
  h6Len = len;
  httpRespOK=httpRespERROR=httpRespPROMPT=false;
  h6TriedFallback = false;
  h6State = H6_WAIT_READY;
}

void httpV6TxProcess() {
  if (!modemReady || netState != NET_DONE) return;
  if (smsTxState != SMS_IDLE) return;

  const unsigned long TO = 8000;
  const unsigned long HTTPSETUP_TO = 60000;
  unsigned long now = millis();

  switch (h6State) {
    case H6_IDLE: break;

    case H6_WAIT_READY: {
      if (cgattAttached && ceregRegistered && haveIPv6) {
        // Start with bracketed URL form
        gsmCmdf("AT+HTTPPARA=url,http://%s%s", IPV6_HOST_BRACKETED, IPV6_PATH);
        h6Timer = now; h6State = H6_W_URL1; httpRespOK=httpRespERROR=false;
      } else {
        static unsigned long pollT=0;
        if (now - pollT > 5000) {
          pollT = now;
          gsmCmd("AT+CGATT?"); gsmCmd("AT+CEREG?"); gsmCmd("AT+CGPADDR=1");
          gsmEchoInfo("[WAIT] Need CGATT=1, CEREG=1/5, IPv6");
        }
      }
    } break;

    case H6_W_URL1:
      if (httpRespOK) { httpRespOK=false; h6State = H6_SETPARA_PORT; }
      else if (httpRespERROR || now - h6Timer > TO) { h6TriedFallback=true; h6State = H6_SETPARA_URL2; }
      break;

    case H6_SETPARA_URL2:
      gsmCmdf("AT+HTTPPARA=url,%s%s", IPV6_HOST_RAW, IPV6_PATH);
      h6Timer = now; h6State = H6_W_URL2; httpRespOK=httpRespERROR=false;
      break;

    case H6_W_URL2:
      if (httpRespOK || now - h6Timer > TO) { httpRespOK=false; h6State = H6_SETPARA_PORT; }
      break;

    case H6_SETPARA_PORT:
      gsmCmdf("AT+HTTPPARA=port,%d", IPV6_PORT);
      h6Timer = now; h6State = H6_W_PORT; httpRespOK=httpRespERROR=false;
      break;

    case H6_W_PORT:
      if (httpRespOK || now - h6Timer > TO) { httpRespOK=false; h6State = H6_SETUP; }
      break;

    case H6_SETUP:
      gsmCmd("AT+HTTPSETUP");
      h6Timer = now; h6State = H6_W_SETUP; httpRespOK=httpRespERROR=false;
      break;

    case H6_W_SETUP:
      if (httpRespOK) { httpRespOK=false; h6State = H6_ACTION; }
      else if (httpRespERROR) {
        if (!h6TriedFallback) { h6TriedFallback=true; h6State = H6_SETPARA_URL2; }
        else { h6BackoffUntil = now + 15000UL; gsmEchoInfo("[HTTP] HTTPSETUP ERROR → backoff 15s"); h6State = H6_BACKOFF; }
      } else if (now - h6Timer > HTTPSETUP_TO) {
        gsmEchoInfo("[HTTP] HTTPSETUP waited 60s (timeout), proceeding");
        h6State = H6_ACTION;
      }
      break;

    case H6_BACKOFF:
      if (now >= h6BackoffUntil) h6State = H6_WAIT_READY;
      break;

    case H6_ACTION:
      gsmCmdf("AT+HTTPACTION=2,%d", h6Len);
      h6Timer = now; h6State = H6_W_PROMPT;
      break;

    case H6_W_PROMPT:
      if (httpRespPROMPT) { httpRespPROMPT=false; h6State = H6_SEND_BODY; }
      else if (now - h6Timer > (TO + 2000)) { h6State = H6_CLOSE; }
      break;

    case H6_SEND_BODY:
      gsmEchoCmdPrefix(); DBG_PRINTF("[HTTP BODY %d bytes] ", h6Len); DBG_PRINTLN(h6Json);
      GSM.print(h6Json);
      gsmWriteCtrl(26, "^Z (Ctrl+Z)");
      h6Timer = now; h6State = H6_W_RESULT; httpRespOK=httpRespERROR=false;
      break;

    case H6_W_RESULT:
      // NOTE: from this case onward the source was reconstructed after the
      // original file was damaged in transfer. See docs/LIMITATIONS.md.
      if (httpRespOK) {
        httpRespOK = false;
        DBG_PRINTLN(F("[HTTP] POST success"));
        h6State = H6_CLOSE;
      } else if (httpRespERROR || now - h6Timer > 30000UL) {
        httpRespERROR = false;
        DBG_PRINTLN(F("[HTTP] POST failed / timeout"));
        h6State = H6_CLOSE;
      }
      break;

    case H6_CLOSE:
      gsmCmd("AT+HTTPCLOSE");
      h6Timer = now; h6State = H6_W_CLOSE; httpRespOK=httpRespERROR=false;
      break;

    case H6_W_CLOSE:
      if (httpRespOK || httpRespERROR || now - h6Timer > TO) {
        httpRespOK = httpRespERROR = false;
        h6State = H6_IDLE;
      }
      break;
  }
}

// ---------------- SMS TX FSM ----------------
void smsTxProcess() {
  if (!modemReady) return;
  if (h6State != H6_IDLE && h6State != H6_WAIT_READY && h6State != H6_BACKOFF) return; // don't interleave with HTTP
  static unsigned long smsTimer = 0;
  unsigned long now = millis();

  switch (smsTxState) {
    case SMS_IDLE:
      if (smsQCount > 0) smsTxState = SMS_SEND_AT;
      break;
    case SMS_SEND_AT:
      gsmCmd("AT"); smsRespOK=smsRespERROR=false; smsTimer=now; smsTxState=SMS_WAIT_AT;
      break;
    case SMS_WAIT_AT:
      if (smsRespOK) { smsRespOK=false; smsTxState=SMS_SET_TEXT; }
      else if (now - smsTimer > 3000UL) smsTxState=SMS_SEND_AT;
      break;
    case SMS_SET_TEXT:
      gsmCmd("AT+CMGF=1"); smsRespOK=smsRespERROR=false; smsTimer=now; smsTxState=SMS_WAIT_TEXT;
      break;
    case SMS_WAIT_TEXT:
      if (smsRespOK || now - smsTimer > 2000UL) { smsRespOK=false; smsTxState=SMS_SEND_NUM; }
      break;
    case SMS_SEND_NUM:
      smsRespPROMPT=false;
      gsmCmdf("AT+CMGS=\"%s\"", ADMIN_NUMBER);
      smsTimer=now; smsTxState=SMS_WAIT_PROMPT;
      break;
    case SMS_WAIT_PROMPT:
      if (smsRespPROMPT) { smsRespPROMPT=false; smsTxState=SMS_SEND_BODY; }
      else if (now - smsTimer > 5000UL) { gsmWriteCtrl(27, "ESC (abort)"); smsTxState=SMS_IDLE; }
      break;
    case SMS_SEND_BODY:
      gsmEchoCmdPrefix(); DBG_PRINTLN(smsQueue[smsQTail]);
      GSM.print(smsQueue[smsQTail]);
      gsmWriteCtrl(26, "^Z (Ctrl+Z)");
      smsRespOK=smsRespERROR=false; smsTimer=now; smsTxState=SMS_WAIT_OK;
      break;
    case SMS_WAIT_OK:
      if (smsRespOK || smsRespERROR || now - smsTimer > 15000UL) {
        if (smsRespOK) DBG_PRINTLN(F("[SMS] Sent")); else DBG_PRINTLN(F("[SMS] Send failed, dropping"));
        smsRespOK=smsRespERROR=false;
        smsQTail = (smsQTail + 1) % SMS_QUEUE_SIZE; smsQCount--;
        smsTxState = SMS_IDLE;
      }
      break;
  }
}

// ---------------- SMS command handler ----------------
void sendStatus() {
  char t[12], h[12], msg[SMS_MAX_LEN];
  fmtFloat1(lastGoodTemp, t, sizeof(t));
  fmtFloat1(lastGoodHumi, h, sizeof(h));
  snprintf(msg, sizeof(msg), "STATUS\nTEMP: %s C\nHUMI: %s %%\nFAN: %s\nHEATER: %s\nALARM: %s",
           t, h,
           digitalRead(RELAY_FAN)    ? "ON" : "OFF",
           digitalRead(RELAY_HEATER) ? "ON" : "OFF",
           alarmActive ? "ACTIVE" : "NORMAL");
  smsPush(msg);
}

void handleSMS() {
  if (!smsAvailable) return;
  smsAvailable = false;

  if (strcmp(smsSenderNum, ADMIN_NUMBER) != 0) {
    DBG_PRINT(F("[SMS] Ignored sender: ")); DBG_PRINTLN(smsSenderNum);
    return;
  }

  // Trim leading whitespace; command is the first character
  const char *p = smsRxBody; while (*p == ' ' || *p == '\t') p++;
  switch (*p) {
    case '1': digitalWrite(RELAY_FAN, HIGH);    smsPush("FAN ON");      break;
    case '2': digitalWrite(RELAY_FAN, LOW);     smsPush("FAN OFF");     break;
    case '3': digitalWrite(RELAY_HEATER, HIGH); smsPush("HEATER ON");   break;
    case '4': digitalWrite(RELAY_HEATER, LOW);  smsPush("HEATER OFF");  break;
    case '5': sendStatus();                                             break;
    case '6': buzzerState=false; digitalWrite(BUZZER_PIN, LOW); smsPush("BUZZER OFF"); break;
    default:  smsPush("UNKNOWN CMD\n1 FAN ON\n2 FAN OFF\n3 HEATER ON\n4 HEATER OFF\n5 STATUS\n6 BUZZER OFF"); break;
  }
}

// ---------------- Alarm ----------------
void checkAlarm() {
  if (millis() - bootTime < BOOT_GUARD_MS) return;
  bool over = (lastGoodTemp >= TEMP_HIGH_LIMIT) || (lastGoodHumi >= HUMI_HIGH_LIMIT);

  if (over && !alarmActive) {
    alarmActive = true; buzzerState = true; digitalWrite(BUZZER_PIN, HIGH);
    char t[12], h[12], msg[SMS_MAX_LEN];
    fmtFloat1(lastGoodTemp, t, sizeof(t));
    fmtFloat1(lastGoodHumi, h, sizeof(h));
    snprintf(msg, sizeof(msg), "ALERT!\nTEMP: %s C\nHUMI: %s %%", t, h);
    smsPush(msg);
  } else if (!over && alarmActive) {
    alarmActive = false; buzzerState = false; digitalWrite(BUZZER_PIN, LOW);
    smsPush("ALARM CLEARED");
  }
}

// ---------------- Telemetry ----------------
void httpStartLogging() {
  if (millis() - lastLogTime < LOG_INTERVAL_MS) return;
  if (!modemReady || netState != NET_DONE || h6State != H6_IDLE) return;
  lastLogTime = millis();

  DateTime now = rtc.now();
  char ts[24], t[12], h[12];
  snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02d",
           now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
  fmtFloat1(lastGoodTemp, t, sizeof(t));
  fmtFloat1(lastGoodHumi, h, sizeof(h));

  char json[256];
  int len = snprintf(json, sizeof(json),
    "{\"DeviceId\":\"%s\",\"Temperature\":%s,\"Humidity\":%s,\"Timestamp\":\"%s\"}",
    DEVICE_ID, t, h, ts);
  if (len <= 0 || len >= (int)sizeof(json)) return;
  httpV6StartOnce(json, len);
}

// ---------------- Arduino entry points ----------------
void setup() {
  DBG_BEGIN(115200);
  GSM.begin(GSM_BAUD);
  Wire.begin();

  pinMode(RELAY_HEATER, OUTPUT); digitalWrite(RELAY_HEATER, LOW);
  pinMode(RELAY_FAN,    OUTPUT); digitalWrite(RELAY_FAN,    LOW);
  pinMode(BUZZER_PIN,   OUTPUT); digitalWrite(BUZZER_PIN,   LOW);

  if (!rtc.begin()) DBG_PRINTLN(F("[RTC] DS3231 not found"));
  dht.begin();

  dmd.setBrightness(255);
  dmd.selectFont(SystemFont5x7);
  dmd.begin();

  bootTime = millis();
  showTimeDateFull();
  lastScreenSwitch = lastRTCRefresh = millis();
  systemReady = true;
  DBG_PRINTLN(F("[SYS] Boot complete"));
}

void loop() {
  unsigned long now = millis();

  // Comms
  gsmRxProcess();
  modemInitProcess();
  netBringupProcess();
  handleSMS();
  smsTxProcess();
  httpV6TxProcess();

  // Sensing + alarms + telemetry
  updateDHT();
  checkAlarm();
  httpStartLogging();

  // Display rotation
  if (now - lastScreenSwitch >= SCREEN_INTERVAL_MS) {
    lastScreenSwitch = now;
    currentScreen = (currentScreen + 1) % 2;
    if (currentScreen == 0) showTimeDateFull(); else showTempHumi();
  }
  if (currentScreen == 0 && now - lastRTCRefresh >= RTC_REFRESH_MS) {
    lastRTCRefresh = now;
    updateTimeOnly();
  }
}
