import time
import tkinter as tk
from tkinter import ttk
import numpy as np
import matplotlib
matplotlib.use('TkAgg')
import matplotlib.pyplot as plt
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg


class PatternGUIHandler:
    def __init__(self, y, sr, total_duration_ms):
        self.y = y
        self.sr = sr
        self.total_duration_ms = total_duration_ms
        self.time_axis_s = np.linspace(0, len(y) / sr, len(y))

        # GUI Throttling
        self.last_update_time = 0
        self.update_interval_s = 0.05  # Throttle updates to ~20 FPS max

        # Tkinter Root Window
        self.root = tk.Tk()
        self.root.title("Real-Time Audio Pattern Finder")
        self.root.geometry("1100x700")

        # Top Control & Status Panel
        self.status_frame = ttk.Frame(self.root, padding=10)
        self.status_frame.pack(side=tk.TOP, fill=tk.X)

        self.lbl_status = ttk.Label(
            self.status_frame,
            text="Initializing analysis...",
            font=("Helvetica", 12, "bold")
        )
        self.lbl_status.pack(side=tk.LEFT, padx=10)

        self.lbl_info = ttk.Label(
            self.status_frame,
            text="",
            font=("Helvetica", 10)
        )
        self.lbl_info.pack(side=tk.RIGHT, padx=10)

        # Matplotlib Figure
        self.fig, (self.ax_wave, self.ax_bar) = plt.subplots(
            2, 1, figsize=(10, 6), gridspec_kw={'height_ratios': [2, 1]}
        )
        self.fig.tight_layout(pad=3.0)

        # Draw Waveform
        self.ax_wave.plot(self.time_axis_s, self.y, color='#2c3e50', alpha=0.6, lw=0.8, label="Waveform")
        self.ax_wave.set_title("Audio Waveform & Segment Comparison", fontsize=12, fontweight='bold')
        self.ax_wave.set_xlabel("Time (seconds)")
        self.ax_wave.set_ylabel("Amplitude")
        self.ax_wave.set_xlim(0, self.total_duration_ms / 1000.0)
        self.ax_wave.grid(True, alpha=0.3)

        # Highlight overlays
        self.seg1_rect = None
        self.seg2_rect = None
        self.pattern_spans = []

        # Bar chart for tested segment lengths
        self.tested_lengths = []
        self.tested_durations = []
        self.bar_container = self.ax_bar.bar([], [], color='#3498db', alpha=0.8)
        self.ax_bar.set_title("Pattern Duration by Segment Length", fontsize=10, fontweight='bold')
        self.ax_bar.set_xlabel("Segment Length (ms)")
        self.ax_bar.set_ylabel("Total Pattern Duration (ms)")
        self.ax_bar.grid(True, alpha=0.3)

        # Canvas embedding
        self.canvas = FigureCanvasTkAgg(self.fig, master=self.root)
        self.canvas.draw()
        self.canvas.get_tk_widget().pack(side=tk.TOP, fill=tk.BOTH, expand=True)

        self.root.update()

    def update_testing_segment(self, seg_len_ms, seg1, seg2, similarity, force=False):
        """
        Visually highlight the two segments currently being compared via DTW.
        Throttled to maintain performance.
        """
        now = time.time()
        if not force and (now - self.last_update_time < self.update_interval_s):
            return

        self.last_update_time = now
        try:
            self.lbl_status.config(
                text=f"Testing Segment Length: {seg_len_ms} ms  |  Comparing Segment {seg1['index']} & {seg2['index']}"
            )
            self.lbl_info.config(
                text=f"Similarity: {similarity:.3f}"
            )

            # Remove previous segment highlight spans
            if self.seg1_rect:
                self.seg1_rect.remove()
            if self.seg2_rect:
                self.seg2_rect.remove()

            s1_start_s = seg1['start_ms'] / 1000.0
            s1_end_s = seg1['end_ms'] / 1000.0
            s2_start_s = seg2['start_ms'] / 1000.0
            s2_end_s = seg2['end_ms'] / 1000.0

            color = '#2ecc71' if similarity >= 0.75 else '#e74c3c'
            self.seg1_rect = self.ax_wave.axvspan(s1_start_s, s1_end_s, color=color, alpha=0.4, label="Testing Segment 1")
            self.seg2_rect = self.ax_wave.axvspan(s2_start_s, s2_end_s, color='#f1c40f', alpha=0.4, label="Testing Segment 2")

            self.canvas.draw_idle()
            self.root.update_idletasks()
            self.root.update()
        except tk.TclError:
            pass  # GUI window closed by user

    def update_completed_length(self, seg_len_ms, patterns, total_duration_ms):
        """
        Updates the segment length pattern duration bar plot after finishing a segment length analysis.
        """
        try:
            self.tested_lengths.append(seg_len_ms)
            self.tested_durations.append(total_duration_ms)

            self.ax_bar.clear()
            self.ax_bar.bar([str(l) for l in self.tested_lengths], self.tested_durations, color='#3498db', alpha=0.8)
            self.ax_bar.set_title("Pattern Duration by Segment Length", fontsize=10, fontweight='bold')
            self.ax_bar.set_xlabel("Segment Length (ms)")
            self.ax_bar.set_ylabel("Total Pattern Duration (ms)")
            self.ax_bar.grid(True, alpha=0.3)

            self.canvas.draw_idle()
            self.root.update_idletasks()
            self.root.update()
        except tk.TclError:
            pass

    def finish_analysis(self, best_bar_length, best_patterns, max_total_duration_ms):
        """
        Final display highlighting all detected patterns at the chosen bar length.
        """
        try:
            # Clear segment highlights
            if self.seg1_rect:
                self.seg1_rect.remove()
                self.seg1_rect = None
            if self.seg2_rect:
                self.seg2_rect.remove()
                self.seg2_rect = None

            # Draw spans for best patterns
            for pat in best_patterns:
                start_s = pat['start_ms'] / 1000.0
                end_s = pat['end_ms'] / 1000.0
                span = self.ax_wave.axvspan(start_s, end_s, color='#9b59b6', alpha=0.4)
                self.pattern_spans.append(span)

            if best_bar_length is not None:
                self.lbl_status.config(
                    text=f"Analysis Complete! Optimal Bar Length: {best_bar_length} ms",
                    foreground="green"
                )
                self.lbl_info.config(
                    text=f"Total Pattern Duration: {max_total_duration_ms:.1f} ms  |  Patterns Saved to WAV"
                )
            else:
                self.lbl_status.config(
                    text="Analysis Complete: No repeating patterns found.",
                    foreground="red"
                )

            self.canvas.draw()
            self.root.update()
            # Keep GUI window open until user closes it
            self.root.mainloop()
        except tk.TclError:
            pass
