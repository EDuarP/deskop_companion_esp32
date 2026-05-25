/* ════════════════════════════════════════════════════════════════
   TOOLS DESKTOP — Rev. B
   ─────────────────────────────────────────────────────────────────
   Reloj digital + clima + info en GC9A01 240x240.
   Todo funciona standalone: nada depende del PC.
   Botones touch: MEJILLA (pin 5) -> izquierda, CABEZA (pin 4) -> derecha.

   PANTALLAS:
     1) CLOCK     - digital con arco arcoiris y Saturno
     2) WEATHER   - temperatura + icono animado + min/max
     3) INFO      - WiFi/IP/uptime/heap

   Credenciales y locacion en secrets.h (gitignoreado).
   ════════════════════════════════════════════════════════════════ */

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <math.h>

// ═══════════════ 1) CONFIG ═════════════════════════════════════
// Credenciales y locacion viven en secrets.h (gitignoreado).
#include "secrets.h"
#define WEATHER_PERIOD   (10UL * 60UL * 1000UL)

// ═══════════════ 2) PINES ═════════════════════════════════════
#define PIN_DISP_SCK     8
#define PIN_DISP_MOSI    10
#define PIN_DISP_DC      7
#define PIN_DISP_CS      9
#define PIN_DISP_RST     6
#define PIN_BTN_LEFT     5   // mejilla
#define PIN_BTN_RIGHT    4   // cabeza

// ═══════════════ 3) COLORES ════════════════════════════════════
#define BG          0x0000
#define FG          0xFFFF
#define DIM         0x630C
#define DIM2        0x4208
#define ACCENT      0x05FF
#define ACCENT2     0xFD20
#define WARN        0xF800
#define OK          0x07E0
#define HOUR_HAND   0xFFFF
#define MIN_HAND    0xFFFF
#define SEC_HAND    0x05FF

// ═══════════════ 4) DRIVER ═════════════════════════════════════
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel;
  lgfx::Bus_SPI      _bus;
