import os
import sys
import argparse
import traceback
import numpy as np
import soundfile as sf
import librosa
from scipy.spatial.distance import cdist

# Global defaults
MIN_SEGMENT_LEN_MS = 100
ATOM_ITERATION_MS = 50
SIMILARITY_THRESHOLD = 0.75  # Normalized similarity threshold (0.0 to 1.0)


def extract_frame_features(y, sr, hop_length=160):
    """
    Extract frame-level features (MFCC + Delta + Chroma) for DTW comparison.
    hop_length=160 samples at 16kHz corresponds to 10ms per frame.
    """
    # Force 16kHz for uniform temporal resolution
    if sr != 16000:
        y = librosa.resample(y, orig_sr=sr, target_sr=16000)
        sr = 16000

    mfcc = librosa.feature.mfcc(y=y, sr=sr, n_mfcc=13, hop_length=hop_length)
    mfcc_delta = librosa.feature.delta(mfcc)
    chroma = librosa.feature.chroma_stft(y=y, sr=sr, hop_length=hop_length)

    # Stack features along feature axis: (n_frames, n_features)
    features = np.vstack([mfcc, mfcc_delta, chroma]).T

    # Normalize features (zero mean, unit variance per feature dimension)
    mean = np.mean(features, axis=0, keepdims=True)
    std = np.std(features, axis=0, keepdims=True) + 1e-8
    features = (features - mean) / std

    frame_duration_ms = (hop_length / sr) * 1000.0
    return y, sr, features, frame_duration_ms


def detect_transient_candidate_lengths(y, sr, min_seg_ms=MIN_SEGMENT_LEN_MS, atom_ms=ATOM_ITERATION_MS, max_seg_ms=None):
    """
    Detects transients in the entire audio file, computes pairwise time differences between all transients,
    rounds each difference to the nearest atom_ms (50ms), and returns a sorted list of unique candidate
    segment lengths.
    """
    print("Detecting transients across audio file...")
    # Onset strength envelope and peak detection
    onset_env = librosa.onset.onset_strength(y=y, sr=sr)
    onset_frames = librosa.onset.onset_detect(onset_envelope=onset_env, sr=sr, backtrack=False)
    onset_times_ms = librosa.frames_to_time(onset_frames, sr=sr) * 1000.0

    print(f"Detected {len(onset_times_ms)} transients.")

    if len(onset_times_ms) < 2:
        print("Not enough transients detected. Falling back to default range.")
        total_dur_ms = (len(y) / sr) * 1000.0
        upper_ms = total_dur_ms / 2.0 if max_seg_ms is None else min(total_dur_ms / 2.0, max_seg_ms)
        curr = min_seg_ms
        candidates = []
        while curr <= upper_ms:
            candidates.append(curr)
            curr += atom_ms
        return candidates

    candidate_set = set()
    total_dur_ms = (len(y) / sr) * 1000.0
    upper_ms = total_dur_ms / 2.0 if max_seg_ms is None else min(total_dur_ms / 2.0, max_seg_ms)

    # Assess time difference between every transient with every other transient
    for i in range(len(onset_times_ms)):
        for j in range(i + 1, len(onset_times_ms)):
            diff_ms = abs(onset_times_ms[j] - onset_times_ms[i])
            # Round to nearest atom_ms (50ms)
            rounded_ms = int(round(diff_ms / atom_ms)) * atom_ms

            if min_seg_ms <= rounded_ms <= upper_ms:
                candidate_set.add(rounded_ms)

    candidate_lengths = sorted(list(candidate_set))

    if not candidate_lengths:
        print("No valid transient interval candidates found in range. Falling back to default step range.")
        curr = min_seg_ms
        while curr <= upper_ms:
            candidate_lengths.append(curr)
            curr += atom_ms

    print(f"Generated {len(candidate_lengths)} plausible segment lengths from transient intervals.")
    return candidate_lengths


def compute_segment_dtw_similarity(feat1, feat2):
    """
    Computes DTW similarity score between two segment feature matrices feat1 and feat2.
    Returns normalized similarity score in range [0, 1].
    """
    if len(feat1) == 0 or len(feat2) == 0:
        return 0.0

    # Compute pairwise cosine distance matrix
    dist_matrix = cdist(feat1, feat2, metric='cosine')

    # Run DTW
    D, wp = librosa.sequence.dtw(C=dist_matrix)
    path_len = len(wp)
    if path_len == 0:
        return 0.0

    norm_cost = D[-1, -1] / path_len
    # Convert distance to similarity score in [0, 1]
    similarity = max(0.0, 1.0 - norm_cost)
    return similarity


