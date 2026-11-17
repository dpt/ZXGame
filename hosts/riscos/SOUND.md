# RISC OS sound: plan

A plan for sound on 8-bit RISC OS machines (VIDC1 logarithmic output:
Archimedes, A3000, A5000 on RISC OS 2/3.1, and later machines without 16-bit
sound), and for sharing one audio renderer across all ZXGame front-ends.

## Current state

* **SDL3** (`hosts/sdl3/SDLHost.c`) is the only host that uses the `tstates`
  argument. It queues timestamped speaker and AY events and renders them on
  the audio thread with a DC-block filter.
* **macOS** (`hosts/macos/ZXAudio.c`) and **RISC OS**
  (`hosts/riscos/zxgame.c`, `emit_sound`) ignore `tstates`. They push speaker
  bits into a `bitfifo` and average five bits per output sample, so speaker
  timing is approximate.
* **RISC OS** passes `ay_out = NULL`, so it has no AY output. Its only output
  is a 16-bit SharedSoundBuffer stream, detected in `main.c`.
* `slopay-chip.c` is integer-only, which matters on FPU-less ARM2/ARM3.

## Plan

### 1. A shared renderer

Move the SDL host's event queue and renderer into
`src/ZXSpectrum/Audio.c`. It turns timestamped speaker and AY events into
signed 16-bit mono at a host-chosen rate, with DC blocking. Rough API:

```c
zxaudio_t *zxaudio_create(int rate, int tstates_per_sec);
void       zxaudio_speaker(zxaudio_t *a, int level, zxclock_t tstates);
void       zxaudio_ay(zxaudio_t *a, uint16_t port, uint8_t byte, zxclock_t tstates);
void       zxaudio_render(zxaudio_t *a, int16_t *buf, int n);
```

Each host then only delivers samples: an SDL stream, a Core Audio render
callback, a SharedSoundBuffer block or an 8-bit log buffer. SDL keeps its
behaviour; macOS and RISC OS 16-bit gain correct speaker timing and AY output.

The renderer must not use floating point. The SDL DC-block filter uses
`float` today and needs converting to fixed point.

### 2. Choose the output path at start-up

* SharedSound/SharedSoundBuffer present on 16-bit hardware: keep the existing
  stream, fed by `zxaudio_render`.
* Otherwise (the `RMEnsure`s fail, or `Sound_Mode` reports no 16-bit
  support): the 8-bit path below.

Check first whether SharedSound can mix down to 8-bit hardware. If it can,
most of step 3 is unnecessary.

### 3. The 8-bit path

**Constraint.** The sound fill routine runs from an interrupt, while the Wimp
pages application space out whenever another task runs. The fill code and its
buffer must therefore live in the RMA.

**Shape.** A small piece of ARM code, copied into an RMA block or built as a
tiny module, plus a single-producer single-consumer ring buffer of
ready-converted log bytes.

* The producer is the game, at each Wimp null event: render 16-bit samples,
  convert them to log bytes and write them into the ring.
* The consumer is the fill routine: copy bytes out, pad with silence on
  underrun. No arithmetic.

**Hooking in.** Install a voice with `Sound_InstallVoice` on one channel, so
other tasks' sounds still mix. Do not claim the channel handler: that silences
the rest of the desktop.

**Conversion.** Build a 256-entry table from the top eight bits of a linear
sample to the VIDC log byte, by calling `Sound_SoundLog` once at start-up.
Do not hand-code the log format.

**Rate.** Configure a sample period of about 48 µs (around 20.8 kHz) with
`Sound_Configure`, with a fallback of about 15.6 kHz for ARM2. Save the
previous configuration and restore it on exit and in `teardown_sound`.

**Buffering.** Hold about 100 ms in the ring (the host's existing `PERIOD`) to
ride out slow null-event polling.

### 4. CPU cost on ARM2/ARM3

Beeper rendering is cheap; AY emulation at about 20 kHz is the risk. If it
proves too slow, render the AY at half rate and duplicate each sample, marked
with a comment naming that ceiling. Measure with `timermod.c` before
optimising.

### 5. Order of work

1. Extract the renderer. Keep the headless `ctest` passing and add one check
   that renders a known speaker pattern and compares the output.
2. Switch SDL to the shared renderer. Confirm by ear that Chase H.Q.'s title
   drums are unchanged.
3. Switch RISC OS 16-bit to it and connect `ay_out`.
4. Add the 8-bit path. Test under Arculator (RISC OS 3.1) before real
   hardware.

## Not planned

Stereo and AY channel panning: the 8-bit voice is mono. Add them when a 16-bit
host wants them.
