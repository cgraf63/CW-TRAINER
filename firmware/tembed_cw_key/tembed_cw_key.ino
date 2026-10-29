/*
  CW-Lernstation fuer LilyGO T-Embed (ESP32-S3, Originalversion mit LED-Ring)
  ==========================================================================
  Sechs Betriebsarten, Wechsel mit kurzem Druck auf den Drehknopf:

    KEYER     Iambic-B-Keyer im Geraet. Sendet das Tastsignal per USB als linke
              Strg-Taste (Web-App: "Handtaste"). Ton ueber den Lautsprecher.
    PADDLE    Kontakte roh per USB: Punkt = Strg links, Strich = Strg rechts.
              Der Browser macht das Timing (Web-App: "Paddle, Iambic A/B").
    HANDTASTE Handtaste an Tip. Strg links per USB, lokaler Ton, Decoder.
    HOEREN    Ohne PC: spielt Koch-Gruppen, zeigt sie danach an.
              Punkt-Paddle tippen = Pause / weiter.
    ECHO      Ohne PC: spielt ein Wort, du gibst es mit dem Paddle zurueck.
              Richtig -> naechstes Wort, falsch -> nochmals.
    DECODER   Liest CW ueber das eingebaute Mikrofon mit (z.B. vom
              Funkgeraete-Lautsprecher). Die Tonhoehe wird automatisch gefunden.

  Drehknopf drehen: Wert aendern. Knopf lang druecken (> 0,8 s): naechster Wert.
    WPM      Zeichentempo 8..40
    ABSTAND  Pausen dehnen (Farnsworth) 1,0x..4,0x, fuer HOEREN und ECHO
    LEKTION  Koch-Lektion 1..40 (Zeichenvorrat fuer HOEREN und ECHO)
    LAUT     Lautstaerke 0..10
  Punkt-Paddle beim Einschalten gedrueckt halten: Paddles tauschen.
  Alle Werte bleiben nach dem Ausschalten gespeichert.

  LED-Ring: leuchtet beim Tasten, zeigt im DECODER den Pegel,
            im ECHO gruen/rot fuer richtig/falsch.

  Anschluss 3,5-mm-Stereobuchse am 2x4-Header:
    Tip -> IO16 (Punkt / Handtaste)   Ring -> IO17 (Strich)   Sleeve -> GND

  Arduino IDE:
    Board-Paket "esp32" von Espressif 3.x, Board "ESP32S3 Dev Module"
    USB Mode: "USB-OTG (TinyUSB)"   USB CDC On Boot: "Disabled"
    Flash Size: 16MB   PSRAM: "OPI PSRAM"
    Bibliothek: "GFX Library for Arduino" (moononournation)
    es7210.cpp / es7210.h / audio_hal.h muessen im selben Ordner liegen
    (Mikrofon-Wandler, Treiber von Espressif/LilyGO, MIT-Lizenz).
  Neu flashen: Drehknopf (= BOOT) gedrueckt halten, RESET druecken, loslassen.
*/

#define USE_DISPLAY 1

#include "USB.h"
#include "USBHIDKeyboard.h"
#include <Preferences.h>
#include <Wire.h>
#include "driver/i2s_std.h"
#include <math.h>
#include "es7210.h"
#if USE_DISPLAY
#include <Arduino_GFX_Library.h>
#endif

// ---------------- Pins T-Embed ----------------
#define PIN_POWER_ON   46
#define PIN_DIT        16
#define PIN_DAH        17
#define PIN_ENC_A       2
#define PIN_ENC_B       1
#define PIN_ENC_BTN     0
#define PIN_LCD_BL     15
#define PIN_LCD_DC     13
#define PIN_LCD_CS     10
#define PIN_LCD_CLK    12
#define PIN_LCD_MOSI   11
#define PIN_LCD_RES     9
#define PIN_I2S_BCLK    7
#define PIN_I2S_WS      5
#define PIN_I2S_DOUT    6
#define PIN_MIC_BCLK   47
#define PIN_MIC_WS     21
#define PIN_MIC_DIN    14
#define PIN_MIC_MCLK   48
#define PIN_IIC_SDA    18
#define PIN_IIC_SCL     8
#define PIN_APA_CLK    45
#define PIN_APA_DI     42

// ---------------- Konstanten ----------------
#define SAMPLE_RATE   16000
#define TONE_HZ       600
#define WPM_MIN       8
#define WPM_MAX       40
#define DEBOUNCE_MS   3
#define NUM_LEDS      7
#define LED_BRIGHT    4         // 1..31
#define MIC_N         128       // 8 ms Block
#define MIC_GAIN      GAIN_30DB

enum Mode : uint8_t { M_KEYER, M_PADDLE, M_STRAIGHT, M_LISTEN, M_ECHO, M_DECODER, M_COUNT };
const char *MODE_NAME[] = { "KEYER", "PADDLE", "HANDTASTE", "HOEREN", "ECHO", "DECODER" };
enum Knob : uint8_t { K_WPM, K_GAP, K_LEVEL, K_VOL, K_COUNT };

