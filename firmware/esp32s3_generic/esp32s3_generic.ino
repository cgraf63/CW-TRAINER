/*
  CW-Key USB-Interface fuer ESP32-S3 (oder ESP32-S2)
  ---------------------------------------------------
  Meldet sich am PC als USB-Tastatur. Kein Treiber, keine App.
  Funktioniert mit der CW-Lernstation, vBand und Vail.

  Zwei Betriebsarten (Umschalten: Pin MODE_PIN beim Einschalten auf GND = Keyer):

  1) PADDLE ROH (Standard)
     DIT-Kontakt  -> linke Strg-Taste
     DAH-Kontakt  -> rechte Strg-Taste
     Der Browser macht das Iambic-Timing. In der Web-App "Paddle, Iambic A/B" waehlen.
     Eine Handtaste am DIT-Kontakt (Tip) funktioniert so ebenfalls: App auf "Handtaste".

  2) INTERNER KEYER
     Der ESP32 erzeugt das Iambic-B-Timing selbst (praezise, unabhaengig vom PC)
     und sendet das Tastsignal als linke Strg-Taste. In der Web-App "Handtaste" waehlen,
     der Decoder passt sich dem Tempo an.

  Mithoerton: Piezo oder kleiner Lautsprecher (ueber Transistor) an BUZZER_PIN.
  Der lokale Ton hat keine Latenz; den Browser-Ton dann in der App ausschalten.

  Verdrahtung 3,5-mm-Stereo-Klinke (Standard bei Paddles):
     Tip    -> DIT_PIN  (GPIO 4)
     Ring   -> DAH_PIN  (GPIO 5)
     Sleeve -> GND
  Interne Pull-ups sind aktiv, keine Widerstaende noetig.

  Arduino IDE:
     Board-Paket "esp32" von Espressif, Version 3.x
     Board: ESP32S3 Dev Module (bzw. XIAO_ESP32S3, ESP32S2 Dev Module ...)
     Tools -> USB Mode: "USB-OTG (TinyUSB)"
     Tools -> USB CDC On Boot: "Disabled"
  Zum erneuten Flashen: BOOT gedrueckt halten, RESET druecken, BOOT loslassen.

  Hinweis: Der klassische ESP32 (WROOM-32) und der ESP32-C3 haben kein natives
  USB-HID und funktionieren mit diesem Sketch NICHT.
*/

#include "USB.h"
#include "USBHIDKeyboard.h"

// ---------- Einstellungen ----------
const int DIT_PIN    = 4;
const int DAH_PIN    = 5;
const int MODE_PIN   = 6;    // beim Start auf GND = interner Keyer
const int BUZZER_PIN = 7;    // -1 = kein Mithoerton
const int LED_PIN    = -1;   // optional, z.B. Onboard-LED, -1 = aus
const int POT_PIN    = -1;   // optional Poti 10k fuer Tempo (ADC-Pin, z.B. 1), -1 = festes Tempo

bool     SWAP_PADDLES = false;  // true fuer Linkshaender
int      WPM          = 20;     // Tempo des internen Keyers, falls kein Poti
const int WPM_MIN = 8, WPM_MAX = 40;
const int TONE_HZ     = 600;
const uint32_t DEBOUNCE_MS = 3;
const bool IAMBIC_B   = true;   // false = Iambic A
// -----------------------------------

USBHIDKeyboard Keyboard;

struct Contact {
  int pin;
  bool state = false;         // true = geschlossen
  bool raw = false;
  uint32_t changedAt = 0;
};
Contact ditC, dahC;
bool keyerMode = false;

void sidetone(bool on) {
  if (BUZZER_PIN >= 0) {
    if (on) ledcWriteTone(BUZZER_PIN, TONE_HZ);
    else    ledcWriteTone(BUZZER_PIN, 0);
  }
  if (LED_PIN >= 0) digitalWrite(LED_PIN, on ? HIGH : LOW);
}

// liefert true, wenn sich der entprellte Zustand geaendert hat
bool updateContact(Contact &c) {
  bool r = digitalRead(c.pin) == LOW;
  uint32_t now = millis();
  if (r != c.raw) { c.raw = r; c.changedAt = now; }
  if (c.raw != c.state && now - c.changedAt >= DEBOUNCE_MS) {
    c.state = c.raw;
    return true;
  }
  return false;
}

// ---------- Betriebsart 1: Paddle roh ----------
void loopRaw() {
  if (updateContact(ditC)) {
    if (ditC.state) Keyboard.press(KEY_LEFT_CTRL); else Keyboard.release(KEY_LEFT_CTRL);
  }
  if (updateContact(dahC)) {
    if (dahC.state) Keyboard.press(KEY_RIGHT_CTRL); else Keyboard.release(KEY_RIGHT_CTRL);
  }
  // Mithoerton nur sinnvoll fuer Handtaste: Ton solange DIT-Kontakt zu ist.
  // Bei Paddle-Betrieb macht der Browser die Elemente, darum dort keinen lokalen Ton.
}

