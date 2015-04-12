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
static size_t min_lat;  // Minimum latency in JACK frames, from reading from JACK until emitting over FM.
static size_t tar_lat;  // Target latency in JACK frames, from reading from JACK until emitting over FM, which we try to approximate.
static size_t max_lat;  // Maximum latency in JACK frames, from reading from JACK until emitting over FM.

static volatile uint64_t iwritten; // Counts samples we attempted to write to ringbuffer, from JACK. [mutex]
static volatile uint64_t owritten; // Counts samples we attempted to write from the ringbuffer, to GPIO. [mutex]
static volatile size_t ipos;       // Input position inside the ring buffer (i.e. where to write next). [mutex]
static volatile size_t opos;       // Output position inside the ring buffer (i.e. where to read next). [mutex]
static volatile double srate;      // Rate at which we last setup the GPIO. [mutex]
static volatile double orate;      // Real rate at which we write from the ringbuffer, to the GPIO. [mutex]

// Other parameters
static jack_client_t *jack_client;
static jack_port_t *jack_ports[2];
static pthread_t thread;
static pthread_mutex_t mutex;
static jackpifm_preemp_t **preemp;
static jackpifm_stereo_t *stereo;
static const uint8_t *rds_data;
static jackpifm_rds_t *rds;
static SRC_STATE *resampler;
static SRC_DATA resampler_data;
static jackpifm_sample_t *ibuffer;
static jackpifm_sample_t *obuffer;
static jackpifm_sample_t *ringbuffer; // [mutex]
static volatile bool thread_started; // [mutex]
static volatile bool thread_running; // [mutex]
static volatile bool calibrated; // [mutex]


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
      int ret = pthread_create(&thread, NULL, output_thread, NULL);
      assert(!ret);
      thread_started = true;
    }
  } else {
    if (calibrated) fprintf(stderr, "Got too many frames from JACK, dropping :(\n");
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
      if (calibrated) fprintf(stderr, "The buffer got empty, delaying! :(\n");
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

  // Reflow
  int reflow_time;
  int calibration_reflows;

  // Resampling
  bool resample;
  size_t period_size;
  size_t ringsize;
  int converter_type;

  // JACK
  const char *name;
  const char *server_name;
  bool force_name;
  const char *target_ports[2];
} client_options;

void stop_client();
void signal_handler(int);

void read_file(const char *name, uint8_t **file_data, size_t *file_size) {
  int r;
  FILE* file = fopen(name, "r");
  if (file == NULL) {
    fprintf(stderr, "Couldn't open '%s': %s\n", name, strerror(errno));
    abort();
  }

  size_t asize = 64;
  uint8_t *data = jackpifm_malloc(asize);
  size_t size = 0;
  while (!feof(file)) {
    size += fread(data + size, 1, 64, file);
    assert(!ferror(file));
    if (size + 64 > asize) {
      asize += 64;
      data = jackpifm_realloc(data, asize);
    }
  }

  r = fclose(file);
  assert(!r);
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
  iwritten = owritten = 0;

  jperiod = jack_get_buffer_size(jack_client);
  operiod = opt->period_size;

  jrate = jack_get_sample_rate(jack_client);
  rate = opt->resample ? 152000 : jrate;
  srate = rate;

  delay = opt->ringsize / 2;
  calibrated = false;

  // Setup resampler
  int error, channels = opt->stereo ? 2 : 1;
  if (opt->resample) {
    resampler = src_new(opt->converter_type, channels, &error);
    assert(resampler);
    assert(sizeof(jackpifm_sample_t) == sizeof(float));

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
    connect_jack_port(jack_client, jack_ports[0], opt->target_ports[0]);
    connect_jack_port(jack_client, jack_ports[0], opt->target_ports[1]);
  } else {
    jack_ports[0] = jack_port_register(jack_client, "in", JACK_DEFAULT_AUDIO_TYPE, port_flags, 0);
    assert(jack_ports[0]);
    connect_jack_port(jack_client, jack_ports[0], opt->target_ports[0]);
  }

  // Calculate latency
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
  jackpifm_outputter_setup(srate, operiod);
  printf("Info: carrier frequency %.2f MHz, rate %.3f Hz, period %u frames.\n", opt->frequency, srate, operiod);

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


// MAIN (OPTION PARSING)
// ---------------------

static const client_options default_values = {
  // Emission
  103.3, // frequency
  false, // stereo
  true,  // preemp
  NULL,  // rds_file

  // Reflow options
  40,    // reflow time
  5,     // calibration reflows

  // Resampling
  false, // resamp
  512,   // period_size
  8192,  // ringsize
  SRC_LINEAR, // converter_type

  // JACK
  "jackpifm", // name
  NULL,  // server_name
  false, // force_name
  {NULL, NULL}, // target_ports
};

void reflow(unsigned int next_reflow) {
  pthread_mutex_lock(&mutex);

  size_t distance = (ringsize + ipos - opos) % ringsize;
  orate = rate * ((double)owritten / (double)iwritten);
  double new_rate = srate + (rate - orate) / 2;
  printf("Reflow: real %.3f Hz (%+6.3f%%), setting %.3f Hz. Deviation: %d frames (%6.2fms).\n", orate, (orate-rate)*100.0/rate, new_rate, distance-delay, (distance-(double)delay)*1000 / rate);

  srate = new_rate;
  new_rate += .25 * (distance - (double)delay) / next_reflow;
  jackpifm_outputter_setup(new_rate, operiod);
  iwritten = owritten = 0;

  pthread_mutex_unlock(&mutex);
}

int main(int argc, char **argv) {
  client_options options = default_values;

  //TODO: option parsing and checking
  options.frequency = atof(argv[1]);

  start_client(&options);

  // Do calibration reflows
  if (options.calibration_reflows > 0) {
    printf("Starting calibration stage (duration %d * %d sec)...\n", options.calibration_reflows, options.reflow_time);
    sleep(5);
    for (int i = 0; i < options.calibration_reflows; i++) {
      reflow(options.reflow_time);
      sleep(options.reflow_time);
    }
  }

  // Keep reflowing until end
  printf("Calibration stage finished.\n");
  pthread_mutex_lock(&mutex);
  calibrated = true;
  pthread_mutex_unlock(&mutex);

  while (1) {
    reflow(options.reflow_time);
    sleep(options.reflow_time);
  }

  return 0;
}
