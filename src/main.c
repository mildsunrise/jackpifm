#include "common.h"
#include "assert.h"

#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <math.h>

#include "preemp.h"
#include "stereo.h"
#include "rds.h"
#include "outputter.h"
#include "resamp.h"


// Following is a graph of the flow the samples follow
// to get from JACK to the GPIO (filters not shown):
//
//
//                           |ipos            |opos
//                           |                |
// +------+   +------------+ | +------------+ | +------+
// | JACK |==>| RESAMPLING |==>| RINGBUFFER |==>| GPIO |
// +------+   +------------+   +------------+   +------+
//       (jrate)             +-----(rate)-----+
//      [jperiod]                         [operiod]
//
//  ------------------------------>     <-------------->
//            JACK thread                  our thread
//
//
// `jperiod` and `operiod` are fixed parameters.
// `jrate` and `rate` are theoretical, or target, sample rates.
// `ipos` and `opos` track the tail and head of the ringbuffer.
//
// A more precise explanation follows.
//
// 1. JACK is supposed to call us with new samples at `jrate` rate,
//    but the real rate of samples going from JACK to the RESAMPLER
//    will be a bit different in practice, creating desync.
//
// 2. The `jperiod` samples from JACK are (optionally) resampled with
//    a fixed ratio (rate/jrate), so that the new samples come in
//    theorical rate `rate` (the little desync from JACK is inherited).
//
// 3. The new samples are written to the ringbuffer, and `ipos`
//    incremented accordingly. If the ringbuffer is full, the samples
//    are dropped instead and a message printed.
//
// 4. At the same time, another thread is constantly reading samples
//    from the ringbuffer, in groups of `operiod` samples, and updating
//    `opos`.
//
//    This thread reads samples at the pace of the GPIO controller, which
//    *should* be equal to `rate`, but again there's a bit of desync.
//
// In order to fix the desync from both sides, the following is done:
//
//  - The reading thread is started only after at least `delay` samples
//    have been written to the ringbuffer.
//
//  - The program should try to adjust the GPIO, making it read faster
//    or slower, so that there's on average `delay` samples of difference
//    between what is written to the ringbuffer, and what is read from it.
//
// This last step is done through a custom PI controller.


// Integer parameters (measures in samples)
static size_t ringsize; // Size of the ring buffer.
static size_t jperiod;  // Period size at which we receive from JACK.
static size_t operiod;  // Period size at which we read from the ringbuffer.
static size_t jrate;    // "Theoretical" rate at which we read from JACK.
static size_t rate;     // "Theoretical" target rate at which we write to the GPIO.
static size_t delay;    // Initial/target delay between writing and reading to ringbuffer.
static size_t min_lat;  // Minimum latency in JACK frames, from reading from JACK until emitting over FM.
static size_t tar_lat;  // Target latency in JACK frames, from reading from JACK until emitting over FM, which we try to approximate.
static size_t max_lat;  // Maximum latency in JACK frames, from reading from JACK until emitting over FM.

static volatile size_t ipos;       // Input position inside the ring buffer (i.e. where to write next). [mutex]
static volatile size_t opos;       // Output position inside the ring buffer (i.e. where to read next). [mutex]

// Other parameters
static jack_client_t *jack_client;
static jack_port_t *jack_ports[2];
static pthread_t thread;
static pthread_mutex_t mutex;
static jackpifm_preemp_t **preemp;
static jackpifm_stereo_t *stereo;
static const uint8_t *rds_data;
static jackpifm_rds_t *rds;
static jackpifm_resamp_t *resampler [2];
static jackpifm_sample_t *resampler_buffer [2];
static jackpifm_sample_t *obuffer;
static jackpifm_sample_t *ringbuffer; // [mutex]
static volatile bool thread_started; // [mutex]
static volatile bool thread_running; // [mutex]


// JACK CALLBACKS
// --------------

void *output_thread(void *arg);

// Utility method
inline void crop_sample(jackpifm_sample_t *sample, size_t *cropped) {
  if (*sample < -1) {
    *sample = -1;
    (*cropped)++;
  } else if (*sample > +1) {
    *sample = +1;
    (*cropped)++;
  }
}

