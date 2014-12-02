#include "common.h"
#include "assert.h"

#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <math.h>

#include "preemp.h"
#include "stereo.h"
#include "rds.h"
#include "outputter.h"

#include <samplerate.h>


// Following is a graph of the flow the samples follow
// to get from JACK to the GPIO:
//
//
//                           |iwritten        |owritten
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
// `jrate` and `rate` are theorical, or target, sample rates.
// `iwritten` and `owritten` are real measures.
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
// 3. The new samples are written to the ringbuffer, and `iwritten`
//    incremented accordingly. If the ringbuffer is full, the samples
//    are dropped instead and a message printed.
//
// 4. At the same time, another thread is constantly reading samples
//    from the ringbuffer, in groups of `operiod` samples, and incrementing
//    `owritten`.
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
//  - In order to accomplish this, when the thread detects that
//    `ìwritten - owritten` exceeds `reflow` samples, it calculates `irate`
//    and `orate` by taking the deltas and calculates a correction.
//
//  - This correction is applied to the GPIO in the hope that it'll read
//    "in sync" with samples pushed to the ringbuffer, and will
//    approximate `rate` as much as possible.


// Integer parameters (measures in samples)
static size_t ringsize; // Size of the ring buffer.
static size_t jperiod;  // Period size at which we receive from JACK.
static size_t operiod;  // Period size at which we read from the ringbuffer.
static size_t jrate;    // "Theoretical" rate at which we read from JACK.
static size_t rate;     // "Theoretical" target rate, which jrate and orate should approximate.
static size_t delay;    // Initial/target delay between writing and reading to ringbuffer.

static volatile size_t iwritten; // Counts samples we attempted to write to ringbuffer, from JACK. [mutex]
static volatile size_t owritten; // Counts samples we attempted to write from the ringbuffer, to GPIO. [mutex]
static volatile size_t ipos;     // Input position inside the ring buffer (i.e. where to write next). [mutex]
static volatile size_t opos;     // Output position inside the ring buffer (i.e. where to read next). [mutex]
static volatile double srate;    // Rate at which we last setup the GPIO. [mutex]
static volatile double orate;    // Real rate at which we write from the ringbuffer, to the GPIO. [mutex]

// Other parameters
static jack_client_t *jack_client;
static jack_port_t *jack_ports[2];
static pthread_t thread;
static pthread_mutex_t mutex;
static jackpifm_preemp_t **preemp;
static jackpifm_stereo_t *stereo;
static jackpifm_rds_t *rds;
static SRC_STATE *resampler;
static SRC_DATA resampler_data;
static jackpifm_sample_t *ibuffer;
static jackpifm_sample_t *obuffer;
static jackpifm_sample_t *ringbuffer; // [mutex]
static volatile bool thread_started; // [mutex]
static volatile bool thread_running; // [mutex]
static volatile bool reflowed; // [mutex]


// JACK CALLBACKS
// --------------

void *output_thread(void *arg);

