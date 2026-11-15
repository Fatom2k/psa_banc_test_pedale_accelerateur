/*
  ============================================================================
   Banc de test pédale d'accélérateur PSA (capteur 1261, 2 pistes)
   Cible : LilyGO TTGO T-Display v1.1 (ESP32 + écran ST7789 135x240)
  ============================================================================
   Pédale (connecteur 4 voies noir) :
     1 = S1 (piste 1)   2 = masse   3 = S2 (piste 2)   4 = +5 V
   Loi attendue : S1 ≈ 2 x S2
     S2 : ~0,15-0,30 V pédale relâchée -> ~1,75-2,0 V pédale à fond
     S1 : ~0,3-0,6 V                   -> ~3,5-4,0 V

   Le module alimente la pédale en 5 V, mesure S1, S2 et le 5 V réel, et
   calcule l'écart  E = (S1 - 2xS2) / Vcc  en % (indépendant de la tension
   d'alim). C'est cette incohérence que le calculateur sanctionne (P0121...).

   Boutons :
     GPIO0  (bouton gauche, USB à gauche)  : écran suivant
     GPIO35 (bouton droit)                 : remise à zéro des mesures
  ============================================================================
*/
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <WiFi.h>
#include <WebServer.h>
#include "web_page.h"

// Interface web : point d'accès Wi-Fi -> http://192.168.4.1
#define WIFI_ENABLE 1
#define WIFI_SSID   "BANC-PEDALE"
#define WIFI_PASS   "psa12345"      // 8 caractères minimum

#ifdef USE_ADS1115
  #include <Wire.h>
  #include <Adafruit_ADS1X15.h>
  static Adafruit_ADS1115 ads;
  #define I2C_SDA 21
  #define I2C_SCL 22
#endif

// ----------------------------------------------------------------------------
// Brochage (entrées ADC1 uniquement)
// ----------------------------------------------------------------------------
#define PIN_S1    33   // via pont 47k / 47k
#define PIN_S2    32   // via 10k en série (pas de pont)
#define PIN_VCC   36   // via pont (47k+47k) / 47k
#define BTN_NEXT  0
#define BTN_RST   35

// Rapports des ponts : Vréelle = Vmesurée x rapport.
// Pour plus de précision, mesurez vos résistances et corrigez ici.
static float DIV_S1  = 2.0f;   // (47k + 47k) / 47k
static float DIV_S2  = 1.0f;   // pas de pont
static float DIV_VCC = 3.0f;   // (94k + 47k) / 47k

// ----------------------------------------------------------------------------
// Critères (réglages du banc, pas des valeurs constructeur)
// ----------------------------------------------------------------------------
static const float K_NOM      = 2.0f;   // S1 = K x S2
static const float WARN_PCT   = 3.0f;   // écart en % de Vcc -> orange
static const float FAULT_PCT  = 6.0f;   // écart en % de Vcc -> rouge + incident compté
static const float S1_MIN_V   = 0.15f, S1_MAX_V = 4.70f;
static const float S2_MIN_V   = 0.08f, S2_MAX_V = 2.45f;
static const float VCC_MIN_V  = 4.50f, VCC_MAX_V = 5.50f;
static const int   OVERSAMPLE = 8;      // moyennage ADC interne

#define NBINS   100     // tranches de position pour le balayage
#define HIST_W  240     // largeur de l'historique (1 colonne par image)
#define FRAME_MS 50     // 20 images/s -> 12 s d'historique

// ----------------------------------------------------------------------------
// Données partagées entre la tâche de mesure (cœur 0) et l'affichage (cœur 1)
// ----------------------------------------------------------------------------
struct Shared {
  // valeurs filtrées pour l'affichage
  float s1, s2, vcc;
  // extrêmes depuis la dernière image (pour voir les pointes brèves)
  bool  wValid;
  float wErrMin, wErrMax, wS1Min, wS1Max, wS2kMin, wS2kMax;
  // idem, depuis la dernière requête de la page web
  bool  bValid;
  float bErrMin, bErrMax, bS1Min, bS1Max, bS2kMin, bS2kMax;
  // depuis la dernière remise à zéro
  float errMaxAbs, errMaxSigned, errMaxPos;
  float s1Min, s1Max, s2Min, s2Max, vccMin, vccMax;
  uint32_t faultEvents, oorEvents, nSamples, rateHz;
  double n, sx, sy, sxx, sxy, syy;     // régression S1/Vcc = a.(S2/Vcc) + b
  float binErr[NBINS];                 // écart max |E| par tranche de position (-1 = non vue)
};