// The main "process" callback. We receive samples from Jack,
// preprocess them and write them to the ringbuffer.
int process_callback(jack_nframes_t nframes, void *arg) {
  jackpifm_sample_t *ibuffer;
  size_t iperiod;
  size_t cropped_now = 0;

  // Preemp, resample and stereo modulate
  if (stereo) {
    jackpifm_sample_t *left = jack_port_get_buffer(jack_ports[0], jperiod);
    jackpifm_sample_t *right = jack_port_get_buffer(jack_ports[1], jperiod);
    iperiod = jperiod;

    for (size_t i = 0; i < iperiod; i++) {
      crop_sample(left + i, &cropped_now);
      crop_sample(right + i, &cropped_now);
    }

    if (preemp) {
      jackpifm_preemp_process(preemp[0], left, iperiod);
      jackpifm_preemp_process(preemp[1], right, iperiod);
    }

    // We assume resampling is enabled
    size_t result = jackpifm_resamp_process(resampler[0], resampler_buffer[0], left, iperiod);
    size_t result_b = jackpifm_resamp_process(resampler[1], resampler_buffer[1], right, iperiod);
    // Since both resamplers are fed the same number
    // of samples at the same time, it's safe to assume
    // they always return the same number of samples.
    assert(result == result_b);
    left = resampler_buffer[0];
    right = resampler_buffer[1];
    iperiod = result;

    jackpifm_stereo_process(stereo, resampler_buffer[0], left, right, iperiod);
    ibuffer = resampler_buffer[0];
  } else {
    ibuffer = jack_port_get_buffer(jack_ports[0], jperiod);
    iperiod = jperiod;

    for (size_t i = 0; i < iperiod; i++)
      crop_sample(ibuffer + i, &cropped_now);

    if (preemp)
      jackpifm_preemp_process(preemp[0], ibuffer, iperiod);

    if (resampler[0]) {
      size_t result = jackpifm_resamp_process(resampler[0], resampler_buffer[0], ibuffer, iperiod);
      ibuffer = resampler_buffer[0];
      iperiod = result;
    }
  }

  // Apply RDS encoding (if needed)
  if (rds)
    // We assume resampling is enabled
    jackpifm_rds_process(rds, ibuffer, iperiod);


  pthread_mutex_lock(&mutex);
  if (!thread_running) {
    pthread_mutex_unlock(&mutex);
    return 0;
  }

  // Check that we don't overwrite
  if ((ringsize + ipos - opos) % ringsize <= ringsize - iperiod) {
    // Write to ringbuffer
    if (ipos + iperiod > ringsize) {
      size_t delta = ringsize - ipos;
      memcpy(ringbuffer + ipos, ibuffer, delta * sizeof(jackpifm_sample_t));
      memcpy(ringbuffer, ibuffer + delta, (iperiod - delta) * sizeof(jackpifm_sample_t));
    } else memcpy(ringbuffer + ipos, ibuffer, iperiod * sizeof(jackpifm_sample_t));

    ipos = (ipos + iperiod) % ringsize;

    // Start thread, if we've reached the delay
    if (!thread_started && iwritten >= delay) {
      int ret = pthread_create(&thread, NULL, output_thread, NULL);
      assert(!ret);
      thread_started = true;
    }
  } else {
    fprintf(stderr, "Got too many frames from JACK, dropping :(\n");
  }

  if (cropped_now) fprintf(stderr, "Cropped %u samples.\n", cropped_now);
  pthread_mutex_unlock(&mutex);

  return 0;
}

int buffer_size_callback(jack_nframes_t nframes, void *arg) {
  if (nframes != jperiod) {
    fprintf(stderr, "Sorry, JACK buffer size changed and I can't take that.\n");
    exit(1);
  }
  return 0;
}
int sample_rate_callback(jack_nframes_t nframes, void *arg) {
  if (nframes != jrate) {
    fprintf(stderr, "Sorry, JACK sample rate changed and I can't take that.\n");
    exit(1);
  }
  return 0;
}

void set_port_latency(jack_port_t *port, jack_nframes_t min, jack_nframes_t max) {
  jack_latency_range_t range;
  jack_port_get_latency_range(port, JackPlaybackLatency, &range);
  range.min += min;
  range.max += max;
  jack_port_set_latency_range(port, JackPlaybackLatency, &range);
}
void latency_callback(jack_latency_callback_mode_t mode, void *arg) {
  if (mode != JackPlaybackLatency) return;

  set_port_latency(jack_ports[0], min_lat, max_lat);
  if (stereo)
    set_port_latency(jack_ports[1], min_lat, max_lat);
}


// OUTPUT THREAD LOGIC
// -------------------

