/* ════════════════════════════════════════════════════════════════
   DESKTOP COMPANION — Rev. I
   ─────────────────────────────────────────────────────────────────
   Cambios respecto a Rev. H:
     · Sub-moods en IDLE que rotan automaticamente:
         · IDLE_NORMAL   — ojos abiertos, mira aleatoria
         · IDLE_BORED    — parpado caido (tipo OLED SLEEPY),
                           mira fija ligeramente abajo
         · IDLE_THINKING — ojos squint, mira arriba-derecha
       Rotacion cada 6-12 s; solo activo cuando mood >= 30
       (con mood bajo se mantiene la cara sad-brow sin cambios).
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

bool transitionDone();

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
#define PIN_BATTERY      11

// ═══════════════ 2) COLORES ═══════════════════════════════════
#define BG            0x0000
#define EYE_WHITE     0xFFFF
#define EYE_BLUE      0x041F
#define EYE_RED       0xF800
#define EYE_DARK_BLUE 0x10B5
#define EYE_RAGE_RED  0xF801
#define HEART         0xF9CC
#define CHEEK_SOFT    0x4082
#define ZZZ           0x8410
#define TEAR          0x4C9F
#define BAT_LOW_RED   0xF800
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

inline bool isSpecial(State s) {
  return s == S_SAD || s == S_RAGE || s == S_FED_UP;
}

State    currentState   = S_SLEEPY;
uint32_t stateEnterTime = 0;
uint32_t lastInteractMs = 0;

const uint32_t DUR_HAPPY     = 1200;          // <<< 1/3 de Rev. G (era 3500)
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
const Note sndRage[]   = {{60,200},{50,200},{60,150},{45,250}};
const Note sndCock[]   = {{500,40},{0,30},{300,40},{0,30},{200,80}};

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
const float    BAT_DIVIDER_K       = 3.00f;
const float    BAT_VREF            = 3.30f;
const float    BAT_LOW_VADC        = 1.17f;
const uint32_t BAT_READ_INTERVAL   = 5000;

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

  if (!batteryLow && batteryVadc < BAT_LOW_VADC)             batteryLow = true;
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

// ═══════════════ 7b) EYE LOOK-AROUND (IDLE + HAPPY + SAD) ══════
//
// El bichito mira alrededor cuando esta en estados "que duran":
// IDLE, HAPPY y SAD. Cada 1.5-4 s elige una direccion aleatoria
// entre 5 opciones (centro, izq, der, arriba, abajo). El offset
// se interpola 1 px por frame -> velocidad ~60 px/s (16 ms/frame).
//
int      lookOffX = 0, lookOffY = 0;
int      lookTargetX = 0, lookTargetY = 0;
uint32_t nextLookChangeAt = 0;

// ── Sub-moods en IDLE: NORMAL / BORED / THINKING ──
enum IdleMood { IDLE_NORMAL, IDLE_BORED, IDLE_THINKING };
IdleMood currentIdleMood        = IDLE_NORMAL;
uint32_t nextIdleMoodChangeAt   = 0;
const uint32_t IDLE_MOOD_MIN_MS = 6000;
const uint32_t IDLE_MOOD_MAX_MS = 12000;

void updateEyeLook(uint32_t t) {
  bool allowLook = (currentState == S_IDLE ||
                    currentState == S_HAPPY ||
                    currentState == S_SAD);

  if (!allowLook) {
    // Volver al centro suavemente al salir
    lookTargetX = 0;
    lookTargetY = 0;
  } else if (t >= nextLookChangeAt && transitionDone()) {
    // Mirada random en todos los sub-moods (la forma cambia, no la mirada)
    const int dirs[5][2] = {{0,0}, {-12,0}, {12,0}, {0,-8}, {0,8}};
    int p = random(0, 5);
    lookTargetX = dirs[p][0];
    lookTargetY = dirs[p][1];
    nextLookChangeAt = t + random(1500, 4000);
  }

  if      (lookOffX < lookTargetX) lookOffX++;
  else if (lookOffX > lookTargetX) lookOffX--;
  if      (lookOffY < lookTargetY) lookOffY++;
  else if (lookOffY > lookTargetY) lookOffY--;
}

// ═══════════════ 7c) IDLE SUB-MOODS rotation ═══════════════════
//
// Cada IDLE_MOOD_MIN..MAX ms, en IDLE estable con mood >= 30,
// elige al azar entre NORMAL/BORED/THINKING (sin repetir el actual).
// Si mood < 30 no rota: se mantiene la cara sad-brow.
//
void updateIdleSubMood(uint32_t t) {
  if (currentState != S_IDLE || !transitionDone() || moodLevel < 30) return;
  if (t < nextIdleMoodChangeAt) return;

  IdleMood nm;
  do { nm = (IdleMood)random(0, 3); } while (nm == currentIdleMood);
  currentIdleMood = nm;
  nextIdleMoodChangeAt = t + random(IDLE_MOOD_MIN_MS, IDLE_MOOD_MAX_MS);

  const char* names[] = {"NORMAL", "BORED", "THINKING"};
  Serial.printf("[idle] sub-mood -> %s\n", names[(int)currentIdleMood]);
}

// ═══════════════ 8) EYE SHAPE por estado ═══════════════════════
EyeShape stateShape(State s) {
  EyeShape e;
  e.rectW=60; e.rectH=80; e.xOffset=0; e.yOffset=0;
  e.slope=0; e.color=EYE_WHITE; e.heartR=0; e.closedW=0; e.closedH=0;

  switch (s) {
    case S_IDLE:
      if (moodLevel >= 60) {
        e.rectW = 60; e.rectH = 80;
      } else if (moodLevel >= 30) {
        e.rectW = 60; e.rectH = 80;
      } else {
        // sad sin llorar: dimensiones normales, color azul oscuro,
        // se renderiza con cejas / \ (estilo OLED SUSPECT)
        e.rectW = 60; e.rectH = 80; e.color = EYE_DARK_BLUE;
      }
      break;
    case S_HAPPY:     e.rectH = 22;                                         break;
    case S_SURPRISED: e.rectW = 72; e.rectH = 100; e.color = EYE_BLUE;      break;
    case S_ANNOYED:   e.rectH = 60; e.slope = 22; e.color = EYE_RED;        break;
    case S_LOVE:      e.rectW = 0; e.rectH = 0; e.heartR = 18;              break;
    case S_SLEEPY:    e.rectW = 0; e.rectH = 0;
                      e.closedW = 70; e.closedH = 26; e.yOffset = -8;       break;
    default: break;
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
    targetEyes      = stateShape(s);
    currentEyes     = targetEyes;
    prevEyes        = targetEyes;
    transitionStart = millis() - TRANS_DURATION;
  } else {
    prevEyes        = currentEyes;
    targetEyes      = stateShape(s);
    transitionStart = millis();
  }

  currentState   = s;
  stateEnterTime = millis();
  lastInteractMs = millis();

  // Reset del sub-mood al entrar a IDLE
  if (s == S_IDLE) {
    currentIdleMood      = IDLE_NORMAL;
    nextIdleMoodChangeAt = millis() + random(5000, 9000);
  }

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
      if (moodLevel >= 50) setState(S_IDLE);
      break;
    case S_SLEEPY: break;
    default: break;
  }
}

// ═══════════════ 12) IDLE SHAPE refresh por mood ═══════════════
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
      bool recentlySad = lastSadEnterMs > 0 &&
                         (millis() - lastSadEnterMs) < RAGE_WINDOW_MS;
      if (recentlySad) {
        setState(S_RAGE);
        lastSadEnterMs = 0;
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
      setState(S_FED_UP);
      cheekTouchTimes[0] = cheekTouchTimes[1] = cheekTouchTimes[2] = 0;
    } else if (currentState == S_RAGE) {
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
const int EYE_CENTER_LEFT  = CX - EYE_GAP/2 - EYE_W_REF/2;
const int EYE_CENTER_RIGHT = CX + EYE_GAP/2 + EYE_W_REF/2;

inline int eyeLeftX(int w)  { return CX - EYE_GAP/2 - w; }
inline int eyeRightX()      { return CX + EYE_GAP/2; }
inline int eyeTopY(int h)   { return CY - h/2; }

// ═══════════════ 16) PRIMITIVAS ════════════════════════════════
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
  float alpha    = beta + angleOff;

  int LxRel = (int)(Rf * cosf(alpha));
  int LyRel = (int)(Rf * sinf(alpha));
  int baseLX = cx - lobeOffX + LxRel;
  int baseRX = cx + lobeOffX - LxRel;
  int baseY  = lobeY + LyRel;

  canvas.fillTriangle(baseLX, baseY, baseRX, baseY, cx, tipY, color);
}

void drawClosedEyeU(int cx, int yTop, int w, int depth, uint16_t color) {
  if (w < 4 || depth < 2) return;
  int rx = w/2, ry = depth;
  canvas.fillEllipse(cx, yTop, rx, ry, color);
  canvas.fillRect(cx - rx - 2, yTop - ry - 1, w + 4, ry + 1, BG);
}

// ── ojo angulado tipo trapecio rojo (usado SOLO por drawRageFace) ──
void drawAnnoyedEye(int xL, int yTop, int w, int h, int slope, bool isLeft, uint16_t color) {
  int xR = xL + w;
  int yTL, yTR;
  if (isLeft) { yTL=yTop;       yTR=yTop+slope; }
  else        { yTL=yTop+slope; yTR=yTop;       }
  int yB = yTop + h;
  canvas.fillTriangle(xL, yTL, xR, yTR, xR, yB, color);
  canvas.fillTriangle(xL, yTL, xR, yB,  xL, yB, color);
}

// ── ojo ANNOYED v2: rounded rect + ceja \ / como el OLED ANGRY ──
//
//   Left eye  -> cut esquina superior-derecha (interior)
//   Right eye -> cut esquina superior-izquierda (interior)
//
// Profundidad del corte = slope; la animacion de squeeze que escala
// el slope tambien anima la ceja.
//
void drawAnnoyedEyeV2(int x, int y, int w, int h, int slope, bool isLeft, uint16_t color) {
  if (w < 4 || h < 4) return;
  canvas.fillRoundRect(x, y, w, h, EYE_R, color);
  if (slope <= 0) return;

  int cutDepth = slope;
  if (isLeft) {
    // Cut interior (esquina superior-derecha del ojo izquierdo)
    canvas.fillTriangle(x - 4,     y - 4,
                        x + w + 4, y - 4,
                        x + w,     y + cutDepth, BG);
  } else {
    // Cut interior (esquina superior-izquierda del ojo derecho)
    canvas.fillTriangle(x - 4,     y - 4,
                        x + w + 4, y - 4,
                        x,         y + cutDepth, BG);
  }
}

// ── ojo SAD sin llorar: rounded rect + cejas / \ como OLED SUSPECT ──
//
// Cejas hacia AFUERA (caen las esquinas externas-superiores).
// Es la expresion clasica de "cara de pena" (inner brows raised).
//
//   Left eye  -> cut esquina superior-izquierda (exterior)
//   Right eye -> cut esquina superior-derecha (exterior)
//
void drawSadBrowEye(int x, int y, int w, int h, bool isLeft, uint16_t color) {
  if (w < 4 || h < 4) return;
  canvas.fillRoundRect(x, y, w, h, EYE_R, color);

  int cutDepth = (int)(h * 0.28f);   // ~22 px en ojo de 80
  if (cutDepth < 6) return;

  if (isLeft) {
    // Cut exterior (esquina superior-izquierda del ojo izquierdo)
    canvas.fillTriangle(x - 4,     y - 4,
                        x + w + 4, y - 4,
                        x - 4,     y + cutDepth, BG);
  } else {
    // Cut exterior (esquina superior-derecha del ojo derecho)
    canvas.fillTriangle(x - 4,     y - 4,
                        x + w + 4, y - 4,
                        x + w + 4, y + cutDepth, BG);
  }
}

// ── ojo HAPPY idle: mordida circular en esquina inferior exterior ──
void drawHappyEye(int x, int y, int w, int h, bool isLeft, uint16_t color) {
  canvas.fillRoundRect(x, y, w, h, EYE_R, color);
  int biteR = 26;
  if (isLeft) canvas.fillCircle(x,     y + h, biteR, BG);
  else        canvas.fillCircle(x + w, y + h, biteR, BG);
}

// ── ojo BORED: parpado caido tapando la mitad superior ──
//
// Se dibuja el rounded rect completo y luego se tapa el 50% de
// arriba con un rect negro. El borde inferior se queda redondeado
// (porque el fillRect solo cubre la parte superior), creando un
// look de ojos entrecerrados de aburrimiento.
//
void drawBoredEye(int x, int y, int w, int h, uint16_t color) {
  canvas.fillRoundRect(x, y, w, h, EYE_R, color);
  int lidH = (int)(h * 0.50f);
  canvas.fillRect(x - 4, y - 4, w + 8, lidH + 4, BG);
}

// ── ojo THINKING: squint (achicar verticalmente y centrar) ──
//
// La mirada arriba-derecha la maneja updateEyeLook(), aqui solo
// reducimos la altura para que el ojo se vea "pensando".
//
void drawThinkingEye(int x, int y, int w, int h, uint16_t color) {
  int newH = (int)(h * 0.55f);
  int newY = y + (h - newH) / 2;
  int newR = EYE_R - 4; if (newR < 4) newR = 4;
  canvas.fillRoundRect(x, newY, w, newH, newR, color);
}

// ── ojo SAD llorando (usado SOLO por drawSadFace) ──
void drawSadEye(int x, int y, int w, int h, uint16_t color) {
  int r = 10;
  canvas.fillRoundRect(x, y, w, h, r, color);
  canvas.fillRect(x,         y + h - r, r, r, color);
  canvas.fillRect(x + w - r, y + h - r, r, r, color);
}

// ═══════════════ 17) DINÁMICAS ═════════════════════════════════
//
// lookOffX/Y se aplica aqui para IDLE y HAPPY.
// Para S_SAD el offset se aplica directamente en drawSadFace
// (estado especial, no pasa por este path).
//
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
      e.xOffset += lookOffX;
      e.yOffset += lookOffY;
      break;
    }
    case S_HAPPY:
      e.yOffset += (int)(sinf(t * 0.012f) * 2.0f);
      e.xOffset += lookOffX;
      e.yOffset += lookOffY;
      break;
    case S_SURPRISED:
      e.rectH += (int)(sinf((float)dt * 0.022f) * 2.0f);
      break;
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

// ── SAD llorando: ojos azul oscuro + charco + lagrima cayendo ──
// Aplica lookOffX/Y para que mire alrededor. El charco sigue al ojo;
// la lagrima cae desde la posicion actual del ojo derecho.
//
void drawSadFace(uint32_t t) {
  int w = 50, h = 32;
  int yT = CY - h/2 + 12 + lookOffY;
  int xL = eyeLeftX(w) + lookOffX;
  int xR = eyeRightX() + lookOffX;

  drawSadEye(xL, yT, w, h, EYE_DARK_BLUE);
  drawSadEye(xR, yT, w, h, EYE_DARK_BLUE);

  int poolH = 10;
  int waveY = yT + h - poolH + (int)(sinf((float)t * 0.004f) * 1.0f);
  canvas.fillRect(xL + 2, waveY, w - 4, poolH - 2, TEAR);
  canvas.fillRect(xR + 2, waveY, w - 4, poolH - 2, TEAR);

  uint32_t cycle = t % 2800;
  if (cycle < 1900) {
    float p     = (float)cycle / 1900.0f;
    int   dy    = (int)(p * 75.0f);
    int   xTear = xR + w - 8;
    int   yTear = yT + h + dy;
    canvas.fillCircle(xTear, yTear, 5, TEAR);
    canvas.fillTriangle(xTear - 5, yTear, xTear + 5, yTear, xTear, yTear - 12, TEAR);
  }
}

// ── RAGE ──
void drawRageFace(uint32_t t) {
  int shakeX = (int)(sinf((float)t * 0.060f) * 5.0f);
  int shakeY = (int)(cosf((float)t * 0.075f) * 3.0f);

  int w = 70, h = 70, slope = 38;
  int yTop = CY - h/2 + shakeY;

  drawAnnoyedEye(eyeLeftX(w) + shakeX, yTop, w, h, slope, true,  EYE_RAGE_RED);
  drawAnnoyedEye(eyeRightX() + shakeX, yTop, w, h, slope, false, EYE_RAGE_RED);

  canvas.fillTriangle(eyeLeftX(w) + shakeX,        yTop - 8 + shakeY,
                      eyeLeftX(w) + shakeX + w,    yTop + slope - 8 + shakeY,
                      eyeLeftX(w) + shakeX + w/2,  yTop - 18 + shakeY, EYE_RAGE_RED);
  canvas.fillTriangle(eyeRightX() + shakeX,        yTop + slope - 8 + shakeY,
                      eyeRightX() + shakeX + w,    yTop - 8 + shakeY,
                      eyeRightX() + shakeX + w/2,  yTop - 18 + shakeY, EYE_RAGE_RED);

  float pulse = 1.0f + sinf((float)t * 0.025f) * 0.3f;
  int pR = (int)(4 * pulse);
  if (pR > 1) {
    canvas.fillCircle(eyeLeftX(w)  + shakeX + w/2 + 5, yTop + h/2 + shakeY, pR, EYE_WHITE);
    canvas.fillCircle(eyeRightX()  + shakeX + w/2 - 5, yTop + h/2 + shakeY, pR, EYE_WHITE);
  }
}

// ── FED_UP ──
void drawFedUpFace(uint32_t t) {
  uint32_t dt = t - stateEnterTime;

  int eyeH = 10, eyeW = 50;
  int yTop = CY - eyeH/2 - 22;
  canvas.fillRoundRect(eyeLeftX(eyeW), yTop, eyeW, eyeH, 5, EYE_WHITE);
  canvas.fillRoundRect(eyeRightX(),    yTop, eyeW, eyeH, 5, EYE_WHITE);

  canvas.fillCircle(EYE_CENTER_LEFT  + 4, yTop + eyeH/2 + 1, 3, BG);
  canvas.fillCircle(EYE_CENTER_RIGHT - 4, yTop + eyeH/2 + 1, 3, BG);

  float p = dt < 900 ? (float)dt / 900.0f : 1.0f;
  p = 1.0f - powf(1.0f - p, 2.5f);

  int gunStart = 240;
  int gunEnd   = 145;
  int gY = gunStart - (int)((gunStart - gunEnd) * p);

  canvas.fillRect(CX - 7, gY + 35, 14, 240 - (gY + 35), ARM_GRAY);
  canvas.fillRoundRect(CX - 10, gY + 25, 20, 14, 3, HAND_LIGHT);
  canvas.fillRoundRect(CX - 9, gY + 8, 18, 20, 2, GUN_DARK);
  canvas.fillRect(CX - 4, gY - 5, 8, 14, GUN_DARK);
  canvas.fillCircle(CX, gY - 5, 2, BG);
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

    bool inIdleSettled = (currentState == S_IDLE && transitionDone());
    bool styleHappy    = inIdleSettled && moodLevel >= 60;
    bool styleSad      = inIdleSettled && moodLevel < 30;
    bool styleBored    = inIdleSettled && moodLevel >= 30 && currentIdleMood == IDLE_BORED;
    bool styleThinking = inIdleSettled && moodLevel >= 30 && currentIdleMood == IDLE_THINKING;

    if (s.slope > 0) {
      drawAnnoyedEyeV2(xL, yT, s.rectW, s.rectH, s.slope, true,  s.color);
      drawAnnoyedEyeV2(xR, yT, s.rectW, s.rectH, s.slope, false, s.color);
    } else if (styleSad) {
      drawSadBrowEye(xL, yT, s.rectW, s.rectH, true,  s.color);
      drawSadBrowEye(xR, yT, s.rectW, s.rectH, false, s.color);
    } else if (styleBored) {
      drawBoredEye(xL, yT, s.rectW, s.rectH, s.color);
      drawBoredEye(xR, yT, s.rectW, s.rectH, s.color);
    } else if (styleThinking) {
      drawThinkingEye(xL, yT, s.rectW, s.rectH, s.color);
      drawThinkingEye(xR, yT, s.rectW, s.rectH, s.color);
    } else if (styleHappy) {
      drawHappyEye(xL, yT, s.rectW, s.rectH, true,  s.color);
      drawHappyEye(xR, yT, s.rectW, s.rectH, false, s.color);
    } else {
      canvas.fillRoundRect(xL, yT, s.rectW, s.rectH, EYE_R, s.color);
      canvas.fillRoundRect(xR, yT, s.rectW, s.rectH, EYE_R, s.color);
    }
  }
}

// ── icono de bateria baja ──
void drawLowBatteryIcon(int x, int y, uint32_t t) {
  int w = 22, h = 12;
  float blink = 0.6f + 0.4f * (sinf((float)t * 0.004f) + 1.0f) * 0.5f;
  uint16_t col = lerpColor565(BG, BAT_LOW_RED, blink);
  canvas.drawRoundRect(x, y, w, h, 2, col);
  canvas.fillRoundRect(x + 2, y + 2, w - 4, h - 4, 1, col);
  canvas.fillRect(x + w, y + 4, 2, h - 8, col);
  int cxIco = x + w / 2;
  canvas.fillRect(cxIco, y + 3, 1, 4, BG);
  canvas.fillRect(cxIco, y + 8, 1, 1, BG);
}

void drawStateExtras(uint32_t t) {
  if (!transitionDone()) return;

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
      canvas.setCursor(CX + 50, base);      canvas.print("z");
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

  if (isSpecial(currentState)) {
    switch (currentState) {
      case S_SAD:    drawSadFace(t);   break;
      case S_RAGE:   drawRageFace(t);  break;
      case S_FED_UP: drawFedUpFace(t); break;
      default: break;
    }
  } else {
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
  Serial.println("\n[Companion rev.H] init");

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
  lastMoodDecayMs  = millis();
  prevMoodTier     = moodTier(moodLevel);
  nextLookChangeAt = millis() + 1500;

  Serial.println("[Companion rev.H] ready — sleeping");
}

void loop() {
  handleTouch();
  handleMic();
  updateAudio();
  updateMood();
  updateBattery();
  updateEyeLook(millis());      // mirada random en IDLE/HAPPY/SAD
  updateIdleSubMood(millis());  // rota NORMAL/BORED/THINKING en IDLE
  refreshIdleShape();
  updateStateTimeouts();
  checkIdle();
  renderFace();
  delay(16);
}
