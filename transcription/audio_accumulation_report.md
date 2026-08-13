# Transcription Bot Audio Buffering and Accumulation Report

## 1. Executive Summary
This report analyzes how audio is processed, accumulated, and sent to the Avalon (AquaVoice) API in the transcription bot suite (`desktop_transcriber.py` and `transcription_bot_aqua.py`).

The current design specifies a **10.0-second minimum accumulation threshold** to bundle speech into efficient, context-rich chunks before sending them to the Avalon API. However, system logs and API usage reports indicate that a significant volume of audio segments is transmitted at lengths **under 10 seconds**.

This investigation reveals that while the primary VAD/buffering logic is mathematically correct, several secondary mechanisms, edge cases, and architectural choices—most notably the **45-second age-based flush timeout**, **low VAD thresholds responding to background noise**, and **independent per-user voice buffer isolation**—bypass or reset the 10-second threshold, causing frequent transmission of short segments.

---

## 2. Audio Buffering Architecture & Data Flows

The suite uses two different VAD/accumulation pipelines depending on the audio source.

### A. Desktop Audio Capture (`desktop_transcriber.py`)
This bot captures desktop audio loopback and standard microphone input via `soundcard`.
```
[Soundcard (48kHz Mono)] ---> [100ms Chunk Queue]
                                     |
                       [RMS > 0.0015 ? Yes] ---> [Active State]
                                                        |
                                                (Accumulate in active_buffer)
                                                        |
                                       [Silence >= 1.0s or Duration >= 45s]
                                                        |
                                                        v
                                          [Concatenated Utterance]
                                                        |
                                                        v
                                        [accumulated_utterances_list]
                                                        |
                 -----------------------------------------------------------------
                 |                                                               |
    [Accumulated Duration >= 10.0s]                               [Age of First Utterance > 45s]
                 |                                                               |
                 v                                                               v
  [Concatenate & Send to Avalon API]                           [Flush List & Send to Avalon API]
```

### B. Discord Voice Receiver (`transcription_bot_aqua.py`)
This bot runs a multi-user, real-time voice channel listener via `discord.ext.voice_recv` with custom decryptors for Discord's DAVE (E2EE) security.
```
[Discord RTP packets] ---> [DAVE AES-GCM Decryptor] ---> [AudioSink write()]
                                                                |
                                                [20ms Stereo 48kHz Chunk]
                                                                |
                                             [User Active State (RMS > 50.0)]
                                                                |
                                                 (Accumulate in active_buffers)
                                                                |
                                          [Silence >= 1.0s or Duration >= 45s]
                                                   (or Packet Timeout >= 1s)
                                                                |
                                                                v
                                              [Completed User Utterance]
                                                                |
                                                                v
                                                 [to_be_sent_buffers[user]]
                                                                |
                 -----------------------------------------------------------------
                 |                                                               |
     [User Duration >= 10.0s]                                     [Age of User Buffer > 45s]
                 |                                                               |
                 v                                                               v
     [Queue to Completed Utterances]                             [Flush Buffer to Completed]
                 |                                                               |
                 -----------------------------------------------------------------
                                                 |
                                                 v
                                   [Downsample Mono 16kHz (::3)]
                                                 |
                                                 v
                                        [Avalon API Request]
```

---

## 3. Deep-Dive: Why Under-10-Second Segments are Transmitted

Despite the `10.0s` requirement, several mechanisms actively bypass or force-trigger transmission under 10 seconds:

### 1. The 45-Second Age-Based Flush Timeout (Primary Cause)
Both bots implement a flush timeout to prevent transcripts from being held back indefinitely when a speaker finishes speaking:
* **The Logic**: When the first utterance is added to the accumulation list, a timestamp (`accumulated_start_time`) is set.
* **The Flaw**: If 45 seconds elapse from that timestamp, the bot sends whatever is in the list/buffer, regardless of its length.
* **The Consequence**:
  - If a user speaks for 2 seconds and stops, that 2-second utterance sits in the buffer. 45 seconds later, the flush timeout hits, and the **2-second segment is sent to the API**.
  - In a typical low-traffic voice channel where people speak occasionally (e.g., answering a question or making sporadic comments spaced more than 45 seconds apart), **100% of these utterances will bypass the 10-second threshold** and be sent as short segments.