volatile uint8_t mode = M_KEYER;
volatile int  wpm = 20;
volatile int  gap10 = 20;           // Abstand-Faktor x10 (10..40)
volatile int  level = 5;            // Koch-Lektion
volatile int  volume = 6;
volatile bool keyState = false;     // Ton durch Tasten
volatile bool playTone = false;     // Ton durch Abspielen
volatile bool micKey = false;       // Ton im Mikrofon erkannt
volatile float micLevel = 0;        // 0..1 fuer Anzeige
volatile float micFreq = 0;
volatile int  encSteps = 0;
volatile bool paddleTap = false;
bool swapPaddles = false;
uint8_t knob = K_WPM;

const char *KOCH = "KMURESNAPTLWI.JZ=FOY,VG5/Q92H38B?47C1D60X";
const int KOCH_LEN = 41;

USBHIDKeyboard Keyboard;
Preferences prefs;
i2s_chan_handle_t tx = nullptr, rx = nullptr;
QueueHandle_t evq;
TaskHandle_t playerH;

struct KeyEv { bool down; uint32_t t; char el; };

#if USE_DISPLAY
Arduino_DataBus *bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_CLK, PIN_LCD_MOSI, GFX_NOT_DEFINED);
Arduino_GFX *gfx = new Arduino_ST7789(bus, PIN_LCD_RES, 1, true, 170, 320, 35, 0, 35, 0);
#endif
const uint16_t C_BG = 0x0000, C_FG = 0xFFFF, C_ACC = 0xFD20, C_DIM = 0x7BEF, C_OK = 0x07E0, C_NO = 0xF800;

uint32_t ditMs() { return 1200UL / (uint32_t)wpm; }
bool hidOut() { return mode == M_KEYER || mode == M_STRAIGHT; }

// =====================================================================
//  Morse-Tabelle
// =====================================================================
const char *CODES[][2] = {
  {".-","A"},{"-...","B"},{"-.-.","C"},{"-..","D"},{".","E"},{"..-.","F"},{"--.","G"},{"....","H"},
  {"..","I"},{".---","J"},{"-.-","K"},{".-..","L"},{"--","M"},{"-.","N"},{"---","O"},{".--.","P"},
  {"--.-","Q"},{".-.","R"},{"...","S"},{"-","T"},{"..-","U"},{"...-","V"},{".--","W"},{"-..-","X"},
  {"-.--","Y"},{"--..","Z"},{"-----","0"},{".----","1"},{"..---","2"},{"...--","3"},{"....-","4"},
  {".....","5"},{"-....","6"},{"--...","7"},{"---..","8"},{"----.","9"},{".-.-.-","."},{"--..--",","},
  {"..--..","?"},{"-..-.","/"},{"-...-","="},{".-.-.","+"},{"-....-","-"},{"...-.-","<SK>"},{"-.--.","<KN>"}
};
const char *toChar(const String &p) { for (auto &c : CODES) if (p == c[0]) return c[1]; return "*"; }
const char *toPattern(char ch) {
  for (auto &c : CODES) if (c[1][0] == ch && c[1][1] == 0) return c[0];
  return nullptr;
}

// =====================================================================
//  LED-Ring (APA102, bit-bang)
// =====================================================================
uint32_t ledCur[NUM_LEDS], ledNew[NUM_LEDS];
void apaByte(uint8_t b) {
  for (int i = 7; i >= 0; i--) { digitalWrite(PIN_APA_DI, (b >> i) & 1); digitalWrite(PIN_APA_CLK, 1); digitalWrite(PIN_APA_CLK, 0); }
}
void ledsFill(uint32_t c) { for (int i = 0; i < NUM_LEDS; i++) ledNew[i] = c; }
void ledsShow() {
  if (!memcmp(ledCur, ledNew, sizeof(ledCur))) return;
  memcpy(ledCur, ledNew, sizeof(ledCur));
  for (int i = 0; i < 4; i++) apaByte(0);
  for (int i = 0; i < NUM_LEDS; i++) {
    uint32_t c = ledCur[i];
    apaByte(0xE0 | LED_BRIGHT); apaByte(c & 0xFF); apaByte((c >> 8) & 0xFF); apaByte((c >> 16) & 0xFF);
  }
  for (int i = 0; i < 4; i++) apaByte(0xFF);
}
const uint32_t L_AMBER = 0xFF8000, L_GREEN = 0x00FF00, L_RED = 0xFF0000, L_IDLE = 0x101010, L_OFF = 0;

// =====================================================================
//  Kontakte und Keyer
// =====================================================================
struct Contact { uint8_t pin; bool state, raw; uint32_t at; };
Contact cDit{PIN_DIT, false, false, 0}, cDah{PIN_DAH, false, false, 0};

