/* ════════════════════════════════════════════════════════════════
   DESKTOP COMPANION — Rev. G
   ─────────────────────────────────────────────────────────────────
   Cambios respecto a Rev. F:
     · Sin icono de mood — el ánimo se refleja en los ojos del IDLE:
         · mood ≥ 60 → ojos achatados estilo "sonrisa con cachetes"
         · mood 30–59 → ojos blancos normales (rounded rect)
         · mood < 30 → ojos azul oscuro medio cerrados, caídos
     · Estado SAD rediseñado (estilo hámster triste):
         · Ojos azul oscuro con reflejo vertical blanco
         · Lágrima MÁS GRANDE saliendo del borde EXTERIOR
     · Estado RAGE: SAD + 3 toques de cabeza → modo furia
         · Ojos rojos muy grandes y angulados
         · Persiste hasta que el mood sube
     · Estado FED_UP: 3+ toques rápidos en mejilla → "ya fue suficiente"
         · Ojos pequeños entrecerrados (mirada fija)
         · Animación de mano con arma subiendo desde abajo
     · Corazón con cálculo de tangente RUNTIME (V continuo)
       y punta más profunda (tipY = 2.4 R) → V más sharp
   ════════════════════════════════════════════════════════════════ */

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <math.h>

// ─── Forward declarations ─────────────────────────────────────
struct Note     { uint16_t freq; uint16_t ms; };
struct EyeShape {
  int      rectW;
  int      rectH;
  int      xOffset;
  int      yOffset;
  int      slope;
  uint16_t color;
  int      heartR;
  int      closedW;
  int      closedH;
};

// ═══════════════ 1) PINES ═════════════════════════════════════
#define PIN_DISP_SCK     8
#define PIN_DISP_MOSI    10
#define PIN_DISP_DC      7
#define PIN_DISP_CS      9
#define PIN_DISP_RST     6
#define PIN_MIC          1
#define PIN_TOUCH_HEAD   4
#define PIN_TOUCH_CHEEK  5
#define PIN_AUDIO        3
#define PIN_BATTERY      11      // GPIO11 (ADC2_CH0) — divisor 200k/100k

// ═══════════════ 2) COLORES ═══════════════════════════════════
#define BG            0x0000
#define EYE_WHITE     0xFFFF
#define EYE_BLUE      0x041F
#define EYE_RED       0xF800
#define EYE_DARK_BLUE 0x10B5     // azul oscuro para SAD idle
#define EYE_RAGE_RED  0xF801     // rojo intenso para RAGE
#define HEART         0xF9CC
#define CHEEK_SOFT    0x4082
#define ZZZ           0x8410
#define TEAR          0x4C9F
#define BAT_LOW_RED   0xF800     // batería descargada (icono)
#define ARM_GRAY      0x6B6D
#define HAND_LIGHT    0xC638
#define GUN_DARK      0x2104

// ═══════════════ 3) DRIVER ═════════════════════════════════════
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

// ═══════════════ 4) ESTADOS ════════════════════════════════════
enum State { S_IDLE, S_HAPPY, S_LOVE, S_SURPRISED, S_ANNOYED,
             S_SLEEPY, S_SAD, S_RAGE, S_FED_UP };

// Estados "especiales" — usan render custom, no la lerp de EyeShape
inline bool isSpecial(State s) {
  return s == S_SAD || s == S_RAGE || s == S_FED_UP;
}

State    currentState   = S_SLEEPY;
uint32_t stateEnterTime = 0;
uint32_t lastInteractMs = 0;

const uint32_t DUR_HAPPY     = 3500;
const uint32_t DUR_LOVE      = 4000;
const uint32_t DUR_SURPRISED = 2500;
const uint32_t DUR_ANNOYED   = 3200;
const uint32_t DUR_FED_UP    = 6000;
const uint32_t IDLE_TO_SLEEP = 5UL * 60UL * 1000UL;

// ═══════════════ 5) MOOD ═══════════════════════════════════════
int       moodLevel       = 80;
uint32_t  lastMoodDecayMs = 0;
const uint32_t MOOD_DECAY_PERIOD = 90UL * 1000UL;
const int      MOOD_DECAY        = 1;
const int MOOD_HAPPY_GAIN    = +6;
const int MOOD_LOVE_GAIN     = +10;
const int MOOD_ANNOYED_LOSS  = -18;
const int MOOD_RAGE_LOSS     = -25;
const int MOOD_SLEEPY_LOSS   = -3;
const int MOOD_INTERACT_GAIN = +1;

int prevMoodTier = -1;

// ── Ventana SAD→RAGE ──
// Cuando el bichito entra a SAD se marca el timestamp. Si dentro de
// los siguientes RAGE_WINDOW_MS el usuario lo molesta (3 toques
// rápidos en cabeza), pasa a RAGE en lugar de ANNOYED — incluso si
// los toques pasaron primero por HAPPY.
uint32_t       lastSadEnterMs = 0;
const uint32_t RAGE_WINDOW_MS = 30UL * 1000UL;

inline int moodTier(int m) {
  if (m >= 60) return 2;
  if (m >= 30) return 1;
  return 0;
}

