/*
 * audio_probe.h -- taps the audio on its way to the DAC and turns it into
 * something a 128x32 panel can draw: a 32-band spectrum, stereo VU levels, a
 * triggered waveform, and a beat flag.
 *
 * Two halves, on two tasks:
 *
 *   audio_probe_feed()     Bluetooth task, once per decoded A2DP packet. Does
 *                          the absolute minimum -- downmix, halve the sample
 *                          rate, store, track peaks -- because it runs in line
 *                          with the audio path and anything slow here is
 *                          audible. No FFT, no floats, no logs.
 *
 *   audio_probe_analyse()  UI task, once per frame. Reads the most recent
 *                          FFT_SIZE samples out of the ring and does all the
 *                          expensive work: window, FFT, log banding, auto-gain,
 *                          smoothing, peak hold, beat detection.
 *
 * The ring buffer is deliberately lock-free. The writer only ever bumps a
 * monotonic counter; the reader copies backwards from wherever that counter
 * happens to be. If a write laps the reader mid-copy, one frame of the
 * visualiser gets a seam in it -- which is invisible at 30 fps and much cheaper
 * than making the audio task wait for a lock.
 *
 * Sample rate: A2DP is 44.1 kHz, and the feed averages sample pairs to get
 * 22.05 kHz. That halves the FFT cost and doubles the frequency resolution per
 * bin, at the price of everything above 11 kHz -- which no 32-bar display could
 * show separately anyway. The pair averaging doubles as the anti-alias filter.
 */

#pragma once

#include <math.h>  // A2DPVolumeControl.h uses pow() without including this
#include <stdint.h>

#include "A2DPVolumeControl.h"  // for Frame
#include "ui_config.h"

/// Number of columns in the oscilloscope trace -- one per display column.
static const uint8_t VIS_WAVE_POINTS = 128;

struct AudioVis {
  /// Bar heights, 0..255 mapped over VIS_RANGE_DB below the auto-gain ceiling.
  uint8_t bands[VIS_BANDS];
  /// Peak-hold caps for the same bands: hang, then fall slowly.
  uint8_t peaks[VIS_BANDS];

  /// Triggered waveform, -127..127, normalised to the window peak.
  int8_t wave[VIS_WAVE_POINTS];

  uint8_t vu_l, vu_r;    ///< RMS level per channel, 0..255, dB-mapped
  uint8_t peak_l, peak_r;  ///< peak-hold per channel, same scale
  uint8_t level;         ///< loudest band this frame, 0..255

  bool active;  ///< audio present (above the noise floor) in the last second
  bool beat;    ///< bass transient detected on this frame only

  float agc_db;  ///< current auto-gain ceiling in dBFS, shown on the info screen
};

/// Builds the window, twiddle and band tables. Call once from setup(), before
/// any feed() can happen.
void audio_probe_init();

/*
 * Bluetooth task. Frames are 16-bit stereo, exactly as they go to the DAC.
 *
 * One caller at a time. In this firmware the connect/disconnect melodies also
 * feed it, from loop(), but they only play when the A2DP stream is stopped, so
 * the two never overlap in practice. If they ever did, the worst case is a
 * garbled visualiser frame: every ring index is masked, so nothing can be
 * written out of bounds whatever order the two writers land in.
 */
void audio_probe_feed(const Frame *frames, uint16_t count);

/// UI task. dt_ms is the time since the previous call, and drives every decay
/// rate so the animation looks the same at any frame rate.
const AudioVis &audio_probe_analyse(uint32_t dt_ms);

/// millis() of the last time audio was above the noise floor. Used by the UI to
/// decide when to stop showing visualisers and go idle.
uint32_t audio_probe_last_active();