static Shared g;
static SemaphoreHandle_t mtx;
static volatile bool resetRequest = false;

static TFT_eSPI tft;
static TFT_eSprite spr(&tft);

// historique pour le graphe temporel
struct HistCol { float s1Min, s1Max, s2kMin, s2kMax, eMin, eMax; bool valid; };
static HistCol hist[HIST_W];
static int histHead = 0;

static int screen = 0;
static const int NSCREENS = 4;

// ----------------------------------------------------------------------------
static void resetWindow(Shared &s) {
  s.wValid = false;
  s.wErrMin = s.wS1Min = s.wS2kMin = 1e9f;
  s.wErrMax = s.wS1Max = s.wS2kMax = -1e9f;
}

static void resetWebWindow(Shared &s) {
  s.bValid = false;
  s.bErrMin = s.bS1Min = s.bS2kMin = 1e9f;
  s.bErrMax = s.bS1Max = s.bS2kMax = -1e9f;
}

static void resetStats(Shared &s) {
  s.errMaxAbs = 0; s.errMaxSigned = 0; s.errMaxPos = 0;
  s.s1Min = s.s2Min = s.vccMin = 1e9f;
  s.s1Max = s.s2Max = s.vccMax = -1e9f;
  s.faultEvents = s.oorEvents = s.nSamples = 0;
  s.n = s.sx = s.sy = s.sxx = s.sxy = s.syy = 0;
  for (int i = 0; i < NBINS; i++) s.binErr[i] = -1.0f;
  resetWindow(s);
  resetWebWindow(s);
}

// ----------------------------------------------------------------------------
// Acquisition
// ----------------------------------------------------------------------------
#ifdef USE_ADS1115
static void readAll(float &s1, float &s2, float &vcc) {
  s1  = ads.computeVolts(ads.readADC_SingleEnded(0)) * DIV_S1;
  s2  = ads.computeVolts(ads.readADC_SingleEnded(1)) * DIV_S2;
  vcc = ads.computeVolts(ads.readADC_SingleEnded(2)) * DIV_VCC;
}
#else
static float readPin(int pin, float div) {
  uint32_t acc = 0;
  for (int i = 0; i < OVERSAMPLE; i++) acc += analogReadMilliVolts(pin);
  return (acc / (float)OVERSAMPLE) * 0.001f * div;
}
static void readAll(float &s1, float &s2, float &vcc) {
  s1  = readPin(PIN_S1, DIV_S1);
  s2  = readPin(PIN_S2, DIV_S2);
  vcc = readPin(PIN_VCC, DIV_VCC);
}
#endif

