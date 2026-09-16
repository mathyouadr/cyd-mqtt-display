"""
╔══════════════════════════════════════════════════════╗
║          CYD MQTT Display — Bot Discord              ║
║  Contrôle l'écran CYD via MQTT (HiveMQ Cloud TLS)   ║
╚══════════════════════════════════════════════════════╝

Utilisation :
  python bot.py
"""

import json
import asyncio
import threading
import aiohttp
import paho.mqtt.client as mqtt

import discord
from discord import app_commands
from discord.ext import commands

from common import (
    MQTT_HOST, MQTT_PORT, MQTT_USER,
    TOPIC_TEXT, TOPIC_IMAGE, TOPIC_CLEAR, TOPIC_BRIGHTNESS, TOPIC_BGCOLOR, TOPIC_BIP,
    create_mqtt_client, prepare_image, require_env,
)

# ══════════════════════════════════════════════════════
# CONFIG DISCORD
# ══════════════════════════════════════════════════════
DISCORD_TOKEN = require_env("DISCORD_TOKEN")

# Couleur principale des embeds
EMBED_COLOR   = 0x5865F2  # bleu Discord
EMBED_SUCCESS = 0x57F287  # vert
EMBED_ERROR   = 0xED4245  # rouge
EMBED_WARN    = 0xFEE75C  # jaune

# ══════════════════════════════════════════════════════
# CLIENT MQTT GLOBAL (thread dédié)
# ══════════════════════════════════════════════════════
mqtt_client   = None
mqtt_ready    = threading.Event()
mqtt_lock     = threading.Lock()

def _build_mqtt_client() -> mqtt.Client:
    client = create_mqtt_client()

    def on_connect(c, userdata, flags, rc, props):
        if rc == 0:
            print("[MQTT] Connecté au broker.")
            mqtt_ready.set()
        else:
            print(f"[MQTT] Connexion refusée (code {rc})")

    def on_disconnect(c, userdata, flags, rc, props):
        mqtt_ready.clear()
        print("[MQTT] Déconnecté, reconnexion...")

    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect
    return client

def mqtt_thread_fn():
    global mqtt_client
    mqtt_client = _build_mqtt_client()
    mqtt_client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
    mqtt_client.loop_forever()

def mqtt_publish(topic: str, payload, qos: int = 1) -> bool:
    """Publie de manière thread-safe. Retourne True si succès."""
    if mqtt_client is None or not mqtt_ready.is_set():
        return False
    with mqtt_lock:
        result = mqtt_client.publish(topic, payload=payload, qos=qos)
    return result.rc == mqtt.MQTT_ERR_SUCCESS

# ══════════════════════════════════════════════════════
# BOT DISCORD
# ══════════════════════════════════════════════════════
intents = discord.Intents.default()

bot = commands.Bot(command_prefix="!", intents=intents)
tree = bot.tree

# ── Helpers embed ──────────────────────────────────────

def embed_ok(titre: str, desc: str, **fields) -> discord.Embed:
    e = discord.Embed(title=f"✅  {titre}", description=desc, color=EMBED_SUCCESS)
    for name, val in fields.items():
        e.add_field(name=name, value=val, inline=True)
    e.set_footer(text="CYD MQTT Display")
    return e

def embed_err(titre: str, desc: str) -> discord.Embed:
    e = discord.Embed(title=f"❌  {titre}", description=desc, color=EMBED_ERROR)
    e.set_footer(text="CYD MQTT Display")
    return e

def embed_info(titre: str, desc: str) -> discord.Embed:
    e = discord.Embed(title=f"ℹ️  {titre}", description=desc, color=EMBED_COLOR)
    e.set_footer(text="CYD MQTT Display")
    return e

def mqtt_check(interaction: discord.Interaction) -> bool:
    """Vérifie la connexion MQTT et répond en cas d'erreur."""
    return mqtt_ready.is_set()

# ══════════════════════════════════════════════════════
# SLASH COMMANDS
# ══════════════════════════════════════════════════════