void changeMood(int delta) {
  moodLevel += delta;
  if (moodLevel > 100) moodLevel = 100;
  if (moodLevel < 0)   moodLevel = 0;
}

// ═══════════════ 6) SONIDOS ════════════════════════════════════
const Note sndChirp[]  = {{180,90},{220,80},{200,90}};
const Note sndCoo[]    = {{150,150},{0,50},{180,180}};
const Note sndGasp[]   = {{180,60},{280,100}};
const Note sndGrunt[]  = {{90,110},{75,110},{90,90},{70,110}};
const Note sndYawn[]   = {{180,150},{150,160},{130,200},{110,280}};
const Note sndPurr[]   = {{120,110},{100,110},{120,110},{100,110},{120,140}};
const Note sndBloop[]  = {{180,60},{140,60},{110,80}};
const Note sndHeart[]  = {{180,90},{0,60},{180,90}};
const Note sndSad[]    = {{140,150},{120,170},{105,200},{95,260}};
const Note sndWhimper[]= {{130,100},{115,120},{100,150}};
const Note sndRage[]   = {{60,200},{50,200},{60,150},{45,250}};                 // gruñido grave largo
const Note sndCock[]   = {{500,40},{0,30},{300,40},{0,30},{200,80}};            // sonido de "amartillar"

#define LEN(a) (sizeof(a)/sizeof((a)[0]))

const Note* curSeq    = nullptr;
int         seqIdx    = 0;
int         seqLen    = 0;
uint32_t    noteEndAt = 0;

void playSeq(const Note* seq, int len) {
  curSeq=seq; seqLen=len; seqIdx=0; noteEndAt=0;
}

void updateAudio() {
  if (!curSeq) return;
  uint32_t now = millis();
  if (now < noteEndAt) return;
  if (seqIdx < seqLen) {
    Note n = curSeq[seqIdx++];
    if (n.freq > 0) tone(PIN_AUDIO, n.freq); else noTone(PIN_AUDIO);
    noteEndAt = now + n.ms;
  } else {
    noTone(PIN_AUDIO); curSeq = nullptr;
  }
}

// ═══════════════ 6b) BATERÍA ═══════════════════════════════════
//
// Divisor de tensión 200kΩ / 100kΩ entre OUT+ del módulo y GND, con
// el punto medio en GPIO11. La caída del divisor es:
//   V_adc = V_bat · R2/(R1+R2) = V_bat / 3
//
// Convertimos ADC raw → voltaje del pin → multiplicamos por 3 para
// recuperar V_bat. Promediamos 16 lecturas para reducir ruido.
//
// Umbrales (V_adc → V_bat):
//   1.17 V → 3.51 V  → ~25% (umbral de "necesita carga")
//   1.40 V → 4.20 V  → 100%
//   1.00 V → 3.00 V  → corte del DW01
//
const float    BAT_DIVIDER_K       = 3.00f;     // (R1+R2)/R2 = 300k/100k
const float    BAT_VREF            = 3.30f;
const float    BAT_LOW_VADC        = 1.17f;     // umbral "descargada"
const uint32_t BAT_READ_INTERVAL   = 5000;      // 5 s

float    batteryVoltage = 4.0f;
float    batteryVadc    = 1.33f;
bool     batteryLow     = false;
uint32_t lastBatRead    = 0;

void updateBattery() {
  if (millis() - lastBatRead < BAT_READ_INTERVAL) return;
  lastBatRead = millis();

  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogRead(PIN_BATTERY);
  int raw = sum / 16;

  batteryVadc    = (raw * BAT_VREF) / 4095.0f;
  batteryVoltage = batteryVadc * BAT_DIVIDER_K;

  // Histéresis: pasa a "low" en 1.17, vuelve a "ok" recién en 1.22
  // (≈3.66 V) para evitar parpadeo del icono cerca del umbral
  if (!batteryLow && batteryVadc < BAT_LOW_VADC)        batteryLow = true;
  else if (batteryLow && batteryVadc > BAT_LOW_VADC + 0.05f) batteryLow = false;
}

void updateMood() {
  uint32_t now = millis();
  if (now - lastMoodDecayMs > MOOD_DECAY_PERIOD) {
    changeMood(-MOOD_DECAY);
    lastMoodDecayMs = now;
  }
}

// ═══════════════ 7) IDLE SOUNDS ════════════════════════════════
struct IdleSound { const Note* seq; int len; };
const IdleSound idlesHigh[] = {{sndChirp,LEN(sndChirp)},{sndCoo,LEN(sndCoo)},{sndHeart,LEN(sndHeart)}};
const IdleSound idlesMid[]  = {{sndPurr,LEN(sndPurr)},{sndBloop,LEN(sndBloop)}};
const IdleSound idlesLow[]  = {{sndWhimper,LEN(sndWhimper)},{sndSad,LEN(sndSad)}};

uint32_t nextIdleAt = 0;

void scheduleIdle() {
  uint32_t minMs = 8UL  * 60000UL;
  uint32_t maxMs = 25UL * 60000UL;
  nextIdleAt = millis() + random(minMs, maxMs);
}

