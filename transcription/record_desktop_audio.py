import soundcard as sc
import soundfile as sf

"""
Proof-of-Concept: Recording Windows Desktop Audio
Dependencies:
    pip install soundcard soundfile

Note: This script uses the WASAPI loopback interface to capture audio
currently playing on the default output device (speakers/headphones).
"""

def record_desktop_audio(duration=10, output_file="desktop_audio.wav"):
    # Get the default speaker
    default_speaker = sc.default_speaker()
    print(f"Recording from: {default_speaker.name}")

    # Use the speaker name to find the corresponding loopback microphone
    # include_loopback=True is essential for capturing desktop audio on Windows
    try:
        # On Windows, sc.get_microphone with include_loopback=True
        # allows capturing what is being played back to the speakers.
        mic = sc.get_microphone(
            id=str(default_speaker.name),
            include_loopback=True
        )
    except Exception as e:
        print(f"Error finding loopback device: {e}")
        print("Available microphones:")
        for m in sc.all_microphones(include_loopback=True):
            print(f" - {m}")
        return

    samplerate = 48000  # Standard high-quality sample rate
    num_frames = samplerate * duration

    print(f"Recording {duration} seconds of desktop audio...")

    # Use the recorder context manager
    with mic.recorder(samplerate=samplerate) as recorder:
        # Capture the audio data as a NumPy array
        data = recorder.record(numframes=num_frames)

    # Save the recorded data to a WAV file
    # soundfile handles the WAV formatting
    sf.write(file=output_file, data=data, samplerate=samplerate)

    print(f"Finished! Saved to {output_file}")

if __name__ == "__main__":
    record_desktop_audio(duration=10)