bool readContact(Contact &c) {
  bool r = digitalRead(c.pin) == LOW;
  uint32_t now = millis();
  if (r != c.raw) { c.raw = r; c.at = now; }
  if (c.raw != c.state && now - c.at >= DEBOUNCE_MS) { c.state = c.raw; return true; }
  return false;
}

void keyOut(bool down, char el = 0) {
  keyState = down;
  if (hidOut()) { if (down) Keyboard.press(KEY_LEFT_CTRL); else Keyboard.release(KEY_LEFT_CTRL); }
  KeyEv e{down, millis(), el};
  xQueueSend(evq, &e, 0);
}

enum KS { K_IDLE, K_SEND, K_SPACE };
KS ks = K_IDLE;
char curEl = 0, lastEl = 0;
bool memDit = false, memDah = false, squeezed = false;
uint32_t tEnd = 0;

void startEl(char el) {
  curEl = lastEl = el; ks = K_SEND;
  tEnd = millis() + (el == '.' ? ditMs() : 3 * ditMs());
  keyOut(true, el);
}

void keyerStep(bool dCh, bool aCh) {
  Contact &D = swapPaddles ? cDah : cDit;
  Contact &A = swapPaddles ? cDit : cDah;
  if (swapPaddles) { bool t = dCh; dCh = aCh; aCh = t; }
  uint32_t now = millis();
  if (ks != K_IDLE) {
    if (dCh && D.state && curEl == '-') memDit = true;
    if (aCh && A.state && curEl == '.') memDah = true;
    if (D.state && A.state) squeezed = true;
  }
  switch (ks) {
    case K_IDLE:
      if (D.state || A.state) { squeezed = D.state && A.state; startEl(D.state ? '.' : '-'); }
      break;
    case K_SEND:
      if ((int32_t)(now - tEnd) >= 0) {
        keyOut(false, curEl);
        if (squeezed && !(D.state && A.state)) { if (curEl == '.') memDah = true; else memDit = true; }
        squeezed = D.state && A.state;
        curEl = 0; ks = K_SPACE; tEnd = now + ditMs();
      }
      break;
    case K_SPACE:
      if ((int32_t)(now - tEnd) >= 0) {
        bool wd = D.state || memDit, wa = A.state || memDah;
        memDit = memDah = false;
        if (wd && wa) startEl(lastEl == '.' ? '-' : '.');
        else if (wd)  startEl('.');
        else if (wa)  startEl('-');
        else { ks = K_IDLE; lastEl = 0; }
      }
      break;
  }
}

void ioTask(void *) {
  uint8_t encLast = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  int8_t encAcc = 0;
  static const int8_t QDEC[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
  uint8_t lastMode = mode;
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    if (mode != lastMode) {
      ks = K_IDLE; memDit = memDah = false; lastEl = curEl = 0;
      keyState = false; Keyboard.releaseAll(); lastMode = mode;
    }
    bool dCh = readContact(cDit), aCh = readContact(cDah);
    switch (mode) {
      case M_KEYER: case M_ECHO: keyerStep(dCh, aCh); break;
      case M_PADDLE: {
        uint8_t kd = swapPaddles ? KEY_RIGHT_CTRL : KEY_LEFT_CTRL, ka = swapPaddles ? KEY_LEFT_CTRL : KEY_RIGHT_CTRL;
        if (dCh) { if (cDit.state) Keyboard.press(kd); else Keyboard.release(kd); }
        if (aCh) { if (cDah.state) Keyboard.press(ka); else Keyboard.release(ka); }
        break;
      }
      case M_STRAIGHT: if (dCh) keyOut(cDit.state); break;
      case M_LISTEN:   if (dCh && cDit.state) paddleTap = true; break;
      default: break;
    }
    uint8_t e = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
    if (e != encLast) {
      encAcc += QDEC[(encLast << 2) | e]; encLast = e;
      if (encAcc >= 4)  { encSteps++; encAcc = 0; }
      if (encAcc <= -4) { encSteps--; encAcc = 0; }
    }
    vTaskDelayUntil(&wake, 1);
  }
}

// =====================================================================
//  Lautsprecher (I2S 0)
// =====================================================================
void speakerInit() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = 3; cc.dma_frame_num = 64; cc.auto_clear = true;   // ca. 12 ms Puffer
  i2s_new_channel(&cc, &tx, nullptr);
  i2s_std_config_t sc = {};
  sc.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
  sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  sc.gpio_cfg.bclk = (gpio_num_t)PIN_I2S_BCLK;
  sc.gpio_cfg.ws   = (gpio_num_t)PIN_I2S_WS;
  sc.gpio_cfg.dout = (gpio_num_t)PIN_I2S_DOUT;
  sc.gpio_cfg.din  = I2S_GPIO_UNUSED;
  i2s_channel_init_std_mode(tx, &sc);
  i2s_channel_enable(tx);
}