void checkIdle() {
  if (currentState != S_IDLE && currentState != S_SAD) return;
  if (curSeq != nullptr || millis() < nextIdleAt) return;
  const IdleSound* pool; int poolLen;
  if      (moodLevel >= 60) { pool=idlesHigh; poolLen=LEN(idlesHigh); }
  else if (moodLevel >= 30) { pool=idlesMid;  poolLen=LEN(idlesMid);  }
  else                      { pool=idlesLow;  poolLen=LEN(idlesLow);  }
  int idx = random(0, poolLen);
  playSeq(pool[idx].seq, pool[idx].len);
  scheduleIdle();
}

// ═══════════════ 8) EYE SHAPE por estado ═══════════════════════
//
// IDLE depende del mood (se calcula al pedir la shape).
// Estados especiales (SAD/RAGE/FED_UP) tienen render dedicado.
//
EyeShape stateShape(State s) {
  EyeShape e;
  e.rectW=60; e.rectH=80; e.xOffset=0; e.yOffset=0;
  e.slope=0; e.color=EYE_WHITE; e.heartR=0; e.closedW=0; e.closedH=0;

  switch (s) {
    case S_IDLE:
      if (moodLevel >= 60) {                     // contento — mismo tamaño, con mordida
        e.rectW = 60; e.rectH = 80;
      } else if (moodLevel >= 30) {              // normal
        e.rectW = 60; e.rectH = 80;
      } else {                                   // triste — rectangular pequeño
        e.rectW = 50; e.rectH = 32; e.yOffset = 12; e.color = EYE_DARK_BLUE;
      }
      break;
    case S_HAPPY:     e.rectH = 22;                                         break;
    case S_SURPRISED: e.rectW = 72; e.rectH = 100; e.color = EYE_BLUE;      break;
    case S_ANNOYED:   e.rectH = 50; e.slope = 28; e.color = EYE_RED;        break;
    case S_LOVE:      e.rectW = 0; e.rectH = 0; e.heartR = 18;              break;
    case S_SLEEPY:    e.rectW = 0; e.rectH = 0;
                      e.closedW = 70; e.closedH = 26; e.yOffset = -8;       break;
    default: break;     // SAD/RAGE/FED_UP no usan EyeShape
  }
  return e;
}

// ═══════════════ 9) LERP HELPERS ═══════════════════════════════
inline int lerpInt(int a, int b, float t) { return a + (int)((b-a)*t); }
uint16_t lerpColor565(uint16_t c1, uint16_t c2, float t) {
  int r1=(c1>>11)&0x1F, g1=(c1>>5)&0x3F, b1=c1&0x1F;
  int r2=(c2>>11)&0x1F, g2=(c2>>5)&0x3F, b2=c2&0x1F;
  int r=r1+(int)((r2-r1)*t), g=g1+(int)((g2-g1)*t), b=b1+(int)((b2-b1)*t);
  return ((r&0x1F)<<11)|((g&0x3F)<<5)|(b&0x1F);
}
EyeShape lerpShape(const EyeShape& a, const EyeShape& b, float t) {
  EyeShape r;
  r.rectW=lerpInt(a.rectW,b.rectW,t);     r.rectH=lerpInt(a.rectH,b.rectH,t);
  r.xOffset=lerpInt(a.xOffset,b.xOffset,t); r.yOffset=lerpInt(a.yOffset,b.yOffset,t);
  r.slope=lerpInt(a.slope,b.slope,t);     r.color=lerpColor565(a.color,b.color,t);
  r.heartR=lerpInt(a.heartR,b.heartR,t);
  r.closedW=lerpInt(a.closedW,b.closedW,t); r.closedH=lerpInt(a.closedH,b.closedH,t);
  return r;
}

// ═══════════════ 10) TRANSITION ════════════════════════════════
EyeShape currentEyes;
EyeShape prevEyes;
EyeShape targetEyes;
uint32_t transitionStart = 0;
const uint32_t TRANS_DURATION = 380;

bool transitionDone() { return (millis() - transitionStart) >= TRANS_DURATION; }
float transitionProgressEased() {
  uint32_t e = millis() - transitionStart;
  if (e >= TRANS_DURATION) return 1.0f;
  float p = (float)e / TRANS_DURATION;
  return 1.0f - powf(1.0f - p, 2.5f);
}

// ═══════════════ 11) STATE MACHINE ═════════════════════════════
void setState(State s) {
  if (s == currentState) return;

  bool wasSp = isSpecial(currentState);
  bool nowSp = isSpecial(s);

  if (wasSp || nowSp) {
    // SNAP — sin morph para estados especiales
    targetEyes      = stateShape(s);
    currentEyes     = targetEyes;
    prevEyes        = targetEyes;
    transitionStart = millis() - TRANS_DURATION;
  } else {
    // Morph normal
    prevEyes        = currentEyes;
    targetEyes      = stateShape(s);
    transitionStart = millis();
  }

  currentState   = s;
  stateEnterTime = millis();
  lastInteractMs = millis();

  switch (s) {
    case S_HAPPY:     playSeq(sndChirp, LEN(sndChirp)); changeMood(MOOD_HAPPY_GAIN);   break;
    case S_LOVE:      playSeq(sndCoo,   LEN(sndCoo));   changeMood(MOOD_LOVE_GAIN);    break;
    case S_SURPRISED: playSeq(sndGasp,  LEN(sndGasp));                                 break;
    case S_ANNOYED:   playSeq(sndGrunt, LEN(sndGrunt)); changeMood(MOOD_ANNOYED_LOSS); break;
    case S_SLEEPY:    playSeq(sndYawn,  LEN(sndYawn));  changeMood(MOOD_SLEEPY_LOSS);  break;
    case S_SAD:       playSeq(sndSad,   LEN(sndSad));   lastSadEnterMs = millis();    break;
    case S_RAGE:      playSeq(sndRage,  LEN(sndRage));  changeMood(MOOD_RAGE_LOSS);    break;
    case S_FED_UP:    playSeq(sndCock,  LEN(sndCock));                                 break;
    default: break;
  }
}