// The main "process" callback. We receive samples from Jack,
// preprocess them and write them to the ringbuffer.
int process_callback(jack_nframes_t nframes, void *arg) {
  jackpifm_sample_t *out;
  size_t out_period; // should be iperiod

  // Preemp, resample and stereo modulate
  if (stereo) {
    jackpifm_sample_t *left = jack_port_get_buffer(jack_ports[0], jperiod);
    jackpifm_sample_t *right = jack_port_get_buffer(jack_ports[1], jperiod);

    if (preemp) {
      jackpifm_preemp_process(preemp[0], left, jperiod);
      jackpifm_preemp_process(preemp[1], right, jperiod);
    }

    // We assume resampling is enabled
    //TODO
    out = ibuffer;
    out_period = resampler_data.output_frames_gen;

    jackpifm_stereo_process(stereo, out, out, out + out_period, out_period);
  } else {
    out = jack_port_get_buffer(jack_ports[0], jperiod);
    out_period = jperiod;

    if (preemp)
      jackpifm_preemp_process(preemp[0], out, out_period);

    if (resampler) {
      //TODO
    }
  }

  // Apply RDS encoding (if needed)
  if (rds)
    jackpifm_rds_process(rds, out, out_period);


  pthread_mutex_lock(&mutex);
  if (!thread_running) {
    pthread_mutex_unlock(&mutex);
    return 0;
  }

  // Check that we don't overwrite
  if ((ringsize + ipos - opos) % ringsize <= ringsize - out_period) {
    // Write to ringbuffer
    if (ipos + out_period > ringsize) {
      size_t delta = ringsize - ipos;
      memcpy(ringbuffer + ipos, out, delta * sizeof(jackpifm_sample_t));
      memcpy(ringbuffer, out + delta, (out_period - delta) * sizeof(jackpifm_sample_t));
    } else memcpy(ringbuffer + ipos, out, out_period * sizeof(jackpifm_sample_t));

    ipos = (ipos + out_period) % ringsize;

    // Start thread, if we've reached the delay
    if (!thread_started && iwritten >= delay) {
      assert(!pthread_create(&thread, NULL, output_thread, NULL));
      thread_started = true;
    }
  } else {
    if (reflowed) fprintf(stderr, "Got too many frames from JACK, dropping :(\n");
  }

  iwritten += out_period;
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

  // Minimum latency is (GPIO latency)
  size_t min = (JACKPIFM_BUFFERINSTRUCTIONS / 4);
  // Maximum latency is (GPIO latency + ringsize)
  size_t max = (JACKPIFM_BUFFERINSTRUCTIONS / 4) + ringsize;

  // Convert min and max into JACK time samples
  min = roundf(min * jrate / (float)rate);
  max = roundf(max * jrate / (float)rate);

  set_port_latency(jack_ports[0], min, max);
  if (stereo)
    set_port_latency(jack_ports[1], min, max);

  printf("Latency: %u frames min, %u frames max\n", min, max);
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

      //printf("\e[J\n\ndiff:  %5d\ndelay: %5u\e[3F", iwritten-owritten, (ringsize + ipos - opos) % ringsize);
      fflush(stdout);
    } else {
      if (reflowed) fprintf(stderr, "The buffer got empty, delaying! :(\n");
    }

    owritten += operiod;
    pthread_mutex_unlock(&mutex);

    jackpifm_outputter_output(obuffer, operiod);
  }

  return NULL;
}


// [DE-]INITIALIZATION LOGIC
// -------------------------

typedef struct {
  // Emission
  float frequency;
  bool stereo;
  bool preemp;
  const char *rds_file;

  // Resampling
  bool resample;
  size_t period_size;
  size_t ringsize;
  int converter_type;
  long reflow;

  // JACK
  const char *name;
  const char *server_name;
  bool force_name;
  const char *target_ports[2];
} client_options;

void stop_client();
void signal_handler(int);