void audioTask(void *) {
  const int N = 64;
  int16_t buf[N];
  float phase = 0, env = 0;
  const float step = 2.0f * PI * TONE_HZ / SAMPLE_RATE;
  const float ramp = 1.0f / (0.004f * SAMPLE_RATE);
  for (;;) {
    float target = (keyState || playTone) && mode != M_DECODER ? 1.0f : 0.0f;
    float amp = 3000.0f * volume;
    for (int i = 0; i < N; i++) {
      if (env < target) env = fminf(target, env + ramp);
      else if (env > target) env = fmaxf(target, env - ramp);
      buf[i] = (int16_t)(sinf(phase) * env * amp);
      phase += step; if (phase > 2 * PI) phase -= 2 * PI;
    }
    size_t w;
    i2s_channel_write(tx, buf, sizeof(buf), &w, portMAX_DELAY);
  }
}

// =====================================================================
//  Abspielen (fuer HOEREN und ECHO)
// =====================================================================
volatile bool playing = false, playAbort = false;
char playBuf[64];

bool waitMs(uint32_t ms) {
  uint32_t end = millis() + ms;
  while ((int32_t)(millis() - end) < 0) { if (playAbort) return false; vTaskDelay(1); }
  return true;
}

void playerTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint32_t d = ditMs();
    uint32_t extra = (uint32_t)(d * (gap10 - 10) / 10);   // zusaetzliche Pause (Farnsworth)
    bool ok = true;
    for (int i = 0; playBuf[i] && ok; i++) {
      char c = playBuf[i];
      if (c == ' ') { ok = waitMs(4 * d + 2 * extra); continue; }
      const char *p = toPattern(c); if (!p) continue;
      for (int j = 0; p[j] && ok; j++) {
        playTone = true;  ok = waitMs(p[j] == '.' ? d : 3 * d);
        playTone = false; if (ok && p[j + 1]) ok = waitMs(d);
      }
      if (ok) ok = waitMs(3 * d + extra);
    }
    playTone = false; playing = false;
  }
}

void playText(const char *s) {
  playAbort = true; while (playing) delay(1);
  strncpy(playBuf, s, sizeof(playBuf) - 1); playBuf[sizeof(playBuf) - 1] = 0;
  playAbort = false; playing = true;
  xTaskNotifyGive(playerH);
}
void playStop() { playAbort = true; while (playing) delay(1); playAbort = false; }

// =====================================================================
//  Mikrofon (ES7210, I2S 1) und Goertzel-Tonerkennung
// =====================================================================
bool micOk = false;

void micInit() {
  Wire.begin(PIN_IIC_SDA, PIN_IIC_SCL);
  audio_hal_codec_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.adc_input = AUDIO_HAL_ADC_INPUT_ALL;
  cfg.codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE;
  cfg.i2s_iface.mode = AUDIO_HAL_MODE_SLAVE;
  cfg.i2s_iface.fmt = AUDIO_HAL_I2S_NORMAL;
  cfg.i2s_iface.samples = AUDIO_HAL_16K_SAMPLES;
  cfg.i2s_iface.bits = AUDIO_HAL_BIT_LENGTH_16BITS;
  esp_err_t r = ESP_OK;
  r |= es7210_adc_init(&Wire, &cfg);
  r |= es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface);
  r |= es7210_adc_set_gain_all((es7210_gain_value_t)MIC_GAIN);
  r |= es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START);

  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  cc.dma_desc_num = 4; cc.dma_frame_num = MIC_N;
  if (i2s_new_channel(&cc, nullptr, &rx) != ESP_OK) return;
  i2s_std_config_t sc = {};
  sc.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);           // MCLK = 256 x fs
  sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  sc.gpio_cfg.mclk = (gpio_num_t)PIN_MIC_MCLK;
  sc.gpio_cfg.bclk = (gpio_num_t)PIN_MIC_BCLK;
  sc.gpio_cfg.ws   = (gpio_num_t)PIN_MIC_WS;
  sc.gpio_cfg.dout = I2S_GPIO_UNUSED;
  sc.gpio_cfg.din  = (gpio_num_t)PIN_MIC_DIN;
  if (i2s_channel_init_std_mode(rx, &sc) != ESP_OK) return;
  micOk = i2s_channel_enable(rx) == ESP_OK && r == ESP_OK;
}

float goertzel(const float *x, int n, float f) {
  float k = 2.0f * cosf(2.0f * PI * f / SAMPLE_RATE), s1 = 0, s2 = 0;
  for (int i = 0; i < n; i++) { float s = x[i] + k * s1 - s2; s2 = s1; s1 = s; }
  return s1 * s1 + s2 * s2 - k * s1 * s2;
}