void updateStateTimeouts() {
  uint32_t elapsed = millis() - stateEnterTime;
  switch (currentState) {
    case S_HAPPY:     if (elapsed > DUR_HAPPY)     setState(S_IDLE); break;
    case S_LOVE:      if (elapsed > DUR_LOVE)      setState(S_IDLE); break;
    case S_SURPRISED: if (elapsed > DUR_SURPRISED) setState(S_IDLE); break;
    case S_ANNOYED:   if (elapsed > DUR_ANNOYED)   setState(S_IDLE); break;
    case S_FED_UP:    if (elapsed > DUR_FED_UP)    setState(S_IDLE); break;
    case S_IDLE:
      if (moodLevel < 20)                                 setState(S_SAD);
      else if (millis() - lastInteractMs > IDLE_TO_SLEEP) setState(S_SLEEPY);
      break;
    case S_SAD:
      if (moodLevel >= 30) setState(S_IDLE);
      break;
    case S_RAGE:
      if (moodLevel >= 50) setState(S_IDLE);    // sale cuando mood se recupera
      break;
    case S_SLEEPY: break;
    default: break;
  }
}

// ═══════════════ 12) IDLE SHAPE refresh por mood ═══════════════
//
// Si el mood cruza un tier mientras estamos en IDLE, hacemos morph
// suave hacia la nueva shape de IDLE.
//
void refreshIdleShape() {
  int tier = moodTier(moodLevel);
  if (tier != prevMoodTier && currentState == S_IDLE && transitionDone()) {
    prevEyes        = currentEyes;
    targetEyes      = stateShape(S_IDLE);
    transitionStart = millis();
  }
  prevMoodTier = tier;
}

// ═══════════════ 13) TOUCH ═════════════════════════════════════
bool prevHead  = false;
bool prevCheek = false;
uint32_t headTouchTimes[3]  = {0,0,0};
uint32_t cheekTouchTimes[3] = {0,0,0};
int      headTouchIdx       = 0;
int      cheekTouchIdx      = 0;

bool isRapidArr(uint32_t arr[3], uint32_t now) {
  for (int i = 0; i < 3; i++)
    if (arr[i] == 0 || now - arr[i] > 1500) return false;
  return true;
}

void handleTouch() {
  bool h = digitalRead(PIN_TOUCH_HEAD)  == HIGH;
  bool c = digitalRead(PIN_TOUCH_CHEEK) == HIGH;

  if (h && !prevHead) {
    uint32_t now = millis();
    headTouchTimes[headTouchIdx] = now;
    headTouchIdx = (headTouchIdx + 1) % 3;
    bool rapid = isRapidArr(headTouchTimes, now);

    if (rapid) {
      // Si pasamos por SAD en los últimos RAGE_WINDOW_MS → RAGE
      bool recentlySad = lastSadEnterMs > 0 &&
                         (millis() - lastSadEnterMs) < RAGE_WINDOW_MS;
      if (recentlySad) {
        setState(S_RAGE);
        lastSadEnterMs = 0;                  // se "consume"
      } else {
        setState(S_ANNOYED);
      }
      headTouchTimes[0] = headTouchTimes[1] = headTouchTimes[2] = 0;
    } else {
      setState(S_HAPPY);
      changeMood(MOOD_INTERACT_GAIN);
    }
  }

  if (c && !prevCheek) {
    uint32_t now = millis();
    cheekTouchTimes[cheekTouchIdx] = now;
    cheekTouchIdx = (cheekTouchIdx + 1) % 3;
    bool rapid = isRapidArr(cheekTouchTimes, now);

    if (rapid) {
      setState(S_FED_UP);                 // 3 toques rápidos en mejilla → "ya fue suficiente"
      cheekTouchTimes[0] = cheekTouchTimes[1] = cheekTouchTimes[2] = 0;
    } else if (currentState == S_RAGE) {
      // Caricia para calmar el RAGE
      setState(S_LOVE);
      changeMood(MOOD_LOVE_GAIN);
    } else {
      setState(S_LOVE);
      changeMood(MOOD_INTERACT_GAIN);
    }
  }

  prevHead = h; prevCheek = c;
}

// ═══════════════ 14) MIC ═══════════════════════════════════════
const int      MIC_THRESHOLD = 2200;
const uint32_t MIC_INTERVAL  = 80;
const uint32_t CLAP_COOLDOWN = 2000;
uint32_t lastMicCheck = 0;
uint32_t lastClapAt   = 0;