@tree.command(name="texte", description="📝 Affiche un message texte sur le CYD")
@app_commands.describe(message="Le texte à afficher sur l'écran")
async def cmd_texte(interaction: discord.Interaction, message: str):
    await interaction.response.defer()

    if not mqtt_check(interaction):
        await interaction.followup.send(embed=embed_err("MQTT non connecté", "Le bot ne peut pas joindre le broker MQTT pour l'instant."))
        return

    ok = mqtt_publish(TOPIC_TEXT, message.encode("utf-8"))

    if ok:
        e = embed_ok(
            "Message envoyé !",
            f"```{message}```",
            **{"Topic": f"`{TOPIC_TEXT}`", "Taille": f"{len(message.encode())} octets"}
        )
        e.set_author(name=interaction.user.display_name, icon_url=interaction.user.display_avatar.url)
        await interaction.followup.send(embed=e)
    else:
        await interaction.followup.send(embed=embed_err("Échec d'envoi", "La publication MQTT a échoué."))


@tree.command(name="image", description="🖼️ Envoie une image sur le CYD (redimensionnement auto)")
@app_commands.describe(fichier="Image à afficher (JPG, PNG, BMP, WebP…)")
async def cmd_image(interaction: discord.Interaction, fichier: discord.Attachment):
    await interaction.response.defer()

    if not mqtt_check(interaction):
        await interaction.followup.send(embed=embed_err("MQTT non connecté", "Broker MQTT inaccessible."))
        return

    # Vérif type
    ctype = fichier.content_type or ""
    if not ctype.startswith("image/"):
        await interaction.followup.send(embed=embed_err("Fichier invalide", "Envoie uniquement une image (JPG, PNG, BMP, WebP…)."))
        return

    # Téléchargement
    async with aiohttp.ClientSession() as session:
        async with session.get(fichier.url) as resp:
            raw = await resp.read()

    # Traitement
    try:
        loop = asyncio.get_event_loop()
        data = await loop.run_in_executor(None, prepare_image, raw)
    except Exception as ex:
        await interaction.followup.send(embed=embed_err("Erreur traitement image", str(ex)))
        return

    ok = mqtt_publish(TOPIC_IMAGE, data)

    if ok:
        e = embed_ok(
            "Image envoyée !",
            "Redimensionnée en **320×240** et compressée en JPEG.",
            **{
                "Fichier original": fichier.filename,
                "Taille originale": f"{len(raw):,} octets",
                "Taille finale": f"{len(data):,} octets",
                "Topic": f"`{TOPIC_IMAGE}`"
            }
        )
        e.set_author(name=interaction.user.display_name, icon_url=interaction.user.display_avatar.url)
        # Miniature de l'image originale dans l'embed
        e.set_thumbnail(url=fichier.url)
        await interaction.followup.send(embed=e)
    else:
        await interaction.followup.send(embed=embed_err("Échec d'envoi", "La publication MQTT a échoué."))


@tree.command(name="bgcolor", description="🎨 Change la couleur de fond de l'écran")
@app_commands.describe(couleur="Couleur au format hexadécimal ex: #FF5733")
async def cmd_bgcolor(interaction: discord.Interaction, couleur: str):
    await interaction.response.defer()

    # Validation format
    couleur = couleur.strip()
    if not couleur.startswith("#"):
        couleur = "#" + couleur
    if len(couleur) != 7:
        await interaction.followup.send(embed=embed_err("Format invalide", "Utilise le format `#RRGGBB` (ex: `#FF5733`)."))
        return
    try:
        int(couleur[1:], 16)
    except ValueError:
        await interaction.followup.send(embed=embed_err("Couleur invalide", f"`{couleur}` n'est pas une couleur hexadécimale valide."))
        return

    if not mqtt_check(interaction):
        await interaction.followup.send(embed=embed_err("MQTT non connecté", "Broker MQTT inaccessible."))
        return

    ok = mqtt_publish(TOPIC_BGCOLOR, couleur.encode())

    if ok:
        # Couleur int pour l'embed
        color_int = int(couleur[1:], 16)
        e = discord.Embed(
            title="🎨  Couleur de fond changée !",
            description=f"Le fond de l'écran est maintenant `{couleur}`",
            color=color_int
        )
        e.add_field(name="Couleur hex", value=f"`{couleur}`", inline=True)
        e.add_field(name="Topic", value=f"`{TOPIC_BGCOLOR}`", inline=True)
        e.set_author(name=interaction.user.display_name, icon_url=interaction.user.display_avatar.url)
        e.set_footer(text="CYD MQTT Display")
        await interaction.followup.send(embed=e)
    else:
        await interaction.followup.send(embed=embed_err("Échec d'envoi", "La publication MQTT a échoué."))