def analyze_segment_length(y, sr, features, frame_dur_ms, seg_len_ms, similarity_threshold=SIMILARITY_THRESHOLD, gui_callback=None):
    """
    Divides audio into contiguous segments of seg_len_ms and analyzes each segment
    against each other using Dynamic Time Warping.
    Returns (patterns, total_pattern_duration_ms)
    """
    frames_per_seg = max(1, int(round(seg_len_ms / frame_dur_ms)))
    n_frames = len(features)
    num_segments = n_frames // frames_per_seg

    if num_segments < 2:
        return [], 0.0

    segments = []
    for i in range(num_segments):
        start_f = i * frames_per_seg
        end_f = (i + 1) * frames_per_seg
        segments.append({
            'index': i,
            'start_ms': start_f * frame_dur_ms,
            'end_ms': end_f * frame_dur_ms,
            'features': features[start_f:end_f]
        })

    matched_segment_indices = set()
    matches = []

    for i in range(num_segments):
        for j in range(i + 1, num_segments):
            sim = compute_segment_dtw_similarity(segments[i]['features'], segments[j]['features'])

            if gui_callback:
                gui_callback(seg_len_ms, segments[i], segments[j], sim)

            if sim >= similarity_threshold:
                matched_segment_indices.add(i)
                matched_segment_indices.add(j)
                matches.append((i, j, sim))

    if not matched_segment_indices:
        return [], 0.0

    # Group contiguous matched segments into patterns
    sorted_matched = sorted(list(matched_segment_indices))
    pattern_groups = []
    current_group = [sorted_matched[0]]

    for idx in sorted_matched[1:]:
        if idx == current_group[-1] + 1:
            current_group.append(idx)
        else:
            pattern_groups.append(current_group)
            current_group = [idx]
    pattern_groups.append(current_group)

    patterns = []
    total_pattern_duration_ms = 0.0

    for group in pattern_groups:
        start_idx = group[0]
        end_idx = group[-1]
        start_ms = segments[start_idx]['start_ms']
        end_ms = segments[end_idx]['end_ms']
        duration_ms = end_ms - start_ms

        patterns.append({
            'start_ms': start_ms,
            'end_ms': end_ms,
            'duration_ms': duration_ms,
            'segments': group
        })
        total_pattern_duration_ms += duration_ms

    return patterns, total_pattern_duration_ms


