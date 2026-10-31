# CW-TRAINER

**DitDah Lab**: Morse lernen im Browser und mit einem echten Paddle oder einer Handtaste am LilyGO T-Embed (ESP32-S3).

## Inhalt

| Ordner | Was |
|---|---|
| `web/` | **DitDah Lab**, eine einzelne HTML-Datei, läuft in jedem Browser. Deutsch und Englisch (Einstellungen → Sprache) |
| `firmware/tembed_cw_key/` | Firmware für den LilyGO T-Embed (Originalversion mit LED-Ring) |
| `firmware/esp32s3_generic/` | Einfache Firmware für ein beliebiges ESP32-S3- oder S2-Board |

## Web-App

`web/index.html` im Browser öffnen. Übungen:

1. **Koch-Lektion:** Fünfergruppen mitschreiben, ab 90 % kommt ein neues Zeichen dazu
2. **Zeichen-Reflex:** Zeichen hören, sofort die Taste drücken, Reaktionszeit messen
3. **Mitschreiben:** Abkürzungen, Q-Gruppen, Rufzeichen, Wörter, Zahlen, mit Tempo-Anpassung
4. **Geben:** Handtaste oder Iambic A/B, Decoder, Morsebaum, Fist-Analyse
5. **QSO-Simulator:** Normales QSO, Contest, POTA

Tastatur: Handtaste = `Leertaste` oder `Strg`. Paddle: `Strg links` / `J` = Punkt, `Strg rechts` / `K` = Strich.
Genau diese Tasten sendet auch die Firmware, darum funktioniert ein echtes Paddle ohne Treiber.

Der Fortschritt wird nur im Browser dieses Geräts gespeichert (localStorage).

## Firmware T-Embed

### Anschluss

3,5-mm-Stereobuchse an den 2x4-Header:

| Klinke | T-Embed |
|---|---|
| Tip | IO16 (Punkt / Handtaste) |
| Ring | IO17 (Strich) |
| Sleeve | GND |

Interne Pull-ups sind aktiv, keine Widerstände nötig.

### Betriebsarten (Knopf kurz drücken)

| Modus | Funktion | Web-App-Einstellung |
|---|---|---|
| KEYER | Iambic B im Gerät, sendet Strg links als Tastsignal | Handtaste, Browser-Mithörton aus |
| PADDLE | Kontakte roh: Strg links / Strg rechts | Paddle, Iambic A/B |
| HANDTASTE | Handtaste an Tip, Strg links | Handtaste, Browser-Mithörton aus |
| HOEREN | Ohne PC: Koch-Gruppen hören, Lösung auf dem Display. Punkt-Paddle = Pause | – |
| ECHO | Ohne PC: Wort hören und mit dem Paddle zurückgeben | – |
| DECODER | CW über das eingebaute Mikrofon mitlesen | – |

### Drehknopf

- Drehen: aktuellen Wert ändern
- Lang drücken (> 0,8 s): nächster Wert. Angezeigt werden nur die Werte, die in der aktuellen Betriebsart wirken:

| Wert | Bedeutung | Betriebsarten |
|---|---|---|
| WPM | Zeichentempo 8–40 | KEYER, HOEREN, ECHO |
| Abstand | Pausen dehnen (Farnsworth) 1,0x–4,0x | HOEREN, ECHO |
| Lektion | Koch-Lektion 1–40 | HOEREN, ECHO |
| Iambic | Keyer-Modus A oder B | KEYER, ECHO |
| PC-Ton | Lautsprecher an/aus, wenn du am PC übst | KEYER, HANDTASTE |
| Laut | Lautstärke 0–10 | alle mit Ton |

**PC-Ton aus** schaltest du, wenn der PC selbst einen Mithörton spielt (Web-App, vBand, Vail). Sonst hörst du zwei Töne mit Verzögerung. HOEREN und ECHO behalten ihren Ton.

- Punkt-Paddle beim Einschalten gedrückt halten: Paddles tauschen
- Alle Werte bleiben gespeichert

### LED-Ring

Orange beim Tasten und Abspielen, grün/rot im ECHO, Pegelanzeige im DECODER, links/rechts für Punkt/Strich im PADDLE-Modus.

### Arduino IDE

- Board-Paket **esp32** von Espressif, Version 3.x
- Board: **ESP32S3 Dev Module**
- USB Mode: **USB-OTG (TinyUSB)**
- USB CDC On Boot: **Disabled**
- Flash Size: **16MB**, PSRAM: **OPI PSRAM**
- Bibliothek: **GFX Library for Arduino** (moononournation)
- Ohne Display: `#define USE_DISPLAY 0`

Neu flashen: Drehknopf (= BOOT) gedrückt halten, RESET drücken, loslassen.

`es7210.cpp`, `es7210.h` und `audio_hal.h` sind der Treiber für den Mikrofon-Wandler ES7210 aus dem [LilyGO T-Embed Repository](https://github.com/Xinyuan-LilyGO/T-Embed) (Espressif, MIT-Lizenz).

### Akku

LiPo 3,7 V mit JST-1,25-Stecker (2-polig), etwa 800–1000 mAh, ca. 50 × 25 × 7 mm. Vor dem Einstecken die Polarität prüfen.

## Status

Noch nicht kompiliert und nicht an echter Hardware getestet. Am ehesten muss die Mikrofon-Ansteuerung (DECODER) nachjustiert werden.