@tree.command(name="bip", description="🔔 Déclenche un bip sur le buzzer du CYD")
@app_commands.describe(
    frequence="Fréquence en Hz (défaut: 2000)",
    duree="Durée d'un bip en ms (défaut: 150)",
    repetitions="Nombre de bips (défaut: 1)"
)
async def cmd_bip(
    interaction: discord.Interaction,
    frequence: int = 2000,
    duree: int = 150,
    repetitions: int = 1
):
    await interaction.response.defer()

    # Limites raisonnables
    frequence   = max(100, min(frequence, 8000))
    duree       = max(50,  min(duree, 2000))
    repetitions = max(1,   min(repetitions, 10))

    if not mqtt_check(interaction):
        await interaction.followup.send(embed=embed_err("MQTT non connecté", "Broker MQTT inaccessible."))
        return

    payload = json.dumps({"freq": frequence, "dur": duree, "repeat": repetitions})
    ok = mqtt_publish(TOPIC_BIP, payload.encode())

    if ok:
        e = embed_ok(
            "Bip envoyé !",
            f"Le buzzer va sonner **{repetitions}×**",
            **{
                "Fréquence": f"`{frequence}` Hz",
                "Durée": f"`{duree}` ms",
                "Répétitions": f"`{repetitions}`"
            }
        )
        e.set_author(name=interaction.user.display_name, icon_url=interaction.user.display_avatar.url)
        await interaction.followup.send(embed=e)
    else:
        await interaction.followup.send(embed=embed_err("Échec d'envoi", "La publication MQTT a échoué."))