void micTask(void *) {
  static int16_t raw[MIC_N * 2];
  static float x[MIC_N];
  const int NB = 25; float binAvg[NB] = {0};          // 400..1000 Hz in 25-Hz-Schritten
  float nf = 0.001f, pk = 0.01f; bool on = false; int lockBin = 8;
  for (;;) {
    if (!micOk || mode != M_DECODER) {
      if (micKey) { micKey = false; KeyEv e{false, millis(), 0}; xQueueSend(evq, &e, 0); }
      vTaskDelay(50); continue;
    }
    size_t got = 0;
    if (i2s_channel_read(rx, raw, sizeof(raw), &got, 100) != ESP_OK || got < sizeof(raw)) continue;
    float mean = 0;
    for (int i = 0; i < MIC_N; i++) { x[i] = (raw[2 * i] + raw[2 * i + 1]) * (0.5f / 32768.0f); mean += x[i]; }
    mean /= MIC_N;
    for (int i = 0; i < MIC_N; i++) {                   // Gleichanteil weg, Hann-Fenster
      float w = 0.5f - 0.5f * cosf(2.0f * PI * i / (MIC_N - 1));
      x[i] = (x[i] - mean) * w;
    }
    // Tonhoehe suchen: Bin mit dem groessten Mittelwert
    int best = 0;
    for (int b = 0; b < NB; b++) {
      float p = goertzel(x, MIC_N, 400 + 25 * b);
      binAvg[b] = binAvg[b] * 0.99f + p * 0.01f;
      if (binAvg[b] > binAvg[best]) best = b;
    }
    if (binAvg[best] > binAvg[lockBin] * 1.5f) lockBin = best;   // Hysterese gegen Springen
    micFreq = 400 + 25 * lockBin;
    float mag = sqrtf(goertzel(x, MIC_N, micFreq)) / MIC_N;

    // Rauschboden und Spitzenwert nachfuehren
    if (mag < nf) nf = mag; else nf += (mag - nf) * 0.003f;
    if (mag > pk) pk = mag; else pk -= (pk - nf) * 0.004f;
    if (pk < nf * 1.2f) pk = nf * 1.2f;
    float span = pk - nf;
    micLevel = constrain((mag - nf) / (span > 1e-6f ? span : 1e-6f), 0.0f, 1.0f);
    bool enough = pk > nf * 4.0f;                       // ca. 12 dB Abstand noetig
    bool next = on ? (enough && mag > nf + span * 0.35f) : (enough && mag > nf + span * 0.6f);
    if (next != on) {
      on = next; micKey = on;
      KeyEv e{on, millis(), 0}; xQueueSend(evq, &e, 0);
    }
  }
}

// =====================================================================
//  Decoder (in loop)
// =====================================================================
String pat, text;
float decDit = 60;
uint32_t downAt = 0, upAt = 0;
bool pending = false, spaceDone = true, textDirty = true;

void addText(const char *s) {
  text += s;
  if (text.length() > 160) text.remove(0, text.length() - 160);
  textDirty = true;
}
void clearText() { text = ""; pat = ""; pending = false; spaceDone = true; textDirty = true; }

void decodeEvents() {
  KeyEv e;
  while (xQueueReceive(evq, &e, 0) == pdTRUE) {
    if (e.down) { downAt = e.t; continue; }
    uint32_t d = e.t - downAt;
    char el = e.el;
    if (!el) {
      if (d < 12) continue;
      el = d < 2 * decDit ? '.' : '-';
      decDit = 0.75f * decDit + 0.25f * (el == '.' ? d : d / 3.0f);
      decDit = constrain(decDit, 25.0f, 400.0f);
    } else decDit = ditMs();
    pat += el; upAt = e.t; pending = true; spaceDone = false;
  }
  if (keyState || micKey) return;
  uint32_t gap = millis() - upAt;
  if (pending && gap > 2.5f * decDit) { addText(toChar(pat)); pat = ""; pending = false; }
  if (!pending && !spaceDone && gap > 5.0f * decDit) { addText(" "); spaceDone = true; }
}

// =====================================================================
//  HOEREN und ECHO
// =====================================================================
const char *WORDS[] = { "CQ","DE","K","TNX","FB","OM","UR","RST","ES","HR","QTH","NAME","OK","TU","GM","GA","GE",
  "WX","ANT","RIG","PSE","AGN","TEST","SUN","RAIN","WARM","KEY","EAR","SEA","TREE","MAN","ART","STAR","RUN",
  "NEAR","TEN","SUM","MASK","SEEN","TRAM","SKI","TEAM","MUTE","PARK","WALK","LAMP","ALPS","FUNK","MORSE",
  "BERG","SEE","WALD","ROT","WEISS","TAG","NAME","ANTENNE","WETTER","QRP","QRS","QRZ","73","5NN" };
const int NWORDS = sizeof(WORDS) / sizeof(WORDS[0]);

int kochCount() { return constrain(level + 1, 2, KOCH_LEN); }
bool inSet(const char *w) { int n = kochCount(); for (; *w; w++) { bool f = false; for (int i = 0; i < n; i++) if (KOCH[i] == *w) f = true; if (!f) return false; } return true; }
char randKoch() {
  int n = kochCount();
  if (esp_random() % 4 == 0) return KOCH[n - 1];      // neuestes Zeichen oefter
  return KOCH[esp_random() % n];
}
String makeGroup(int len) { String s; for (int i = 0; i < len; i++) s += randKoch(); return s; }
String makeEchoWord() {
  const char *cand[80]; int nc = 0;
  for (int i = 0; i < NWORDS && nc < 80; i++) if (inSet(WORDS[i])) cand[nc++] = WORDS[i];
  if (nc >= 4 && esp_random() % 2) return String(cand[esp_random() % nc]);
  return makeGroup(2 + esp_random() % 3);
}

