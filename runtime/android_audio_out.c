/* Android audio output for the DirectSound mixer (runtime/dsound_thunk.cpp):
 * one AAudio stream, float stereo, whose data callback pulls the mix.
 * Compiled only for Android; on the host the mixer is clock driven. */
#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#include <stdint.h>
#include <stdio.h>

void dsoundt_mix(float *out, uint32_t frames);

static AAudioStream *g_stream;

static aaudio_data_callback_result_t on_data(AAudioStream *stream, void *user, void *audio, int32_t frames) {
    (void)stream; (void)user;
    dsoundt_mix((float *)audio, (uint32_t)frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void on_error(AAudioStream *stream, void *user, aaudio_result_t error) {
    (void)stream; (void)user;
    fprintf(stderr, "PORT_AUDIO stream error %d (%s)\n", (int)error, AAudio_convertResultToText(error));
}

/* Returns 0 when the stream is running. */
int port_audio_start(uint32_t rate, uint32_t channels) {
    if (g_stream) return 0;
    AAudioStreamBuilder *builder = NULL;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return -1;
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(builder, (int32_t)rate);
    AAudioStreamBuilder_setChannelCount(builder, (int32_t)channels);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(builder, on_data, NULL);
    AAudioStreamBuilder_setErrorCallback(builder, on_error, NULL);
    aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &g_stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK) { fprintf(stderr, "PORT_AUDIO openStream failed: %s\n", AAudio_convertResultToText(result)); g_stream = NULL; return -1; }
    if (AAudioStream_getSampleRate(g_stream) != (int32_t)rate || AAudioStream_getChannelCount(g_stream) != (int32_t)channels) {
        fprintf(stderr, "PORT_AUDIO device did not grant %u Hz / %u channels\n", (unsigned)rate, (unsigned)channels);
        AAudioStream_close(g_stream); g_stream = NULL;
        return -1;
    }
    result = AAudioStream_requestStart(g_stream);
    if (result != AAUDIO_OK) { fprintf(stderr, "PORT_AUDIO start failed: %s\n", AAudio_convertResultToText(result)); AAudioStream_close(g_stream); g_stream = NULL; return -1; }
    fprintf(stderr, "PORT_AUDIO AAudio output running: %d Hz, %d channels, burst %d frames\n", (int)AAudioStream_getSampleRate(g_stream),
            (int)AAudioStream_getChannelCount(g_stream), (int)AAudioStream_getFramesPerBurst(g_stream));
    return 0;
}
#else
typedef int port_audio_out_unused;
#endif