### 2. VAD Triggered by Brief Noises, Glitches, and Microphone Static
* **The Logic**: The VAD is designed to trigger when RMS energy exceeds a threshold (`0.0015` in `desktop_transcriber.py` and `50.0` in `transcription_bot_aqua.py`).
* **The Flaw**: These thresholds are relatively low. Keyboard clicks, heavy breathing, mouth noises, microphone bumps, and static easily exceed these values, triggering the "Active" state.
* **The Consequence**:
  - The bot records the short noise (e.g., 0.1s of static) and then waits for the **1.0-second silence timeout**.
  - This results in a 1.1-second completed utterance of mostly silence and static.
  - The utterance is added to `to_be_sent_buffers`. Since it is `< 10s`, it waits.
  - 45 seconds later, the flush timeout is reached, transmitting a tiny segment containing only static to the Avalon API. In noisy environments, this creates a continuous stream of sub-2-second API requests.

### 3. User Buffer Isolation (Independent Timers)
In `transcription_bot_aqua.py`, all buffering and VAD state variables are strictly partitioned **per user** using `collections.defaultdict`.
* **The Flaw**:
  - If User A speaks for 5 seconds and User B speaks for 5 seconds, these are captured in `to_be_sent_buffers[User A]` and `to_be_sent_buffers[User B]`.
  - Because they are isolated, neither buffer meets the 10-second threshold individually.
  - Eventually, both users' buffers will independently trigger their respective 45-second flush timeouts, sending **two separate 5-second segments** to the API instead of combining them.

### 4. Discord Packet Dropping & Force Endpointing
* **The Logic**: To save bandwidth, Discord stops transmitting RTP voice packets when a user is silent.
* **The Flaw**: When a stream stops abruptly, the bot's standard `write()` method is no longer called, preventing the normal silence-timeout accumulator from executing.
* **The Guard Logic**: To handle this, `_process_buffers()` runs a force-endpoint check:
  ```python
  time_since = now - self.last_audio_times[user]
  if time_since >= 1.0:
      # Force voice ended...
  ```
* **The Consequence**: If a user pauses mid-sentence for exactly 1.0s and Discord stops sending packets, the bot force-endpoints them and appends the fragment to `to_be_sent_buffers`. If they do not resume speaking immediately, or if they speak sporadically, these fragments are eventually flushed as under-10-second API requests.

---

## 4. Architectural Solutions and Proposed Optimizations

To drastically reduce the frequency of under-10-second segments while maintaining responsive transcription, we recommend implementing the following improvements:

### Option A: Transition from "Age-Based" to "Inactivity-Based" Flush
* **Current Behavior**: The flush timer starts when the *first* utterance is added. If a user speaks intermittently every 20 seconds, the 45-second timer will expire since the first utterance, forcing a premature flush of a small buffer even though the user is actively in the channel.
* **Proposed Behavior**: Reset the flush timer (`accumulated_start_time`) whenever *new* audio is added to the buffer. Only flush the buffer if there has been **complete inactivity** (e.g., 30 seconds of no new speech at all). This keeps the buffer open as long as a conversation is ongoing, giving it more opportunities to cross the 10-second threshold.

### Option B: Implement a Minimum Utterance Duration Filter
* **Current Behavior**: Any VAD trigger (even 20ms of keyboard noise) is treated as a valid utterance and eventually flushed.
* **Proposed Behavior**: Before adding an utterance to `to_be_sent_buffers`, verify its active duration (excluding the trailing 1-second silence timeout). If the active duration is less than **0.5 seconds**, discard the utterance entirely as background noise. This prevents useless static/clicks from constantly queueing and triggering flushes.

### Option C: Raise the Flush Timeout Limit
* **Current Behavior**: The buffer flushes after 45 seconds of sitting.
* **Proposed Behavior**: Increase the flush timeout to **90.0 or 120.0 seconds**. This is particularly effective for slow-paced conversations, giving users more time to accumulate 10.0 seconds of audio.

### Option D: Unified Conversation Buffers (Optional)
* **Current Behavior**: Independent buffers per user.
* **Proposed Behavior**: For `transcription_bot_aqua.py`, provide a configuration option to pool non-overlapping utterances from all users in a channel into a single shared conversation buffer. Since multiple users talking together will easily exceed 10.0 seconds of combined speech within 45 seconds, this would almost completely eliminate short API requests in multi-user channels.

---

## 5. Conclusion
Under the current architecture, **under-10-second segments are a natural and expected outcome** of any conversational gap exceeding 45 seconds or any ambient noise triggering the VAD.

By implementing **Inactivity-Based Flushing** (Option A) and a **Minimum Utterance Duration Filter** (Option B), the transcription bots can maintain their high accuracy while significantly reducing API overhead and eliminating the transmission of short, fragmented, or noisy segments.
