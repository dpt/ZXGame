/* ZXAudio.c -- Spectrum speaker output through Core Audio */

#include <assert.h>
#include <pthread.h>
#include <stdlib.h>

#include <AudioUnit/AudioUnit.h>

#include "bitfifo.h"

#include "ZXAudio.h"

#define SAMPLE_RATE     (44100)
#define PERIOD          (10)    /* fraction of a second (10 => 0.1s) */
#define BUFFER_SAMPLES  (SAMPLE_RATE / PERIOD)
#define BITS_SAMPLE     (5)     /* we take the mean of this many input bits to make an output sample */
#define BITFIFO_LENGTH  (BUFFER_SAMPLES * BITS_SAMPLE) /* in bits */

#define MAXVOL          (32767 / 5)
#define NORMSPEED       (100)   /* percent */

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

  /* CHECK: Is this going to screw up with mNumberBuffers > 1 since i'm
   *        fetching from the fifo? */

  fetch = BITS_SAMPLE * audio->speed / NORMSPEED;
  fetch = (fetch < 1) ? 1 : (fetch > 32) ? 32 : fetch;

  rc = noErr;

  for (i = 0; i < ioData->mNumberBuffers && rc == noErr; i++)
  {
    UInt32 outputCapacity;
    UInt32 outputUsed;

    assert(inNumberFrames == ioData->mBuffers[i].mDataByteSize / bytesPerSample);

    outputCapacity = inNumberFrames;
    outputUsed     = 0;

    while (outputUsed < outputCapacity)
    {
      UInt32        outputCapacityRemaining;
      size_t        bitsInFifo;
      size_t        toCopy;
      signed short *p;
      UInt32        j;

      outputCapacityRemaining = outputCapacity - outputUsed;
      bitsInFifo              = bitfifo_used(audio->fifo);
      toCopy                  = bitsInFifo < outputCapacityRemaining ?
                                bitsInFifo : outputCapacityRemaining;

      if (bitsInFifo == 0)
      {
        ioData->mBuffers[0].mDataByteSize = 0;
        rc = -1;
        break;
      }

      p = (signed short *) ioData->mBuffers[i].mData + outputUsed;
      for (j = 0; j < toCopy; j++)
      {
        unsigned int bits;
        result_t     err;
        signed short vol;

        bits = 0;
        err = bitfifo_dequeue(audio->fifo, &bits, fetch);
        if (err == result_BITFIFO_INSUFFICIENT ||
            err == result_BITFIFO_EMPTY)
          /* When the fifo empties maintain the most recent volume */
          vol = audio->lastvol;
        else
          vol = (int) __builtin_popcount(bits) * MAXVOL / fetch;
        *p++ = vol;

        audio->lastvol = vol;
      }

      outputUsed += toCopy;
    }

    if (rc == noErr)
    {
      ioData->mBuffers[i].mDataByteSize = outputUsed * bytesPerSample;
      assert(outputUsed == outputCapacity);
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