static void samplingTask(void *) {
  bool inFault = false, inOor = false, first = true;
  uint32_t cnt = 0, t0 = millis();
  float f1 = 0, f2 = 0, fv = 0;

  for (;;) {
    float s1, s2, vcc;
    readAll(s1, s2, vcc);

    const float vref = (vcc > 3.0f) ? vcc : 5.0f;
    const float s2k  = K_NOM * s2;
    const float err  = (s1 - s2k) / vref * 100.0f;
    const float aerr = fabsf(err);
    const float x = s2 / vref, y = s1 / vref;
    const bool  oor = (s1 < S1_MIN_V) || (s1 > S1_MAX_V) || (s2 < S2_MIN_V) || (s2 > S2_MAX_V);

    if (first) { f1 = s1; f2 = s2; fv = vcc; first = false; }
    f1 += 0.15f * (s1 - f1);
    f2 += 0.15f * (s2 - f2);
    fv += 0.05f * (vcc - fv);

    xSemaphoreTake(mtx, portMAX_DELAY);
    if (resetRequest) { resetStats(g); resetRequest = false; inFault = inOor = false; }

    g.s1 = f1; g.s2 = f2; g.vcc = fv;

    // fenêtre courte (brutes, non filtrées -> on voit les pointes)
    g.wValid = true;
    if (err < g.wErrMin) g.wErrMin = err;
    if (err > g.wErrMax) g.wErrMax = err;
    if (s1  < g.wS1Min)  g.wS1Min  = s1;
    if (s1  > g.wS1Max)  g.wS1Max  = s1;
    if (s2k < g.wS2kMin) g.wS2kMin = s2k;
    if (s2k > g.wS2kMax) g.wS2kMax = s2k;
    g.bValid = true;
    if (err < g.bErrMin) g.bErrMin = err;
    if (err > g.bErrMax) g.bErrMax = err;
    if (s1  < g.bS1Min)  g.bS1Min  = s1;
    if (s1  > g.bS1Max)  g.bS1Max  = s1;
    if (s2k < g.bS2kMin) g.bS2kMin = s2k;
    if (s2k > g.bS2kMax) g.bS2kMax = s2k;

    // extrêmes
    if (s1  < g.s1Min)  g.s1Min  = s1;   if (s1  > g.s1Max)  g.s1Max  = s1;
    if (s2  < g.s2Min)  g.s2Min  = s2;   if (s2  > g.s2Max)  g.s2Max  = s2;
    if (vcc < g.vccMin) g.vccMin = vcc;  if (vcc > g.vccMax) g.vccMax = vcc;

    // hors plage (coupure / court-circuit) avec hystérésis
    if (oor && !inOor)  { g.oorEvents++; inOor = true; }
    if (!oor)           inOor = false;

    if (!oor) {
      // incohérence S1 / S2 avec hystérésis
      if (aerr > FAULT_PCT && !inFault) { g.faultEvents++; inFault = true; }
      if (aerr < WARN_PCT)              inFault = false;

      if (aerr > g.errMaxAbs) { g.errMaxAbs = aerr; g.errMaxSigned = err; g.errMaxPos = y * 100.0f; }

      int b = (int)(y * NBINS);
      if (b < 0) b = 0; if (b >= NBINS) b = NBINS - 1;
      if (aerr > g.binErr[b]) g.binErr[b] = aerr;

      g.n += 1; g.sx += x; g.sy += y; g.sxx += x * x; g.sxy += x * y; g.syy += y * y;
    }
    g.nSamples++;
    xSemaphoreGive(mtx);

    cnt++;
    uint32_t now = millis();
    if (now - t0 >= 1000) { g.rateHz = cnt; cnt = 0; t0 = now; }
    vTaskDelay(1);   // laisse respirer le chien de garde
  }
}

// ----------------------------------------------------------------------------
// Affichage
// ----------------------------------------------------------------------------
#define C_BG     TFT_BLACK
#define C_TXT    TFT_WHITE
#define C_DIM    0x7BEF
#define C_GRID   0x2104
#define C_S1     TFT_YELLOW
#define C_S2     TFT_CYAN
#define C_OK     TFT_GREEN
#define C_WARN   TFT_ORANGE
#define C_FAULT  TFT_RED

static uint16_t errColor(float aerr) {
  if (aerr > FAULT_PCT) return C_FAULT;
  if (aerr > WARN_PCT)  return C_WARN;
  return C_OK;
}

// état global d'après les valeurs filtrées
static void status(const Shared &s, const char *&txt, uint16_t &col) {
  const float vref = (s.vcc > 3.0f) ? s.vcc : 5.0f;
  const float aerr = fabsf((s.s1 - K_NOM * s.s2) / vref * 100.0f);
  if (s.vcc < VCC_MIN_V || s.vcc > VCC_MAX_V) { txt = "ALIM 5V ?";  col = C_FAULT; return; }
  if (s.s1 < S1_MIN_V && s.s2 < S2_MIN_V)     { txt = "PEDALE ?";   col = C_FAULT; return; }
  if (s.s1 < S1_MIN_V || s.s1 > S1_MAX_V)     { txt = "S1 HS";      col = C_FAULT; return; }
  if (s.s2 < S2_MIN_V || s.s2 > S2_MAX_V)     { txt = "S2 HS";      col = C_FAULT; return; }
  if (aerr > FAULT_PCT)                       { txt = "INCOHERENT"; col = C_FAULT; return; }
  if (aerr > WARN_PCT)                        { txt = "ECART";      col = C_WARN;  return; }
  txt = "OK"; col = C_OK;
}

static float pedalPos(const Shared &s) {
  // course apprise (min/max vus depuis le reset), sinon % de Vcc
  if (s.s1Max - s.s1Min > 1.0f) {
    float p = (s.s1 - s.s1Min) / (s.s1Max - s.s1Min) * 100.0f;
    return constrain(p, 0.0f, 100.0f);
  }
  return s.s1 / ((s.vcc > 3.0f) ? s.vcc : 5.0f) * 100.0f;
}