void handleMic() {
  uint32_t now = millis();
  if (now - lastMicCheck < MIC_INTERVAL) return;
  lastMicCheck = now;
  int minV=4095, maxV=0;
  for (int i = 0; i < 24; i++) {
    int v = analogRead(PIN_MIC);
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
  }
  if ((maxV - minV) > MIC_THRESHOLD && now - lastClapAt > CLAP_COOLDOWN) {
    lastClapAt = now;
    if (currentState == S_IDLE || currentState == S_SLEEPY || currentState == S_SAD)
      setState(S_SURPRISED);
  }
}

// ═══════════════ 15) GEOMETRÍA ═════════════════════════════════
const int CX = 120, CY = 120;
const int EYE_GAP   = 30;
const int EYE_R     = 18;
const int EYE_W_REF = 60;
const int EYE_CENTER_LEFT  = CX - EYE_GAP/2 - EYE_W_REF/2;   // 75
const int EYE_CENTER_RIGHT = CX + EYE_GAP/2 + EYE_W_REF/2;   // 165

inline int eyeLeftX(int w)  { return CX - EYE_GAP/2 - w; }
inline int eyeRightX()      { return CX + EYE_GAP/2; }
inline int eyeTopY(int h)   { return CY - h/2; }

// ═══════════════ 16) PRIMITIVAS ════════════════════════════════
//
// CORAZÓN — V calculada en runtime con el teorema del tangente.
// El triángulo arranca exactamente donde el lóbulo lo "deja", sin
// step visible. Punta en cy + 2.4·R → V más sharp que antes.
//
//   Tangencia: |C-P_T| ⊥ (P - P_T) donde P es el ápice.
//
void drawHeartBig(int cx, int cy, int R, uint16_t color) {
  if (R < 2) return;
  int lobeOffX = (int)(R * 0.65f);
  int lobeY    = cy - (int)(R * 0.30f);
  int tipY     = cy + (int)(R * 2.4f);

  canvas.fillCircle(cx - lobeOffX, lobeY, R, color);
  canvas.fillCircle(cx + lobeOffX, lobeY, R, color);

  float vx = (float)lobeOffX;
  float vy = (float)(tipY - lobeY);
  float d  = sqrtf(vx*vx + vy*vy);
  float Rf = (float)R;
  if (d <= Rf + 0.5f) return;

  float beta     = atan2f(vy, vx);
  float angleOff = acosf(Rf / d);
  float alpha    = beta + angleOff;       // tangente exterior

  int LxRel = (int)(Rf * cosf(alpha));
  int LyRel = (int)(Rf * sinf(alpha));
  int baseLX = cx - lobeOffX + LxRel;
  int baseRX = cx + lobeOffX - LxRel;
  int baseY  = lobeY + LyRel;

  canvas.fillTriangle(baseLX, baseY, baseRX, baseY, cx, tipY, color);
}

// ── ojo cerrado en U ──
void drawClosedEyeU(int cx, int yTop, int w, int depth, uint16_t color) {
  if (w < 4 || depth < 2) return;
  int rx = w/2, ry = depth;
  canvas.fillEllipse(cx, yTop, rx, ry, color);
  canvas.fillRect(cx - rx - 2, yTop - ry - 1, w + 4, ry + 1, BG);
}

// ── ojo angulado annoyed ──
void drawAnnoyedEye(int xL, int yTop, int w, int h, int slope, bool isLeft, uint16_t color) {
  int xR = xL + w;
  int yTL, yTR;
  if (isLeft) { yTL=yTop;       yTR=yTop+slope; }
  else        { yTL=yTop+slope; yTR=yTop;       }
  int yB = yTop + h;
  canvas.fillTriangle(xL, yTL, xR, yTR, xR, yB, color);
  canvas.fillTriangle(xL, yTL, xR, yB,  xL, yB, color);
}

// ── ojo HAPPY idle: mismo tamaño que el normal + mordida circular
//    en la esquina inferior EXTERIOR (simula cachete subiendo).
//
//    Truco: rounded rect completo (60x80, igual al normal) + un
//    fillCircle en BG sobre la esquina afuera-abajo. El cuarto del
//    círculo que queda dentro del rect se vuelve negro, "comiéndose"
//    la esquina.
//
void drawHappyEye(int x, int y, int w, int h, bool isLeft, uint16_t color) {
  canvas.fillRoundRect(x, y, w, h, EYE_R, color);

  int biteR = 26;
  if (isLeft) canvas.fillCircle(x,     y + h, biteR, BG);    // outer = izq → muerde bot-left
  else        canvas.fillCircle(x + w, y + h, biteR, BG);    // outer = der → muerde bot-right
}

// ── ojo SAD: redondo arriba, recto abajo ──
void drawSadEye(int x, int y, int w, int h, uint16_t color) {
  int r = 10;
  canvas.fillRoundRect(x, y, w, h, r, color);
  // tapar esquinas inferiores → bottom recto
  canvas.fillRect(x,         y + h - r, r, r, color);
  canvas.fillRect(x + w - r, y + h - r, r, r, color);
}

