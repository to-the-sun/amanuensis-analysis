import os
import re
import sys
import shutil
import wave
from datetime import datetime

def get_next_prefix(directory):
    max_prefix = -1
    try:
        filenames = os.listdir(directory)
    except OSError:
        filenames = []

    for filename in filenames:
        match = re.match(r'^(\d{2})(?!\d)', filename)
        if match:
            val = int(match.group(1))
            if val > max_prefix:
                max_prefix = val
    return f"{max_prefix + 1:02d}"

def silence_wave_file(filepath):
    try:
        with wave.open(filepath, 'rb') as wav_in:
            params = wav_in.getparams()
            n_frames = wav_in.getnframes()
            sampwidth = params.sampwidth
            nchannels = params.nchannels

        with wave.open(filepath, 'wb') as wav_out:
            wav_out.setparams(params)
            if sampwidth == 1:
                # 8-bit unsigned PCM: 128 (0x80) is silence
                silence_data = b'\x80' * (n_frames * nchannels)
            else:
                # 16, 24, 32-bit signed PCM: 0 is silence
                silence_data = b'\x00' * (n_frames * nchannels * sampwidth)
            wav_out.writeframes(silence_data)
        return True
    except Exception as e:
        print(f"Error silencing {filepath}: {e}")
        return False

def process_file(filepath):
    if not os.path.exists(filepath):
        print(f"File not found: {filepath}")
        return

    abs_filepath = os.path.abspath(filepath)
    directory = os.path.dirname(abs_filepath)
    filename = os.path.basename(abs_filepath)
    name, ext = os.path.splitext(filename)

    now = datetime.now()
    timestamp_str = now.strftime("%Y-%m-%d %H%M%S")
    ts_pattern = r'\[\d{4}-\d{1,2}-\d{1,2}(?: \d{6}|-\d{1,2}-\d{1,2}-\d{1,2})\]'

    # 1. Prefix for the copy
    next_prefix = get_next_prefix(directory)

    copy_name = name
    if re.match(r'^\d{2}(?!\d)', name):
        copy_name = re.sub(r'^\d{2}', next_prefix, name, count=1)
    else:
        copy_name = f"{next_prefix} {name}"

    # 2. Timestamp for the copy
    # "it should not actually update the timestamp... unless that timestamp is being added for the first time"
    if not re.search(ts_pattern, copy_name):
        copy_name = f"{copy_name} [{timestamp_str}]"

    new_filepath = os.path.join(directory, copy_name + ext)

    # Perform copy
    shutil.copy2(abs_filepath, new_filepath)
    print(f"Copied: {filename} -> {copy_name + ext}")

    # 3. Silencing and renaming original
    match = re.match(r'^(\d{1,2})(?!\d)', filename)
    if match:
        original_num = int(match.group(1))
        if original_num <= 4:
            if silence_wave_file(abs_filepath):
                print(f"Silenced: {filename}")

                # Update timestamp on original file name
                if re.search(ts_pattern, name):
                    original_new_name = re.sub(ts_pattern, f"[{timestamp_str}]", name)
                else:
                    original_new_name = f"{name} [{timestamp_str}]"

                original_new_path = os.path.join(directory, original_new_name + ext)
                if abs_filepath != original_new_path:
                    os.rename(abs_filepath, original_new_path)
                    print(f"Renamed original: {filename} -> {original_new_name + ext}")

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 preserve.py <file1> <file2> ...")
        sys.exit(1)

    for arg in sys.argv[1:]:
        process_file(arg)

if __name__ == "__main__":
    main()
