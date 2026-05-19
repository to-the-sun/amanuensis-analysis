# DAVE Decryption Progress Report - May 2024

## Overview
This document summarizes the technical progress made in transitioning the transcription bot from local audio capture to real-time Discord voice channel transcription, specifically focusing on the implementation of the **DAVE (Discord Audio-Video Encryption)** protocol.

## Current Goal
To enable the bot to join a Discord voice channel, receive encrypted audio packets, decrypt them through both the Outer (RTP) and Inner (DAVE) layers, and transcribe the resulting PCM data using Whisper.

## Key Technical Milestones

### 1. Outer Layer (RTP) Decryption Resolved
*   **Issue:** Standard `discord-ext-voice-recv` decryption failed with "Corrupted Stream" errors on modern Discord servers.
*   **Solution:** Identified that Discord uses `aead_aes256_gcm_rtpsize`. Patched the `PacketDecryptor` to use the 12-byte RTP header as Additional Authenticated Data (AAD) and a 12-byte nonce (4-byte suffix + 8 zero bytes).
*   **Status:** **Verified.** The outer layer now decrypts successfully, providing access to the (still DAVE-encrypted) media frames.

### 2. Identifying the DAVE Barrier
*   **Observation:** Even with outer decryption working, Whisper produced "hallucinations" (random phrases like "Thank you for watching" or "Please like and subscribe") because it was processing high-entropy encrypted noise.
*   **Diagnosis:** Discord enforced DAVE/E2EE. Decrypted RTP packets contain an inner layer of encryption that requires the `davey` library and session-key synchronization.

### 3. Emulating Craig Bot Logic
*   **Strategy:** We are emulating the architecture used by the **Craig** recording bot, which is currently one of the few open-source implementations successfully handling DAVE in Python.
*   **Implementation:** 
    *   Integrating the `davey` library to manage epoch-based keys.
    *   Patching `AudioReader` and `PacketDecryptor` to perform a second decryption pass (`dave.decrypt`) using the User ID mapped from the packet's SSRC.

### 4. Autonomous Testing Rig
*   **Mechanism:** The user is running a `play_test_audio.py` script.
*   **Function:** This script puts a secondary bot in the voice channel that plays high-quality `.wav` files of speech.
*   **Benefit:** This allows for headlong iteration on decryption and Whisper performance without requiring a human to be present to speak in the channel.

## Current Status & Next Steps
*   **DAVE Integration:** The bot successfully initializes a `davey` session and reaches the `Ready` state.
*   **SSRC Mapping:** We have implemented logic to retrieve User IDs from SSRCs to ensure the correct keys are used for decryption.
*   **Remaining Challenge:** Finalizing the synchronization of the DAVE frame index to prevent decryption failures on sporadic packets.

## Environment Requirements
*   `discord.py[voice]` (>= 2.4.0)
*   `discord-ext-voice-recv`
*   `davey`
*   `faster-whisper`
*   `libopus-dev` (Linux system dependency)