// ═══════════════ 17) DINÁMICAS ═════════════════════════════════
void applyStateDynamics(EyeShape& e, uint32_t t) {
  uint32_t dt = t - stateEnterTime;
  switch (currentState) {
    case S_IDLE: {
      uint32_t cycle = t % 3500;
      float openness;
      if      (cycle < 120) openness = 1.0f - (float)cycle / 120.0f;
      else if (cycle < 270) openness = (float)(cycle - 120) / 150.0f;
      else                  openness = 1.0f;
      int targetH = targetEyes.rectH;
      e.rectH = 4 + (int)((targetH - 4) * openness);
      break;
    }
    case S_HAPPY: e.yOffset += (int)(sinf(t * 0.012f) * 2.0f); break;
    case S_SURPRISED: e.rectH += (int)(sinf((float)dt * 0.022f) * 2.0f); break;
    case S_ANNOYED: {
      float squeeze = 0.55f + 0.45f * (sinf((float)t * 0.018f) + 1.0f) * 0.5f;
      e.rectH = (int)(targetEyes.rectH * squeeze);
      e.slope = (int)(targetEyes.slope * squeeze);
      float fade = 1.0f - (float)dt / (float)DUR_ANNOYED;
      if (fade < 0.0f) fade = 0.0f;
      e.xOffset = (int)(sinf((float)t * 0.040f) * 4.0f * fade);
      break;
    }
    case S_LOVE: {
      float pulse = 1.0f + sinf((float)t * 0.012f) * 0.10f;
      e.heartR = (int)(targetEyes.heartR * pulse);
      break;
    }
    case S_SLEEPY:
      e.closedH = targetEyes.closedH + (int)(sinf((float)t * 0.0015f) * 2.0f);
      break;
    default: break;
  }
}

// ═══════════════ 18) RENDERS ESPECIALES ════════════════════════

// ── SAD: ojos rectangulares pequeños, redondos arriba / rectos abajo ──
//
// El ojo tiene:
//   · Cuerpo azul oscuro
//   · Charco celeste en la parte inferior (lágrimas acumuladas)
//   · Una gota cayendo solo del ojo DERECHO
//
void drawSadFace(uint32_t t) {
  int w = 50, h = 32;
  int yT = CY - h/2 + 12;

  // Ojos
  drawSadEye(eyeLeftX(w),  yT, w, h, EYE_DARK_BLUE);
  drawSadEye(eyeRightX(),  yT, w, h, EYE_DARK_BLUE);

  // Charco de lágrimas dentro de cada ojo (parte inferior)
  // Onda sutil en la superficie usando seno → ilusión de líquido
  int poolH = 10;
  int waveY = yT + h - poolH + (int)(sinf((float)t * 0.004f) * 1.0f);
  canvas.fillRect(eyeLeftX(w) + 2,  waveY, w - 4, poolH - 2, TEAR);
  canvas.fillRect(eyeRightX() + 2,  waveY, w - 4, poolH - 2, TEAR);

  // Lágrima desbordando solo desde el ojo DERECHO
  uint32_t cycle = t % 2800;
  if (cycle < 1900) {
    float p  = (float)cycle / 1900.0f;
    int dy   = (int)(p * 75.0f);
    int xR   = eyeRightX() + w - 8;
    int yR   = yT + h + dy;
    canvas.fillCircle(xR, yR, 5, TEAR);
    canvas.fillTriangle(xR - 5, yR, xR + 5, yR, xR, yR - 12, TEAR);
  }
}

// ── RAGE: ojos rojos enormes y angulados, vibración fuerte ───
void drawRageFace(uint32_t t) {
  uint32_t dt = t - stateEnterTime;
  int shakeX = (int)(sinf((float)t * 0.060f) * 5.0f);
  int shakeY = (int)(cosf((float)t * 0.075f) * 3.0f);

  int w = 70, h = 70, slope = 38;
  int yTop = CY - h/2 + shakeY;

  drawAnnoyedEye(eyeLeftX(w) + shakeX, yTop, w, h, slope, true,  EYE_RAGE_RED);
  drawAnnoyedEye(eyeRightX() + shakeX, yTop, w, h, slope, false, EYE_RAGE_RED);

  // Cejas extra arriba (ceño bestial)
  canvas.fillTriangle(eyeLeftX(w) + shakeX,        yTop - 8 + shakeY,
                      eyeLeftX(w) + shakeX + w,    yTop + slope - 8 + shakeY,
                      eyeLeftX(w) + shakeX + w/2,  yTop - 18 + shakeY, EYE_RAGE_RED);
  canvas.fillTriangle(eyeRightX() + shakeX,        yTop + slope - 8 + shakeY,
                      eyeRightX() + shakeX + w,    yTop - 8 + shakeY,
                      eyeRightX() + shakeX + w/2,  yTop - 18 + shakeY, EYE_RAGE_RED);

  // Pulso en los ojos — pequeñas pupilas blancas pulsantes
  float pulse = 1.0f + sinf((float)t * 0.025f) * 0.3f;
  int pR = (int)(4 * pulse);
  if (pR > 1) {
    canvas.fillCircle(eyeLeftX(w)  + shakeX + w/2 + 5, yTop + h/2 + shakeY, pR, EYE_WHITE);
    canvas.fillCircle(eyeRightX()  + shakeX + w/2 - 5, yTop + h/2 + shakeY, pR, EYE_WHITE);
  }
}

