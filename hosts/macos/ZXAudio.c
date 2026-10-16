/* ZXAudio.c -- Spectrum speaker output through Core Audio */

#include <assert.h>
#include <pthread.h>
#include <stdlib.h>

#include <AudioUnit/AudioUnit.h>

#include "ZXSpectrum/Spectrum.h"
#include "ZXSpectrum/slopay-chip.h"

#include "bitfifo.h"

#include "ZXAudio.h"

#define SAMPLE_RATE     (44100)
#define PERIOD          (10)    /* fraction of a second (10 => 0.1s) */
#define BUFFER_SAMPLES  (SAMPLE_RATE / PERIOD)
#define BITS_SAMPLE     (5)     /* we take the mean of this many input bits to make an output sample */
#define BITFIFO_LENGTH  (BUFFER_SAMPLES * BITS_SAMPLE) /* in bits */

#define MAXVOL          (32767 / 5)
#define NORMSPEED       (100)   /* percent */

#define AY_CLOCK_FREQ   (1773400) /* ZX Spectrum 128K AY-3-8912 clock rate */
#define AY_VOLUME_PCT   (20)      /* 0..AY_MASTER_VOLUME_MAX */

/* The beeper is a hard on/off level, so summing it straight into the AY signal
 * biases the waveform upward whenever it is high. This headroom keeps the sum
 * inside int16 range. (As in the SDL3 front-end.) */
#define AY_GAIN_WITH_BEEPER (0.90f)

/* Use output bus zero */
#define kOutputBus      (0)

/* To make things easy we assume that the emulated ZX Spectrum is pumping out
 * 220,500 samples per second into the fifo. We then sample this down by
 * averaging five value to produce a 44.1KHz signal. This certainly sounds
 * right but I'm not currently sure how accurate it really is.
 * To produce a 22.05KHz signal instead: average ten values, and so on.
 *
 * Z80 @ 3.5MHz
 * Each OUT takes 11 cycles
 * >>> 3.5e6 / 11
 * 318181.81 recurring OUTs/s theoretical max */

struct zxaudio
{
  pthread_mutex_t         lock; /* guards everything below */
  int                     enabled;
  int                     speed; /* percent */
  bitfifo_t              *fifo;
  int                     lastvol; /* most recently output volume */
  slopay_chip_t          *ay;
  slopay_chip_reg_t       ay_reg; /* register selected by the last port_AY_REGISTER write */
  AudioComponentInstance  instance;
};

/* This is the same as an AURenderCallback except it's not a pointer type. */
static OSStatus playback(void                       *inRefCon,
                         AudioUnitRenderActionFlags *ioActionFlags,
                         const AudioTimeStamp       *inTimeStamp,
                         UInt32                      inBusNumber,
                         UInt32                      inNumberFrames,
                         AudioBufferList            *ioData)
{
  const int     bytesPerSample = 2;

  zxaudio_t    *audio = inRefCon;
  OSStatus      rc;
  int           fetch;
  UInt32        i;

  pthread_mutex_lock(&audio->lock);

  fetch = BITS_SAMPLE * audio->speed / NORMSPEED;
  fetch = (fetch < 1) ? 1 : (fetch > 32) ? 32 : fetch;

  rc = noErr;

  for (i = 0; i < ioData->mNumberBuffers; i++)
  {
    signed short *p;
    UInt32        j;

    assert(inNumberFrames == ioData->mBuffers[i].mDataByteSize / bytesPerSample);

    p = ioData->mBuffers[i].mData;
    for (j = 0; j < inNumberFrames; j++)
    {
      size_t                 used;
      unsigned int           bits;
      slopay_chip_sample_t   ay;
      int                    mix;

      /* Beeper: the mean of 'fetch' speaker bits. Silent when idle, so a
       * beeper left high doesn't leave a DC offset under the AY. */
      used = bitfifo_used(audio->fifo);
      if (used >= (size_t) fetch)
      {
        bits = 0;
        bitfifo_dequeue(audio->fifo, &bits, fetch);
        audio->lastvol = (int) __builtin_popcount(bits) * MAXVOL / fetch;
      }
      else if (used == 0)
      {
        audio->lastvol = 0;
      }
      /* else a partial fifo: maintain the most recent volume */

      /* AY: mono, so the left half is the whole signal */
      ay  = slopay_chip_get_sample(audio->ay);
      mix = (int) ((signed short) (ay & 0xFFFF) * AY_GAIN_WITH_BEEPER) +
            audio->lastvol;
      if (mix > 32767)
        mix = 32767;
      else if (mix < -32768)
        mix = -32768;
      *p++ = (signed short) mix;
    }
  }

  pthread_mutex_unlock(&audio->lock);

  return rc;
}

