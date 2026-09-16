"""
Envoyer une image vers le CYD via MQTT (sans passer par Discord)
----------------------------------------------------------------
Utilisation :
  - Double-cliquer sur le script  →  une fenêtre s'ouvre pour choisir l'image
  - OU : python send_image.py mon_image.jpg
"""

import sys
import time

from common import MQTT_HOST, MQTT_PORT, TOPIC_IMAGE, create_mqtt_client, prepare_image

# ══════════════════════════════════════════════════════
# SÉLECTION DU FICHIER
# ══════════════════════════════════════════════════════
def choisir_fichier() -> str:
    try:
        import tkinter as tk
        from tkinter import filedialog
        root = tk.Tk()
        root.withdraw()
        root.wm_attributes("-topmost", True)
        chemin = filedialog.askopenfilename(
            title="Choisir une image à envoyer",
            filetypes=[
                ("Images", "*.jpg *.jpeg *.png *.bmp *.webp *.gif"),
                ("Tous les fichiers", "*.*")
            ]
        )
        root.destroy()
        return chemin
    except Exception as e:
        print(f"Impossible d'ouvrir la fenêtre de sélection : {e}")
        return ""

# ══════════════════════════════════════════════════════
# ENVOYER VIA MQTT
# ══════════════════════════════════════════════════════
def envoyer(chemin: str):
    with open(chemin, "rb") as f:
        data = prepare_image(f.read())
    print(f"Image prête : {len(data)} octets")

    published = False

    def on_connect(client, userdata, flags, reason_code, properties):
        if reason_code == 0:
            print("🔗 Connecté au broker MQTT")
            client.publish(TOPIC_IMAGE, payload=data, qos=1)
            print(f"📤 Image envoyée sur {TOPIC_IMAGE}")
        else:
            print(f"❌ Connexion refusée (code {reason_code})")

    def on_publish(client, userdata, mid, reason_code, properties):
        nonlocal published
        published = True
        print("✅ Publication confirmée par le broker")

    client = create_mqtt_client()
    client.on_connect = on_connect
    client.on_publish = on_publish

    print(f"🌐 Connexion à {MQTT_HOST}:{MQTT_PORT} ...")
    client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)

    # Attendre la confirmation (max 10 secondes)
    debut = time.time()
    client.loop_start()
    while not published and time.time() - debut < 10:
        time.sleep(0.1)
    client.loop_stop()
    client.disconnect()

    if not published:
        print("⚠️  Pas de confirmation reçue, mais l'image a probablement été envoyée.")

# ══════════════════════════════════════════════════════
# MAIN
# ══════════════════════════════════════════════════════
if __name__ == "__main__":
    if len(sys.argv) >= 2:
        chemin = sys.argv[1]
    else:
        print("Aucun fichier spécifié, ouverture de la fenêtre de sélection...")
        chemin = choisir_fichier()

    if not chemin:
        print("Aucun fichier sélectionné, abandon.")
        input("Appuyez sur Entrée pour quitter...")
        sys.exit(0)

    try:
        envoyer(chemin)
    except Exception as e:
        print(f"\nErreur : {e}")

    input("\nAppuyez sur Entrée pour quitter...")