// ── helpers para dibujar rotado ───────────────────────────────
static inline int rotPx(int cx, int dx, int dy, float ca, float sa) {
  return cx + (int)(dx * ca - dy * sa);
}
static inline int rotPy(int cy, int dx, int dy, float ca, float sa) {
  return cy + (int)(dx * sa + dy * ca);
}
// rect rotado alrededor de (cx, cy) — definido en coords locales (x1,y1)-(x2,y2)
static void fillRotRect(int cx, int cy, int x1, int y1, int x2, int y2,
                        float ca, float sa, uint16_t color) {
  int ax  = rotPx(cx, x1, y1, ca, sa), ay  = rotPy(cy, x1, y1, ca, sa);
  int bx_ = rotPx(cx, x2, y1, ca, sa), by_ = rotPy(cy, x2, y1, ca, sa);
  int cx_ = rotPx(cx, x2, y2, ca, sa), cy_ = rotPy(cy, x2, y2, ca, sa);
  int dx_ = rotPx(cx, x1, y2, ca, sa), dy_ = rotPy(cy, x1, y2, ca, sa);
  canvas.fillTriangle(ax, ay, bx_, by_, cx_, cy_, color);
  canvas.fillTriangle(ax, ay, cx_, cy_, dx_, dy_, color);
}

// ── FED_UP: "Ya fue suficiente" — ojos pequeños + arma frontal ──
//
// Animación de la mano con arma subiendo desde abajo:
// - 0 a 900 ms: arma sube de y=240 hasta y=145 (ease-out)
// - >900 ms: queda apuntando, mira fija
//
void drawFedUpFace(uint32_t t) {
  uint32_t dt = t - stateEnterTime;

  // Ojos pequeños entrecerrados
  int eyeH = 10, eyeW = 50;
  int yTop = CY - eyeH/2 - 22;
  canvas.fillRoundRect(eyeLeftX(eyeW), yTop, eyeW, eyeH, 5, EYE_WHITE);
  canvas.fillRoundRect(eyeRightX(),    yTop, eyeW, eyeH, 5, EYE_WHITE);

  // Pupilas pequeñas mirando fijo (ligeramente abajo, hacia el arma)
  canvas.fillCircle(EYE_CENTER_LEFT  + 4, yTop + eyeH/2 + 1, 3, BG);
  canvas.fillCircle(EYE_CENTER_RIGHT - 4, yTop + eyeH/2 + 1, 3, BG);

  // Mano + arma subiendo desde abajo
  float p = dt < 900 ? (float)dt / 900.0f : 1.0f;
  p = 1.0f - powf(1.0f - p, 2.5f);                  // ease-out

  int gunStart = 240;
  int gunEnd   = 145;
  int gY = gunStart - (int)((gunStart - gunEnd) * p);

  // Brazo (gris, vertical)
  canvas.fillRect(CX - 7, gY + 35, 14, 240 - (gY + 35), ARM_GRAY);

  // Mano (un poco más clara)
  canvas.fillRoundRect(CX - 10, gY + 25, 20, 14, 3, HAND_LIGHT);

  // Cuerpo del arma
  canvas.fillRoundRect(CX - 9, gY + 8, 18, 20, 2, GUN_DARK);

  // Cañón apuntando arriba
  canvas.fillRect(CX - 4, gY - 5, 8, 14, GUN_DARK);
  canvas.fillCircle(CX, gY - 5, 2, BG);             // boquilla del cañón

  // Gatillo y guardamonte (detalle)
  canvas.drawCircle(CX, gY + 28, 5, GUN_DARK);
}

// ═══════════════ 19) RENDER PRINCIPAL ══════════════════════════
void drawShape(const EyeShape& s) {
  if (s.heartR > 1) {
    drawHeartBig(EYE_CENTER_LEFT  + s.xOffset, CY, s.heartR, HEART);
    drawHeartBig(EYE_CENTER_RIGHT + s.xOffset, CY, s.heartR, HEART);
  }
  if (s.closedW > 2 && s.closedH > 2) {
    int yTop = CY + s.yOffset;
    drawClosedEyeU(EYE_CENTER_LEFT  + s.xOffset, yTop, s.closedW, s.closedH, s.color);
    drawClosedEyeU(EYE_CENTER_RIGHT + s.xOffset, yTop, s.closedW, s.closedH, s.color);
  }
  if (s.rectW > 2 && s.rectH > 2) {
    int xL = eyeLeftX(s.rectW) + s.xOffset;
    int xR = eyeRightX()       + s.xOffset;
    int yT = eyeTopY(s.rectH)  + s.yOffset;

    // Selección de estilo según estado IDLE + mood (después del morph)
    bool inIdleSettled = (currentState == S_IDLE && transitionDone());
    bool styleHappy    = inIdleSettled && moodLevel >= 60;
    bool styleSad      = inIdleSettled && moodLevel < 30;

    if (s.slope > 0) {
      drawAnnoyedEye(xL, yT, s.rectW, s.rectH, s.slope, true,  s.color);
      drawAnnoyedEye(xR, yT, s.rectW, s.rectH, s.slope, false, s.color);
    } else if (styleHappy) {
      drawHappyEye(xL, yT, s.rectW, s.rectH, true,  s.color);
      drawHappyEye(xR, yT, s.rectW, s.rectH, false, s.color);
    } else if (styleSad) {
      drawSadEye(xL, yT, s.rectW, s.rectH, s.color);
      drawSadEye(xR, yT, s.rectW, s.rectH, s.color);
    } else {
      canvas.fillRoundRect(xL, yT, s.rectW, s.rectH, EYE_R, s.color);
      canvas.fillRoundRect(xR, yT, s.rectW, s.rectH, EYE_R, s.color);
    }
  }
}