void start_client(const client_options *opt) {
  // Initialize JACK client
  jack_options_t options = JackNullOption;
  jack_status_t status;
  if (opt->force_name) options |= JackUseExactName;
  if (opt->server_name) options |= JackServerName;
  jack_client = jack_client_open(opt->name, options, &status, opt->server_name);
  assert(jack_client);

  // Set parameters
  iwritten = owritten = 0;

  jperiod = jack_get_buffer_size(jack_client);
  operiod = opt->period_size;

  jrate = jack_get_sample_rate(jack_client);
  rate = opt->resample ? 152000 : jrate;
  srate = rate;

  delay = opt->ringsize / 2;
  reflowed = false;

  // Setup resampler
  int error, channels = opt->stereo ? 2 : 1;
  if (opt->resample) {
    resampler = src_new(opt->converter_type, channels, &error);
    assert(resampler);

    resampler_data.end_of_input = 0;
    resampler_data.src_ratio = rate / (double)jrate;

    resampler_data.input_frames = jperiod;
    resampler_data.data_in = jackpifm_calloc(channels * jperiod, sizeof(jackpifm_sample_t));

    resampler_data.output_frames = (int)(1.02 * jperiod * resampler_data.src_ratio);
    resampler_data.data_out = jackpifm_calloc(channels * resampler_data.output_frames, sizeof(jackpifm_sample_t));
    ibuffer = jackpifm_calloc(channels * resampler_data.output_frames, sizeof(jackpifm_sample_t));
  } else resampler = NULL;

  // Create ringbuffer
  ringsize = opt->ringsize;
  ringbuffer = jackpifm_calloc(ringsize, sizeof(jackpifm_sample_t));
  obuffer = jackpifm_calloc(operiod, sizeof(jackpifm_sample_t));
  ipos = opos = 0;

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
  rds = opt->rds_file ? NULL : NULL; //TODO

  // Create ports
  unsigned long port_flags = JackPortIsInput | JackPortIsTerminal | JackPortIsPhysical;
  if (stereo) {
    jack_ports[0] = jack_port_register(jack_client, "left", JACK_DEFAULT_AUDIO_TYPE, port_flags, 0);
    jack_ports[1] = jack_port_register(jack_client, "right", JACK_DEFAULT_AUDIO_TYPE, port_flags, 0);
    assert(jack_ports[0] && jack_ports[1]);
    //TODO: connect ports
  } else {
    jack_ports[0] = jack_port_register(jack_client, "in", JACK_DEFAULT_AUDIO_TYPE, port_flags, 0);
    assert(jack_ports[0]);
  }

  // Set JACK callbacks
  jack_set_process_callback(jack_client, process_callback, NULL);
  jack_set_buffer_size_callback(jack_client, buffer_size_callback, NULL);
  jack_set_sample_rate_callback(jack_client, sample_rate_callback, NULL);
  jack_set_latency_callback(jack_client, latency_callback, NULL);

  // Setup FM and subscribe to exit
  assert(!jackpifm_setup_fm());
  jackpifm_setup_dma(opt->frequency);
  jackpifm_outputter_setup(srate, operiod);

  // Subscribe signal handlers
  atexit(stop_client);
  signal(SIGQUIT, signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGHUP, signal_handler);
  signal(SIGINT, signal_handler);

  // ACTIVATE!!!
  jack_activate(jack_client); //FIXME
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
  if (resampler) {
    free(resampler_data.data_in);
    free(resampler_data.data_out);
    free(ibuffer);
    src_delete(resampler);
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

  // Unsetup FM
  jackpifm_unsetup_dma();

  // Finally, destroy the mutex
  pthread_mutex_destroy(&mutex);

  printf("\nAll done.\n");
}

void signal_handler(int sig) {
  exit(0);
}


// MAIN (OPTION PARSING)
// ---------------------

static const client_options default_values = {
  // Emission
  103.3, // frequency
  false, // stereo
  true,  // preemp
  NULL,  // rds_file

  // Resampling
  false, // resamp
  512,   // period_size
  8092,  // ringsize
  SRC_LINEAR, // converter_type
  10,    // reflow

  // JACK
  "jackpifm", // name
  NULL,  // server_name
  false, // force_name
  {NULL, NULL}, // target_ports
};

int main(int argc, char **argv) {
  client_options options = default_values;

  //TODO: option parsing and checking
  options.frequency = atof(argv[1]);
  printf("Using frequency %.2f MHz\n", options.frequency);

  start_client(&options);

  // Keep reflowing until end
  while (1) {
    sleep(options.reflow);

    pthread_mutex_lock(&mutex);
    if (!thread_running) {
      pthread_mutex_unlock(&mutex);
      break;
    }

    size_t distance = (ringsize + ipos - opos) % ringsize;
    orate = rate * (owritten / (double)iwritten);
    double new_rate = srate + (rate - orate) / 2;
    printf("Reflow, real %.3f Hz, current %.3f Hz, new %.3f Hz\n", orate, srate, new_rate);

    srate = new_rate;
    new_rate += .5 * (distance - (double)delay) / options.reflow;
    jackpifm_outputter_setup(new_rate, operiod);
    reflowed = true;
    iwritten = owritten = 0;

    pthread_mutex_unlock(&mutex);
  }

  return 0;
}
