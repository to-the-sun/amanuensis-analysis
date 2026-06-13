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
    import re
    import os
    import wave
    import io
    import collections
    import time
    import threading
    import json
    import asyncio
    import numpy as np
    import requests
    from typing import Optional
    from concurrent.futures import ThreadPoolExecutor

    import llama_query
    import discord
    from discord import app_commands
    from discord.ext import voice_recv
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    from discord.gateway import DiscordVoiceWebSocket
    import davey
    from davey import MediaType

    # --- LOGGING ---
    logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(name)s - %(message)s')
    logger = logging.getLogger(__name__)
    logging.getLogger('discord').setLevel(logging.WARNING)
    logging.getLogger('discord.ext.voice_recv').setLevel(logging.INFO)

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
        self.consecutive_failures = 0
        self.last_success = time.time()
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
            self.consecutive_failures += 1
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
                    self.consecutive_failures = 0
                    self.last_success = time.time()
                    return dec
                except Exception as e:
                    self.consecutive_failures += 1
                    if seq % 100 == 0:
                        logger.warning(f"DAVE FAIL: SSRC={ssrc}, UID={uid}, Seq={seq}, ROC={roc}, Err={type(e).__name__}: {e}")
                    return None

        self.consecutive_failures = 0
        self.last_success = time.time()
        return res

    PacketDecryptor._decrypt_rtp_aead_aes256_gcm_rtpsize = _decrypt_rtp_aead_aes256_gcm_rtpsize

    def _decrypt_rtcp_aead_aes256_gcm_rtpsize(self, data):
        header = data[:8]
        nonce = bytearray(12)
        nonce[:4] = data[-4:]
        ciphertext = data[8:-4]
        try:
            dec = self._aesgcm.decrypt(bytes(nonce), ciphertext, header)
            self.consecutive_failures = 0
            self.last_success = time.time()
            return header + dec
        except Exception as e:
            self.consecutive_failures += 10
            logger.error(f"RTCP Decryption Failed (Failures={self.consecutive_failures}): Err={type(e).__name__}: {e}")
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
    class AquaTranscriptionSink(voice_recv.AudioSink):
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

        def _poetic_parse(self, text):
            # Split by punctuation: . ! ? , ; : ( ) and dashes (em, en, double-hyphen, spaced-hyphen)
            # but NOT internal hyphens.
            # We use | as a temporary delimiter for dashes to simplify splitting
            normalized = re.sub(r'—|–|--|\s-\s', '|', text)
            parts = re.split(r'[.!?,;:()|]', normalized)

            lines = []
            for part in parts:
                # Remove remaining punctuation except hyphens
                clean = re.sub(r'[^a-zA-Z0-9\s-]', '', part)
                clean = clean.strip()
                if clean:
                    # Lowercase the first character
                    clean = clean[0].lower() + clean[1:]
                    lines.append(clean)
            return '\n'.join(lines)

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
                                clean_text = self._poetic_parse(text)
                                logger.info(f"AQUA RESULT [{user}]:\n{clean_text}")
                                channel = self.bot.get_channel(self.text_id)
                                if channel: await channel.send(f"**{user}**:\n{clean_text}")

                        with self.lock:
                            self.user_buffers[user] = self.user_buffers[user][len(audio_bytes):]
                except Exception as e:
                    logger.error(f"Error in processing loop: {e}")

        def _transcribe(self, audio_16k):
            try:
                # Convert float32 mono_16k back to int16 PCM
                audio_int16 = (audio_16k * 32767).astype(np.int16)

                # Write to in-memory WAV file
                buffer = io.BytesIO()
                buffer.name = "audio.wav"
                with wave.open(buffer, 'wb') as wav_file:
                    wav_file.setnchannels(1)
                    wav_file.setsampwidth(2)
                    wav_file.setframerate(16000)
                    wav_file.writeframes(audio_int16.tobytes())

                buffer.seek(0)

                url = "https://api.aquavoice.com/api/v1/audio/transcriptions"
                headers = {"Authorization": f"Bearer {AQUA_KEY}"}
                files = {"file": (buffer.name, buffer, "audio/wav")}
                data = {"model": "avalon-v1.5"}

                response = requests.post(url, headers=headers, files=files, data=data)
                response.raise_for_status()
                return response.json().get("text", "")
            except Exception as e:
                logger.error(f"Aqua transcription error: {e}")
                return ""

    # --- BOT CLIENT ---
    class TranscriptionBot(discord.Client):
        def __init__(self):
            super().__init__(intents=discord.Intents.all())
            self.tree = app_commands.CommandTree(self)
            self.context_window_size = 2048
            self.prompt_overhead = 0

        async def setup_hook(self):
            @self.tree.command(name="purge", description="Purge all messages in the world channel")
            async def purge(interaction: discord.Interaction):
                await self.purge_logic(interaction)

            @self.tree.command(name="analyze", description="Identify the most poetic phrase in the world channel")
            async def analyze(interaction: discord.Interaction):
                await self.analyze_logic(interaction)

            self.context_window_size = llama_query.get_context_window_size()
            self.prompt_overhead = llama_query.get_prompt_overhead()
            logger.info(f"Context window size: {self.context_window_size}, Prompt overhead: {self.prompt_overhead}")

            self.loop.create_task(self.health_check_loop())
            await self.tree.sync()
            logger.info("Application commands synced and health check loop started.")

        async def purge_logic(self, interaction: discord.Interaction):
            if not (isinstance(interaction.channel, discord.TextChannel) and interaction.channel.name == "world"):
                await interaction.response.send_message("This command can only be used in the 'world' channel.", ephemeral=True)
                return

            await interaction.response.defer(ephemeral=True)
            try:
                logger.info(f"Purge command received in {interaction.channel.name} from {interaction.user}")
                total_deleted = 0
                while True:
                    deleted = await interaction.channel.purge(limit=100)
                    num_deleted = len(deleted)
                    total_deleted += num_deleted
                    logger.info(f"Purged {num_deleted} messages in this chunk. Total deleted: {total_deleted}")
                    if num_deleted < 100:
                        break
                    await asyncio.sleep(1.5)
                logger.info(f"Successfully completed purge of {interaction.channel.name}. Total deleted: {total_deleted}")
                await interaction.followup.send(f"Successfully purged {total_deleted} messages.")
            except discord.Forbidden:
                logger.error(f"Failed to purge {interaction.channel.name}: Missing permissions.")
                await interaction.followup.send("Failed to purge: Missing permissions.")
            except Exception as e:
                logger.error(f"Error during purge in {interaction.channel.name}: {e}")
                await interaction.followup.send(f"Error during purge: {e}")

        async def analyze_logic(self, interaction: discord.Interaction):
            if not (isinstance(interaction.channel, discord.TextChannel) and interaction.channel.name == "world"):
                await interaction.response.send_message("This command can only be used in the 'world' channel.", ephemeral=True)
                return

            await interaction.response.defer()
            try:
                logger.info(f"Analyze command received in {interaction.channel.name} from {interaction.user}")
                messages_to_analyze = []
                async for msg in interaction.channel.history(limit=None):
                    if msg.content.strip() in ['/analyze', '/purge']:
                        continue

                    if msg.author == self.user:
                        if msg.content.startswith("**"):
                            parts = msg.content.split("**: ", 1)
                            if len(parts) > 1:
                                messages_to_analyze.append(parts[1])
                    else:
                        messages_to_analyze.append(msg.content)

                if not messages_to_analyze:
                    await interaction.followup.send("No messages found to analyze.")
                    return

                # Process in chronological order
                messages_to_analyze.reverse()

                max_response_tokens = 128
                prompt_template = "The following is a collection of sentences from a conversation:\n\n{text}\n\nYour task is to identify the single most poetic phrase from the text above. It is extremely important that you return ONLY that phrase and nothing else. Do not explain your choice or provide any introductory text. Just the single most poetic phrase."

                # Base prompt tokens (template minus the {text} placeholder)
                base_prompt_tokens = llama_query.count_tokens(prompt_template.replace("{text}", ""))

                # We need a buffer for the previous poetic phrase and new messages
                # available_tokens = context_limit - system_overhead - response_buffer - base_prompt_overhead
                available_tokens_for_content = self.context_window_size - self.prompt_overhead - max_response_tokens - base_prompt_tokens - 20 # 20 for safety

                running_poetic_phrase = ""
                current_chunk = []
                current_chunk_tokens = 0

                async def process_chunk(chunk_texts, prev_phrase):
                    combined_text = "\n".join(chunk_texts)
                    if prev_phrase:
                        combined_text = f"{prev_phrase}\n{combined_text}"

                    full_prompt = prompt_template.format(text=combined_text)
                    logger.info(f"Processing chunk with {len(chunk_texts)} messages. Total tokens approx: {llama_query.count_tokens(full_prompt)}")
                    response, _ = await self.loop.run_in_executor(_executor, llama_query.run_query, full_prompt)
                    return response.strip()

                for msg in messages_to_analyze:
                    msg_tokens = llama_query.count_tokens(msg) + 1 # +1 for newline

                    # If a single message is too long, we might need to truncate it, but let's assume they are reasonable.
                    # If adding this message exceeds available tokens:
                    prev_phrase_tokens = llama_query.count_tokens(running_poetic_phrase) if running_poetic_phrase else 0

                    if current_chunk_tokens + msg_tokens + prev_phrase_tokens > available_tokens_for_content and current_chunk:
                        # Process the current chunk before adding the new message
                        running_poetic_phrase = await process_chunk(current_chunk, running_poetic_phrase)
                        current_chunk = []
                        current_chunk_tokens = 0
                        # Recalculate prev_phrase_tokens
                        prev_phrase_tokens = llama_query.count_tokens(running_poetic_phrase)

                    current_chunk.append(msg)
                    current_chunk_tokens += msg_tokens

                # Process final chunk
                if current_chunk:
                    running_poetic_phrase = await process_chunk(current_chunk, running_poetic_phrase)

                await interaction.followup.send(running_poetic_phrase if running_poetic_phrase else "Could not determine a poetic phrase.")

            except Exception as e:
                logger.exception(f"Error during analyze in {interaction.channel.name}: {e}")
                await interaction.followup.send(f"Error during analyze: {e}")

        async def connect_to_world(self, guild):
            vc_channel = discord.utils.get(guild.voice_channels, name="world")
            text_channel = discord.utils.get(guild.text_channels, name="world")

            if vc_channel and text_channel:
                try:
                    if guild.voice_client:
                        if guild.voice_client.is_connected():
                            return
                        else:
                            await guild.voice_client.disconnect(force=True)

                    client = await vc_channel.connect(cls=voice_recv.VoiceRecvClient)
                    client.listen(AquaTranscriptionSink(self, text_channel.id))
                    logger.info(f"Connected to {vc_channel.name} in {guild.name}. Listening...")
                except Exception as e:
                    logger.error(f"Failed to connect to voice in {guild.name}: {e}")
            else:
                if not vc_channel:
                    logger.warning(f"Voice channel 'world' not found in {guild.name}")
                if not text_channel:
                    logger.warning(f"Text channel 'world' not found in {guild.name}")

        async def health_check_loop(self):
            await self.wait_until_ready()
            while not self.is_closed():
                try:
                    await asyncio.sleep(20)
                    for vc in self.voice_clients:
                        if not isinstance(vc, voice_recv.VoiceRecvClient):
                            continue

                        reader = getattr(vc, '_reader', None)
                        if reader:
                            decryptor = getattr(reader, 'decryptor', None)
                            if decryptor:
                                failures = getattr(decryptor, 'consecutive_failures', 0)
                                last_success = getattr(decryptor, 'last_success', 0)
                                if failures > 100 or (failures > 0 and time.time() - last_success > 30):
                                    reason = f"Excessive failures ({failures})" if failures > 100 else f"Decryption stalled for {time.time() - last_success:.1f}s"
                                    logger.warning(f"{reason} detected in {vc.guild.name}. Reconnecting...")
                                    await vc.disconnect(force=True)
                                    await self.connect_to_world(vc.guild)
                        else:
                            # If we are connected but not listening, maybe we should start listening?
                            # Or just wait for the next iteration to connect_to_world if it's completely disconnected
                            if not vc.is_connected():
                                await self.connect_to_world(vc.guild)

                except Exception as e:
                    logger.error(f"Error in health check loop: {e}")

        async def on_ready(self):
            logger.info(f'Logged in as {self.user} (ID: {self.user.id})')
            for guild in self.guilds:
                await self.connect_to_world(guild)

        async def on_message(self, message):
            # We no longer handle commands here as they are migrated to Slash Commands
            pass

    # --- MAIN ---
    if __name__ == '__main__':
        with open('credentials.json', 'r') as f:
            config = json.load(f)
        TOKEN = config['token']
        AQUA_KEY = config['aqua_key']

        if not discord.opus.is_loaded():
            try: discord.opus.load_opus('libopus.so.0')
            except: pass

        _executor = ThreadPoolExecutor(max_workers=1)

        bot = TranscriptionBot()
        bot.run(TOKEN)

except Exception as e:
    logging.error(f"STARTUP ERROR: {type(e).__name__}: {e}", exc_info=True)
    input("\nFATAL ERROR: Script halted during startup. Press Enter to close window...")
    sys.exit(1)
