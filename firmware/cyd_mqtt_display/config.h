// Configuration matérielle et MQTT du CYD MQTT Display
#pragma once

#include "secrets.h"   // identifiants MQTT (voir secrets.example.h)

// ══════════════════════════════════════════════════════
// MQTT
// ══════════════════════════════════════════════════════
const char* const MQTT_HOST   = SECRET_MQTT_HOST;
const int         MQTT_PORT   = SECRET_MQTT_PORT;
const char* const MQTT_USER   = SECRET_MQTT_USER;
const char* const MQTT_PASS   = SECRET_MQTT_PASS;
const char* const MQTT_CLIENT = "cyd_mqtt_display";

// Topics (identiques à controller/common.py)
const char* const TOPIC_TEXT       = "cyd/message";
const char* const TOPIC_IMAGE      = "cyd/image";
const char* const TOPIC_CLEAR      = "cyd/clear";
const char* const TOPIC_BRIGHTNESS = "cyd/brightness";   // "0" à "255"
const char* const TOPIC_BGCOLOR    = "cyd/bgcolor";      // "#RRGGBB"
const char* const TOPIC_BIP        = "cyd/bip";          // "1" ou JSON
const char* const TOPIC_STATUS     = "cyd/status";       // publié : "online"

// Taille max d'une image JPEG reçue (MAX_IMG_BYTES côté Python doit rester en dessous)
#define IMG_BUF_SIZE 40000

// ══════════════════════════════════════════════════════
// WIFI (portail captif)
// ══════════════════════════════════════════════════════
const char* const WIFI_AP_NAME = "CYD-MQTT-Display-Setup";

// ══════════════════════════════════════════════════════
// BACKLIGHT (PWM via LEDC)
// ══════════════════════════════════════════════════════
#define BACKLIGHT_PIN     21
#define BACKLIGHT_CHANNEL 0
#define BACKLIGHT_FREQ    5000
#define BACKLIGHT_RES     8        // 8 bits → valeurs 0-255

// ══════════════════════════════════════════════════════
// TOUCH (XPT2046 — CYD standard)
// ══════════════════════════════════════════════════════
#define TOUCH_CLK  25
#define TOUCH_MISO 39
#define TOUCH_MOSI 32
#define TOUCH_CS   33
#define TOUCH_IRQ  36

#define DOUBLE_TAP_MS     400
#define TOUCH_DEBOUNCE_MS 80

// ══════════════════════════════════════════════════════
// BUZZER
// ══════════════════════════════════════════════════════
#define BUZZER_PIN   22
#define BIP_FREQ_DEF 2000
#define BIP_DUR_DEF  150
#define BIP_REP_DEF  4
