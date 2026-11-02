/*
 * flux_report_html.c
 *
 * Emits one source file's spectral flux report and its companion JS waveform
 * data file, plus the batch dashboard that links every report in [flux]/.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "flux_shared.h"
#include "flux_envelope.h"
#include "flux_report_html.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


static const char b64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char* encode_base64_bytes(const uint8_t *data, size_t input_len) {
    size_t output_len = 4 * ((input_len + 2) / 3);
    char *encoded = (char*)malloc(output_len + 1);
    if (!encoded) return NULL;

    size_t i = 0, j = 0;
    for (i = 0; i < input_len; ) {
        uint32_t octet_a = i < input_len ? data[i++] : 0;
        uint32_t octet_b = i < input_len ? data[i++] : 0;
        uint32_t octet_c = i < input_len ? data[i++] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        encoded[j++] = b64_chars[(triple >> 18) & 0x3F];
        encoded[j++] = b64_chars[(triple >> 12) & 0x3F];

        if (i == input_len && (input_len % 3 == 1)) {
            encoded[j++] = '=';
            encoded[j++] = '=';
        } else if (i == input_len && (input_len % 3 == 2)) {
            encoded[j++] = b64_chars[(triple >> 6) & 0x3F];
            encoded[j++] = '=';
        } else {
            encoded[j++] = b64_chars[(triple >> 6) & 0x3F];
            encoded[j++] = b64_chars[triple & 0x3F];
        }
    }
    encoded[j] = '\0';
    return encoded;
}

void generate_flux_html_report(
    const char *flux_dir,
    const char *basename,
    uint32_t sample_rate,
    uint32_t num_channels,
    uint32_t total_samples,
    double duration,
    const double *orig_audio_buffer,
    const FluxStem *stems,
    int stem_count
) {
    char html_path[4096];
    snprintf(html_path, sizeof(html_path), "%s/%s_index.html", flux_dir, basename);

    FILE *f = fopen(html_path, "w");
    if (!f) {
        pthread_mutex_lock(&g_console_mutex);
        printf("Error: Could not open Flux HTML report file '%s' for writing.\n", html_path);
        pthread_mutex_unlock(&g_console_mutex);
        return;
    }

    fprintf(f, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n");
    fprintf(f, "<meta charset=\"UTF-8\">\n");
    fprintf(f, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n");
    fprintf(f, "<title>Spectral Flux Transience Report - %s</title>\n", basename);
    fprintf(f, "<style>\n");
    fprintf(f, "  :root { --bg: #121216; --card-bg: #1a1a22; --border: #2a2a36; --text: #e1e1e6; --muted: #a0a0b0; --accent: #2196f3; }\n");
    fprintf(f, "  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; }\n");
    fprintf(f, "  body { background: var(--bg); color: var(--text); padding: 20px; line-height: 1.5; }\n");
    fprintf(f, "  header { background: var(--card-bg); padding: 20px; border-radius: 10px; border: 1px solid var(--border); margin-bottom: 20px; }\n");
    fprintf(f, "  h1 { font-size: 1.6rem; color: #fff; margin-bottom: 6px; }\n");
    fprintf(f, "  .meta-p { font-size: 0.9rem; color: var(--muted); display: flex; gap: 20px; flex-wrap: wrap; margin-bottom: 14px; }\n");
    fprintf(f, "  .toolbar { display: flex; align-items: center; gap: 15px; flex-wrap: wrap; padding-top: 12px; border-top: 1px solid var(--border); }\n");
    fprintf(f, "  .btn { background: var(--accent); color: #fff; border: none; padding: 8px 16px; border-radius: 6px; font-weight: 600; cursor: pointer; transition: opacity 0.2s; text-decoration: none; display: inline-block; }\n");
    fprintf(f, "  .btn:hover { opacity: 0.9; }\n");
    fprintf(f, "  .toggle-label { font-size: 0.9rem; color: var(--text); display: flex; align-items: center; gap: 6px; cursor: pointer; user-select: none; }\n");
    fprintf(f, "  .view-info { font-size: 0.85rem; color: #4caf50; font-family: monospace; font-weight: bold; }\n");
    fprintf(f, "  .legend { display: flex; gap: 15px; font-size: 0.8rem; margin-left: auto; }\n");
    fprintf(f, "  .legend-item { display: flex; align-items: center; gap: 6px; }\n");
    fprintf(f, "  .legend-box { width: 14px; height: 14px; border-radius: 3px; }\n");
    fprintf(f, "  .stem-card { background: var(--card-bg); border: 1px solid var(--border); border-radius: 10px; padding: 16px; margin-bottom: 16px; }\n");
    fprintf(f, "  .stem-header { display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 10px; margin-bottom: 10px; }\n");
    fprintf(f, "  .stem-title { font-size: 1.1rem; font-weight: bold; color: #fff; display: flex; align-items: center; gap: 10px; }\n");
    fprintf(f, "  .pitch-tag { background: #333344; color: #82aaff; padding: 2px 8px; border-radius: 4px; font-size: 0.85rem; font-family: monospace; }\n");
    fprintf(f, "  .stem-metrics { font-size: 0.85rem; color: var(--muted); display: flex; gap: 15px; font-family: monospace; }\n");
    fprintf(f, "  audio { height: 32px; outline: none; border-radius: 4px; }\n");
    fprintf(f, "  .canvas-container { position: relative; width: 100%%; height: 140px; background: #0d0d11; border-radius: 6px; border: 1px solid #22222e; overflow: hidden; margin-top: 8px; user-select: none; }\n");
    fprintf(f, "  canvas { width: 100%%; height: 100%%; display: block; cursor: crosshair; }\n");
    fprintf(f, "  .tooltip { position: absolute; top: 8px; right: 12px; background: rgba(10, 10, 15, 0.85); border: 1px solid #333344; padding: 4px 8px; border-radius: 4px; font-size: 0.75rem; font-family: monospace; color: #fff; pointer-events: none; opacity: 0; transition: opacity 0.15s; }\n");
    fprintf(f, "  .instructions { font-size: 0.8rem; color: var(--muted); margin-top: 6px; font-style: italic; }\n");
    fprintf(f, "</style>\n</head>\n<body>\n");

    fprintf(f, "<header>\n");
    fprintf(f, "  <h1>Spectral Flux Transience Report</h1>\n");
    fprintf(f, "  <div class=\"meta-p\">\n");
    fprintf(f, "    <span><strong>Source File:</strong> %s</span>\n", basename);
    fprintf(f, "    <span><strong>Duration:</strong> %.2fs</span>\n", duration);
    fprintf(f, "    <span><strong>Sample Rate:</strong> %u Hz</span>\n", sample_rate);
    fprintf(f, "    <span><strong>Channels:</strong> %u (%s)</span>\n", num_channels, num_channels == 1 ? "Mono" : "Stereo");
    fprintf(f, "    <span><strong>Stems:</strong> %d</span>\n", stem_count);
    fprintf(f, "  </div>\n");
    fprintf(f, "  <div class=\"toolbar\">\n");
    fprintf(f, "    <a class=\"btn\" href=\"index.html\">&larr; Batch Dashboard</a>\n");
    fprintf(f, "    <button class=\"btn\" id=\"resetZoomBtn\" onclick=\"resetZoom()\">Reset Zoom</button>\n");
    fprintf(f, "    <label class=\"toggle-label\"><input type=\"checkbox\" id=\"syncZoomCheck\" checked> Sync Zoom Across Rows</label>\n");
    fprintf(f, "    <span class=\"view-info\" id=\"viewInfo\">Visible: 0.000s &ndash; %.3fs (Duration: %.3fs)</span>\n", duration, duration);
    fprintf(f, "    <div class=\"legend\">\n");
    fprintf(f, "      <div class=\"legend-item\"><div class=\"legend-box\" style=\"background: rgba(180, 190, 210, 0.45);\"></div> Original Audio Waveform</div>\n");
    fprintf(f, "      <div class=\"legend-item\"><div class=\"legend-box\" style=\"background: #ff5722;\"></div> Raw Spectral Flux Envelope</div>\n");
    fprintf(f, "      <div class=\"legend-item\"><div class=\"legend-box\" style=\"background: #4caf50;\"></div> Modified Flux Envelope</div>\n");
    fprintf(f, "      <div class=\"legend-item\"><div class=\"legend-box\" style=\"background: #ffeb3b;\"></div> Detected Peaks (Top Marks)</div>\n");
    fprintf(f, "      <div class=\"legend-item\"><div class=\"legend-box\" style=\"background: #00bcd4;\"></div> Detected Troughs (Bottom Marks)</div>\n");
    fprintf(f, "    </div>\n");
    fprintf(f, "  </div>\n");
    fprintf(f, "  <div class=\"instructions\">Click and drag horizontally on any graph to zoom in. Right-click on a graph to zoom out one level. Click 'Reset Zoom' to restore full view.</div>\n");
    fprintf(f, "</header>\n\n");

    fprintf(f, "<div id=\"stemsContainer\">\n");
    for (int i = 0; i < stem_count; i++) {
        const FluxStem *st = &stems[i];
        fprintf(f, "  <div class=\"stem-card\">\n");
        fprintf(f, "    <div class=\"stem-header\">\n");
        fprintf(f, "      <div class=\"stem-title\">\n");
        fprintf(f, "        <span class=\"pitch-tag\">MIDI %03d</span> %s (%.2f Hz)\n", st->pitch, st->note_name, midi_to_frequency(st->pitch));
        fprintf(f, "      </div>\n");
        fprintf(f, "      <div class=\"stem-metrics\">\n");
        fprintf(f, "        <span>Peaks: %d</span>\n", st->num_peaks);
        fprintf(f, "        <span>Troughs: %d</span>\n", st->num_troughs);
        fprintf(f, "      </div>\n");
        fprintf(f, "      <audio controls src=\"../%s\" preload=\"metadata\"></audio>\n", st->mod_filename);
        fprintf(f, "    </div>\n");
        fprintf(f, "    <div class=\"canvas-container\">\n");
        fprintf(f, "      <canvas id=\"canvas-%d\" data-index=\"%d\"></canvas>\n", i, i);
        fprintf(f, "      <div class=\"tooltip\" id=\"tooltip-%d\"></div>\n", i);
        fprintf(f, "    </div>\n");
        fprintf(f, "  </div>\n");
    }

    fprintf(f, "</div>\n\n");

    char js_data_filename[1024];
    snprintf(js_data_filename, sizeof(js_data_filename), "%s_flux_waveform_data.js", basename);
    char js_data_path[4096];
    snprintf(js_data_path, sizeof(js_data_path), "%s/%s", flux_dir, js_data_filename);

    FILE *fjs = fopen(js_data_path, "w");
    if (fjs) {
        fprintf(fjs, "window.WAVEFORM_DATA = {\n");

        int16_t *pcm_i16 = (int16_t*)malloc(total_samples * sizeof(int16_t));

        if (orig_audio_buffer && pcm_i16) {
            for (uint32_t s = 0; s < total_samples; s++) {
                double sum_f = 0.0;
                for (uint32_t c = 0; c < num_channels; c++) {
                    sum_f += orig_audio_buffer[s * num_channels + c];
                }
                double val = sum_f / num_channels;
                if (val > 1.0) val = 1.0;
                if (val < -1.0) val = -1.0;
                pcm_i16[s] = (int16_t)round(val * 32767.0);
            }
            char *b64 = encode_base64_bytes((const uint8_t*)pcm_i16, total_samples * sizeof(int16_t));
            fprintf(fjs, "  origPcm: \"%s\",\n", b64 ? b64 : "");
            if (b64) free(b64);
        } else {
            fprintf(fjs, "  origPcm: \"\",\n");
        }
        if (pcm_i16) free(pcm_i16);

        fprintf(fjs, "  rawFluxPcm: [\n");
        for (int i = 0; i < stem_count; i++) {
            const FluxStem *st = &stems[i];
            if (st->raw_flux_i16) {
                char *b64 = encode_base64_bytes((const uint8_t*)st->raw_flux_i16, total_samples * sizeof(int16_t));
                fprintf(fjs, "    \"%s\"%s\n", b64 ? b64 : "", (i + 1 < stem_count) ? "," : "");
                if (b64) free(b64);
            } else {
                fprintf(fjs, "    \"\"%s\n", (i + 1 < stem_count) ? "," : "");
            }
        }
        fprintf(fjs, "  ],\n");

        fprintf(fjs, "  modFluxPcm: [\n");
        for (int i = 0; i < stem_count; i++) {
            const FluxStem *st = &stems[i];
            if (st->mod_flux_i16) {
                char *b64 = encode_base64_bytes((const uint8_t*)st->mod_flux_i16, total_samples * sizeof(int16_t));
                fprintf(fjs, "    \"%s\"%s\n", b64 ? b64 : "", (i + 1 < stem_count) ? "," : "");
                if (b64) free(b64);
            } else {
                fprintf(fjs, "    \"\"%s\n", (i + 1 < stem_count) ? "," : "");
            }
        }
        fprintf(fjs, "  ],\n");

        fprintf(fjs, "  peaks: [\n");
        for (int i = 0; i < stem_count; i++) {
            const FluxStem *st = &stems[i];
            fprintf(fjs, "    [");
            for (int k = 0; k < st->num_peaks; k++) {
                fprintf(fjs, "%d%s", st->peaks[k], (k + 1 < st->num_peaks) ? "," : "");
            }
            fprintf(fjs, "]%s\n", (i + 1 < stem_count) ? "," : "");
        }
        fprintf(fjs, "  ],\n");

        fprintf(fjs, "  troughs: [\n");
        for (int i = 0; i < stem_count; i++) {
            const FluxStem *st = &stems[i];
            fprintf(fjs, "    [");
            for (int k = 0; k < st->num_troughs; k++) {
                fprintf(fjs, "%d%s", st->troughs[k], (k + 1 < st->num_troughs) ? "," : "");
            }
            fprintf(fjs, "]%s\n", (i + 1 < stem_count) ? "," : "");
        }
        fprintf(fjs, "  ]\n");

        fprintf(fjs, "};\n");
        fclose(fjs);
    }

    fprintf(f, "<script src=\"%s\"></script>\n", js_data_filename);
    fprintf(f, "<script>\n");
    fprintf(f, "const DATA = {\n");
    fprintf(f, "  sampleRate: %u,\n", sample_rate);
    fprintf(f, "  numChannels: %u,\n", num_channels);
    fprintf(f, "  totalSamples: %u,\n", total_samples);
    fprintf(f, "  duration: %.6f,\n", duration);
    fprintf(f, "  stems: [\n");

    for (int i = 0; i < stem_count; i++) {
        const FluxStem *st = &stems[i];
        fprintf(f, "    {\n");
        fprintf(f, "      pitch: %d,\n", st->pitch);
        fprintf(f, "      name: \"%s\",\n", st->note_name);
        fprintf(f, "      file: \"../%s\"\n", st->mod_filename);
        fprintf(f, "    }%s\n", (i + 1 < stem_count) ? "," : "");
    }

    fprintf(f, "  ]\n");
    fprintf(f, "};\n\n");

    fprintf(f, "function base64ToFloat32(b64) {\n");
    fprintf(f, "  if (!b64) return new Float32Array(DATA.totalSamples);\n");
    fprintf(f, "  const binaryString = atob(b64);\n");
    fprintf(f, "  const len = binaryString.length;\n");
    fprintf(f, "  const bytes = new Uint8Array(len);\n");
    fprintf(f, "  for (let i = 0; i < len; i++) bytes[i] = binaryString.charCodeAt(i);\n");
    fprintf(f, "  const i16 = new Int16Array(bytes.buffer, 0, Math.floor(len / 2));\n");
    fprintf(f, "  const f32 = new Float32Array(i16.length);\n");
    fprintf(f, "  for (let i = 0; i < i16.length; i++) f32[i] = i16[i] / 32767.0;\n");
    fprintf(f, "  return f32;\n");
    fprintf(f, "}\n\n");

    fprintf(f, "function formatMSSPrecise(sec) {\n");
    fprintf(f, "  if (isNaN(sec) || sec < 0) sec = 0;\n");
    fprintf(f, "  const m = Math.floor(sec / 60);\n");
    fprintf(f, "  const s = sec %% 60;\n");
    fprintf(f, "  const sStr = s < 10 ? '0' + s.toFixed(2) : s.toFixed(2);\n");
    fprintf(f, "  return `${m}:${sStr}`;\n");
    fprintf(f, "}\n\n");

    fprintf(f, "function formatMSS(sec) {\n");
    fprintf(f, "  if (isNaN(sec) || sec < 0) sec = 0;\n");
    fprintf(f, "  const m = Math.floor(sec / 60);\n");
    fprintf(f, "  const s = sec %% 60;\n");
    fprintf(f, "  const sInt = Math.floor(s);\n");
    fprintf(f, "  const sStr = sInt < 10 ? '0' + sInt : '' + sInt;\n");
    fprintf(f, "  return `${m}:${sStr}`;\n");
    fprintf(f, "}\n\n");

    fprintf(f, "function initAudioData() {\n");
    fprintf(f, "  if (window.WAVEFORM_DATA) {\n");
    fprintf(f, "    if (window.WAVEFORM_DATA.origPcm) {\n");
    fprintf(f, "      DATA.origPcm = base64ToFloat32(window.WAVEFORM_DATA.origPcm);\n");
    fprintf(f, "    }\n");
    fprintf(f, "    if (window.WAVEFORM_DATA.rawFluxPcm) {\n");
    fprintf(f, "      for (let i = 0; i < DATA.stems.length; i++) {\n");
    fprintf(f, "        if (window.WAVEFORM_DATA.rawFluxPcm[i]) {\n");
    fprintf(f, "          DATA.stems[i].rawFluxPcm = base64ToFloat32(window.WAVEFORM_DATA.rawFluxPcm[i]);\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "    if (window.WAVEFORM_DATA.modFluxPcm) {\n");
    fprintf(f, "      for (let i = 0; i < DATA.stems.length; i++) {\n");
    fprintf(f, "        if (window.WAVEFORM_DATA.modFluxPcm[i]) {\n");
    fprintf(f, "          DATA.stems[i].modFluxPcm = base64ToFloat32(window.WAVEFORM_DATA.modFluxPcm[i]);\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "    if (window.WAVEFORM_DATA.peaks) {\n");
    fprintf(f, "      for (let i = 0; i < DATA.stems.length; i++) {\n");
    fprintf(f, "        DATA.stems[i].peaks = window.WAVEFORM_DATA.peaks[i] || [];\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "    if (window.WAVEFORM_DATA.troughs) {\n");
    fprintf(f, "      for (let i = 0; i < DATA.stems.length; i++) {\n");
    fprintf(f, "        DATA.stems[i].troughs = window.WAVEFORM_DATA.troughs[i] || [];\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n");
    fprintf(f, "  renderAll();\n");
    fprintf(f, "}\n\n");

    fprintf(f, "let viewStart = 0.0;\n");
    fprintf(f, "let viewEnd = DATA.duration;\n");
    fprintf(f, "let zoomHistory = [];\n");
    fprintf(f, "let activeDrag = null;\n");
    fprintf(f, "let hoverState = null;\n");

    fprintf(f, "function timeToX(t, width) {\n");
    fprintf(f, "  return ((t - viewStart) / (viewEnd - viewStart)) * width;\n");
    fprintf(f, "}\n");
    fprintf(f, "function xToTime(x, width) {\n");
    fprintf(f, "  return viewStart + (x / width) * (viewEnd - viewStart);\n");
    fprintf(f, "}\n");

    fprintf(f, "function updateViewInfo() {\n");
    fprintf(f, "  const dur = viewEnd - viewStart;\n");
    fprintf(f, "  document.getElementById('viewInfo').innerHTML = `Visible: ${formatMSSPrecise(viewStart)} &ndash; ${formatMSSPrecise(viewEnd)} (Duration: ${formatMSSPrecise(dur)})`;\n");
    fprintf(f, "}\n");

    fprintf(f, "function resetZoom() {\n");
    fprintf(f, "  zoomHistory = [];\n");
    fprintf(f, "  viewStart = 0.0;\n");
    fprintf(f, "  viewEnd = DATA.duration;\n");
    fprintf(f, "  updateViewInfo();\n");
    fprintf(f, "  renderAll();\n");
    fprintf(f, "}\n");

    fprintf(f, "function zoomOut() {\n");
    fprintf(f, "  if (zoomHistory.length > 0) {\n");
    fprintf(f, "    const prev = zoomHistory.pop();\n");
    fprintf(f, "    viewStart = prev.start;\n");
    fprintf(f, "    viewEnd = prev.end;\n");
    fprintf(f, "    updateViewInfo();\n");
    fprintf(f, "    renderAll();\n");
    fprintf(f, "  }\n");
    fprintf(f, "}\n");

    fprintf(f, "function renderCanvas(index) {\n");
    fprintf(f, "  const canvas = document.getElementById(`canvas-${index}`);\n");
    fprintf(f, "  if (!canvas) return;\n");
    fprintf(f, "  const rect = canvas.getBoundingClientRect();\n");
    fprintf(f, "  const dpr = window.devicePixelRatio || 1;\n");
    fprintf(f, "  const width = rect.width;\n");
    fprintf(f, "  const height = rect.height;\n");
    fprintf(f, "  if (width === 0 || height === 0) return;\n");
    fprintf(f, "  if (canvas.width !== Math.floor(width * dpr) || canvas.height !== Math.floor(height * dpr)) {\n");
    fprintf(f, "    canvas.width = Math.floor(width * dpr);\n");
    fprintf(f, "    canvas.height = Math.floor(height * dpr);\n");
    fprintf(f, "  }\n");
    fprintf(f, "  const ctx = canvas.getContext('2d');\n");
    fprintf(f, "  ctx.save();\n");
    fprintf(f, "  ctx.scale(dpr, dpr);\n");
    fprintf(f, "  ctx.clearRect(0, 0, width, height);\n");

    fprintf(f, "  const graphBottom = height - 18;\n");
    fprintf(f, "  const graphTop = 8;\n");
    fprintf(f, "  const graphHeight = graphBottom - graphTop;\n");

    fprintf(f, "  // Grid lines\n");
    fprintf(f, "  ctx.strokeStyle = '#1e1e2c';\n");
    fprintf(f, "  ctx.lineWidth = 1;\n");
    fprintf(f, "  [0.0, 0.25, 0.5, 0.75, 1.0].forEach(val => {\n");
    fprintf(f, "    const y = graphBottom - val * graphHeight;\n");
    fprintf(f, "    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(width, y); ctx.stroke();\n");
    fprintf(f, "  });\n");

    fprintf(f, "  const sampleStart = Math.max(0, Math.floor((viewStart / DATA.duration) * DATA.totalSamples));\n");
    fprintf(f, "  const sampleEnd = Math.min(DATA.totalSamples - 1, Math.ceil((viewEnd / DATA.duration) * DATA.totalSamples));\n");
    fprintf(f, "  const visibleSamples = Math.max(1, sampleEnd - sampleStart + 1);\n");
    fprintf(f, "  const stem = DATA.stems[index];\n");

    fprintf(f, "  function drawOriginalWaveform(pcm) {\n");
    fprintf(f, "    if (!pcm || pcm.length === 0) return;\n");
    fprintf(f, "    ctx.fillStyle = 'rgba(180, 190, 210, 0.35)';\n");
    fprintf(f, "    ctx.strokeStyle = 'rgba(180, 190, 210, 0.55)';\n");
    fprintf(f, "    ctx.lineWidth = 1;\n");
    fprintf(f, "    const midY = graphTop + graphHeight / 2;\n");
    fprintf(f, "    const halfH = graphHeight / 2;\n");
    fprintf(f, "    if (visibleSamples <= width * 2) {\n");
    fprintf(f, "      ctx.beginPath();\n");
    fprintf(f, "      for (let s = sampleStart; s <= sampleEnd; s++) {\n");
    fprintf(f, "        const t = s / DATA.sampleRate;\n");
    fprintf(f, "        const x = timeToX(t, width);\n");
    fprintf(f, "        const val = pcm[s];\n");
    fprintf(f, "        const y = midY - val * halfH;\n");
    fprintf(f, "        if (s === sampleStart) ctx.moveTo(x, y); else ctx.lineTo(x, y);\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.stroke();\n");
    fprintf(f, "    } else {\n");
    fprintf(f, "      const pxWidth = Math.ceil(width);\n");
    fprintf(f, "      for (let px = 0; px < pxWidth; px++) {\n");
    fprintf(f, "        const t1 = xToTime(px, width);\n");
    fprintf(f, "        const t2 = xToTime(px + 1, width);\n");
    fprintf(f, "        const s1 = Math.max(0, Math.floor(t1 * DATA.sampleRate));\n");
    fprintf(f, "        const s2 = Math.min(DATA.totalSamples - 1, Math.ceil(t2 * DATA.sampleRate));\n");
    fprintf(f, "        if (s1 <= s2 && s1 < DATA.totalSamples) {\n");
    fprintf(f, "          let minV = pcm[s1];\n");
    fprintf(f, "          let maxV = pcm[s1];\n");
    fprintf(f, "          for (let s = s1; s <= s2; s++) {\n");
    fprintf(f, "            const v = pcm[s];\n");
    fprintf(f, "            if (v < minV) minV = v;\n");
    fprintf(f, "            if (v > maxV) maxV = v;\n");
    fprintf(f, "          }\n");
    fprintf(f, "          const y1 = midY - maxV * halfH;\n");
    fprintf(f, "          const y2 = midY - minV * halfH;\n");
    fprintf(f, "          ctx.fillRect(px, y1, 1, Math.max(1, y2 - y1));\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  function drawFluxLine(pcm, color) {\n");
    fprintf(f, "    if (!pcm) return;\n");
    fprintf(f, "    ctx.strokeStyle = color;\n");
    fprintf(f, "    ctx.lineWidth = 1.8;\n");
    fprintf(f, "    if (visibleSamples <= width * 2) {\n");
    fprintf(f, "      ctx.beginPath();\n");
    fprintf(f, "      for (let s = sampleStart; s <= sampleEnd; s++) {\n");
    fprintf(f, "        const t = s / DATA.sampleRate;\n");
    fprintf(f, "        const x = timeToX(t, width);\n");
    fprintf(f, "        const val = Math.max(0, Math.min(1, pcm[s]));\n");
    fprintf(f, "        const y = graphBottom - val * graphHeight;\n");
    fprintf(f, "        if (s === sampleStart) ctx.moveTo(x, y); else ctx.lineTo(x, y);\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.stroke();\n");
    fprintf(f, "    } else {\n");
    fprintf(f, "      const pxWidth = Math.ceil(width);\n");
    fprintf(f, "      const maxArr = new Float32Array(pxWidth);\n");
    fprintf(f, "      for (let px = 0; px < pxWidth; px++) {\n");
    fprintf(f, "        const t1 = xToTime(px, width);\n");
    fprintf(f, "        const t2 = xToTime(px + 1, width);\n");
    fprintf(f, "        const s1 = Math.max(0, Math.floor(t1 * DATA.sampleRate));\n");
    fprintf(f, "        const s2 = Math.min(DATA.totalSamples - 1, Math.ceil(t2 * DATA.sampleRate));\n");
    fprintf(f, "        if (s1 <= s2 && s1 < DATA.totalSamples) {\n");
    fprintf(f, "          let maxV = pcm[s1];\n");
    fprintf(f, "          for (let s = s1; s <= s2; s++) {\n");
    fprintf(f, "            const v = pcm[s];\n");
    fprintf(f, "            if (v > maxV) maxV = v;\n");
    fprintf(f, "          }\n");
    fprintf(f, "          maxArr[px] = Math.max(0, Math.min(1, maxV));\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.beginPath();\n");
    fprintf(f, "      for (let px = 0; px < pxWidth; px++) {\n");
    fprintf(f, "        const y = graphBottom - maxArr[px] * graphHeight;\n");
    fprintf(f, "        if (px === 0) ctx.moveTo(px, y); else ctx.lineTo(px, y);\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.stroke();\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // 0. Draw Original Waveform Background\n");
    fprintf(f, "  if (DATA.origPcm) {\n");
    fprintf(f, "    drawOriginalWaveform(DATA.origPcm);\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // 1. Draw Raw Spectral Flux Envelope\n");
    fprintf(f, "  drawFluxLine(stem.rawFluxPcm, '#ff5722');\n");

    fprintf(f, "  // 2. Draw Modified Flux Envelope\n");
    fprintf(f, "  drawFluxLine(stem.modFluxPcm, '#4caf50');\n");

    fprintf(f, "  // 3. Draw Peak Hash Marks (coming down from top)\n");
    fprintf(f, "  if (stem.peaks) {\n");
    fprintf(f, "    ctx.strokeStyle = '#ffeb3b';\n");
    fprintf(f, "    ctx.lineWidth = 1.5;\n");
    fprintf(f, "    for (let k = 0; k < stem.peaks.length; k++) {\n");
    fprintf(f, "      const s = stem.peaks[k];\n");
    fprintf(f, "      if (s >= sampleStart && s <= sampleEnd) {\n");
    fprintf(f, "        const t = s / DATA.sampleRate;\n");
    fprintf(f, "        const x = timeToX(t, width);\n");
    fprintf(f, "        ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, 10); ctx.stroke();\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // 4. Draw Trough Hash Marks (coming up from bottom)\n");
    fprintf(f, "  if (stem.troughs) {\n");
    fprintf(f, "    ctx.strokeStyle = '#00bcd4';\n");
    fprintf(f, "    ctx.lineWidth = 1.5;\n");
    fprintf(f, "    for (let k = 0; k < stem.troughs.length; k++) {\n");
    fprintf(f, "      const s = stem.troughs[k];\n");
    fprintf(f, "      if (s >= sampleStart && s <= sampleEnd) {\n");
    fprintf(f, "        const t = s / DATA.sampleRate;\n");
    fprintf(f, "        const x = timeToX(t, width);\n");
    fprintf(f, "        ctx.beginPath(); ctx.moveTo(x, graphBottom); ctx.lineTo(x, graphBottom - 10); ctx.stroke();\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // Time Axis Legend Along Bottom\n");
    fprintf(f, "  const legendY = height - 16;\n");
    fprintf(f, "  ctx.strokeStyle = 'rgba(255, 255, 255, 0.3)';\n");
    fprintf(f, "  ctx.lineWidth = 1;\n");
    fprintf(f, "  ctx.beginPath(); ctx.moveTo(0, legendY); ctx.lineTo(width, legendY); ctx.stroke();\n");

    fprintf(f, "  ctx.fillStyle = '#a0a0b0';\n");
    fprintf(f, "  ctx.font = '10px monospace';\n");
    fprintf(f, "  const numTicks = 8;\n");
    fprintf(f, "  const durWindow = viewEnd - viewStart;\n");
    fprintf(f, "  for (let i = 0; i <= numTicks; i++) {\n");
    fprintf(f, "    const frac = i / numTicks;\n");
    fprintf(f, "    const x = frac * width;\n");
    fprintf(f, "    const timeVal = viewStart + frac * durWindow;\n");
    fprintf(f, "    ctx.beginPath(); ctx.moveTo(x, legendY); ctx.lineTo(x, legendY + 4); ctx.stroke();\n");
    fprintf(f, "    if (i === 0) ctx.textAlign = 'left';\n");
    fprintf(f, "    else if (i === numTicks) ctx.textAlign = 'right';\n");
    fprintf(f, "    else ctx.textAlign = 'center';\n");
    fprintf(f, "    ctx.fillText(formatMSS(timeVal), x, height - 2);\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // Drag selection highlight\n");
    fprintf(f, "  if (activeDrag && activeDrag.index === index) {\n");
    fprintf(f, "    const x1 = Math.min(activeDrag.startX, activeDrag.currentX);\n");
    fprintf(f, "    const x2 = Math.max(activeDrag.startX, activeDrag.currentX);\n");
    fprintf(f, "    ctx.fillStyle = 'rgba(255, 235, 59, 0.25)';\n");
    fprintf(f, "    ctx.fillRect(x1, 0, x2 - x1, height);\n");
    fprintf(f, "    ctx.strokeStyle = '#ffeb3b';\n");
    fprintf(f, "    ctx.lineWidth = 1.5;\n");
    fprintf(f, "    ctx.strokeRect(x1, 0, x2 - x1, height);\n");
    fprintf(f, "  }\n");

    fprintf(f, "  // Hover crosshair\n");
    fprintf(f, "  if (hoverState && hoverState.index === index) {\n");
    fprintf(f, "    ctx.strokeStyle = 'rgba(255, 255, 255, 0.6)';\n");
    fprintf(f, "    ctx.setLineDash([4, 4]);\n");
    fprintf(f, "    ctx.beginPath(); ctx.moveTo(hoverState.mouseX, 0); ctx.lineTo(hoverState.mouseX, height); ctx.stroke();\n");
    fprintf(f, "    ctx.setLineDash([]);\n");
    fprintf(f, "  }\n");

    fprintf(f, "  ctx.restore();\n");
    fprintf(f, "}\n");

    fprintf(f, "function renderAll() {\n");
    fprintf(f, "  for (let i = 0; i < DATA.stems.length; i++) {\n");
    fprintf(f, "    renderCanvas(i);\n");
    fprintf(f, "  }\n");
    fprintf(f, "}\n");

    fprintf(f, "function attachListeners() {\n");
    fprintf(f, "  DATA.stems.forEach((stem, idx) => {\n");
    fprintf(f, "    const canvas = document.getElementById(`canvas-${idx}`);\n");
    fprintf(f, "    const tooltip = document.getElementById(`tooltip-${idx}`);\n");
    fprintf(f, "    if (!canvas) return;\n");

    fprintf(f, "    canvas.addEventListener('mousedown', (e) => {\n");
    fprintf(f, "      const rect = canvas.getBoundingClientRect();\n");
    fprintf(f, "      const x = e.clientX - rect.left;\n");
    fprintf(f, "      activeDrag = { index: idx, startX: x, currentX: x };\n");
    fprintf(f, "      renderCanvas(idx);\n");
    fprintf(f, "    });\n");

    fprintf(f, "    canvas.addEventListener('mousemove', (e) => {\n");
    fprintf(f, "      const rect = canvas.getBoundingClientRect();\n");
    fprintf(f, "      const x = Math.max(0, Math.min(rect.width, e.clientX - rect.left));\n");
    fprintf(f, "      hoverState = { index: idx, mouseX: x };\n");
    fprintf(f, "      if (activeDrag) {\n");
    fprintf(f, "        activeDrag.currentX = x;\n");
    fprintf(f, "      }\n");
    fprintf(f, "      renderCanvas(idx);\n");

    fprintf(f, "      const t = xToTime(x, rect.width);\n");
    fprintf(f, "      const s = Math.min(DATA.totalSamples - 1, Math.max(0, Math.round(t * DATA.sampleRate)));\n");
    fprintf(f, "      const rawVal = (stem.rawFluxPcm && stem.rawFluxPcm.length > s) ? stem.rawFluxPcm[s] : 0;\n");
    fprintf(f, "      const modVal = (stem.modFluxPcm && stem.modFluxPcm.length > s) ? stem.modFluxPcm[s] : 0;\n");
    fprintf(f, "      tooltip.style.opacity = '1';\n");
    fprintf(f, "      tooltip.innerHTML = `Time: ${formatMSSPrecise(t)} (Sample #${s}) | Raw Flux: ${rawVal.toFixed(4)} | Modified Flux: ${modVal.toFixed(4)}`;\n");
    fprintf(f, "    });\n");

    fprintf(f, "    canvas.addEventListener('mouseleave', () => {\n");
    fprintf(f, "      if (activeDrag && activeDrag.index === idx) {\n");
    fprintf(f, "        activeDrag = null;\n");
    fprintf(f, "      }\n");
    fprintf(f, "      hoverState = null;\n");
    fprintf(f, "      tooltip.style.opacity = '0';\n");
    fprintf(f, "      renderCanvas(idx);\n");
    fprintf(f, "    });\n");

    fprintf(f, "    canvas.addEventListener('mouseup', (e) => {\n");
    fprintf(f, "      if (activeDrag && activeDrag.index === idx) {\n");
    fprintf(f, "        const rect = canvas.getBoundingClientRect();\n");
    fprintf(f, "        const x1 = Math.min(activeDrag.startX, activeDrag.currentX);\n");
    fprintf(f, "        const x2 = Math.max(activeDrag.startX, activeDrag.currentX);\n");
    fprintf(f, "        activeDrag = null;\n");
    fprintf(f, "        if (x2 - x1 > 4) {\n");
    fprintf(f, "          const t1 = xToTime(x1, rect.width);\n");
    fprintf(f, "          const t2 = xToTime(x2, rect.width);\n");
    fprintf(f, "          zoomHistory.push({ start: viewStart, end: viewEnd });\n");
    fprintf(f, "          viewStart = Math.max(0, t1);\n");
    fprintf(f, "          viewEnd = Math.min(DATA.duration, t2);\n");
    fprintf(f, "          updateViewInfo();\n");
    fprintf(f, "          renderAll();\n");
    fprintf(f, "        } else {\n");
    fprintf(f, "          renderCanvas(idx);\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");
    fprintf(f, "    });\n");

    fprintf(f, "    canvas.addEventListener('contextmenu', (e) => {\n");
    fprintf(f, "      e.preventDefault();\n");
    fprintf(f, "      zoomOut();\n");
    fprintf(f, "    });\n");
    fprintf(f, "  });\n");
    fprintf(f, "}\n");

    fprintf(f, "window.addEventListener('resize', renderAll);\n");
    fprintf(f, "window.addEventListener('DOMContentLoaded', () => {\n");
    fprintf(f, "  attachListeners();\n");
    fprintf(f, "  renderAll();\n");
    fprintf(f, "  initAudioData();\n");
    fprintf(f, "});\n");

    fprintf(f, "</script>\n");
    fprintf(f, "</body>\n</html>\n");

    fclose(f);
}

void generate_master_flux_html_report(
    const char *flux_dir,
    const FileBatchResult *results,
    int num_results,
    double total_wall_clock_sec
) {
    char master_html_path[4096];
    snprintf(master_html_path, sizeof(master_html_path), "%s/index.html", flux_dir);

    FILE *f = fopen(master_html_path, "w");
    if (!f) return;

    if (num_results == 1 && (results[0].success || results[0].skipped)) {
        /* Single file redirect to <basename>_index.html */
        fprintf(f, "<!DOCTYPE html>\n<html>\n<head>\n");
        fprintf(f, "<meta http-equiv=\"refresh\" content=\"0; url=%s_index.html\">\n", results[0].basename);
        fprintf(f, "<title>Spectral Flux Report Redirect</title>\n</head>\n<body>\n");
        fprintf(f, "<p>Redirecting to <a href=\"%s_index.html\">%s Spectral Flux Report</a>...</p>\n", results[0].basename, results[0].basename);
        fprintf(f, "</body>\n</html>\n");
        fclose(f);
    } else {
        /* Multi-file Master Batch Dashboard */
        fprintf(f, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n");
        fprintf(f, "<meta charset=\"UTF-8\">\n");
        fprintf(f, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n");
        fprintf(f, "<title>Spectral Flux Batch Analysis Dashboard</title>\n");
        fprintf(f, "<style>\n");
        fprintf(f, "  :root { --bg: #121216; --card-bg: #1a1a22; --border: #2a2a36; --text: #e1e1e6; --muted: #a0a0b0; --accent: #2196f3; }\n");
        fprintf(f, "  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; }\n");
        fprintf(f, "  body { background: var(--bg); color: var(--text); padding: 24px; line-height: 1.5; }\n");
        fprintf(f, "  header { background: var(--card-bg); padding: 24px; border-radius: 12px; border: 1px solid var(--border); margin-bottom: 24px; }\n");
        fprintf(f, "  h1 { font-size: 1.8rem; color: #fff; margin-bottom: 8px; }\n");
        fprintf(f, "  .batch-stats { display: flex; gap: 24px; flex-wrap: wrap; font-size: 0.95rem; color: var(--muted); margin-top: 10px; }\n");
        fprintf(f, "  .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(320px, 1fr)); gap: 20px; }\n");
        fprintf(f, "  .card { background: var(--card-bg); border: 1px solid var(--border); border-radius: 10px; padding: 20px; transition: transform 0.2s, border-color 0.2s; }\n");
        fprintf(f, "  .card:hover { border-color: var(--accent); transform: translateY(-2px); }\n");
        fprintf(f, "  .card-title { font-size: 1.1rem; font-weight: bold; color: #fff; margin-bottom: 10px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }\n");
        fprintf(f, "  .card-meta { font-size: 0.85rem; color: var(--muted); font-family: monospace; display: flex; flex-direction: column; gap: 4px; margin-bottom: 16px; }\n");
        fprintf(f, "  .btn { display: inline-block; background: var(--accent); color: #fff; text-decoration: none; padding: 8px 16px; border-radius: 6px; font-weight: 600; font-size: 0.9rem; text-align: center; width: 100%%; transition: opacity 0.2s; }\n");
        fprintf(f, "  .btn:hover { opacity: 0.9; }\n");
        fprintf(f, "  .status-badge { display: inline-block; padding: 2px 8px; border-radius: 4px; font-size: 0.75rem; font-weight: bold; font-family: monospace; }\n");
        fprintf(f, "  .badge-success { background: #1b5e20; color: #81c784; }\n");
        fprintf(f, "  .badge-fail { background: #b71c1c; color: #e57373; }\n");
    fprintf(f, "  .badge-skip { background: #4a3b12; color: #ffd54f; }\n");
        fprintf(f, "</style>\n</head>\n<body>\n");

        fprintf(f, "<header>\n");
        fprintf(f, "  <h1>Spectral Flux Batch Analysis Dashboard</h1>\n");
        fprintf(f, "  <div class=\"batch-stats\">\n");
        fprintf(f, "    <span><strong>Files Reported:</strong> %d</span>\n", num_results);
        fprintf(f, "    <span><strong>Total Report Build Time:</strong> %.2fs</span>\n", total_wall_clock_sec);
        fprintf(f, "  </div>\n");
        fprintf(f, "</header>\n\n");

        fprintf(f, "<div class=\"grid\">\n");
        for (int i = 0; i < num_results; i++) {
            const FileBatchResult *r = &results[i];
            fprintf(f, "  <div class=\"card\">\n");
            fprintf(f, "    <div class=\"card-title\" title=\"%s\">%s.wav</div>\n", r->basename, r->basename);
            fprintf(f, "    <div class=\"card-meta\">\n");
            fprintf(f, "      <span>Status: %s</span>\n", r->skipped
                ? "<span class=\"status-badge badge-skip\">SKIPPED (EXISTING STEMS)</span>"
                : (r->success
                    ? "<span class=\"status-badge badge-success\">COMPLETE</span>"
                    : "<span class=\"status-badge badge-fail\">FAILED</span>"));
            if (r->skipped) {
                fprintf(f, "      <span>Pitch stems already existed; analysis skipped.</span>\n");
            }
            fprintf(f, "      <span>Duration: %.2fs (%u Hz, %s)</span>\n", r->duration, r->sample_rate, r->num_channels == 1 ? "Mono" : "Stereo");
            fprintf(f, "      <span>Flux Stems Exported: %d</span>\n", r->stem_count);
            fprintf(f, "      <span>Report Build Time: %.2fs</span>\n", r->elapsed_sec);
            fprintf(f, "    </div>\n");
            if (r->success || r->skipped) {
                fprintf(f, "    <a class=\"btn\" href=\"%s_index.html\">Open Interactive Report &rarr;</a>\n", r->basename);
            }
            fprintf(f, "  </div>\n");
        }
        fprintf(f, "</div>\n");
        fprintf(f, "</body>\n</html>\n");
        fclose(f);
    }
}