zxaudio_t *zxaudio_create(void)
{
  AudioComponentDescription   desc;
  AudioComponent              component;
  OSStatus                    status;
  UInt32                      flag;
  AudioStreamBasicDescription audioFormat;
  AURenderCallbackStruct      input;
  zxaudio_t                  *audio;

  /* Describe audio component */
  desc.componentType          = kAudioUnitType_Output;
  desc.componentSubType       = kAudioUnitSubType_DefaultOutput;
  desc.componentManufacturer  = kAudioUnitManufacturer_Apple;
  desc.componentFlags         = 0;
  desc.componentFlagsMask     = 0;

  /* Find a matching audio component */
  component = AudioComponentFindNext(NULL, &desc);
  if (component == NULL)
    return NULL; /* no matching audio component was found */

  audio = calloc(1, sizeof(*audio));
  if (audio == NULL)
    return NULL;

  pthread_mutex_init(&audio->lock, NULL);
  audio->enabled = 1;
  audio->speed   = NORMSPEED;

  audio->ay = slopay_chip_create(AY_CLOCK_FREQ, SAMPLE_RATE);
  if (audio->ay == NULL)
    goto failure; /* OOM */
  slopay_chip_set_stereo_mode(audio->ay, SLOPAY_CHIP_STEREO_MODE_MONO);
  slopay_chip_set_volume(audio->ay, AY_VOLUME_PCT);

  /* Create a bit fifo */
  audio->fifo = bitfifo_create(BITFIFO_LENGTH);
  if (audio->fifo == NULL)
    goto failure; /* OOM */

  /* Create a new instance of the audio component */
  status = AudioComponentInstanceNew(component, &audio->instance);
  if (status)
  {
    audio->instance = NULL;
    goto failure;
  }

  /* Enable IO for playback */
  flag = 1;
  status = AudioUnitSetProperty(audio->instance,
                                kAudioOutputUnitProperty_EnableIO,
                                kAudioUnitScope_Output,
                                kOutputBus,
                                &flag,
                                sizeof(flag));
  if (status && status != kAudioUnitErr_InvalidProperty)
    goto failure;

  /* Describe the format */
  audioFormat.mSampleRate       = SAMPLE_RATE;
  audioFormat.mFormatID         = kAudioFormatLinearPCM;
  audioFormat.mFormatFlags      = kLinearPCMFormatFlagIsSignedInteger |
                                  kLinearPCMFormatFlagIsPacked;
  audioFormat.mBytesPerPacket   = 2;
  audioFormat.mFramesPerPacket  = 1;
  audioFormat.mBytesPerFrame    = 2;
  audioFormat.mChannelsPerFrame = 1;
  audioFormat.mBitsPerChannel   = 16;
  audioFormat.mReserved         = 0;

  /* Apply the format */
  status = AudioUnitSetProperty(audio->instance,
                                kAudioUnitProperty_StreamFormat,
                                kAudioUnitScope_Input,
                                kOutputBus,
                                &audioFormat,
                                sizeof(audioFormat));
  if (status)
    goto failure;

  /* Set the output callback */
  input.inputProc       = playback;
  input.inputProcRefCon = audio;
  status = AudioUnitSetProperty(audio->instance,
                                kAudioUnitProperty_SetRenderCallback,
                                kAudioUnitScope_Input,
                                kOutputBus,
                                &input,
                                sizeof(input));
  if (status)
    goto failure;

  /* Initialise */
  status = AudioUnitInitialize(audio->instance);
  if (status)
    goto failure;

  /* Start playing */
  status = AudioOutputUnitStart(audio->instance);
  if (status)
    goto failure;

  return audio;


failure:
  zxaudio_destroy(audio);
  return NULL;
}

void zxaudio_destroy(zxaudio_t *audio)
{
  if (audio == NULL)
    return;

  if (audio->instance)
  {
    AudioOutputUnitStop(audio->instance);
    AudioUnitUninitialize(audio->instance);
    AudioComponentInstanceDispose(audio->instance);
  }

  bitfifo_destroy(audio->fifo);
  if (audio->ay)
    slopay_chip_destroy(audio->ay);
  pthread_mutex_destroy(&audio->lock);
  free(audio);
}

void zxaudio_toggle(zxaudio_t *audio)
{
  if (audio == NULL)
    return;

  audio->enabled = !audio->enabled;

  if (audio->enabled)
    AudioOutputUnitStart(audio->instance);
  else
    AudioOutputUnitStop(audio->instance);
}

void zxaudio_set_speed(zxaudio_t *audio, int percent)
{
  if (audio == NULL)
    return;

  pthread_mutex_lock(&audio->lock);
  audio->speed = percent;
  pthread_mutex_unlock(&audio->lock);
}

void zxaudio_speaker(zxaudio_t *audio, int on)
{
  unsigned int bit;

  if (audio == NULL)
    return;

  bit = on;

  pthread_mutex_lock(&audio->lock);
  /* There's nothing we can do if the buffer is full, so ignore errors */
  (void) bitfifo_enqueue(audio->fifo, &bit, 0, 1);
  pthread_mutex_unlock(&audio->lock);
}

void zxaudio_ay(zxaudio_t *audio, uint16_t port, uint8_t byte)
{
  if (audio == NULL)
    return;

  pthread_mutex_lock(&audio->lock);
  if (port == port_AY_REGISTER)
    audio->ay_reg = byte;
  else
    slopay_chip_write_register(audio->ay, audio->ay_reg, byte);
  pthread_mutex_unlock(&audio->lock);
}
