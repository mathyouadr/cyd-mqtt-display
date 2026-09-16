# CYD MQTT Display

Afficher du texte et des images à distance sur un écran **ESP32 Cheap Yellow Display (CYD)**, via MQTT (HiveMQ Cloud, TLS) — depuis Discord ou depuis un PC.

```
Discord (/texte, /image…) ──► controller/bot.py ─────────┐
                                                         ├──► Broker MQTT ──► CYD (firmware ESP32)
PC (image locale) ─────────► controller/send_image.py ───┘
```

## Fonctionnalités

- Texte avec taille adaptée automatiquement et image JPEG 320×240
- Couleur de fond, luminosité (PWM) et buzzer pilotables à distance
- Portail captif pour configurer le Wi-Fi au premier démarrage (réseau `CYD-MQTT-Display-Setup`)
- Double-tap sur l'écran pour éteindre le rétroéclairage, simple tap pour le rallumer

## Structure

```
.
├── controller/               # Côté PC (Python)
│   ├── common.py             # Config .env, topics MQTT, traitement d'image
│   ├── bot.py                # Bot Discord (commandes slash)
│   └── send_image.py         # Envoi d'une image depuis le PC
├── firmware/                 # Côté ESP32 (Arduino)
│   ├── cyd_mqtt_display/
│   │   ├── cyd_mqtt_display.ino
│   │   ├── config.h          # Pins, topics, constantes
│   │   └── secrets.example.h # Modèle d'identifiants MQTT
│   └── TFT_eSPI/
│       └── User_Setup.h      # Configuration écran pour TFT_eSPI
├── .env.example              # Modèle d'identifiants (Python)
└── requirements.txt
```

## Prérequis

- Un broker MQTT avec TLS (ex. [HiveMQ Cloud](https://www.hivemq.com/mqtt-cloud-broker/), offre gratuite)
- Un bot Discord ([portail développeur](https://discord.com/developers/applications)) — uniquement pour `bot.py`
- Python 3.10+
- Arduino IDE 2 avec le core **ESP32 3.x** (Espressif)

## Installation

### 1. Firmware (ESP32)

1. Installer les bibliothèques via le gestionnaire de bibliothèques d'Arduino IDE :

   | Bibliothèque | Auteur | Version testée |
   |---|---|---|
   | TFT_eSPI | Bodmer | 2.5.43 |
   | TJpg_Decoder | Bodmer | 1.1.0 |
   | XPT2046_Touchscreen | Paul Stoffregen | 1.4 |
   | PubSubClient | Nick O'Leary | 2.8 |
   | ArduinoJson | Benoit Blanchon | 7.4.3 |

2. Copier `firmware/TFT_eSPI/User_Setup.h` dans le dossier de la bibliothèque TFT_eSPI (`Documents/Arduino/libraries/TFT_eSPI/`), en remplaçant le fichier existant.
3. Copier `firmware/cyd_mqtt_display/secrets.example.h` en `secrets.h` (même dossier) et renseigner les identifiants MQTT.
4. Ouvrir `firmware/cyd_mqtt_display/cyd_mqtt_display.ino`, choisir la carte **ESP32 Dev Module** et téléverser.
5. Au premier démarrage, se connecter au Wi-Fi `CYD-MQTT-Display-Setup`, ouvrir `192.168.4.1` et choisir le réseau.

### 2. Controller (PC)

```bash
pip install -r requirements.txt
cp .env.example .env   # puis renseigner les identifiants
```

Lancer le bot Discord :

```bash
python controller/bot.py
```

Envoyer une image sans Discord (ou double-cliquer sur le script) :

```bash
python controller/send_image.py mon_image.jpg
```

## Commandes Discord

| Commande | Effet |
|---|---|
| `/texte message` | Affiche un texte |
| `/image fichier` | Affiche une image (redimensionnée en 320×240) |
| `/bgcolor #RRGGBB` | Change la couleur de fond |
| `/bip [frequence] [duree] [repetitions]` | Fait sonner le buzzer |
| `/luminosite 0-255` | Règle la luminosité |
| `/clear` | Revient à l'écran de veille |
| `/status` | État de la connexion MQTT |
| `/aide` | Liste des commandes |

## Topics MQTT

| Topic | Payload |
|---|---|
| `cyd/message` | Texte UTF-8 |
| `cyd/image` | JPEG 320×240, ≤ 39 Ko |
| `cyd/clear` | Quelconque |
| `cyd/brightness` | `0` à `255` |
| `cyd/bgcolor` | `#RRGGBB` |
| `cyd/bip` | `1` ou `{"freq":2000,"dur":150,"repeat":3}` |
| `cyd/status` | Publié par l'écran : `online` |

Les topics sont définis à deux endroits qui doivent rester identiques : `firmware/cyd_mqtt_display/config.h` et `controller/common.py`.
