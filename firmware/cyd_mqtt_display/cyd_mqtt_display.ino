/*
 * CYD MQTT Display - Firmware ESP32
 * Affiche texte et images reçus via MQTT (HiveMQ Cloud TLS)
 *
 * Topics MQTT (voir config.h) :
 *   - cyd/message     → texte à afficher
 *   - cyd/image       → image JPEG 320×240
 *   - cyd/clear       → retour à l'écran de veille
 *   - cyd/brightness  → valeur 0-255 (PWM LEDC, pin 21)
 *   - cyd/bgcolor     → couleur hex "#RRGGBB" pour le fond d'écran
 *   - cyd/bip         → "1" = bip simple, ou JSON {"freq":2000,"dur":200,"repeat":3}
 *
 * Autres fonctionnalités :
 *   - Portail captif WiFi au premier démarrage (AP "CYD-MQTT-Display-Setup")
 *     → page web pour saisir SSID/password → sauvegarde en EEPROM
 *   - Double-tap écran → éteint le rétroéclairage
 *   - Simple-tap écran (si éteint) → rallume le rétroéclairage
 *   - Réception MQTT → rallume automatiquement le rétroéclairage
 *
 * Installation (bibliothèques, User_Setup.h, secrets.h) : voir README.md
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <EEPROM.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <XPT2046_Touchscreen.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <vector>
#include "config.h"    // pins, topics, identifiants

// ══════════════════════════════════════════════════════
// BACKLIGHT (PWM via LEDC)
// ══════════════════════════════════════════════════════
uint8_t brightnessLevel = 255;    // 255 = pleine luminosité
bool    backlightOn     = true;

// Configure le canal LEDC au démarrage (compatible ESP32 core v3.x)
void backlightInit() {
  // v3.x : ledcAttach(pin, freq, resolution) remplace ledcSetup + ledcAttachPin
  ledcAttach(BACKLIGHT_PIN, BACKLIGHT_FREQ, BACKLIGHT_RES);
  ledcWrite(BACKLIGHT_PIN, brightnessLevel);
}

// Allume/éteint sans changer brightnessLevel
void setBacklight(bool on) {
  backlightOn = on;
  ledcWrite(BACKLIGHT_PIN, on ? brightnessLevel : 0);
}

// Change la luminosité (0-255) et allume si éteint
void setBrightness(uint8_t val) {
  brightnessLevel = val;
  backlightOn     = (val > 0);
  ledcWrite(BACKLIGHT_PIN, val);
  Serial.printf("[BL] Luminosité : %d\n", val);
}

// ══════════════════════════════════════════════════════
// COULEUR DE FOND
// ══════════════════════════════════════════════════════
uint16_t bgColor = TFT_BLACK;   // couleur de fond active (RGB565)

// Convertit "#RRGGBB" en uint16_t RGB565
uint16_t hexToRgb565(const char* hex) {
  if (!hex || hex[0] != '#' || strlen(hex) < 7) return TFT_BLACK;
  long rgb = strtol(hex + 1, nullptr, 16);
  uint8_t r = (rgb >> 16) & 0xFF;
  uint8_t g = (rgb >> 8)  & 0xFF;
  uint8_t b =  rgb        & 0xFF;
  // RGB888 → RGB565
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Calcule si le texte doit être blanc ou noir selon la luminance du fond
uint16_t contrastTextColor(uint16_t rgb565) {
  uint8_t r = (rgb565 >> 8) & 0xF8;
  uint8_t g = (rgb565 >> 3) & 0xFC;
  uint8_t b = (rgb565 << 3) & 0xF8;
  // Luminance perçue (formule ITU-R BT.601)
  float lum = 0.299f * r + 0.587f * g + 0.114f * b;
  return (lum > 128) ? TFT_BLACK : TFT_WHITE;
}

// ══════════════════════════════════════════════════════
// TOUCH (XPT2046 — CYD standard)
// ══════════════════════════════════════════════════════
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);

// ── Détection double-tap ──
unsigned long lastTapTime      = 0;
bool          waitingSecondTap = false;
bool          touchWasPressed  = false;
unsigned long touchPressedAt   = 0;

void handleTouch() {
  bool pressed = touch.tirqTouched() && touch.touched();
  unsigned long now = millis();

  if (pressed && !touchWasPressed) {
    touchWasPressed = true;
    touchPressedAt  = now;

    if (!backlightOn) {
      setBacklight(true);
      waitingSecondTap = false;
      lastTapTime      = 0;
      return;
    }

    if (waitingSecondTap && (now - lastTapTime) <= DOUBLE_TAP_MS) {
      setBacklight(false);
      waitingSecondTap = false;
      lastTapTime      = 0;
    } else {
      waitingSecondTap = true;
      lastTapTime      = now;
    }
  }

  if (!pressed && touchWasPressed) {
    touchWasPressed = false;
  }

  if (waitingSecondTap && (now - lastTapTime) > DOUBLE_TAP_MS) {
    waitingSecondTap = false;
  }
}

// ══════════════════════════════════════════════════════
// EEPROM - stockage SSID / password WiFi
// ══════════════════════════════════════════════════════
#define EEPROM_SIZE   128
#define EEPROM_FLAG   0
#define EEPROM_SSID   1
#define EEPROM_PASS   34
#define EEPROM_VALID  0xAB

char savedSSID[33] = {0};
char savedPass[64] = {0};

void eepromLoad() {
  EEPROM.begin(EEPROM_SIZE);
  if (EEPROM.read(EEPROM_FLAG) == EEPROM_VALID) {
    for (int i = 0; i < 32; i++) savedSSID[i] = EEPROM.read(EEPROM_SSID + i);
    for (int i = 0; i < 63; i++) savedPass[i] = EEPROM.read(EEPROM_PASS + i);
    savedSSID[32] = '\0';
    savedPass[63] = '\0';
  }
}

void eepromSave(const char* ssid, const char* pass) {
  EEPROM.write(EEPROM_FLAG, EEPROM_VALID);
  for (int i = 0; i < 32; i++) EEPROM.write(EEPROM_SSID + i, i < (int)strlen(ssid) ? ssid[i] : 0);
  for (int i = 0; i < 63; i++) EEPROM.write(EEPROM_PASS + i, i < (int)strlen(pass) ? pass[i] : 0);
  EEPROM.commit();
}

void eepromClear() {
  EEPROM.write(EEPROM_FLAG, 0x00);
  EEPROM.commit();
}

// ══════════════════════════════════════════════════════
// BUZZER
// ══════════════════════════════════════════════════════
void bip(int freq = BIP_FREQ_DEF, int dureeMs = BIP_DUR_DEF) {
  tone(BUZZER_PIN, freq, dureeMs);
  delay(dureeMs + 50);
}

// bips paramétrables : fréquence, durée, répétitions
void bipCustom(int freq, int dureeMs, int repeat) {
  for (int i = 0; i < repeat; i++) {
    tone(BUZZER_PIN, freq, dureeMs);
    delay(dureeMs + 80);
  }
  noTone(BUZZER_PIN);
}

void troisBips() {
  bipCustom(BIP_FREQ_DEF, 200, BIP_REP_DEF);
}

// Paramètres bip en attente (définis depuis MQTT, joués dans loop())
bool  pendingBip       = false;
int   pendingBipFreq   = BIP_FREQ_DEF;
int   pendingBipDur    = BIP_DUR_DEF;
int   pendingBipRepeat = 1;

// ══════════════════════════════════════════════════════
// DISPLAY
// ══════════════════════════════════════════════════════
TFT_eSPI tft = TFT_eSPI();

uint8_t imgBuffer[IMG_BUF_SIZE];
size_t  imgLen = 0;

// ══════════════════════════════════════════════════════
// MQTT
// ══════════════════════════════════════════════════════
WiFiClientSecure wifiClient;
PubSubClient     mqttClient(wifiClient);

// ══════════════════════════════════════════════════════
// ÉTAT
// ══════════════════════════════════════════════════════
enum DisplayMode { MODE_IDLE, MODE_TEXT, MODE_IMAGE };
DisplayMode currentMode = MODE_IDLE;
String      currentText = "";
bool        needsRedraw = false;
bool        needsBip    = false;

// ══════════════════════════════════════════════════════
// CALLBACK TJpg_Decoder → TFT
// ══════════════════════════════════════════════════════
bool tftOutputCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= tft.height()) return 0;
  tft.pushImage(x, y, w, h, bitmap);
  return 1;
}

// ══════════════════════════════════════════════════════
// AFFICHAGE
// ══════════════════════════════════════════════════════
void drawIdle() {
  tft.fillScreen(bgColor);
  uint16_t textCol = contrastTextColor(bgColor);
  tft.setTextColor(textCol, bgColor);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("En attente...", 160, 120);
}

void drawText(const String& msg) {
  tft.fillScreen(bgColor);

  uint16_t borderCol = contrastTextColor(bgColor);

  tft.drawRoundRect(10, 10, 300, 220, 12, borderCol);
  tft.drawRoundRect(12, 12, 296, 216, 10,
    (borderCol == TFT_WHITE) ? TFT_LIGHTGREY : TFT_DARKGREY);

  tft.fillCircle(30,  30,  5, borderCol);
  tft.fillCircle(290, 30,  5, borderCol);
  tft.fillCircle(30,  210, 5, borderCol);
  tft.fillCircle(290, 210, 5, borderCol);

  tft.setTextColor(borderCol, bgColor);
  tft.setTextDatum(MC_DATUM);

  const int TEXT_X     = 160;
  const int TEXT_Y_MIN = 22;
  const int TEXT_Y_MAX = 205;
  const int TEXT_W     = 280;
  const int TEXT_H     = TEXT_Y_MAX - TEXT_Y_MIN;

  struct SizeConfig { uint8_t gfxSize; int charW; int lineH; };
  SizeConfig sizes[] = {
    { 2, 12, 20 },
    { 1,  6, 10 }
  };

  uint8_t             chosenSize  = 1;
  int                 chosenLineH = 10;
  std::vector<String> chosenLines;

  for (auto& cfg : sizes) {
    int charsPerLine = TEXT_W / cfg.charW;
    std::vector<String> lines;
    int pos = 0;
    while (pos < (int)msg.length()) {
      int end = min(pos + charsPerLine, (int)msg.length());
      if (end < (int)msg.length()) {
        int space = msg.lastIndexOf(' ', end);
        if (space > pos) end = space + 1;
      }
      String line = msg.substring(pos, end);
      line.trim();
      lines.push_back(line);
      pos = end;
    }
    int totalH = (int)lines.size() * cfg.lineH;
    chosenSize  = cfg.gfxSize;
    chosenLineH = cfg.lineH;
    chosenLines = lines;
    if (totalH <= TEXT_H) break;
  }

  tft.setTextSize(chosenSize);

  int totalH = (int)chosenLines.size() * chosenLineH;
  int startY = TEXT_Y_MIN + (TEXT_H - totalH) / 2 + chosenLineH / 2;

  for (int i = 0; i < (int)chosenLines.size(); i++) {
    int y = startY + i * chosenLineH;
    if (y > TEXT_Y_MAX) break;
    tft.drawString(chosenLines[i], TEXT_X, y);
  }
}

void drawImage() {
  tft.fillScreen(bgColor);
  if (imgLen > 0) {
    TJpgDec.drawJpg(0, 0, imgBuffer, imgLen);
  }
}

// ══════════════════════════════════════════════════════
// PORTAIL CAPTIF WIFI
// ══════════════════════════════════════════════════════
WebServer portalServer(80);
DNSServer dnsServer;

const char PORTAL_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CYD MQTT Display — Configuration WiFi</title>
<style>
  body{margin:0;font-family:'Segoe UI',sans-serif;background:#0a0a0a;color:#f0f0f0;display:flex;justify-content:center;align-items:center;min-height:100vh;}
  .card{background:#1a1a1a;border:1px solid #333;border-radius:16px;padding:32px 28px;width:100%;max-width:380px;box-shadow:0 8px 32px #000a;}
  h1{margin:0 0 6px;font-size:1.4rem;letter-spacing:.04em;}
  p{margin:0 0 24px;color:#888;font-size:.85rem;}
  label{display:block;margin-bottom:6px;font-size:.85rem;color:#aaa;}
  input,select{width:100%;padding:10px 12px;border-radius:8px;border:1px solid #333;background:#111;color:#f0f0f0;font-size:1rem;box-sizing:border-box;margin-bottom:18px;outline:none;}
  input:focus,select:focus{border-color:#fff;}
  button{width:100%;padding:12px;border:none;border-radius:8px;background:#f0f0f0;color:#111;font-size:1rem;font-weight:600;cursor:pointer;transition:background .2s;}
  button:hover{background:#fff;}
  .hint{margin-top:16px;text-align:center;color:#555;font-size:.78rem;}
</style>
</head>
<body>
<div class="card">
  <h1>📡 CYD MQTT Display</h1>
  <p>Configurez le WiFi de votre écran</p>
  <form method="POST" action="/save">
    <label for="ssid">Réseau WiFi</label>
    <select id="ssid" name="ssid">NETWORKS_PLACEHOLDER</select>
    <label for="pass">Mot de passe</label>
    <input type="password" id="pass" name="pass" placeholder="Mot de passe WiFi" autocomplete="off">
    <button type="submit">Connecter</button>
  </form>
  <p class="hint">L'écran redémarrera automatiquement après la sauvegarde.</p>
</div>
</body>
</html>
)rawhtml";

String buildPortalPage() {
  int n = WiFi.scanNetworks();
  String options = "";
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    ssid.replace("\"", "&quot;");
    options += "<option value=\"" + ssid + "\">" + ssid;
    if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) options += " (ouvert)";
    options += "</option>";
  }
  if (options.isEmpty()) options = "<option value=''>Aucun réseau trouvé</option>";
  String page = String(PORTAL_HTML);
  page.replace("NETWORKS_PLACEHOLDER", options);
  return page;
}

void handlePortalRoot() { portalServer.send(200, "text/html", buildPortalPage()); }

void handlePortalSave() {
  String ssid = portalServer.arg("ssid");
  String pass = portalServer.arg("pass");

  if (ssid.length() == 0) {
    portalServer.send(400, "text/html",
      "<html><body style='background:#0a0a0a;color:#f00;font-family:sans-serif;text-align:center;padding-top:80px'>"
      "<h2>SSID vide, veuillez réessayer.</h2>"
      "<a style='color:#fff' href='/'>← Retour</a></body></html>");
    return;
  }

  eepromSave(ssid.c_str(), pass.c_str());

  portalServer.send(200, "text/html",
    "<html><body style='background:#0a0a0a;color:#f0f0f0;font-family:sans-serif;text-align:center;padding-top:80px'>"
    "<h2>✅ Sauvegardé !</h2>"
    "<p>L'écran va redémarrer et se connecter à <b>" + ssid + "</b>.</p>"
    "</body></html>");

  delay(2000);
  ESP.restart();
}

void runCaptivePortal() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(1);
  tft.drawString("Mode configuration WiFi", 160, 90);
  tft.drawString("Connectez-vous au WiFi :", 160, 110);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(WIFI_AP_NAME, 160, 130);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("puis ouvrez 192.168.4.1", 160, 150);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(WIFI_AP_NAME);
  dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));

  portalServer.on("/",     HTTP_GET,  handlePortalRoot);
  portalServer.on("/save", HTTP_POST, handlePortalSave);
  portalServer.onNotFound([]() {
    portalServer.sendHeader("Location", "http://192.168.4.1/", true);
    portalServer.send(302, "text/plain", "");
  });
  portalServer.begin();
  Serial.printf("[Portal] AP démarré : %s\n", WIFI_AP_NAME);

  while (true) {
    dnsServer.processNextRequest();
    portalServer.handleClient();
    delay(5);
  }
}

// ══════════════════════════════════════════════════════
// MQTT CALLBACK
// ══════════════════════════════════════════════════════
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t = String(topic);

  // ── Message texte ──
  if (t == TOPIC_TEXT) {
    currentText = "";
    for (unsigned int i = 0; i < length; i++) currentText += (char)payload[i];
    currentMode = MODE_TEXT;
    needsRedraw = true;
    needsBip    = true;
    if (!backlightOn) setBacklight(true);
    Serial.println("[MQTT] Texte : " + currentText);
  }

  // ── Image JPEG ──
  else if (t == TOPIC_IMAGE) {
    if (length <= IMG_BUF_SIZE) {
      memcpy(imgBuffer, payload, length);
      imgLen      = length;
      currentMode = MODE_IMAGE;
      needsRedraw = true;
      needsBip    = true;
      if (!backlightOn) setBacklight(true);
      Serial.printf("[MQTT] Image : %u octets\n", length);
    } else {
      Serial.printf("[MQTT] Image trop grande : %u (max %d)\n", length, IMG_BUF_SIZE);
    }
  }

  // ── Clear ──
  else if (t == TOPIC_CLEAR) {
    currentMode = MODE_IDLE;
    needsRedraw = true;
  }

  // ── Luminosité : payload "0" à "255" ──
  else if (t == TOPIC_BRIGHTNESS) {
    char buf[8] = {0};
    int  len    = min((int)length, 7);
    memcpy(buf, payload, len);
    int val = atoi(buf);
    val = constrain(val, 0, 255);
    setBrightness((uint8_t)val);
    // Pas de redraw : la luminosité ne change pas le contenu affiché
  }

  // ── Couleur de fond : payload "#RRGGBB" ──
  else if (t == TOPIC_BGCOLOR) {
    char buf[10] = {0};
    int  len     = min((int)length, 9);
    memcpy(buf, payload, len);
    bgColor     = hexToRgb565(buf);
    needsRedraw = true;   // redessine immédiatement avec la nouvelle couleur
    Serial.printf("[MQTT] Bgcolor : %s → 0x%04X\n", buf, bgColor);
  }

  // ── Bip configurable ──
  // Formats acceptés :
  //   "1"                              → bip simple avec valeurs par défaut
  //   {"freq":2000,"dur":200,"repeat":3}
  else if (t == TOPIC_BIP) {
    char buf[128] = {0};
    int  len      = min((int)length, 127);
    memcpy(buf, payload, len);

    pendingBipFreq   = BIP_FREQ_DEF;
    pendingBipDur    = BIP_DUR_DEF;
    pendingBipRepeat = 1;

    // Essaie de parser en JSON
    StaticJsonDocument<128> doc;
    DeserializationError err = deserializeJson(doc, buf);
    if (!err) {
      if (doc.containsKey("freq"))   pendingBipFreq   = doc["freq"]   | BIP_FREQ_DEF;
      if (doc.containsKey("dur"))    pendingBipDur    = doc["dur"]    | BIP_DUR_DEF;
      if (doc.containsKey("repeat")) pendingBipRepeat = doc["repeat"] | 1;
    }
    // Sinon payload simple ("1") → valeurs par défaut déjà fixées

    pendingBip = true;
    Serial.printf("[MQTT] Bip : freq=%d dur=%d repeat=%d\n",
                  pendingBipFreq, pendingBipDur, pendingBipRepeat);
  }
}

// ══════════════════════════════════════════════════════
// CONNEXION MQTT
// ══════════════════════════════════════════════════════
void connectMQTT() {
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(1);

  while (!mqttClient.connected()) {
    tft.fillScreen(TFT_BLACK);
    tft.drawString("Connexion MQTT...", 160, 120);
    Serial.print("[MQTT] Connexion... ");

    if (mqttClient.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS)) {
      Serial.println("OK");
      mqttClient.subscribe(TOPIC_TEXT);
      mqttClient.subscribe(TOPIC_IMAGE);
      mqttClient.subscribe(TOPIC_CLEAR);
      mqttClient.subscribe(TOPIC_BRIGHTNESS);
      mqttClient.subscribe(TOPIC_BGCOLOR);
      mqttClient.subscribe(TOPIC_BIP);
      mqttClient.publish(TOPIC_STATUS, "online");
    } else {
      Serial.printf("Echec (etat=%d), retry 5s\n", mqttClient.state());
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.drawString("MQTT echec, retry...", 160, 120);
      delay(5000);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
    }
  }
}

// ══════════════════════════════════════════════════════
// SETUP
// ══════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // ── Backlight PWM ──
  backlightInit();

  tft.init();
  tft.setRotation(2);
  tft.fillScreen(TFT_BLACK);
  delay(100);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(1);
  tft.drawString("Demarrage...", 160, 120);

  // ── Touch XPT2046 ──
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  touch.begin(touchSPI);
  touch.setRotation(2);

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tftOutputCallback);

  // ── Lecture config WiFi ──
  eepromLoad();
  bool hasConfig = (strlen(savedSSID) > 0);

  if (hasConfig) {
    tft.fillScreen(TFT_BLACK);
    tft.drawString("Connexion WiFi...", 160, 110);
    tft.drawString(savedSSID, 160, 130);

    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSSID, savedPass);

    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
      delay(500);
      tries++;
      Serial.print(".");
    }

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("\n[WiFi] Echec, lancement portail...");
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.drawString("WiFi echoue !", 160, 100);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString("Lancement config...", 160, 120);
      delay(1500);
      eepromClear();
      runCaptivePortal();
    }
  } else {
    Serial.println("[WiFi] Pas de config, lancement portail...");
    runCaptivePortal();
  }

  Serial.println("\n[WiFi] OK : " + WiFi.localIP().toString());
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("WiFi OK !", 160, 120);
  delay(800);

  wifiClient.setInsecure();
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(41000);
  mqttClient.setKeepAlive(60);

  connectMQTT();
  drawIdle();
}

// ══════════════════════════════════════════════════════
// LOOP
// ══════════════════════════════════════════════════════
void loop() {
  // ── Touch ──
  handleTouch();

  // ── MQTT ──
  if (!mqttClient.connected()) connectMQTT();
  mqttClient.loop();

  // ── Redessiner ──
  if (needsRedraw) {
    needsRedraw = false;
    switch (currentMode) {
      case MODE_TEXT:  drawText(currentText); break;
      case MODE_IMAGE: drawImage();           break;
      default:         drawIdle();            break;
    }
  }

  // ── Bip messagerie (après redraw) ──
  if (needsBip) {
    needsBip = false;
    troisBips();
  }

  // ── Bip MQTT cyd/bip ──
  if (pendingBip) {
    pendingBip = false;
    bipCustom(pendingBipFreq, pendingBipDur, pendingBipRepeat);
  }
}
