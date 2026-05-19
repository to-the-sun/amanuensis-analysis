import discord
import asyncio
import json
import os
import sys

# Simple script to play a WAV file in a Discord voice channel on repeat.
# Requires: pip install discord.py[voice]

try:
    with open('credentials.json', 'r') as f:
        config = json.load(f)
except FileNotFoundError:
    print("Error: credentials.json not found on your local machine.")
    sys.exit(1)

TOKEN = config.get('token')
VOICE_CHANNEL_ID = int(config.get('world_voice', 0))
TEST_AUDIO_FILE = "test_phrase.wav" # Ensure this file exists on your computer

class PlayerBot(discord.Client):
    def __init__(self):
        intents = discord.Intents.default()
        super().__init__(intents=intents)

    async def on_ready(self):
        print(f'Logged in as {self.user}. Ready to play.')
        channel = self.get_channel(VOICE_CHANNEL_ID)
        if not channel:
            print(f"Error: Voice channel {VOICE_CHANNEL_ID} not found.")
            return

        print(f"Connecting to {channel.name}...")
        vc = await channel.connect()
        
        while True:
            if not os.path.exists(TEST_AUDIO_FILE):
                print(f"Waiting for {TEST_AUDIO_FILE} to be present...")
                await asyncio.sleep(5)
                continue

            print(f"Playing {TEST_AUDIO_FILE}...")
            # We use FFmpegPCMAudio. Ensure ffmpeg is installed on your local system.
            vc.play(discord.FFmpegPCMAudio(TEST_AUDIO_FILE), after=lambda e: print('Done playing, restarting...'))
            
            while vc.is_playing():
                await asyncio.sleep(1)
            
            await asyncio.sleep(1) # Small gap between repeats

if __name__ == "__main__":
    bot = PlayerBot()
    try:
        bot.run(TOKEN)
    except Exception as e:
        print(f"Player Error: {e}")