enum LS { LS_START, LS_PLAY, LS_THINK, LS_REVEAL, LS_PAUSE };
LS ls = LS_START; String lsGroup; uint32_t lsAt = 0; int lsCount = 0;
String history;

enum ES { ES_START, ES_PLAY, ES_LISTEN, ES_RESULT };
ES es = ES_START; String esTarget; uint32_t esAt = 0; int esTries = 0, esOk = 0, esTotal = 0; bool esLastOk = false;

bool bodyDirty = true;

void runListen() {
  if (paddleTap) {
    paddleTap = false;
    if (ls == LS_PAUSE) ls = LS_START; else { playStop(); ls = LS_PAUSE; }
    bodyDirty = true;
  }
  switch (ls) {
    case LS_START:
      lsGroup = makeGroup(5); lsCount++;
      playText(lsGroup.c_str()); ls = LS_PLAY; bodyDirty = true; break;
    case LS_PLAY:                                   // abspielen
      if (!playing) { ls = LS_THINK; lsAt = millis(); }
      break;
    case LS_THINK:                                  // kurz nachdenken
      if (millis() - lsAt > 1200) {
        history = lsGroup + " " + history; if (history.length() > 80) history = history.substring(0, 78);
        ls = LS_REVEAL; lsAt = millis(); bodyDirty = true; ledsFill(L_GREEN);
      }
      break;
    case LS_REVEAL:                                 // Loesung zeigen
      if (millis() - lsAt > 2200) ls = LS_START;
      break;
    case LS_PAUSE: break;
  }
}

void runEcho() {
  switch (es) {
    case ES_START:
      if (esTries == 0) esTarget = makeEchoWord();
      clearText(); playText(esTarget.c_str()); es = ES_PLAY; bodyDirty = true; break;
    case ES_PLAY:
      if (!playing) { clearText(); es = ES_LISTEN; esAt = millis(); bodyDirty = true; }
      break;
    case ES_LISTEN: {
      String got = text; got.replace(" ", "");
      bool idle = !keyState && !pending && ks == K_IDLE;
      if (got.length() && idle && millis() - upAt > 7 * ditMs()) {
        esTotal++; esTries++;
        esLastOk = got == esTarget;
        if (esLastOk) esOk++;
        es = ES_RESULT; esAt = millis(); bodyDirty = true;
        ledsFill(esLastOk ? L_GREEN : L_RED);
      } else if (!got.length() && millis() - esAt > 10000) {
        es = ES_START;                                       // nichts gegeben: nochmals vorspielen
      }
      break;
    }
    case ES_RESULT:
      if (millis() - esAt > (esLastOk ? 1200 : 2000)) {
        if (esLastOk || esTries >= 3) esTries = 0;            // nach 3 Fehlversuchen neues Wort
        es = ES_START;
      }
      break;
  }
}

// =====================================================================
//  Anzeige
// =====================================================================
bool hdrDirty = true;

void knobText(char *buf, uint8_t k) {
  switch (k) {
    case K_WPM:   sprintf(buf, "%d WPM", wpm); break;
    case K_GAP:   sprintf(buf, "Abst %d.%dx", gap10 / 10, gap10 % 10); break;
    case K_LEVEL: sprintf(buf, "Lek %d", level); break;
    case K_VOL:   sprintf(buf, "Laut %d", volume); break;
  }
}

void drawHeader() {
#if USE_DISPLAY
  gfx->fillRect(0, 0, 320, 46, C_BG);
  gfx->setTextSize(2); gfx->setTextColor(C_ACC); gfx->setCursor(6, 5); gfx->print(MODE_NAME[mode]);
  char b[24]; knobText(b, knob);
  gfx->setTextColor(C_FG); gfx->setCursor(320 - 12 * strlen(b) - 6, 5); gfx->print(b);
  gfx->setTextSize(1); int x = 6;
  for (uint8_t k = 0; k < K_COUNT; k++) {
    knobText(b, k); gfx->setTextColor(k == knob ? C_ACC : C_DIM); gfx->setCursor(x, 30); gfx->print(b); x += 6 * strlen(b) + 14;
  }
  if (swapPaddles) { gfx->setTextColor(C_DIM); gfx->setCursor(x, 30); gfx->print("getauscht"); }
  gfx->drawFastHLine(0, 44, 320, C_DIM);
#endif
}

void printWrapped(const String &t, int y, int size, int lines) {
#if USE_DISPLAY
  int per = 316 / (6 * size);
  String s = t; int maxc = per * lines;
  if ((int)s.length() > maxc) s = s.substring(s.length() - maxc);
  gfx->setTextSize(size);
  for (int i = 0; i < (int)s.length(); i += per, y += 8 * size + 4) {
    gfx->setCursor(4, y); gfx->print(s.substring(i, min((int)s.length(), i + per)));
  }
#endif
}