void *output_thread(void *arg) {
  // Sync FM
  jackpifm_outputter_sync();

  while (1) {
    pthread_mutex_lock(&mutex);
    if (!thread_running) {
      pthread_mutex_unlock(&mutex);
      break;
    }

    if ((ringsize + ipos - opos) % ringsize >= operiod) {
      // Read from the ringbuffer
      if (opos + operiod > ringsize) {
        size_t delta = ringsize - opos;
        memcpy(obuffer, ringbuffer + opos, delta * sizeof(jackpifm_sample_t));
        memcpy(obuffer + delta, ringbuffer, (operiod - delta) * sizeof(jackpifm_sample_t));
      } else memcpy(obuffer, ringbuffer + opos, operiod * sizeof(jackpifm_sample_t));

      opos = (opos + operiod) % ringsize;
    } else {
      fprintf(stderr, "The buffer got empty, delaying! :(\n");
    }

    pthread_mutex_unlock(&mutex);

    jackpifm_outputter_output(obuffer, operiod);
  }

  return NULL;
}


// [DE-]INITIALIZATION LOGIC
// -------------------------

#include "options.c"

void stop_client();
void signal_handler(int);

void read_file(const char *name, uint8_t **file_data, size_t *file_size) {
  int r;
  FILE* file = fopen(name, "r");
  if (file == NULL) {
    fprintf(stderr, "Couldn't open '%s': %s\n", name, strerror(errno));
    abort();
  }

  size_t asize = 0, size = 0;
  uint8_t *data = NULL;
  while (!feof(file)) {
    if (size + 64 > asize) {
      asize += 64;
      data = jackpifm_realloc(data, asize);
    }
    size += fread(data + size, 1, 64, file);
    assert(!ferror(file));
  }

  r = fclose(file);
  assert(!r);
  *file_data = data;
  *file_size = size;
}

void connect_jack_port(jack_client_t *client, jack_port_t *port, const char *name) {
  if (!name) return;
  if (jack_connect(client, name, jack_port_name(port))) {
    fprintf(stderr, "Couldn't connect to '%s'.\n", name);
    abort();
  }
}