@tree.command(name="luminosite", description="💡 Règle la luminosité de l'écran (0 = éteint, 255 = max)")
@app_commands.describe(valeur="Luminosité entre 0 et 255")
async def cmd_luminosite(interaction: discord.Interaction, valeur: int):
    await interaction.response.defer()

    valeur = max(0, min(255, valeur))

    if not mqtt_check(interaction):
        await interaction.followup.send(embed=embed_err("MQTT non connecté", "Broker MQTT inaccessible."))
        return

    ok = mqtt_publish(TOPIC_BRIGHTNESS, str(valeur).encode())

    if ok:
        pourcentage = round(valeur / 255 * 100)
        barre = "█" * (pourcentage // 10) + "░" * (10 - pourcentage // 10)
        e = embed_ok(
            "Luminosité réglée !",
            f"`{barre}` **{pourcentage}%**",
            **{"Valeur PWM": f"`{valeur}` / 255", "Topic": f"`{TOPIC_BRIGHTNESS}`"}
        )
        e.set_author(name=interaction.user.display_name, icon_url=interaction.user.display_avatar.url)
        await interaction.followup.send(embed=e)
    else:
        await interaction.followup.send(embed=embed_err("Échec d'envoi", "La publication MQTT a échoué."))


@tree.command(name="clear", description="🗑️ Efface l'écran du CYD")
async def cmd_clear(interaction: discord.Interaction):
    await interaction.response.defer()

    if not mqtt_check(interaction):
        await interaction.followup.send(embed=embed_err("MQTT non connecté", "Broker MQTT inaccessible."))
        return

    ok = mqtt_publish(TOPIC_CLEAR, b"1")

    if ok:
        e = embed_ok("Écran effacé !", "Le CYD affiche maintenant l'écran de veille.", **{"Topic": f"`{TOPIC_CLEAR}`"})
        e.set_author(name=interaction.user.display_name, icon_url=interaction.user.display_avatar.url)
        await interaction.followup.send(embed=e)
    else:
        await interaction.followup.send(embed=embed_err("Échec d'envoi", "La publication MQTT a échoué."))


@tree.command(name="status", description="📡 Vérifie l'état de la connexion MQTT")
async def cmd_status(interaction: discord.Interaction):
    await interaction.response.defer()

    connecte = mqtt_ready.is_set()
    e = discord.Embed(
        title="📡  Statut MQTT",
        color=EMBED_SUCCESS if connecte else EMBED_ERROR
    )
    e.add_field(name="Broker", value=f"`{MQTT_HOST}`", inline=False)
    e.add_field(name="Port", value=f"`{MQTT_PORT}` (TLS)", inline=True)
    e.add_field(name="Utilisateur", value=f"`{MQTT_USER}`", inline=True)
    e.add_field(name="Connexion", value="🟢 Connecté" if connecte else "🔴 Déconnecté", inline=True)
    e.add_field(
        name="Topics actifs",
        value="\n".join([
            f"• `{TOPIC_TEXT}`",
            f"• `{TOPIC_IMAGE}`",
            f"• `{TOPIC_BGCOLOR}`",
            f"• `{TOPIC_BIP}`",
            f"• `{TOPIC_BRIGHTNESS}`",
            f"• `{TOPIC_CLEAR}`"
        ]),
        inline=False
    )
    e.set_footer(text="CYD MQTT Display")
    await interaction.followup.send(embed=e)


@tree.command(name="aide", description="❓ Liste toutes les commandes du bot CYD")
async def cmd_aide(interaction: discord.Interaction):
    e = discord.Embed(
        title="📋  Commandes CYD MQTT Display",
        description="Contrôle ton écran CYD via MQTT directement depuis Discord !",
        color=EMBED_COLOR
    )
    commandes = [
        ("/texte `message`", "📝 Affiche un texte sur l'écran"),
        ("/image `fichier`", "🖼️ Envoie une image (redimensionnement auto 320×240)"),
        ("/bgcolor `#RRGGBB`", "🎨 Change la couleur de fond"),
        ("/bip `freq` `dur` `rep`", "🔔 Bip buzzer personnalisable"),
        ("/luminosite `0-255`", "💡 Règle la luminosité"),
        ("/clear", "🗑️ Efface l'écran"),
        ("/status", "📡 Vérifie la connexion MQTT"),
        ("/aide", "❓ Affiche ce message"),
    ]
    for nom, desc in commandes:
        e.add_field(name=nom, value=desc, inline=False)
    e.set_footer(text="CYD MQTT Display — HiveMQ Cloud TLS")
    await interaction.response.send_message(embed=e)


# ══════════════════════════════════════════════════════
# EVENTS
# ══════════════════════════════════════════════════════

@bot.event
async def on_ready():
    print(f"[Discord] Connecté en tant que {bot.user} (id={bot.user.id})")
    try:
        synced = await tree.sync()
        print(f"[Discord] {len(synced)} commandes slash synchronisées.")
    except Exception as e:
        print(f"[Discord] Erreur sync commandes : {e}")


# ══════════════════════════════════════════════════════
# MAIN
# ══════════════════════════════════════════════════════
if __name__ == "__main__":
    # Lancer MQTT dans un thread background
    t = threading.Thread(target=mqtt_thread_fn, daemon=True)
    t.start()
    print("[MQTT] Thread démarré, attente connexion...")

    # Lancer le bot Discord (bloquant)
    bot.run(DISCORD_TOKEN)