void drawBody() {
#if USE_DISPLAY
  gfx->fillRect(0, 48, 320, 122, C_BG);
  gfx->setTextColor(C_FG);
  switch (mode) {
    case M_KEYER: case M_STRAIGHT:
      printWrapped(text, 52, 2, 6); break;
    case M_PADDLE:
      gfx->setTextSize(2); gfx->setCursor(4, 60); gfx->print("Kontakte -> PC");
      gfx->setTextColor(C_DIM); gfx->setCursor(4, 90); gfx->print("Timing macht die App");
      gfx->setCursor(4, 116); gfx->print("Web-App: Paddle A/B"); break;
    case M_LISTEN: {
      gfx->setTextSize(5); gfx->setCursor(10, 56);
      if (ls == LS_PAUSE) { gfx->setTextColor(C_DIM); gfx->print("PAUSE"); }
      else if (ls == LS_REVEAL) { gfx->setTextColor(C_OK); gfx->print(lsGroup); }
      else { gfx->setTextColor(C_DIM); gfx->print("?????"); }
      gfx->setTextSize(1); gfx->setTextColor(C_DIM); gfx->setCursor(220, 60); gfx->print("Gruppe "); gfx->print(lsCount);
      gfx->setCursor(220, 74); gfx->print("Punkt = Pause");
      gfx->setTextColor(C_FG); printWrapped(history, 112, 2, 3);
      break;
    }
    case M_ECHO: {
      gfx->setTextSize(1); gfx->setTextColor(C_DIM); gfx->setCursor(4, 52);
      gfx->print(es == ES_PLAY ? "Hoeren ..." : es == ES_LISTEN ? "Jetzt zurueckgeben" : "Ergebnis");
      gfx->setCursor(230, 52); gfx->print(esOk); gfx->print(" / "); gfx->print(esTotal); gfx->print(" richtig");
      gfx->setTextSize(4); gfx->setCursor(4, 70);
      if (es == ES_RESULT) { gfx->setTextColor(esLastOk ? C_OK : C_NO); gfx->print(esTarget); }
      else { gfx->setTextColor(C_DIM); for (unsigned i = 0; i < esTarget.length(); i++) gfx->print('?'); }
      gfx->setTextColor(C_FG); gfx->setTextSize(1); gfx->setCursor(4, 118); gfx->setTextColor(C_DIM); gfx->print("Du:");
      gfx->setTextColor(C_FG); gfx->setTextSize(3); gfx->setCursor(4, 132); gfx->print(text.length() > 16 ? text.substring(text.length() - 16) : text);
      break;
    }
    case M_DECODER:
      gfx->setTextSize(1); gfx->setTextColor(C_DIM); gfx->setCursor(4, 52);
      if (!micOk) { gfx->setTextColor(C_NO); gfx->print("Mikrofon nicht bereit (CC1101-Shield?)"); }
      else { gfx->print((int)micFreq); gfx->print(" Hz   ~"); gfx->print((int)(1200 / decDit)); gfx->print(" WPM"); }
      printWrapped(text, 78, 2, 4);
      break;
  }
#endif
}

void drawLevelBar() {
#if USE_DISPLAY
  static uint32_t last = 0; if (millis() - last < 60) return; last = millis();
  int w = (int)(micLevel * 140);
  gfx->fillRect(170, 52, 140, 10, C_BG);
  gfx->fillRect(170, 52, w, 10, micKey ? C_ACC : C_DIM);
  static uint32_t lastInfo = 0;
  if (millis() - lastInfo > 1000) {                     // Frequenz/Tempo jede Sekunde nachfuehren
    lastInfo = millis();
    gfx->fillRect(0, 50, 168, 14, C_BG);
    gfx->setTextSize(1); gfx->setTextColor(C_DIM); gfx->setCursor(4, 52);
    if (micOk) { gfx->print((int)micFreq); gfx->print(" Hz   ~"); gfx->print((int)(1200 / decDit)); gfx->print(" WPM"); }
  }
#endif
}

// =====================================================================
//  LEDs je Betriebsart
// =====================================================================
uint32_t flashUntil = 0;
void updateLeds() {
  if (ledNew[0] == L_GREEN || ledNew[0] == L_RED) {          // Ergebnis-Blitz halten
    if (!flashUntil) flashUntil = millis() + 600;
    if (millis() < flashUntil) { ledsShow(); return; }
    flashUntil = 0;
  }
  switch (mode) {
    case M_PADDLE:
      for (int i = 0; i < NUM_LEDS; i++) ledNew[i] = L_IDLE;
      if (cDit.state) { ledNew[0] = ledNew[1] = ledNew[2] = L_AMBER; }
      if (cDah.state) { ledNew[4] = ledNew[5] = ledNew[6] = L_AMBER; }
      break;
    case M_DECODER: {
      int n = (int)(micLevel * NUM_LEDS + 0.5f);
      for (int i = 0; i < NUM_LEDS; i++) ledNew[i] = i < n ? (micKey ? L_AMBER : 0x202000) : L_OFF;
      break;
    }
    default:
      ledsFill((keyState || playTone) ? L_AMBER : L_OFF);
      ledNew[0] = (keyState || playTone) ? L_AMBER : L_IDLE;
  }
  ledsShow();
}

