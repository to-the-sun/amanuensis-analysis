import sys
import logging

# --- EARLY CRASH HANDLING ---
def handle_exception(exc_type, exc_value, exc_traceback):
    if issubclass(exc_type, KeyboardInterrupt):
        sys.__excepthook__(exc_type, exc_value, exc_traceback)
        return
    logging.critical("Uncaught exception", exc_info=(exc_type, exc_value, exc_traceback))
    input("\nFATAL ERROR: Script halted. Press Enter to close window...")
    sys.exit(1)

sys.excepthook = handle_exception

try:
    import os
    import collections
    import time
    import threading
    import json
    import asyncio
    import numpy as np
    import io
    from typing import Optional
    from concurrent.futures import ThreadPoolExecutor

    import llama_query
    import discord
    from discord.ext import voice_recv
    from faster_whisper import WhisperModel
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    from discord.gateway import DiscordVoiceWebSocket
    import davey
    from davey import MediaType

    # --- LOGGING ---
    logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(name)s - %(message)s')
    logger = logging.getLogger(__name__)
    logging.getLogger('discord').setLevel(logging.WARNING)
    logging.getLogger('discord.ext.voice_recv').setLevel(logging.INFO)
    logging.getLogger('faster_whisper').setLevel(logging.WARNING)

    # --- DAVE DECRYPTION PATCHES ---
    from discord.ext.voice_recv import rtp
    from discord.ext.voice_recv.reader import PacketDecryptor, AudioReader
    import discord.ext.voice_recv.opus as opus_module

    # 1. Broaden supported modes
    if 'aead_aes256_gcm_rtpsize' not in voice_recv.VoiceRecvClient.supported_modes:
        voice_recv.VoiceRecvClient.supported_modes += ('aead_aes256_gcm_rtpsize',)

    if 'aead_aes256_gcm_rtpsize' not in PacketDecryptor.supported_modes:
        PacketDecryptor.supported_modes.append('aead_aes256_gcm_rtpsize')

    # 2. Patch AudioReader to pass voice_client to the decryptor
    _orig_reader_init = AudioReader.__init__
    def patched_reader_init(self, sink, voice_client, **kwargs):
        _orig_reader_init(self, sink, voice_client, **kwargs)
        self.decryptor.voice_client = voice_client
    AudioReader.__init__ = patched_reader_init

    # 3. Patch PacketDecryptor
    _orig_decryptor_init = PacketDecryptor.__init__
    def _patched_decryptor_init(self, mode, secret_key):
        self._secret_key = bytes(secret_key)
        self._aesgcm = AESGCM(self._secret_key)
        self._dave_roc = collections.defaultdict(int)
        self._dave_last_seq = {}
        return _orig_decryptor_init(self, mode, secret_key)
    PacketDecryptor.__init__ = _patched_decryptor_init

    def _decrypt_rtp_aead_aes256_gcm_rtpsize(self, packet):
        packet.adjust_rtpsize()
        header = bytes(packet.header)
        nonce = bytearray(12)
        nonce[:4] = packet.nonce

        # Layer 1: Outer RTP Decryption
        try:
            res = self._aesgcm.decrypt(bytes(nonce), bytes(packet.data), header)
        except Exception as e:
            if packet.sequence % 100 == 0:
                logger.error(f"Outer Decryption Failed: SSRC={packet.ssrc}, Seq={packet.sequence}, Err={type(e).__name__}: {e}")
            return None

        if packet.extended:
            offset = packet.update_ext_headers(res)
            res = res[offset:]

        # Layer 2: DAVE/E2EE Decryption
        vc = getattr(self, 'voice_client', None)
        if not vc:
            return res

        state = getattr(vc, '_connection', None)
        dave = getattr(state, 'dave_session', None)

        # Track sequence for ROC
        seq = packet.sequence
        ssrc = packet.ssrc
        last_seq = self._dave_last_seq.get(ssrc, seq)
        if seq < last_seq and (last_seq - seq) > 32768:
            self._dave_roc[ssrc] += 1
        elif seq > last_seq and (seq - last_seq) > 32768:
            self._dave_roc[ssrc] -= 1
        self._dave_last_seq[ssrc] = seq
        roc = self._dave_roc[ssrc]

        if dave and dave.ready:
            uid = vc._get_id_from_ssrc(ssrc)
            if uid:
                try:
                    dec = dave.decrypt(uid, MediaType.audio, res)
                    if seq % 500 == 0:
                        logger.info(f"DAVE OK: SSRC={ssrc}, UID={uid}, Seq={seq}, ROC={roc}")
                    return dec
                except Exception as e:
                    if seq % 100 == 0:
                        logger.warning(f"DAVE FAIL: SSRC={ssrc}, UID={uid}, Seq={seq}, ROC={roc}, Err={type(e).__name__}: {e}")
                    return None

        return res

    PacketDecryptor._decrypt_rtp_aead_aes256_gcm_rtpsize = _decrypt_rtp_aead_aes256_gcm_rtpsize

    def _decrypt_rtcp_aead_aes256_gcm_rtpsize(self, data):
        header = data[:8]
        nonce = bytearray(12)
        nonce[:4] = data[-4:]
        ciphertext = data[8:-4]
        try:
            dec = self._aesgcm.decrypt(bytes(nonce), ciphertext, header)
            return header + dec
        except Exception as e:
            logger.error(f"RTCP Decryption Failed: Err={type(e).__name__}: {e}")
            return data

    PacketDecryptor._decrypt_rtcp_aead_aes256_gcm_rtpsize = _decrypt_rtcp_aead_aes256_gcm_rtpsize

    def _patched_callback(self, packet_data):
        packet = rtp_packet = rtcp_packet = None
        try:
            if not rtp.is_rtcp(packet_data):
                packet = rtp_packet = rtp.decode_rtp(packet_data)
                packet.decrypted_data = self.decryptor.decrypt_rtp(packet)
            else:
                packet = rtcp_packet = rtp.decode_rtcp(self.decryptor.decrypt_rtcp(packet_data))
                if not isinstance(packet, (rtp.ReceiverReportPacket, rtp.SenderReportPacket)):
                    logger.info("Received unexpected rtcp packet: type=%s, %s", packet.type, type(packet))
        except Exception as e:
            if self._is_ip_discovery_packet(packet_data): return
            logger.exception("Error unpacking packet")
        finally:
            if self.error: self.stop(); return
            if not packet: return

        if rtcp_packet:
            self.packet_router.feed_rtcp(rtcp_packet)
        elif rtp_packet:
            ssrc = rtp_packet.ssrc
            if ssrc not in self.voice_client._ssrc_to_id:
                if rtp_packet.is_silence(): return
                else: logger.debug("Received packet for unknown ssrc %s", ssrc)

            self.speaking_timer.notify(ssrc)
            try:
                self.packet_router.feed_rtp(rtp_packet)
            except Exception as e:
                logger.exception('Error processing rtp packet')
                self.error = e; self.stop()

    AudioReader.callback = _patched_callback

    _orig_decode_packet = opus_module.PacketDecoder._decode_packet
    def _patched_decode_packet(self, packet):
        if packet and packet.decrypted_data is None: return packet, b''
        try: return _orig_decode_packet(self, packet)
        except Exception: return packet, b''
    opus_module.PacketDecoder._decode_packet = _patched_decode_packet

    # --- TRANSCRIPTION SINK ---
    class WhisperTranscriptionSink(voice_recv.AudioSink):
        def __init__(self, bot, text_id):
            super().__init__()
            self.bot = bot
            self.text_id = text_id
            self.user_buffers = collections.defaultdict(bytearray)
            self.last_audio_time = collections.defaultdict(float)
            self.lock = threading.Lock()
            self.processing_task = self.bot.loop.create_task(self._process_buffers())

        def cleanup(self):
            self.processing_task.cancel()

        def wants_opus(self): return False

        def write(self, user, data):
            if data.pcm:
                with self.lock:
                    self.user_buffers[user].extend(data.pcm)
                    self.last_audio_time[user] = time.time()

        async def _process_buffers(self):
            while True:
                try:
                    await asyncio.sleep(1.0)
                    users_to_process = []
                    now = time.time()
                    with self.lock:
                        for user, buffer in list(self.user_buffers.items()):
                            if not buffer: continue
                            duration = len(buffer)/(48000*4)
                            time_since = now - self.last_audio_time[user]
                            if duration > 1.5 and (time_since > 0.8 or duration > 15.0):
                                users_to_process.append((user, bytes(buffer)))

                    for user, audio_bytes in users_to_process:
                        audio_np = np.frombuffer(audio_bytes, dtype=np.int16)
                        rms = np.sqrt(np.mean(audio_np.astype(np.float64)**2))
                        if rms > 50:
                            audio_float32 = audio_np.reshape(-1, 2).astype(np.float32) / 32768.0
                            mono_16k = audio_float32.mean(axis=1)[::3]
                            text = await self.bot.loop.run_in_executor(_executor, self._transcribe, mono_16k)
                            if text:
                                clean_text = text.strip()
                                logger.info(f"WHISPER RESULT [{user}]: {clean_text}")
                                channel = self.bot.get_channel(self.text_id)
                                if channel: await channel.send(f"**{user}**: {clean_text}")

                        with self.lock:
                            self.user_buffers[user] = self.user_buffers[user][len(audio_bytes):]
                except Exception as e:
                    logger.error(f"Error in processing loop: {e}")

        def _transcribe(self, audio_16k):
            if not MODEL: return ""
            try:
                segments, _ = MODEL.transcribe(audio_16k, beam_size=5, language='en')
                return "".join([s.text for s in segments])
            except Exception: return ""

    # --- BOT CLIENT ---
    class TranscriptionBot(discord.Client):
        def __init__(self):
            super().__init__(intents=discord.Intents.all())

        async def on_ready(self):
            logger.info(f'Logged in as {self.user} (ID: {self.user.id})')

            for guild in self.guilds:
                vc_channel = discord.utils.get(guild.voice_channels, name="world")
                text_channel = discord.utils.get(guild.text_channels, name="world")

                if vc_channel and text_channel:
                    try:
                        if guild.voice_client:
                            logger.info(f"Already connected in guild {guild.name}")
                            continue
                        client = await vc_channel.connect(cls=voice_recv.VoiceRecvClient)
                        client.listen(WhisperTranscriptionSink(self, text_channel.id))
                        logger.info(f"Connected to {vc_channel.name} in {guild.name}. Listening...")
                    except Exception as e:
                        logger.error(f"Failed to connect to voice in {guild.name}: {e}")
                else:
                    if not vc_channel:
                        logger.warning(f"Voice channel 'world' not found in {guild.name}")
                    if not text_channel:
                        logger.warning(f"Text channel 'world' not found in {guild.name}")

        async def on_message(self, message):
            if message.author == self.user:
                return

            if message.content.strip() == '/purge':
                if isinstance(message.channel, discord.TextChannel) and message.channel.name == "world":
                    try:
                        logger.info(f"Purge command received in {message.channel.name} from {message.author}")
                        total_deleted = 0
                        while True:
                            deleted = await message.channel.purge(limit=100)
                            num_deleted = len(deleted)
                            total_deleted += num_deleted
                            logger.info(f"Purged {num_deleted} messages in this chunk. Total deleted: {total_deleted}")
                            if num_deleted < 100:
                                break
                            await asyncio.sleep(1.5)
                        logger.info(f"Successfully completed purge of {message.channel.name}. Total deleted: {total_deleted}")
                    except discord.Forbidden:
                        logger.error(f"Failed to purge {message.channel.name}: Missing permissions.")
                    except Exception as e:
                        logger.error(f"Error during purge in {message.channel.name}: {e}")

            if message.content.strip() == '/analyze':
                if isinstance(message.channel, discord.TextChannel) and message.channel.name == "world":
                    try:
                        logger.info(f"Analyze command received in {message.channel.name} from {message.author}")
                        transcriptions = []
                        async for msg in message.channel.history(limit=None):
                            if msg.author == self.user and msg.content.startswith("**"):
                                # Extract text after the second ** and the colon
                                # Format is **user**: text
                                parts = msg.content.split("**: ", 1)
                                if len(parts) > 1:
                                    transcriptions.append(parts[1])

                        if transcriptions:
                            # Join transcriptions in chronological order (history is newest first)
                            all_text = "\n".join(reversed(transcriptions))
                            prompt = f"The following are transcriptions of a conversation:\n\n{all_text}\n\nFind the most poetic phrase among these sentences and return ONLY that phrase."
                            # Use the same executor as Whisper for LLM query
                            response, _ = await self.loop.run_in_executor(_executor, llama_query.run_query, prompt)
                            await message.channel.send(response)
                        else:
                            await message.channel.send("No transcriptions found to analyze.")
                    except Exception as e:
                        logger.error(f"Error during analyze in {message.channel.name}: {e}")

    # --- MAIN ---
    if __name__ == '__main__':
        with open('credentials.json', 'r') as f:
            config = json.load(f)
        TOKEN = config['token']

        if not discord.opus.is_loaded():
            try: discord.opus.load_opus('libopus.so.0')
            except: pass

        logger.info("Loading Whisper model...")
        MODEL = WhisperModel('small', device='cpu', compute_type='int8')
        _executor = ThreadPoolExecutor(max_workers=1)

        bot = TranscriptionBot()
        bot.run(TOKEN)

except Exception as e:
    logging.error(f"STARTUP ERROR: {type(e).__name__}: {e}", exc_info=True)
    input("\nFATAL ERROR: Script halted during startup. Press Enter to close window...")
    sys.exit(1)