void start_client(const client_options *opt) {
  // Initialize JACK client
  jack_options_t options = JackNullOption;
  jack_status_t status;
  int ret;
  if (opt->force_name) options |= JackUseExactName;
  if (opt->server_name) options |= JackServerName;
  jack_client = jack_client_open(opt->name, options, &status, opt->server_name);
  assert(jack_client);
  printf("Info: registered as '%s'\n", jack_get_client_name(jack_client));

  // Set parameters
  jperiod = jack_get_buffer_size(jack_client);
  operiod = opt->period_size;

  jrate = jack_get_sample_rate(jack_client);
  rate = opt->resample ? 152000 : jrate;

  if (opt->ringsize < 3*jperiod*rate/jrate) {
    fprintf(stderr, "Ringbuffer has to be at least 3x the real period size (%d).\n", jperiod*rate/jrate);
    abort();
  }

  delay = opt->ringsize / 2;

  // Setup resampler
  int channels = opt->stereo ? 2 : 1;
  if (opt->resample) {
    double ratio = jrate / (float)rate;
    size_t iperiod = (int)(1.02 * jperiod / ratio);
    for (int i = 0; i < channels; i++) {
      resampler[i] = jackpifm_resamp_new(jrate / (float)rate, opt->resamp_quality, opt->resamp_squality);
      resampler_buffer[i] = jackpifm_calloc(channels * iperiod, sizeof(jackpifm_sample_t));
    }
  } else resampler[0] = NULL;

  // Create ringbuffer
  ringsize = opt->ringsize;
  ringbuffer = jackpifm_calloc(ringsize, sizeof(jackpifm_sample_t));
  obuffer = jackpifm_calloc(operiod, sizeof(jackpifm_sample_t));
  ipos = opos = 0;
  printf("Info: created ringbuffer of %d frames.\n", ringsize);

  // Prepare thread / mutex
  pthread_mutex_init(&mutex, NULL);
  thread_started = false;
  thread_running = true;

  // Create filters
  if (opt->preemp) {
    preemp = jackpifm_calloc(channels, sizeof(jackpifm_preemp_t *));
    for (int c = 0; c < channels; c++)
      preemp[c] = jackpifm_preemp_new(jrate);
  } else preemp = NULL;

  stereo = opt->stereo ? jackpifm_stereo_new() : NULL;

  if (opt->rds_file) {
    uint8_t *data;
    size_t size;
    read_file(opt->rds_file, &data, &size);
    rds_data = data;
    rds = jackpifm_rds_new(data, size);
  } else {
    rds_data = NULL;
    rds = NULL;
  }

  // Create ports
  unsigned long port_flags = JackPortIsInput | JackPortIsTerminal | JackPortIsPhysical;
  if (stereo) {
    jack_ports[0] = jack_port_register(jack_client, "left", JACK_DEFAULT_AUDIO_TYPE, port_flags, 0);
    jack_ports[1] = jack_port_register(jack_client, "right", JACK_DEFAULT_AUDIO_TYPE, port_flags, 0);
    assert(jack_ports[0] && jack_ports[1]);
  } else {
    jack_ports[0] = jack_port_register(jack_client, "in", JACK_DEFAULT_AUDIO_TYPE, port_flags, 0);
    assert(jack_ports[0]);
  }

  // Calculate latency // FIXME: Take that 4 out
  // Minimum latency is (GPIO latency)
  min_lat = (JACKPIFM_BUFFERINSTRUCTIONS / 4);
  // Target latency is (GPIO latency + delay)
  tar_lat = (JACKPIFM_BUFFERINSTRUCTIONS / 4) + delay;
  // Maximum latency is (GPIO latency + ringsize)
  max_lat = (JACKPIFM_BUFFERINSTRUCTIONS / 4) + ringsize;

  // Convert min, tar and max into JACK time samples
  min_lat = roundf(min_lat * jrate / (float)rate);
  tar_lat = roundf(tar_lat * jrate / (float)rate);
  max_lat = roundf(max_lat * jrate / (float)rate);

  printf("Info: minimum latency is %u frames (%.2fms)\n", min_lat, min_lat*1000 / (double)jrate);
  printf("Info: target latency is %u frames (%.2fms)\n", tar_lat, tar_lat*1000 / (double)jrate);
  printf("Info: maximum latency is %u frames (%.2fms)\n", max_lat, max_lat*1000 / (double)jrate);
  min_lat = max_lat = tar_lat; // FIXME remove this

  // Set JACK callbacks
  jack_set_process_callback(jack_client, process_callback, NULL);
  jack_set_buffer_size_callback(jack_client, buffer_size_callback, NULL);
  jack_set_sample_rate_callback(jack_client, sample_rate_callback, NULL);
  jack_set_latency_callback(jack_client, latency_callback, NULL);

  // Setup FM and subscribe to exit
  ret = jackpifm_setup_fm();
  assert(!ret);
  jackpifm_setup_dma(opt->frequency);
  jackpifm_outputter_setup(rate, operiod);
  printf("Info: carrier frequency %.2f MHz, rate %u Hz, period %u frames.\n", opt->frequency, rate, operiod);

  // Subscribe signal handlers
  atexit(stop_client);
  signal(SIGQUIT, signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGHUP, signal_handler);
  signal(SIGINT, signal_handler);

  // ACTIVATE!!!
  ret = jack_activate(jack_client);
  assert(!ret);
  printf("\n");

  // Connect ports
  connect_jack_port(jack_client, jack_ports[0], opt->target_ports[0]);
  if (stereo)
    connect_jack_port(jack_client, jack_ports[1], opt->target_ports[1]);
}

void stop_client() {
  // Stop processing audio
  jack_deactivate(jack_client);

  // Stop the thread if running
  pthread_mutex_lock(&mutex);
  thread_running = false;
  pthread_mutex_unlock(&mutex);

  if (thread_started) {
    void *ret;
    pthread_join(thread, &ret);
  }

  // Disconnect from JACK
  jack_client_close(jack_client);

  // Free everything
  int channels = stereo ? 2 : 1;
  if (resampler[0]) {
    for (int i = 0; i < channels; i++) {
      free(resampler_buffer[i]);
      jackpifm_resamp_free(resampler[i]);
    }
  }

  free(ringbuffer);
  free(obuffer);

  if (preemp) {
    for (int c = 0; c < channels; c++)
      jackpifm_preemp_free(preemp[c]);
    free(preemp);
  }

  jackpifm_stereo_free(stereo);
  jackpifm_rds_free(rds);
  free((uint8_t *)rds_data);

  // Unsetup FM
  jackpifm_unsetup_dma();

  // Finally, destroy the mutex
  pthread_mutex_destroy(&mutex);

  printf("\nAll done.\n");
}

void signal_handler(int sig) {
  exit(0);
}

int main(int argc, char **argv) {
  client_options options;
  parse_jackpifm_options(&options, argc, argv);

  start_client(&options);

  while (1) sleep(600);
}
