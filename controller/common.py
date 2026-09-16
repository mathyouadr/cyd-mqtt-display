"""
Code partagé par bot.py et send_image.py :
configuration (.env), topics MQTT, client MQTT et préparation des images.
"""

import io
import os
import ssl
from pathlib import Path

import paho.mqtt.client as mqtt
from dotenv import load_dotenv
from PIL import Image

# ══════════════════════════════════════════════════════
# CONFIG (.env à la racine du projet)
# ══════════════════════════════════════════════════════
load_dotenv(Path(__file__).resolve().parent.parent / ".env")


def require_env(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        raise SystemExit(f"Variable {name} manquante dans .env (voir .env.example)")
    return value


MQTT_HOST = require_env("MQTT_HOST")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "8883"))
MQTT_USER = require_env("MQTT_USER")
MQTT_PASS = require_env("MQTT_PASS")

# ══════════════════════════════════════════════════════
# TOPICS (identiques à firmware/cyd_mqtt_display/config.h)
# ══════════════════════════════════════════════════════
TOPIC_TEXT       = "cyd/message"
TOPIC_IMAGE      = "cyd/image"
TOPIC_CLEAR      = "cyd/clear"
TOPIC_BRIGHTNESS = "cyd/brightness"
TOPIC_BGCOLOR    = "cyd/bgcolor"
TOPIC_BIP        = "cyd/bip"

# ══════════════════════════════════════════════════════
# ÉCRAN
# ══════════════════════════════════════════════════════
MAX_IMG_BYTES = 39000   # doit rester sous IMG_BUF_SIZE du firmware
SCREEN_W      = 320
SCREEN_H      = 240


def create_mqtt_client() -> mqtt.Client:
    """Client MQTT configuré (identifiants + TLS), non connecté."""
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.tls_set(cert_reqs=ssl.CERT_NONE)
    client.tls_insecure_set(True)
    return client


def prepare_image(raw: bytes) -> bytes:
    """Redimensionne + centre l'image en 320×240 JPEG ≤ MAX_IMG_BYTES."""
    img = Image.open(io.BytesIO(raw)).convert("RGB")
    img.thumbnail((SCREEN_W, SCREEN_H), Image.LANCZOS)

    fond = Image.new("RGB", (SCREEN_W, SCREEN_H), (0, 0, 0))
    offset = ((SCREEN_W - img.width) // 2, (SCREEN_H - img.height) // 2)
    fond.paste(img, offset)
    img = fond

    qualite = 85
    while qualite >= 10:
        buf = io.BytesIO()
        img.save(buf, format="JPEG", quality=qualite)
        data = buf.getvalue()
        if len(data) <= MAX_IMG_BYTES:
            return data
        qualite -= 5

    raise ValueError(f"Image impossible à comprimer sous {MAX_IMG_BYTES} octets.")
