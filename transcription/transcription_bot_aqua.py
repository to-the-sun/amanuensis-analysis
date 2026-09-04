import sys
import os
import logging

import analyze_transcript

sys.excepthook = analyze_transcript.handle_exception

logger = logging.getLogger(__name__)

try:
    import collections
    import time
    import threading
    import asyncio
    import numpy as np

    import discord
    from discord.ext import voice_recv
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    import davey
    from davey import MediaType

    # --- DAVE DECRYPTION PATCHES ---
    from discord.ext.voice_recv import rtp
    from discord.ext.voice_recv.reader import PacketDecryptor, AudioReader
    import discord.ext.voice_recv.opus as opus_module

    if 'aead_aes256_gcm_rtpsize' not in voice_recv.VoiceRecvClient.supported_modes:
        voice_recv.VoiceRecvClient.supported_modes += ('aead_aes256_gcm_rtpsize',)

    if 'aead_aes256_gcm_rtpsize' not in PacketDecryptor.supported_modes:
        PacketDecryptor.supported_modes.append('aead_aes256_gcm_rtpsize')

    _orig_reader_init = AudioReader.__init__
    def patched_reader_init(self, sink, voice_client, **kwargs):
        _orig_reader_init(self, sink, voice_client, **kwargs)
        self.decryptor.voice_client = voice_client
    AudioReader.__init__ = patched_reader_init

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

        vc = getattr(self, 'voice_client', None)
        if not vc:
            return res

        state = getattr(vc, '_connection', None)
        dave = getattr(state, 'dave_session', None)

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
            self.lock = threading.Lock()

            self.is_active = collections.defaultdict(bool)
            self.active_buffers = collections.defaultdict(bytearray)
            self.pre_roll_buffers = collections.defaultdict(lambda: collections.deque(maxlen=15))
            self.silence_durations = collections.defaultdict(float)
            self.utterance_durations = collections.defaultdict(float)
            self.last_audio_times = collections.defaultdict(float)
            self.completed_utterances = collections.defaultdict(list)
            self.to_be_sent_buffers = collections.defaultdict(bytearray)
            self.accumulated_start_times = collections.defaultdict(lambda: None)

            self.processing_task = asyncio.create_task(self._process_buffers())

        def cleanup(self):
            self.processing_task.cancel()

        def wants_opus(self): return False

        def write(self, user, data):
            if not data.pcm:
                return

            with self.lock:
                self.last_audio_times[user] = time.time()

                chunk_np = np.frombuffer(data.pcm, dtype=np.int16)
                if len(chunk_np) == 0:
                    return
                rms = np.sqrt(np.mean(chunk_np.astype(np.float64)**2))

                chunk_len_bytes = len(data.pcm)
                chunk_duration = chunk_len_bytes / (48000 * 4)

                is_active = self.is_active[user]
                pre_roll = self.pre_roll_buffers[user]

                if not is_active:
                    pre_roll.append(data.pcm)
                    if rms > 50.0:
                        logger.info(f"VAD: Voice started for {user} (RMS: {rms:.1f})")
                        self.is_active[user] = True
                        self.active_buffers[user] = bytearray()
                        for c in pre_roll:
                            self.active_buffers[user].extend(c)
                        pre_roll.clear()
                        self.silence_durations[user] = 0.0
                        self.utterance_durations[user] = chunk_duration
                else:
                    self.active_buffers[user].extend(data.pcm)
                    self.utterance_durations[user] += chunk_duration

                    if rms < 50.0:
                        self.silence_durations[user] += chunk_duration
                    else:
                        self.silence_durations[user] = 0.0

                    if self.silence_durations[user] >= 1.0 or self.utterance_durations[user] >= 45.0:
                        reason = "silence" if self.silence_durations[user] >= 1.0 else "max duration"
                        logger.info(f"VAD: Voice ended for {user} due to {reason} (duration: {self.utterance_durations[user]:.2f}s)")

                        audio_data = bytes(self.active_buffers[user])

                        self.to_be_sent_buffers[user].extend(audio_data)
                        if self.accumulated_start_times[user] is None:
                            self.accumulated_start_times[user] = time.time()

                        accum_len_bytes = len(self.to_be_sent_buffers[user])
                        accum_duration = accum_len_bytes / (48000 * 4)

                        if accum_duration >= 10.0:
                            logger.info(f"VAD: Accumulated speech duration for {user} is {accum_duration:.2f}s (>= 10s). Sending to transcription...")
                            self.completed_utterances[user].append(bytes(self.to_be_sent_buffers[user]))
                            self.to_be_sent_buffers[user] = bytearray()
                            self.accumulated_start_times[user] = None
                        else:
                            logger.info(f"VAD: Accumulated speech duration for {user} is {accum_duration:.2f}s (< 10s). Waiting for more speech to fill the time...")

                        self.is_active[user] = False
                        self.active_buffers[user] = bytearray()
                        self.silence_durations[user] = 0.0
                        self.utterance_durations[user] = 0.0

        async def _process_buffers(self):
            while True:
                try:
                    await asyncio.sleep(0.5)
                    users_to_process = []
                    now = time.time()

                    with self.lock:
                        for user, buf in list(self.to_be_sent_buffers.items()):
                            if len(buf) > 0 and self.accumulated_start_times[user] is not None:
                                if now - self.bot.last_world_message_time > 49.0:
                                    logger.info(f"VAD: Flush timeout reached (49 seconds of no world messages) for {user}. Sending accumulated speech anyway...")
                                    self.completed_utterances[user].append(bytes(buf))
                                    self.to_be_sent_buffers[user] = bytearray()
                                    self.accumulated_start_times[user] = None

                        for user in list(self.is_active.keys()):
                            if self.is_active[user]:
                                time_since = now - self.last_audio_times[user]
                                if time_since >= 1.0:
                                    logger.info(f"VAD: Voice ended for {user} due to packet timeout (duration: {self.utterance_durations[user]:.2f}s)")
                                    audio_data = bytes(self.active_buffers[user])

                                    self.to_be_sent_buffers[user].extend(audio_data)
                                    if self.accumulated_start_times[user] is None:
                                        self.accumulated_start_times[user] = time.time()

                                    accum_len_bytes = len(self.to_be_sent_buffers[user])
                                    accum_duration = accum_len_bytes / (48000 * 4)

                                    if accum_duration >= 10.0:
                                        logger.info(f"VAD: Accumulated speech duration for {user} is {accum_duration:.2f}s (>= 10s). Sending to transcription...")
                                        self.completed_utterances[user].append(bytes(self.to_be_sent_buffers[user]))
                                        self.to_be_sent_buffers[user] = bytearray()
                                        self.accumulated_start_times[user] = None
                                    else:
                                        logger.info(f"VAD: Accumulated speech duration for {user} is {accum_duration:.2f}s (< 10s). Waiting for more speech to fill the time...")

                                    self.is_active[user] = False
                                    self.active_buffers[user] = bytearray()
                                    self.silence_durations[user] = 0.0
                                    self.utterance_durations[user] = 0.0

                        for user in list(self.completed_utterances.keys()):
                            while self.completed_utterances[user]:
                                audio_bytes = self.completed_utterances[user].pop(0)
                                users_to_process.append((user, audio_bytes))

                    for user, audio_bytes in users_to_process:
                        audio_np = np.frombuffer(audio_bytes, dtype=np.int16)
                        if len(audio_np) == 0:
                            continue

                        if len(audio_np) % 2 != 0:
                            audio_np = audio_np[:(len(audio_np) // 2) * 2]

                        audio_float32 = audio_np.reshape(-1, 2).astype(np.float32) / 32768.0
                        mono_16k = audio_float32.mean(axis=1)[::3]
                        await self.bot.transcribe_and_post_async(mono_16k, speaker=user)
                except Exception as e:
                    logger.error(f"Error in processing loop: {e}")

    # --- BOT CLIENT ---
    class AquaBot(analyze_transcript.BaseTranscriptionBot):
        async def setup_hook(self):
            await super().setup_hook()
            asyncio.create_task(self.health_check_loop())
            logger.info("AquaBot health check loop started.")

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
                            if not vc.is_connected():
                                await self.connect_to_world(vc.guild)

                except Exception as e:
                    logger.error(f"Error in health check loop: {e}")

        async def on_ready(self):
            logger.info(f'Logged in as {self.user} (ID: {self.user.id})')
            for guild in self.guilds:
                await self.connect_to_world(guild)

    if __name__ == '__main__':
        script_dir = os.path.dirname(os.path.abspath(__file__))
        analyze_transcript.cleanup_old_mp3s(script_dir)

        config = analyze_transcript.load_credentials(script_dir)
        TOKEN = config['token']
        AQUA_KEY = config['aqua_key']

        if not discord.opus.is_loaded():
            try: discord.opus.load_opus('libopus.so.0')
            except: pass

        bot = AquaBot(aqua_key=AQUA_KEY, token=TOKEN)
        bot.run(TOKEN)

except Exception as e:
    logging.error(f"STARTUP ERROR: {type(e).__name__}: {e}", exc_info=True)
    input("\nFATAL ERROR: Script halted during startup. Press Enter to close window...")
    sys.exit(1)