// ── icono de batería baja (rojo) ──
//
// Posicionado a la derecha de la cara en IDLE. Forma estilizada:
// un rectángulo redondeado con un terminal pequeño a la derecha,
// relleno rojo y un símbolo "!" interno para indicar el aviso.
// Pulsación sutil (alpha) para llamar la atención sin molestar.
//
void drawLowBatteryIcon(int x, int y, uint32_t t) {
  int w = 22, h = 12;

  // Pulso sutil: parpadea suave con período ~1.5 s
  float blink = 0.6f + 0.4f * (sinf((float)t * 0.004f) + 1.0f) * 0.5f;
  uint16_t col = lerpColor565(BG, BAT_LOW_RED, blink);

  // Cuerpo
  canvas.drawRoundRect(x, y, w, h, 2, col);
  canvas.fillRoundRect(x + 2, y + 2, w - 4, h - 4, 1, col);
  // Terminal positivo
  canvas.fillRect(x + w, y + 4, 2, h - 8, col);

  // Símbolo "!" en el centro (huecos en BG sobre el rojo)
  int cxIco = x + w / 2;
  canvas.fillRect(cxIco, y + 3,  1, 4, BG);
  canvas.fillRect(cxIco, y + 8,  1, 1, BG);
}

void drawStateExtras(uint32_t t) {
  if (!transitionDone()) return;

  // Icono de batería baja: solo en IDLE, posicionado a la derecha
  // de la cara (dentro del círculo de la pantalla circular)
  if (currentState == S_IDLE && batteryLow) {
    drawLowBatteryIcon(180, CY - 6, t);
  }

  switch (currentState) {
    case S_HAPPY:
      canvas.fillEllipse(CX - 75, CY + 30, 14, 8, CHEEK_SOFT);
      canvas.fillEllipse(CX + 75, CY + 30, 14, 8, CHEEK_SOFT);
      break;
    case S_SLEEPY: {
      uint32_t phase = (t / 250) % 8;
      int      base  = CY - 50 - (int)phase * 5;
      canvas.setTextColor(ZZZ);
      canvas.setTextSize(2);
      canvas.setCursor(CX + 50, base);     canvas.print("z");
      canvas.setCursor(CX + 60, base - 14); canvas.print("z");
      canvas.setCursor(CX + 70, base - 28); canvas.print("Z");
      break;
    }
    default: break;
  }
}

void renderFace() {
  uint32_t t = millis();
  canvas.fillSprite(BG);

  // Estados especiales: render dedicado
  if (isSpecial(currentState)) {
    switch (currentState) {
      case S_SAD:    drawSadFace(t);    break;
      case S_RAGE:   drawRageFace(t);   break;
      case S_FED_UP: drawFedUpFace(t);  break;
      default: break;
    }
  } else {
    // Estados normales: morph + dynamics + extras
    float p = transitionProgressEased();
    currentEyes = lerpShape(prevEyes, targetEyes, p);
    if (transitionDone()) applyStateDynamics(currentEyes, t);
    drawShape(currentEyes);
    drawStateExtras(t);
  }

  canvas.pushSprite(0, 0);
}

// ═══════════════ 20) SETUP & LOOP ══════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[Companion rev.G] init");

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(BG);
  canvas.setColorDepth(16);
  canvas.createSprite(240, 240);

  pinMode(PIN_TOUCH_HEAD,  INPUT);
  pinMode(PIN_TOUCH_CHEEK, INPUT);
  pinMode(PIN_BATTERY,     INPUT);
  analogReadResolution(12);
  noTone(PIN_AUDIO);
  randomSeed(analogRead(PIN_MIC) ^ micros());

  currentEyes     = stateShape(S_SLEEPY);
  prevEyes        = currentEyes;
  targetEyes      = currentEyes;
  transitionStart = millis() - TRANS_DURATION;
  currentState    = S_SLEEPY;
  stateEnterTime  = millis();
  lastInteractMs  = millis();

  scheduleIdle();
  lastMoodDecayMs = millis();
  prevMoodTier    = moodTier(moodLevel);

  Serial.println("[Companion rev.G] ready — sleeping");
}

void loop() {
  handleTouch();
  handleMic();
  updateAudio();
  updateMood();
  updateBattery();
  refreshIdleShape();
  updateStateTimeouts();
  checkIdle();
  renderFace();
  delay(16);
}