def find_patterns(audio_path, min_segment_ms=MIN_SEGMENT_LEN_MS, atom_iteration_ms=ATOM_ITERATION_MS, similarity_threshold=SIMILARITY_THRESHOLD, max_len_ms=None, gui_mode=True):
    """
    Full pipeline to search for patterns across transient-derived segment lengths,
    determine optimal bar length, and export pattern WAV files.
    """
    if not os.path.exists(audio_path):
        raise FileNotFoundError(f"Audio file not found: {audio_path}")

    print(f"Loading audio file: {audio_path}")
    raw_y, orig_sr = sf.read(audio_path)
    if raw_y.ndim > 1:
        raw_y_mono = np.mean(raw_y, axis=1)
    else:
        raw_y_mono = raw_y

    # Extract audio and features at 16kHz
    y, sr, features, frame_dur_ms = extract_frame_features(raw_y_mono, orig_sr)
    total_duration_ms = (len(y) / sr) * 1000.0

    print(f"Audio duration: {total_duration_ms:.2f} ms ({total_duration_ms/1000.0:.2f} s)")

    # Conduct transient detection to find candidate segment lengths
    segment_lengths = detect_transient_candidate_lengths(
        y=y,
        sr=sr,
        min_seg_ms=min_segment_ms,
        atom_ms=atom_iteration_ms,
        max_seg_ms=max_len_ms
    )

    print(f"Testing {len(segment_lengths)} plausible segment lengths (range: {segment_lengths[0]} ms to {segment_lengths[-1]} ms)...")

    gui_handler = None
    if gui_mode:
        try:
            from pattern_gui import PatternGUIHandler
            gui_handler = PatternGUIHandler(y, sr, total_duration_ms)
        except Exception as e:
            print(f"GUI Initialization skipped/failed: {e}. Falling back to CLI mode.")
            gui_handler = None

    best_bar_length = None
    max_total_pattern_len_ms = -1.0
    best_patterns = []
    results = {}

    def step_gui_callback(seg_len_ms, seg1, seg2, sim):
        if gui_handler:
            gui_handler.update_testing_segment(seg_len_ms, seg1, seg2, sim)

    for seg_len in segment_lengths:
        print(f"\nAnalyzing segment length: {seg_len} ms...")
        patterns, total_pat_len_ms = analyze_segment_length(
            y, sr, features, frame_dur_ms, seg_len,
            similarity_threshold=similarity_threshold,
            gui_callback=step_gui_callback if gui_handler else None
        )

        results[seg_len] = {
            'patterns': patterns,
            'total_pattern_len_ms': total_pat_len_ms
        }

        print(f"Segment length {seg_len} ms: Found {len(patterns)} patterns, Total Duration = {total_pat_len_ms:.2f} ms")

        if gui_handler:
            gui_handler.update_completed_length(seg_len, patterns, total_pat_len_ms)

        if total_pat_len_ms > max_total_pattern_len_ms and total_pat_len_ms > 0:
            max_total_pattern_len_ms = total_pat_len_ms
            best_bar_length = seg_len
            best_patterns = patterns

    if best_bar_length is None or not best_patterns:
        print("\nNo repeating patterns were detected across tested segment lengths.")
        if gui_handler:
            gui_handler.finish_analysis(None, [], 0)
        return

    print("\n" + "="*60)
    print(f"ANALYSIS COMPLETE: Bar length determined to be {best_bar_length} ms")
    print(f"Total pattern duration at bar length: {max_total_pattern_len_ms:.2f} ms")
    print(f"Number of patterns identified: {len(best_patterns)}")
    print("="*60)

    # Export WAV files for patterns at the best bar length
    audio_dir = os.path.dirname(os.path.abspath(audio_path))
    audio_base_name = os.path.splitext(os.path.basename(audio_path))[0]

    saved_files = []
    for idx, pat in enumerate(best_patterns, 1):
        start_samp = int(round((pat['start_ms'] / 1000.0) * orig_sr))
        end_samp = int(round((pat['end_ms'] / 1000.0) * orig_sr))

        pattern_audio = raw_y[start_samp:end_samp]

        out_filename = f"{audio_base_name}_bar_{int(best_bar_length)}ms_pattern_{idx}.wav"
        out_filepath = os.path.join(audio_dir, out_filename)

        sf.write(out_filepath, pattern_audio, orig_sr)
        saved_files.append(out_filepath)
        print(f"Saved pattern {idx}: {out_filepath} (Start: {pat['start_ms']:.1f}ms, End: {pat['end_ms']:.1f}ms)")

    if gui_handler:
        gui_handler.finish_analysis(best_bar_length, best_patterns, max_total_pattern_len_ms)

    return best_bar_length, best_patterns, saved_files


def main():
    parser = argparse.ArgumentParser(description="Find audio patterns and bar length using transient-guided Dynamic Time Warping (DTW).")
    parser.add_argument("audio_file", nargs="?", help="Path to input audio file.")
    parser.add_argument("--min-segment", type=int, default=MIN_SEGMENT_LEN_MS, help="Minimum segment length in ms (default: 100ms).")
    parser.add_argument("--atom-iteration", type=int, default=ATOM_ITERATION_MS, help="Atom of iteration in ms (default: 50ms).")
    parser.add_argument("--threshold", type=float, default=SIMILARITY_THRESHOLD, help="DTW similarity threshold (default: 0.75).")
    parser.add_argument("--max-len", type=int, default=None, help="Maximum segment length to test in ms.")
    parser.add_argument("--no-gui", action="store_true", help="Run in headless mode without GUI visualization.")

    args = parser.parse_args()

    audio_path = args.audio_file
    if not audio_path:
        current_dir = os.path.dirname(os.path.abspath(__file__))
        extensions = ('.wav', '.mp3', '.flac', '.ogg', '.aiff')
        files = [os.path.join(current_dir, f) for f in os.listdir(current_dir) if f.lower().endswith(extensions)]
        files.sort()
        if files:
            audio_path = files[0]
            print(f"No audio file specified. Defaulting to: {audio_path}")
        else:
            print("Error: No audio file specified or found in current directory.")
            return

    gui_mode = not args.no_gui
    find_patterns(
        audio_path=audio_path,
        min_segment_ms=args.min_segment,
        atom_iteration_ms=args.atom_iteration,
        similarity_threshold=args.threshold,
        max_len_ms=args.max_len,
        gui_mode=gui_mode
    )


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print("\n" + "="*60)
        print("ERROR OCCURRED")
        print("="*60)
        traceback.print_exc()
        print("="*60)
    finally:
        try:
            input("\nPress Enter to exit...")
        except EOFError:
            pass