static void drawHeader(const char *title, const Shared &s) {
  const char *st; uint16_t col;
  status(s, st, col);
  spr.fillRect(0, 0, 240, 18, 0x18E3);
  spr.setTextFont(2);
  spr.setTextDatum(ML_DATUM);
  spr.setTextColor(C_TXT);
  spr.drawString(title, 4, 9);
  int w = spr.textWidth(st) + 10;
  spr.fillRoundRect(240 - w - 2, 1, w, 16, 4, col);
  spr.setTextColor(TFT_BLACK);
  spr.setTextDatum(MC_DATUM);
  spr.drawString(st, 240 - 2 - w / 2, 9);
}

static void bar(int x, int y, int w, int h, float v, float vmax, uint16_t col) {
  spr.drawRect(x, y, w, h, C_DIM);
  int fw = (int)((w - 2) * constrain(v / vmax, 0.0f, 1.0f));
  spr.fillRect(x + 1, y + 1, fw, h - 2, col);
}

static void screenLive(const Shared &s) {
  char buf[48];
  drawHeader("MESURE", s);
  const float vref = (s.vcc > 3.0f) ? s.vcc : 5.0f;
  const float err  = (s.s1 - K_NOM * s.s2) / vref * 100.0f;

  spr.setTextFont(2);
  spr.setTextDatum(TL_DATUM);

  spr.setTextColor(C_S1);
  snprintf(buf, sizeof buf, "S1   %5.3fV", s.s1);
  spr.drawString(buf, 4, 22);
  bar(116, 25, 120, 11, s.s1, 5.0f, C_S1);

  spr.setTextColor(C_S2);
  snprintf(buf, sizeof buf, "2xS2 %5.3fV", K_NOM * s.s2);
  spr.drawString(buf, 4, 40);
  bar(116, 43, 120, 11, K_NOM * s.s2, 5.0f, C_S2);

  spr.setTextColor(C_DIM);
  float ratio = (s.s2 > 0.05f) ? s.s1 / s.s2 : 0;
  snprintf(buf, sizeof buf, "S2 %4.3fV  Vcc %4.2fV  S1/S2 %4.2f", s.s2, s.vcc, ratio);
  spr.drawString(buf, 4, 58);

  // écart instantané, en gros
  spr.setTextColor(C_TXT);
  spr.drawString("Ecart", 4, 80);
  spr.setTextFont(4);
  spr.setTextColor(errColor(fabsf(err)));
  snprintf(buf, sizeof buf, "%+5.1f%%", err);
  spr.drawString(buf, 46, 76);

  spr.setTextFont(2);
  spr.setTextColor(C_TXT);
  snprintf(buf, sizeof buf, "Pos %3.0f%%", pedalPos(s));
  spr.setTextDatum(TR_DATUM);
  spr.drawString(buf, 236, 80);

  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(errColor(s.errMaxAbs));
  snprintf(buf, sizeof buf, "max %+4.1f%% a %2.0f%%", s.errMaxSigned, s.errMaxPos);
  spr.drawString(buf, 4, 104);
  spr.setTextColor(s.faultEvents ? C_FAULT : C_DIM);
  snprintf(buf, sizeof buf, "incid %lu", (unsigned long)s.faultEvents);
  spr.setTextDatum(TR_DATUM);
  spr.drawString(buf, 236, 104);
  spr.setTextColor(s.oorEvents ? C_FAULT : C_DIM);
  snprintf(buf, sizeof buf, "hors plage %lu", (unsigned long)s.oorEvents);
  spr.drawString(buf, 236, 119);
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_DIM);
  snprintf(buf, sizeof buf, "%lu Hz", (unsigned long)s.rateHz);
  spr.drawString(buf, 4, 119);
}