// =====================================================================
void savePrefs() {
  prefs.putUChar("mode", mode); prefs.putInt("wpm", wpm); prefs.putInt("gap", gap10);
  prefs.putInt("lvl", level);   prefs.putInt("vol", volume); prefs.putBool("swap", swapPaddles);
}

void setMode(uint8_t m) {
  playStop();
  mode = m; savePrefs();
  clearText(); history = ""; lsCount = 0; ls = LS_START; es = ES_START; esTries = 0; esOk = esTotal = 0;
  if (mode == M_DECODER) decDit = 60;
  hdrDirty = bodyDirty = true;
}

void setup() {
  pinMode(PIN_POWER_ON, OUTPUT); digitalWrite(PIN_POWER_ON, HIGH);
  pinMode(PIN_DIT, INPUT_PULLUP); pinMode(PIN_DAH, INPUT_PULLUP);
  pinMode(PIN_ENC_A, INPUT_PULLUP); pinMode(PIN_ENC_B, INPUT_PULLUP); pinMode(PIN_ENC_BTN, INPUT_PULLUP);
  pinMode(PIN_APA_CLK, OUTPUT); pinMode(PIN_APA_DI, OUTPUT);
  ledsFill(L_OFF); memset(ledCur, 0xFF, sizeof(ledCur)); ledsShow();

  prefs.begin("cwkey", false);
  mode = prefs.getUChar("mode", M_KEYER); if (mode >= M_COUNT) mode = M_KEYER;
  wpm = prefs.getInt("wpm", 20); gap10 = prefs.getInt("gap", 20); level = prefs.getInt("lvl", 5);
  volume = prefs.getInt("vol", 6); swapPaddles = prefs.getBool("swap", false);
  delay(20);
  if (digitalRead(PIN_DIT) == LOW) { swapPaddles = !swapPaddles; prefs.putBool("swap", swapPaddles); }

#if USE_DISPLAY
  gfx->begin(); gfx->fillScreen(C_BG);
  pinMode(PIN_LCD_BL, OUTPUT); digitalWrite(PIN_LCD_BL, HIGH);
#endif

  Keyboard.begin();
  USB.productName("T-Embed CW Key");
  USB.manufacturerName("DIY");
  USB.begin();

  evq = xQueueCreate(64, sizeof(KeyEv));
  speakerInit();
  micInit();
  xTaskCreatePinnedToCore(audioTask,  "audio",  4096, nullptr, 4, nullptr, 0);
  xTaskCreatePinnedToCore(ioTask,     "io",     4096, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(playerTask, "player", 3072, nullptr, 3, &playerH, 0);
  xTaskCreatePinnedToCore(micTask,    "mic",    8192, nullptr, 2, nullptr, 1);
}

void loop() {
  // Drehknopf drehen
  int s = encSteps;
  if (s) {
    encSteps -= s;
    switch (knob) {
      case K_WPM:   wpm    = constrain(wpm + s, WPM_MIN, WPM_MAX); break;
      case K_GAP:   gap10  = constrain(gap10 + s, 10, 40); break;
      case K_LEVEL: level  = constrain(level + s, 1, KOCH_LEN - 1); break;
      case K_VOL:   volume = constrain(volume + s, 0, 10); break;
    }
    hdrDirty = true; savePrefs();
  }
  // Knopf: kurz = Betriebsart, lang = naechster Wert am Drehknopf
  static bool prev = false; static uint32_t at = 0; static bool longDone = false;
  bool b = digitalRead(PIN_ENC_BTN) == LOW;
  if (b && !prev) { at = millis(); longDone = false; }
  if (b && !longDone && millis() - at > 800) { knob = (knob + 1) % K_COUNT; longDone = true; hdrDirty = true; }
  if (!b && prev && !longDone && millis() - at > 30) setMode((mode + 1) % M_COUNT);
  prev = b;

  decodeEvents();
  if (mode == M_LISTEN) runListen();
  if (mode == M_ECHO)   runEcho();
  if (textDirty && mode != M_LISTEN) { bodyDirty = true; textDirty = false; }

  updateLeds();

  // Zeichnen nur, wenn gerade kein Element laeuft (SPI-Last stoert sonst das Timing nicht, aber den Ton-Task)
  if (!keyState) {
    if (hdrDirty)  { drawHeader(); hdrDirty = false; }
    if (bodyDirty) { drawBody();   bodyDirty = false; }
    if (mode == M_DECODER) drawLevelBar();
  }
  delay(2);
}