public:
  LGFX() {
    { auto cfg = _bus.config();
      cfg.spi_host=SPI2_HOST; cfg.spi_mode=0;
      cfg.freq_write=40000000; cfg.freq_read=16000000;
      cfg.pin_sclk=PIN_DISP_SCK; cfg.pin_mosi=PIN_DISP_MOSI; cfg.pin_miso=-1;
      cfg.pin_dc=PIN_DISP_DC; cfg.dma_channel=SPI_DMA_CH_AUTO;
      _bus.config(cfg); _panel.setBus(&_bus);
    }
    { auto cfg = _panel.config();
      cfg.pin_cs=PIN_DISP_CS; cfg.pin_rst=PIN_DISP_RST;
      cfg.panel_width=240; cfg.panel_height=240; cfg.invert=true;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};

LGFX        tft;
LGFX_Sprite canvas(&tft);

// ═══════════════ 5) ESTADO GLOBAL ══════════════════════════════
const int CX = 120, CY = 120;

enum Page { PG_CLOCK, PG_WEATHER, PG_INFO, PG_COUNT };

Page     currentPage = PG_CLOCK;
uint32_t pageEnterMs = 0;

bool     wifiOnline  = false;
bool     timeSynced  = false;

// ═══════════════ 6) BOTONES ════════════════════════════════════
bool prevRight=false, prevLeft=false;
uint32_t lastBtnMs = 0;
const uint32_t BTN_DEBOUNCE = 150;
int  pressFlashUntil = 0;

void changePage(int dir) {
  int n = (int)currentPage + dir;
  if (n < 0) n = PG_COUNT - 1;
  if (n >= PG_COUNT) n = 0;
  currentPage = (Page)n;
  pageEnterMs = millis();
  pressFlashUntil = millis() + 250;
}

void handleButtons() {
  uint32_t now = millis();
  bool r = digitalRead(PIN_BTN_RIGHT) == HIGH;
  bool l = digitalRead(PIN_BTN_LEFT)  == HIGH;
  if (now - lastBtnMs > BTN_DEBOUNCE) {
    if (r && !prevRight) { changePage(+1); lastBtnMs = now; }
    if (l && !prevLeft)  { changePage(-1); lastBtnMs = now; }
  }
  prevRight = r; prevLeft = l;
}

// ═══════════════ 7) WIFI + NTP ═════════════════════════════════
uint32_t wifiRetryAt = 0;

void wifiInit() {
  if (String(WIFI_SSID) == "YOUR_WIFI") return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(100);
  wifiOnline = (WiFi.status() == WL_CONNECTED);
  if (wifiOnline) {
    configTime(TZ_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
    struct tm ti;
    if (getLocalTime(&ti, 5000)) timeSynced = true;
  }
}

void wifiKeepAlive() {
  if (String(WIFI_SSID) == "YOUR_WIFI") return;
  if (WiFi.status() == WL_CONNECTED) { wifiOnline = true; return; }
  wifiOnline = false;
  if (millis() < wifiRetryAt) return;
  WiFi.reconnect();
  wifiRetryAt = millis() + 15000;
}

// ═══════════════ 8) CLIMA (OpenWeather) ════════════════════════
struct WeatherData {
  float    tempC      = NAN;
  float    feelsC     = NAN;
  float    tempMinC   = NAN;
  float    tempMaxC   = NAN;
  int      humidity   = -1;
  float    windMs     = NAN;
  String   cond       = "";
  String   desc       = "";
  uint32_t lastUpdate = 0;
};
WeatherData wx;
int      wxLastHttpCode = 0;
uint32_t wxRetryAt      = 0;

static float jsonNum(const String& j, const String& key) {
  String pat = "\"" + key + "\":";
  int idx = j.indexOf(pat);
  if (idx < 0) return NAN;
  idx += pat.length();
  int end = idx;
  while (end < (int)j.length() && (isDigit(j[end]) || j[end]=='.' || j[end]=='-')) end++;
  if (end == idx) return NAN;
  return j.substring(idx, end).toFloat();
}
static String jsonStr(const String& j, const String& key) {
  String pat = "\"" + key + "\":\"";
  int idx = j.indexOf(pat);
  if (idx < 0) return "";
  idx += pat.length();
  int end = j.indexOf("\"", idx);
  if (end < 0) return "";
  return j.substring(idx, end);
}

void fetchWeather() {
  if (!wifiOnline) return;
  if (String(OWM_API_KEY) == "YOUR_OPENWEATHER_KEY") return;
  uint32_t now = millis();
  if (wx.lastUpdate != 0 && now - wx.lastUpdate < WEATHER_PERIOD) return;
  if (now < wxRetryAt) return;

  HTTPClient http;
  String url = String("http://api.openweathermap.org/data/2.5/weather?q=")
             + OWM_CITY + "&appid=" + OWM_API_KEY + "&units=metric&lang=es";
  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();
  wxLastHttpCode = code;
  Serial.printf("[wx] GET -> %d\n", code);
  if (code == 200) {
    String body = http.getString();
    wx.tempC    = jsonNum(body, "temp");
    wx.feelsC   = jsonNum(body, "feels_like");
    wx.tempMinC = jsonNum(body, "temp_min");
    wx.tempMaxC = jsonNum(body, "temp_max");
    wx.humidity = (int)jsonNum(body, "humidity");
    wx.windMs   = jsonNum(body, "speed");
    wx.cond     = jsonStr(body, "main");
    wx.desc     = jsonStr(body, "description");
    wx.lastUpdate = now;
    Serial.printf("[wx] %.1fC %s\n", wx.tempC, wx.cond.c_str());
  } else {
    // Backoff: 30s en error de red, 5min en error HTTP (key invalida, etc.)
    wxRetryAt = now + (code < 0 ? 30000 : 300000);
  }
  http.end();
}

// ═══════════════ 9) HELPERS GEOMETRIA + COLOR ══════════════════
inline void polar(int cx, int cy, float r, float angDeg, int& x, int& y) {
  float a = (angDeg - 90.0f) * DEG_TO_RAD;
  x = cx + (int)(r * cosf(a));
  y = cy + (int)(r * sinf(a));
}

void thickRadialLine(int cx, int cy, float r0, float r1, float angDeg, int thickness, uint16_t color) {
  int x0, y0, x1, y1;
  polar(cx, cy, r0, angDeg, x0, y0);
  polar(cx, cy, r1, angDeg, x1, y1);
  canvas.drawWideLine(x0, y0, x1, y1, thickness, color);
}

uint16_t lerpColor565(uint16_t c1, uint16_t c2, float t) {
  int r1 = (c1 >> 11) & 0x1F, g1 = (c1 >> 5) & 0x3F, b1 = c1 & 0x1F;
  int r2 = (c2 >> 11) & 0x1F, g2 = (c2 >> 5) & 0x3F, b2 = c2 & 0x1F;
  int r = r1 + (int)((r2 - r1) * t);
  int g = g1 + (int)((g2 - g1) * t);
  int b = b1 + (int)((b2 - b1) * t);
  return ((r & 0x1F) << 11) | ((g & 0x3F) << 5) | (b & 0x1F);
}

// Arco arcoiris con paradas de color interpoladas a lo largo del span.
// Resolucion 0.5° para sin gaps entre segmentos.
void drawGradientArc(int cx, int cy, int rOuter, int rInner,
                     float startAng, float spanDeg,
                     const uint16_t* colors, int nColors) {
  int steps = (int)(fabsf(spanDeg) * 2);
  for (int i = 0; i <= steps; i++) {
    float frac = (float)i / steps;
    float segPos = frac * (nColors - 1);
    int seg = (int)segPos;
    if (seg >= nColors - 1) seg = nColors - 2;
    float segFrac = segPos - seg;
    uint16_t col = lerpColor565(colors[seg], colors[seg + 1], segFrac);
    float a = startAng + frac * spanDeg;
    int x0, y0, x1, y1;
    polar(cx, cy, rInner, a, x0, y0);
    polar(cx, cy, rOuter, a, x1, y1);
    canvas.drawWideLine(x0, y0, x1, y1, 3, col);
  }
}

// Saturno: cuerpo dorado + anillo horizontal con perspectiva (atras/adelante).
void drawSaturn(int cx, int cy, int bodyR) {
  const uint16_t body = 0xFEA0;   // amarillo dorado
  const uint16_t ring = 0xD69A;   // beige claro
  int rx = bodyR + 7;
  int ry = (bodyR / 2) + 1;

  // Anillo posterior (atras del planeta) - mitad superior de la elipse
  for (int a = 180; a <= 360; a += 4) {
    float rad = a * DEG_TO_RAD;
    int x = cx + (int)(rx * cosf(rad));
    int y = cy + (int)(ry * sinf(rad));
    canvas.fillCircle(x, y, 1, ring);
  }
  // Cuerpo
  canvas.fillCircle(cx, cy, bodyR, body);
  // Anillo frontal (cruza por delante)
  for (int a = 0; a <= 180; a += 4) {
    float rad = a * DEG_TO_RAD;
    int x = cx + (int)(rx * cosf(rad));
    int y = cy + (int)(ry * sinf(rad));
    canvas.fillCircle(x, y, 1, ring);
  }
}

// ═══════════════ 11) CLOCK PAGE (DIGITAL + RAINBOW) ════════════
//
// Layout:
//   - Arco arcoiris superior (~270°, gap abajo)
//   - HH:MM grande con colon parpadeante
//   - "DIA DD-MM-YYYY" debajo
//   - Saturno chico al fondo
//
void drawClockPage(uint32_t t) {
  struct tm ti;
  bool gotTime = timeSynced && getLocalTime(&ti, 0);

  int hh = 0, mm = 0;
  int dow = -1, dd = 0, mo = 0, yr = 0;
  if (gotTime) {
    hh = ti.tm_hour;
    mm = ti.tm_min;
    dow = ti.tm_wday;
    dd  = ti.tm_mday;
    mo  = ti.tm_mon + 1;
    yr  = ti.tm_year + 1900;
  } else {
    uint32_t s = t / 1000;
    hh = (s / 3600) % 24;
    mm = (s / 60)   % 60;
  }

  // Decoracion astronomica: orbita punteada + estrellas + luna creciente
  // Orbita exterior punteada
  for (int a = 0; a < 360; a += 9) {
    int x, y;
    polar(CX, CY, 117, a, x, y);
    canvas.fillCircle(x, y, 1, DIM2);
  }
  // Estrellas a posiciones fijas (angulo, radio, brillo)
  static const int starsAng[] = { 22,  48,  72,  108, 152, 198, 248, 292, 332 };
  static const int starsR[]   = {108, 113, 105, 110, 112, 107, 109, 113, 106 };
  static const int starsB[]   = {  2,   1,   2,   1,   2,   1,   2,   1,   2 };
  for (int i = 0; i < 9; i++) {
    int x, y;
    polar(CX, CY, starsR[i], starsAng[i], x, y);
    uint16_t col = starsB[i] == 2 ? FG : DIM;
    canvas.fillCircle(x, y, starsB[i], col);
    if (starsB[i] == 2) {  // brillo cruz
      canvas.drawPixel(x - 2, y, DIM);
      canvas.drawPixel(x + 2, y, DIM);
      canvas.drawPixel(x, y - 2, DIM);
      canvas.drawPixel(x, y + 2, DIM);
    }
  }
  // Luna creciente arriba (en el "norte" del reloj)
  int mx = CX, my = CY - 95;
  canvas.fillCircle(mx, my, 8, FG);
  canvas.fillCircle(mx + 4, my - 1, 8, BG);

  // Label TIME pixelado encima
  canvas.setFont(&fonts::Font0);
  canvas.setTextSize(1);
  canvas.setTextColor(DIM);
  canvas.setTextDatum(middle_center);
  canvas.drawString("TIME", CX, CY - 42);

  // HH:MM 8-bit (Font0 x5 = ~40px alto, 30px ancho por char)
  canvas.setTextSize(5);
  canvas.setTextColor(FG);
  bool colon = (t % 1000) < 500;
  char hm[8];
  snprintf(hm, sizeof(hm), "%02d%c%02d", hh, colon ? ':' : ' ', mm);
  canvas.drawString(hm, CX, CY - 12);
  canvas.setTextSize(1);

  // Label DATE pixelado
  canvas.setTextColor(DIM);
  canvas.drawString("DATE", CX, CY + 16);

  // Fecha pixel pequena, color dorado (Saturno)
  const uint16_t SAT_GOLD = 0xFEA0;
  canvas.setTextSize(1);
  canvas.setTextColor(SAT_GOLD);
  if (gotTime) {
    const char* dias[] = {"DOM","LUN","MAR","MIE","JUE","VIE","SAB"};
    char buf[24];
    snprintf(buf, sizeof(buf), "%s %02d-%02d-%d", dias[dow], dd, mo, yr);
    canvas.drawString(buf, CX, CY + 30);
  } else {
    canvas.setTextColor(WARN);
    canvas.drawString("SIN NTP", CX, CY + 30);
  }

  // Temp pixel
  if (!isnan(wx.tempC)) {
    canvas.setTextColor(ACCENT);
    char buf[12]; snprintf(buf, sizeof(buf), "%.0fC", wx.tempC);
    canvas.drawString(buf, CX, CY + 50);
  }

  // Saturno al fondo
  drawSaturn(CX, CY + 76, 8);
}

// ═══════════════ 12) WEATHER PAGE ══════════════════════════════
// ── Sprite engine pixel-art 8-bit con paletas multi-color ──
// Cada char en el sprite es un indice de paleta (0-9, A-F). '.' = transparente.
inline void px(int x, int y, int n, uint16_t c) { canvas.fillRect(x, y, n, n, c); }

void drawPixelSprite(int cx, int cy, int blk,
                     const char* const* lines, int rows, int cols,
                     const uint16_t* palette) {
  int x0 = cx - (cols * blk) / 2;
  int y0 = cy - (rows * blk) / 2;
  for (int y = 0; y < rows; y++) {
    for (int x = 0; x < cols; x++) {
      char c = lines[y][x];
      if (c == '.') continue;
      int idx;
      if      (c >= '0' && c <= '9') idx = c - '0';
      else if (c >= 'A' && c <= 'F') idx = 10 + c - 'A';
      else continue;
      canvas.fillRect(x0 + x * blk, y0 + y * blk, blk, blk, palette[idx]);
    }
  }
}

// ── Sprites ──
// Paleta nube:  0=cuerpo gris claro, 1=highlight blanco, 2=shadow medio, 3=drop oscuro
static const uint16_t CLOUD_PAL[4] = { 0xC618, 0xFFFF, 0x8410, 0x4208 };
// 16 col x 8 fila, bloque 4 = 64x32 px
static const char* CLOUD16[] = {
  "....11111.......",
  "..1100000011....",
  ".100000000001...",
  "100000000000001.",
  "1000000000000001",
  ".0000000000000..",
  ".2222222222222..",
  "..3333333333....",
};

// Paleta sol:  0=cuerpo amarillo, 1=highlight blanco-amarillo, 2=shadow naranja
static const uint16_t SUN_PAL[3] = { 0xFEA0, 0xFFE5, 0xFB60 };
// 11 col x 11 fila, bloque 4 = 44x44 px
static const char* SUN_SPR[] = {
  "....111....",
  "..1100022..",
  ".110000222.",
  ".100000022.",
  "10000000022",
  "10000000022",
  "10000000022",
  ".100000022.",
  ".000000022.",
  "..0000022..",
  "....222....",
};

// Paleta lluvia: 0=cuerpo azul claro, 1=highlight blanco, 2=shadow azul oscuro
static const uint16_t DROP_PAL[3] = { 0x05FF, 0xFFFF, 0x0017 };
// 3 col x 4 fila, bloque 3 = 9x12 px (gota individual)
static const char* DROP_SPR[] = {
  ".1.",
  "10.",
  "002",
  ".2.",
};

// Paleta nieve: 0=blanco, 1=cyan claro
static const uint16_t SNOW_PAL[2] = { 0xFFFF, 0xD7FF };
// 3 col x 3 fila, bloque 3 = 9x9 px (copo +)
static const char* SNOW_SPR[] = {
  ".0.",
  "010",
  ".0.",
};

// Paleta rayo: 0=amarillo, 1=highlight, 2=shadow naranja
static const uint16_t BOLT_PAL[3] = { 0xFFE0, 0xFFFF, 0xFB60 };
// 5 col x 7 fila, bloque 4 = 20x28 px
static const char* BOLT_SPR[] = {
  "..11.",
  ".110.",
  "11000",
  ".0000",
  "..120",
  "...12",
  "....2",
};

void drawWeatherIcon(int cx, int cy, const String& cond, uint32_t t) {
  if (cond == "Clear") {
    drawPixelSprite(cx, cy, 4, SUN_SPR, 11, 11, SUN_PAL);
    // Rayos extra (bloques 3x3)
    const uint16_t r = 0xFEA0;
    px(cx - 2, cy - 28, 4, r);
    px(cx - 2, cy + 24, 4, r);
    px(cx - 28, cy - 2, 4, r);
    px(cx + 24, cy - 2, 4, r);
  } else if (cond == "Clouds" || cond == "Mist" || cond == "Fog" || cond == "Haze") {
    int drift = (int)(sinf(t * 0.001f) * 4.0f);
    drawPixelSprite(cx + drift, cy, 4, CLOUD16, 8, 16, CLOUD_PAL);
  } else if (cond == "Rain" || cond == "Drizzle") {
    drawPixelSprite(cx, cy - 6, 4, CLOUD16, 8, 16, CLOUD_PAL);
    for (int i = 0; i < 4; i++) {
      int x = cx - 16 + i * 11;
      int yOff = ((int)(t / 70) + i * 5) % 22;
      drawPixelSprite(x, cy + 22 + yOff, 3, DROP_SPR, 4, 3, DROP_PAL);
    }
  } else if (cond == "Snow") {
    drawPixelSprite(cx, cy - 6, 4, CLOUD16, 8, 16, CLOUD_PAL);
    for (int i = 0; i < 4; i++) {
      int x = cx - 18 + i * 12;
      int yOff = ((int)(t / 80) + i * 6) % 22;
      drawPixelSprite(x, cy + 22 + yOff, 3, SNOW_SPR, 3, 3, SNOW_PAL);
    }
  } else if (cond == "Thunderstorm") {
    // Cloud en gris oscuro
    static const uint16_t STORM_PAL[4] = { 0x8410, 0xC618, 0x4208, 0x2104 };
    drawPixelSprite(cx, cy - 8, 4, CLOUD16, 8, 16, STORM_PAL);
    drawPixelSprite(cx, cy + 22, 4, BOLT_SPR, 7, 5, BOLT_PAL);
  } else {
    canvas.drawRect(cx - 14, cy - 14, 28, 28, DIM);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(DIM);
    canvas.setTextDatum(middle_center);
    canvas.setTextSize(2);
    canvas.drawString("?", cx, cy);
  }
}

void drawWeatherPage(uint32_t t) {
  canvas.setFont(&fonts::Font0);
  canvas.setTextSize(1);
  // Anillo decorativo exterior con 12 tics
  for (int i = 0; i < 12; i++) {
    float a = i * 30.0f;
    int x0, y0, x1, y1;
    polar(CX, CY, 112, a, x0, y0);
    polar(CX, CY, 118, a, x1, y1);
    canvas.drawWideLine(x0, y0, x1, y1, 1, DIM2);
  }

  if (isnan(wx.tempC)) {
    canvas.setTextDatum(middle_center);
    canvas.setTextSize(1);
    if (!wifiOnline) {
      canvas.setTextColor(WARN);
      canvas.drawString("SIN WIFI", CX, CY - 12);
    } else if (wxLastHttpCode == 0) {
      canvas.setTextColor(DIM);
      canvas.drawString("Cargando clima...", CX, CY - 12);
    } else if (wxLastHttpCode == 401) {
      canvas.setTextColor(WARN);
      canvas.drawString("API KEY INVALIDA", CX, CY - 12);
    } else if (wxLastHttpCode == 404) {
      canvas.setTextColor(WARN);
      canvas.drawString("CIUDAD NO ENCONTRADA", CX, CY - 12);
    } else {
      canvas.setTextColor(WARN);
      char eb[32]; snprintf(eb, sizeof(eb), "HTTP ERROR %d", wxLastHttpCode);
      canvas.drawString(eb, CX, CY - 12);
    }
    canvas.setTextColor(DIM2);
    canvas.drawString(OWM_CITY, CX, CY + 12);
    return;
  }

  canvas.setTextColor(DIM);
  canvas.setTextSize(1);
  canvas.setTextDatum(middle_center);
  canvas.drawString(OWM_CITY, CX, CY - 78);

  drawWeatherIcon(CX - 56, CY - 18, wx.cond, t);

  canvas.setTextColor(FG);
  canvas.setTextSize(4);
  canvas.setTextDatum(middle_right);
  char tbuf[12]; snprintf(tbuf, sizeof(tbuf), "%.0fC", wx.tempC);
  canvas.drawString(tbuf, CX + 56, CY - 18);

  canvas.setTextColor(ACCENT);
  canvas.setTextSize(1);
  canvas.setTextDatum(middle_center);
  String desc = wx.desc; desc.toUpperCase();
  canvas.drawString(desc, CX, CY + 18);

  float range = wx.tempMaxC - wx.tempMinC;
  if (range < 0.5f) range = 0.5f;
  float pos = (wx.tempC - wx.tempMinC) / range;
  if (pos < 0) pos = 0; if (pos > 1) pos = 1;
  int barX = CX - 60, barY = CY + 38, barW = 120;
  canvas.drawRoundRect(barX, barY, barW, 6, 3, DIM2);
  canvas.fillRect(barX + (int)(pos * barW) - 1, barY - 2, 3, 10, ACCENT);
  canvas.setTextColor(DIM);
  canvas.setTextDatum(top_left);
  char b1[8]; snprintf(b1, sizeof(b1), "%.0f", wx.tempMinC);
  canvas.drawString(b1, barX - 4, barY + 10);
  canvas.setTextDatum(top_right);
  char b2[8]; snprintf(b2, sizeof(b2), "%.0f", wx.tempMaxC);
  canvas.drawString(b2, barX + barW + 4, barY + 10);

  canvas.setTextColor(DIM);
  canvas.setTextDatum(middle_center);
  canvas.setTextSize(1);
  char hbuf[12]; snprintf(hbuf, sizeof(hbuf), "%d%% HUM", wx.humidity);
  canvas.drawString(hbuf, CX - 36, CY + 72);
  char wbuf[16]; snprintf(wbuf, sizeof(wbuf), "%.1f m/s", wx.windMs);
  canvas.drawString(wbuf, CX + 36, CY + 72);
}

// ═══════════════ 13) INFO PAGE ═════════════════════════════════
void drawWifiBars(int cx, int cy, int rssi) {
  int bars = 0;
  if      (rssi >= -55) bars = 5;
  else if (rssi >= -65) bars = 4;
  else if (rssi >= -75) bars = 3;
  else if (rssi >= -85) bars = 2;
  else if (rssi != 0)   bars = 1;

  for (int i = 0; i < 5; i++) {
    int x = cx - 18 + i * 9;
    int h = 4 + i * 4;
    uint16_t col = (i < bars) ? ACCENT : DIM2;
    canvas.fillRect(x, cy + 12 - h, 6, h, col);
  }
}

void drawInfoPage(uint32_t t) {
  canvas.setTextDatum(middle_center);
  canvas.setTextSize(2);
  canvas.setTextColor(FG);
  canvas.drawString("SYSTEM", CX, CY - 92);

  canvas.setTextSize(1);
  canvas.setTextColor(DIM);
  canvas.drawString("WIFI", CX, CY - 68);
  drawWifiBars(CX, CY - 50, wifiOnline ? WiFi.RSSI() : 0);

  canvas.setTextColor(wifiOnline ? OK : WARN);
  canvas.drawString(wifiOnline ? WiFi.SSID() : "OFFLINE", CX, CY - 22);
  if (wifiOnline) {
    canvas.setTextColor(DIM);
    canvas.drawString(WiFi.localIP().toString(), CX, CY - 8);
  }

  uint32_t s = t / 1000;
  uint32_t h = s / 3600, m = (s / 60) % 60, sec = s % 60;
  char ub[24]; snprintf(ub, sizeof(ub), "UP %02lu:%02lu:%02lu",
                       (unsigned long)h, (unsigned long)m, (unsigned long)sec);
  canvas.setTextColor(ACCENT);
  canvas.drawString(ub, CX, CY + 18);

  char hb[24]; snprintf(hb, sizeof(hb), "HEAP %lu KB",
                       (unsigned long)(ESP.getFreeHeap() / 1024));
  canvas.setTextColor(DIM);
  canvas.drawString(hb, CX, CY + 36);

  canvas.setTextColor(DIM2);
  canvas.drawString("< MEJILLA   CABEZA >", CX, CY + 70);
}

// ═══════════════ 15) PAGE INDICATOR ════════════════════════════
void drawPageDots() {
  int gap = 10;
  int totalW = (PG_COUNT - 1) * gap;
  int x0 = CX - totalW / 2;
  int y  = 220;
  bool flashing = millis() < (uint32_t)pressFlashUntil;
  for (int i = 0; i < PG_COUNT; i++) {
    int x = x0 + i * gap;
    bool active = (i == (int)currentPage);
    uint16_t col = active ? FG : DIM2;
    if (flashing && active) col = ACCENT;
    canvas.fillCircle(x, y, active ? 3 : 2, col);
  }
}

// ═══════════════ 16) RENDER PRINCIPAL ══════════════════════════
void renderFrame() {
  uint32_t t = millis();
  canvas.fillSprite(BG);

  switch (currentPage) {
    case PG_CLOCK:    drawClockPage(t);       break;
    case PG_WEATHER:  drawWeatherPage(t);     break;
    case PG_INFO:     drawInfoPage(t);        break;
    default: break;
  }

  drawPageDots();
  canvas.pushSprite(0, 0);
}

// ═══════════════ 17) SETUP & LOOP ══════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[tools_desktop rev.A] init");

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(BG);
  canvas.setColorDepth(16);
  canvas.createSprite(240, 240);

  pinMode(PIN_BTN_RIGHT, INPUT);
  pinMode(PIN_BTN_LEFT,  INPUT);

  canvas.fillSprite(BG);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(FG);
  canvas.setTextSize(2);
  canvas.drawString("TOOLS", CX, CY - 8);
  canvas.setTextSize(1);
  canvas.setTextColor(ACCENT);
  canvas.drawString("conectando...", CX, CY + 14);
  canvas.pushSprite(0, 0);

  wifiInit();
  fetchWeather();

  pageEnterMs = millis();
  Serial.println("[tools_desktop rev.A] ready");
}

void loop() {
  handleButtons();
  wifiKeepAlive();
  fetchWeather();
  renderFrame();
  delay(33);
}