static void screenTime(const Shared &s) {
  drawHeader("TEMPS 12s", s);
  // panneau tensions : y 20..82 (0..5 V)
  const int vT = 20, vB = 82;
  // panneau écart : y 86..134 (±10 %)
  const int eT = 86, eB = 134, eMid = (eT + eB) / 2;
  const float eScale = (eB - eT) / 2 / 10.0f;   // px par %

  for (int v = 1; v <= 4; v++) {
    int yy = vB - (int)((vB - vT) * v / 5.0f);
    spr.drawFastHLine(0, yy, 240, C_GRID);
  }
  spr.drawFastHLine(0, eMid, 240, C_DIM);
  for (int sgn = -1; sgn <= 1; sgn += 2) {
    spr.drawFastHLine(0, eMid - (int)(sgn * WARN_PCT * eScale), 240, 0x4200);
    spr.drawFastHLine(0, eMid - (int)(sgn * FAULT_PCT * eScale), 240, 0x5000);
  }

  auto vy = [&](float v) { return vB - (int)((vB - vT) * constrain(v / 5.0f, 0.0f, 1.0f)); };
  auto ey = [&](float e) { return eMid - (int)(constrain(e, -10.0f, 10.0f) * eScale); };

  for (int i = 0; i < HIST_W; i++) {
    const HistCol &h = hist[(histHead + i) % HIST_W];
    if (!h.valid) continue;
    spr.drawFastVLine(i, vy(h.s2kMax), vy(h.s2kMin) - vy(h.s2kMax) + 1, C_S2);
    spr.drawFastVLine(i, vy(h.s1Max),  vy(h.s1Min)  - vy(h.s1Max)  + 1, C_S1);
    float am = max(fabsf(h.eMin), fabsf(h.eMax));
    spr.drawFastVLine(i, ey(h.eMax), ey(h.eMin) - ey(h.eMax) + 1, errColor(am));
  }
  spr.setTextFont(1);
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_S1);  spr.drawString("S1", 2, 22);
  spr.setTextColor(C_S2);  spr.drawString("2xS2", 16, 22);
  spr.setTextColor(C_DIM); spr.drawString("ecart +-10%", 2, eT);
}

static void screenSweep(const Shared &s) {
  drawHeader("BALAYAGE", s);
  // x = S1 en % de Vcc (0..100 %), y = écart max |E| (0..10 %)
  const int top = 22, bot = 124;
  const float yScale = (bot - top) / 10.0f;
  int seen = 0;
  spr.drawFastHLine(0, bot - (int)(WARN_PCT * yScale), 240, 0x4200);
  spr.drawFastHLine(0, bot - (int)(FAULT_PCT * yScale), 240, 0x5000);
  spr.drawFastHLine(0, bot, 240, C_DIM);
  for (int b = 0; b < NBINS; b++) {
    float e = s.binErr[b];
    if (e < 0) continue;
    seen++;
    int x0 = b * 240 / NBINS, x1 = (b + 1) * 240 / NBINS;
    int hgt = max(1, (int)(min(e, 10.0f) * yScale));
    spr.fillRect(x0, bot - hgt, x1 - x0, hgt, errColor(e));
  }
  // curseur position actuelle
  float vref = (s.vcc > 3.0f) ? s.vcc : 5.0f;
  int cx = (int)constrain(s.s1 / vref * 240.0f, 0.0f, 239.0f);
  spr.drawFastVLine(cx, top, bot - top, TFT_WHITE);

  char buf[48];
  spr.setTextFont(1);
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_DIM);
  snprintf(buf, sizeof buf, "ecart max par position (0-10%%)  vu %d/100", seen);
  spr.drawString(buf, 2, 22);
  spr.drawString("S1 0%Vcc", 2, 127);
  spr.setTextDatum(TR_DATUM);
  spr.drawString("100%", 238, 127);
}

static bool regression(const Shared &s, double &a, double &b, double &r2) {
  const double den = s.n * s.sxx - s.sx * s.sx;
  if (s.n <= 50 || den <= 1e-6 * s.n * s.n) return false;
  a = (s.n * s.sxy - s.sx * s.sy) / den;
  b = (s.sy - a * s.sx) / s.n;
  const double dy = s.n * s.syy - s.sy * s.sy;
  r2 = (dy > 0) ? pow(s.n * s.sxy - s.sx * s.sy, 2) / (den * dy) : 0;
  return true;
}