// ---------- Betriebsart 2: interner Iambic-Keyer ----------
enum KState { IDLE, SENDING, SPACING };
KState ks = IDLE;
char curEl = 0, lastEl = 0;
bool memDit = false, memDah = false, squeezed = false;
uint32_t tEnd = 0;

uint32_t ditMs() { return 1200UL / (uint32_t)WPM; }

void keyDown() { Keyboard.press(KEY_LEFT_CTRL);   sidetone(true);  }
void keyUp()   { Keyboard.release(KEY_LEFT_CTRL); sidetone(false); }

void startElement(char el) {
  curEl = el; lastEl = el; ks = SENDING;
  squeezed = ditC.state && dahC.state;
  tEnd = millis() + (el == '.' ? ditMs() : 3 * ditMs());
  keyDown();
}

void loopKeyer() {
  bool dChanged = updateContact(ditC);
  bool aChanged = updateContact(dahC);
  uint32_t now = millis();

  // Speicher: Gegen-Paddle waehrend eines Elements gedrueckt
  if (ks != IDLE) {
    if (dChanged && ditC.state && curEl == '-') memDit = true;
    if (aChanged && dahC.state && curEl == '.') memDah = true;
    if (ditC.state && dahC.state) squeezed = true;
  }

  switch (ks) {
    case IDLE: {
      if (ditC.state || dahC.state) {
        startElement(ditC.state ? '.' : '-');   // bei gleichzeitigem Druck: Punkt zuerst
      }
      break;
    }
    case SENDING:
      if ((int32_t)(now - tEnd) >= 0) {
        keyUp();
        if (IAMBIC_B && squeezed && !(ditC.state && dahC.state)) {
          if (curEl == '.') memDah = true; else memDit = true;
        }
        curEl = 0;
        ks = SPACING;
        tEnd = now + ditMs();
      }
      break;
    case SPACING:
      if ((int32_t)(now - tEnd) >= 0) {
        bool wantDit = ditC.state || memDit;
        bool wantDah = dahC.state || memDah;
        memDit = memDah = false;
        if (wantDit && wantDah)  startElement(lastEl == '.' ? '-' : '.');
        else if (wantDit)        startElement('.');
        else if (wantDah)        startElement('-');
        else { ks = IDLE; lastEl = 0; }
      }
      break;
  }
}

void readPot() {
  if (POT_PIN < 0) return;
  static uint32_t last = 0;
  if (millis() - last < 50) return;
  last = millis();
  int v = analogRead(POT_PIN);                       // 0..4095
  int w = WPM_MIN + (v * (WPM_MAX - WPM_MIN + 1)) / 4096;
  if (abs(w - WPM) >= 1) WPM = constrain(w, WPM_MIN, WPM_MAX);
}

void setup() {
  ditC.pin = SWAP_PADDLES ? DAH_PIN : DIT_PIN;
  dahC.pin = SWAP_PADDLES ? DIT_PIN : DAH_PIN;
  pinMode(DIT_PIN, INPUT_PULLUP);
  pinMode(DAH_PIN, INPUT_PULLUP);
  pinMode(MODE_PIN, INPUT_PULLUP);
  if (LED_PIN >= 0) pinMode(LED_PIN, OUTPUT);
  if (BUZZER_PIN >= 0) { ledcAttach(BUZZER_PIN, TONE_HZ, 8); ledcWriteTone(BUZZER_PIN, 0); }
  delay(20);
  keyerMode = digitalRead(MODE_PIN) == LOW;

  Keyboard.begin();
  USB.productName(keyerMode ? "CW Key (Keyer)" : "CW Key (Paddle)");
  USB.manufacturerName("DIY");
  USB.begin();

  // Kurzes Startsignal: "R" im Raw-Modus, "K" im Keyer-Modus (nur lokal, nicht an den PC)
  const char *p = keyerMode ? "-.-" : ".-.";
  for (; *p; p++) {
    sidetone(true);  delay(*p == '.' ? 60 : 180);
    sidetone(false); delay(60);
  }
}

void loop() {
  if (keyerMode) { readPot(); loopKeyer(); }
  else {
    loopRaw();
    // Handtasten-Mithoerton im Roh-Modus: Ton folgt dem DIT-Kontakt,
    // aber nur, wenn DAH nie benutzt wurde (= vermutlich Handtaste).
    static bool dahSeen = false;
    if (dahC.state) dahSeen = true;
    static bool toneOn = false;
    bool want = !dahSeen && ditC.state;
    if (want != toneOn) { toneOn = want; sidetone(want); }
  }
}
