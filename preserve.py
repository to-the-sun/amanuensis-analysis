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
        print(f"Silenced: {filepath}")
    except Exception as e:
        print(f"Error silencing {filepath}: {e}")

def process_file(filepath):
    if not os.path.exists(filepath):
        print(f"File not found: {filepath}")
        return

    abs_filepath = os.path.abspath(filepath)
    directory = os.path.dirname(abs_filepath)
    filename = os.path.basename(abs_filepath)
    name, ext = os.path.splitext(filename)

    # 1. Prefix
    next_prefix = get_next_prefix(directory)

    new_name = name
    if re.match(r'^\d{2}(?!\d)', name):
        # Replace double-digit
        new_name = re.sub(r'^\d{2}', next_prefix, name, count=1)
    else:
        # Prepend double-digit + space (covers single digit or no digit)
        new_name = f"{next_prefix} {name}"

    # 2. Timestamp
    now = datetime.now()
    timestamp_str = now.strftime("%Y-%m-%d %H%M%S")

    # Patterns: [yyyy-mm-dd HHmmss] or [yyyy-m-d-h-m-s]
    # We'll use a combined regex that looks for these specifically
    ts_pattern = r'\[\d{4}-\d{1,2}-\d{1,2}(?: \d{6}|-\d{1,2}-\d{1,2}-\d{1,2})\]'

    if re.search(ts_pattern, new_name):
        new_name = re.sub(ts_pattern, f"[{timestamp_str}]", new_name)
    else:
        # Append before extension
        new_name = f"{new_name} [{timestamp_str}]"

    new_filepath = os.path.join(directory, new_name + ext)

    # Perform copy
    shutil.copy2(abs_filepath, new_filepath)
    print(f"Copied: {filepath} -> {new_filepath}")

    # 3. Silencing original
    # "if the original file that was copied begins with either a double or single digit number that is four or less"
    match = re.match(r'^(\d{1,2})(?!\d)', filename)
    if match:
        original_num = int(match.group(1))
        if original_num <= 4:
            silence_wave_file(abs_filepath)

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 preserve.py <file1> <file2> ...")
        sys.exit(1)

    # We should probably collect all new prefixes upfront if we want to be consistent,
    # but the prompt says "scan the folder... finding any filenames... and then edit the new copy... one greater than the largest found in the folder prior."
    # This implies we should re-scan for each file if we want them to increment.

    for arg in sys.argv[1:]:
        process_file(arg)

if __name__ == "__main__":
    main()
