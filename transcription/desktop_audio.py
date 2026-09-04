import os
import sys
import json
import time
import queue
import logging
import threading
import asyncio
import numpy as np

import discord

# --- EARLY CRASH / DOUBLE-CLICK EXCEPTION HANDLING ---
def handle_exception(exc_type, exc_value, exc_traceback):
    if issubclass(exc_type, KeyboardInterrupt):
        sys.__excepthook__(exc_type, exc_value, exc_traceback)
        return
    logging.critical("Uncaught exception", exc_info=(exc_type, exc_value, exc_traceback))
    input("\nFATAL ERROR: Script halted. Press Enter to close window...")
    sys.exit(1)

sys.excepthook = handle_exception

# --- LOGGING SETUP ---
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(name)s - %(message)s'
)
logger = logging.getLogger("desktop_audio")

# --- SOUNDCARD INITIALIZATION WITH GRACEFUL FALLBACK ---
SOUNDCARD_AVAILABLE = False
sc = None
try:
    import soundcard as sc
    _test_spk = sc.default_speaker()
    SOUNDCARD_AVAILABLE = True
    logger.info(f"soundcard initialized successfully. Default speaker: {_test_spk.name if _test_spk else 'None'}")
except Exception as e:
    logger.warning(f"soundcard could not be initialized (falling back to mock mode): {e}")

# --- CREDENTIALS LOADING ---
def load_credentials(script_dir=None):
    if script_dir is None:
        script_dir = os.path.dirname(os.path.abspath(__file__)) if '__file__' in globals() else os.getcwd()
    dirs_to_check = [script_dir, os.getcwd(), os.path.dirname(script_dir)]

    for d in dirs_to_check:
        cp = os.path.join(d, "credentials.json")
        if os.path.exists(cp):
            logger.info(f"Loading credentials from {cp}")
            with open(cp, "r", encoding="utf-8") as f:
                return json.load(f)
    logger.error("credentials.json not found in search paths.")
    sys.exit(1)

# --- DISCORD AUDIO SOURCE FOR DESKTOP LOOPBACK ---
class DesktopAudioSource(discord.AudioSource):
    """
    AudioSource that captures 48kHz 16-bit stereo PCM desktop loopback audio
    and streams 20ms chunks (3840 bytes) to Discord's voice client.
    """
    FRAME_SIZE = 960  # 20ms at 48000 Hz
    CHANNELS = 2
    BYTES_PER_FRAME = FRAME_SIZE * CHANNELS * 2  # 3840 bytes

    def __init__(self):
        super().__init__()
        self.audio_queue = queue.Queue(maxsize=100)
        self.stop_event = threading.Event()
        self.capture_thread = threading.Thread(target=self._audio_capture_loop, daemon=True)
        self.capture_thread.start()

    def _audio_capture_loop(self):
        sample_rate = 48000
        mic = None

        if SOUNDCARD_AVAILABLE:
            try:
                default_speaker = sc.default_speaker()
                logger.info(f"Using default speaker: {default_speaker.name}")
                mic = sc.get_microphone(id=str(default_speaker.name), include_loopback=True)
                logger.info(f"Loopback microphone successfully resolved: {mic.name}")
            except Exception as e:
                logger.error(f"Failed to resolve loopback microphone: {e}. Falling back to mock audio stream.")
                mic = None

        if mic is None:
            logger.info("Starting DesktopAudioSource in Mock (Silence) Mode.")
            silence_pcm = b'\x00' * self.BYTES_PER_FRAME
            while not self.stop_event.is_set():
                if self.audio_queue.full():
                    try:
                        self.audio_queue.get_nowait()
                    except queue.Empty:
                        pass
                self.audio_queue.put(silence_pcm)
                time.sleep(0.02)
            return

        try:
            with mic.recorder(samplerate=sample_rate, channels=self.CHANNELS) as recorder:
                logger.info("Desktop loopback stream opened. Streaming audio to voice channel...")
                while not self.stop_event.is_set():
                    chunk = recorder.record(numframes=self.FRAME_SIZE)

                    if chunk.ndim == 1:
                        chunk = np.column_stack([chunk, chunk])
                    elif chunk.shape[1] == 1:
                        chunk = np.column_stack([chunk[:, 0], chunk[:, 0]])

                    pcm_int16 = (chunk * 32767.0).clip(-32768, 32767).astype(np.int16)
                    pcm_bytes = pcm_int16.tobytes()

                    if self.audio_queue.full():
                        try:
                            self.audio_queue.get_nowait()
                        except queue.Empty:
                            pass
                    self.audio_queue.put(pcm_bytes)
        except Exception as e:
            logger.error(f"Error in desktop audio capture loop: {e}")

    def read(self):
        try:
            return self.audio_queue.get(timeout=0.05)
        except queue.Empty:
            return b'\x00' * self.BYTES_PER_FRAME

    def is_opus(self):
        return False

    def cleanup(self):
        self.stop_event.set()
        super().cleanup()

# --- DISCORD BOT CLIENT ---
class DesktopAudioBot(discord.Client):
    def __init__(self, **kwargs):
        intents = kwargs.pop('intents', discord.Intents.all())
        super().__init__(intents=intents, **kwargs)
        self.audio_source = None

    async def connect_to_world_voice(self, guild):
        voice_channel = discord.utils.get(guild.voice_channels, name="world")
        if not voice_channel:
            logger.warning(f"Voice channel 'world' not found in guild '{guild.name}'")
            return

        try:
            if guild.voice_client:
                if guild.voice_client.is_connected():
                    logger.info(f"Already connected to voice in '{guild.name}'")
                    return
                else:
                    await guild.voice_client.disconnect(force=True)

            logger.info(f"Connecting to voice channel '{voice_channel.name}' in '{guild.name}'...")
            vc = await voice_channel.connect()
            self.audio_source = DesktopAudioSource()
            vc.play(self.audio_source)
            logger.info(f"Successfully connected to '{voice_channel.name}' and playing desktop audio.")
        except Exception as e:
            logger.error(f"Failed to connect to voice channel in '{guild.name}': {e}")

    async def on_ready(self):
        logger.info(f"Logged in as {self.user} (ID: {self.user.id})")
        for guild in self.guilds:
            await self.connect_to_world_voice(guild)

    async def close(self):
        if self.audio_source:
            self.audio_source.cleanup()
        await super().close()

if __name__ == '__main__':
    script_dir = os.path.dirname(os.path.abspath(__file__))
    config = load_credentials(script_dir)
    TOKEN = config['token']

    if not discord.opus.is_loaded():
        try:
            discord.opus.load_opus('libopus.so.0')
        except Exception as e:
            logger.warning(f"Could not load libopus.so.0: {e}")

    bot = DesktopAudioBot()
    bot.run(TOKEN)