static void screenStats(const Shared &s) {
  drawHeader("STATISTIQUES", s);
  char buf[64];
  spr.setTextFont(2);
  spr.setTextDatum(TL_DATUM);
  int y = 21;
  auto line = [&](uint16_t c) { spr.setTextColor(c); spr.drawString(buf, 4, y); y += 16; };

  double a, b, r2;
  if (regression(s, a, b, r2)) {
    snprintf(buf, sizeof buf, "Pente S1/S2 %.3f (nominal %.1f)", a, K_NOM);
    line(fabs(a - K_NOM) > 0.1 ? C_WARN : C_OK);
    snprintf(buf, sizeof buf, "Decalage %+.3fV   R2 %.5f", b * 5.0, r2);
    line(C_TXT);
  } else {
    snprintf(buf, sizeof buf, "Pente : enfoncer la pedale..."); line(C_DIM);
    snprintf(buf, sizeof buf, " "); line(C_DIM);
  }
  snprintf(buf, sizeof buf, "S1  %.3f .. %.3f V", s.s1Min, s.s1Max);  line(C_S1);
  snprintf(buf, sizeof buf, "S2  %.3f .. %.3f V", s.s2Min, s.s2Max);  line(C_S2);
  snprintf(buf, sizeof buf, "Vcc %.3f .. %.3f V", s.vccMin, s.vccMax); line(C_TXT);
#ifdef USE_ADS1115
  snprintf(buf, sizeof buf, "ADS1115  %lu ech.  %lu Hz", (unsigned long)s.nSamples, (unsigned long)s.rateHz);
#else
  snprintf(buf, sizeof buf, "ADC ESP32  %lu ech.  %lu Hz", (unsigned long)s.nSamples, (unsigned long)s.rateHz);
#endif
  line(C_DIM);
  if (WIFI_ENABLE) { snprintf(buf, sizeof buf, "WiFi %s  192.168.4.1", WIFI_SSID); line(C_DIM); }
}

// ----------------------------------------------------------------------------
// Boutons (front descendant, anti-rebond simple)
// ----------------------------------------------------------------------------
static bool pressed(int pin, bool &last, uint32_t &tLast) {
  bool now = digitalRead(pin) == LOW;
  bool edge = now && !last && (millis() - tLast > 150);
  if (now != last) tLast = millis();
  last = now;
  return edge;
}


// ----------------------------------------------------------------------------
// Serveur web
// ----------------------------------------------------------------------------
static WebServer server(80);

static void handleLive() {
  Shared s;
  xSemaphoreTake(mtx, portMAX_DELAY);
  s = g;
  resetWebWindow(g);
  xSemaphoreGive(mtx);

  const char *st; uint16_t col;
  status(s, st, col);
  const float vref = (s.vcc > 3.0f) ? s.vcc : 5.0f;
  const float err = (s.s1 - K_NOM * s.s2) / vref * 100.0f;
  double a = 0, b = 0, r2 = 0;
  const bool reg = regression(s, a, b, r2);
  if (!s.bValid) { s.bErrMin = s.bErrMax = err; s.bS1Min = s.bS1Max = s.s1; s.bS2kMin = s.bS2kMax = K_NOM * s.s2; }

  static char j[1024];
  snprintf(j, sizeof j,
    "{\"t\":%lu,\"st\":\"%s\",\"s1\":%.4f,\"s2\":%.4f,\"vcc\":%.4f,\"err\":%.3f,\"pos\":%.1f,"
    "\"w\":{\"emin\":%.3f,\"emax\":%.3f,\"s1min\":%.4f,\"s1max\":%.4f,\"s2kmin\":%.4f,\"s2kmax\":%.4f},"
    "\"errMax\":%.3f,\"errMaxPos\":%.1f,\"faults\":%lu,\"oor\":%lu,\"n\":%lu,\"hz\":%lu,"
    "\"s1r\":[%.4f,%.4f],\"s2r\":[%.4f,%.4f],\"vccr\":[%.4f,%.4f],"
    "\"reg\":%s,\"a\":%.4f,\"b\":%.4f,\"r2\":%.6f,"
    "\"th\":{\"k\":%.2f,\"warn\":%.1f,\"fault\":%.1f},\"adc\":\"%s\"}",
    (unsigned long)millis(), st, s.s1, s.s2, s.vcc, err, pedalPos(s),
    s.bErrMin, s.bErrMax, s.bS1Min, s.bS1Max, s.bS2kMin, s.bS2kMax,
    s.errMaxSigned, s.errMaxPos, (unsigned long)s.faultEvents, (unsigned long)s.oorEvents,
    (unsigned long)s.nSamples, (unsigned long)s.rateHz,
    s.s1Min < 1e8f ? s.s1Min : 0, s.s1Max > -1e8f ? s.s1Max : 0,
    s.s2Min < 1e8f ? s.s2Min : 0, s.s2Max > -1e8f ? s.s2Max : 0,
    s.vccMin < 1e8f ? s.vccMin : 0, s.vccMax > -1e8f ? s.vccMax : 0,
    reg ? "true" : "false", a, b * 5.0, r2,
    K_NOM, WARN_PCT, FAULT_PCT,
#ifdef USE_ADS1115
    "ADS1115"
#else
    "ESP32"
#endif
  );
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

static void handleSweep() {
  float bins[NBINS];
  xSemaphoreTake(mtx, portMAX_DELAY);
  memcpy(bins, g.binErr, sizeof bins);
  xSemaphoreGive(mtx);
  String s = "[";
  char b[16];
  for (int i = 0; i < NBINS; i++) {
    if (bins[i] < 0) snprintf(b, sizeof b, "%snull", i ? "," : "");
    else snprintf(b, sizeof b, "%s%.2f", i ? "," : "", bins[i]);
    s += b;
  }
  s += "]";
  server.send(200, "application/json", s);
}

static void startWeb() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, WIFI_PASS);
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", WEB_PAGE); });
  server.on("/api/live", HTTP_GET, handleLive);
  server.on("/api/sweep", HTTP_GET, handleSweep);
  server.on("/api/reset", HTTP_POST, []() {
    resetRequest = true;
    for (auto &h : hist) h.valid = false;
    server.send(200, "text/plain", "ok");
  });
  server.begin();
}

// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_RST, INPUT);          // GPIO35 : pull-up externe sur la carte

  tft.init();
  tft.setRotation(1);               // paysage 240 x 135
  tft.fillScreen(C_BG);
  spr.setColorDepth(16);
  if (!spr.createSprite(240, 135)) { spr.setColorDepth(8); spr.createSprite(240, 135); }

#ifdef USE_ADS1115
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  if (!ads.begin(0x48)) {
    tft.setTextColor(TFT_RED); tft.drawString("ADS1115 absent !", 10, 60, 4);
    while (true) delay(1000);
  }
  ads.setGain(GAIN_ONE);                 // ±4,096 V
  ads.setDataRate(RATE_ADS1115_860SPS);
#else
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_S1, ADC_11db);
  analogSetPinAttenuation(PIN_S2, ADC_11db);
  analogSetPinAttenuation(PIN_VCC, ADC_11db);
#endif

  mtx = xSemaphoreCreateMutex();
  resetStats(g);
  for (auto &h : hist) h.valid = false;
  xTaskCreatePinnedToCore(samplingTask, "mesure", 4096, nullptr, 2, nullptr, 0);

  if (WIFI_ENABLE) startWeb();
  Serial.println("t_ms,S1_V,S2_V,Vcc_V,ecart_pct,ecart_min,ecart_max");
}

void loop() {
  if (WIFI_ENABLE) server.handleClient();
  static uint32_t tFrame = 0;
  static bool lastN = false, lastR = false;
  static uint32_t tN = 0, tR = 0;

  if (pressed(BTN_NEXT, lastN, tN)) screen = (screen + 1) % NSCREENS;
  if (pressed(BTN_RST, lastR, tR)) {
    resetRequest = true;
    for (auto &h : hist) h.valid = false;
  }

  uint32_t now = millis();
  if (now - tFrame < FRAME_MS) { delay(2); return; }
  tFrame = now;

  // instantané + vidage de la fenêtre courte
  static Shared snap;
  xSemaphoreTake(mtx, portMAX_DELAY);
  snap = g;
  resetWindow(g);
  xSemaphoreGive(mtx);

  if (snap.wValid) {
    HistCol &h = hist[histHead];
    h.s1Min = snap.wS1Min;   h.s1Max = snap.wS1Max;
    h.s2kMin = snap.wS2kMin; h.s2kMax = snap.wS2kMax;
    h.eMin = snap.wErrMin;   h.eMax = snap.wErrMax;
    h.valid = true;
    histHead = (histHead + 1) % HIST_W;

    // journal CSV (Moniteur / Traceur série, ou capture sur PC)
    const float vref = (snap.vcc > 3.0f) ? snap.vcc : 5.0f;
    Serial.printf("%lu,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f\n", (unsigned long)now,
                  snap.s1, snap.s2, snap.vcc,
                  (snap.s1 - K_NOM * snap.s2) / vref * 100.0f, snap.wErrMin, snap.wErrMax);
  }

  spr.fillSprite(C_BG);
  switch (screen) {
    case 0: screenLive(snap);  break;
    case 1: screenTime(snap);  break;
    case 2: screenSweep(snap); break;
    case 3: screenStats(snap); break;
  }
  spr.pushSprite(0, 0);
}
