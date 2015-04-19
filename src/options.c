static int parse_int(const char *string, long *result) {
  char *end;
  errno = 0;
  *result = strtol(string, &end, 10);
  return !(*end || errno);
}

static int parse_float(const char *string, double *result) {
  char *end;
  errno = 0;
  *result = strtod(string, &end);
  return !(*end || errno);
}

static void print_option(char short_opt, const char *long_opt, const char *description) {
  if (short_opt)
    printf("  -%c, ", short_opt);
  else
    printf("      ");

  printf("--%-15s  %s\n", long_opt, description);
}

static int parse_options(
  int argc, char **argv,
  int(*parse_short_option)(char opt, char *next, void *opaque),
  int(*parse_long_option)(char *opt, char *next, void *opaque),
  int(*parse_argument)(int argn, char *arg, int is_forced, void *opaque),
  void *opaque
){
  int result;
  int i = 1, regular_args = 0;

  /* Parse options mixed with arguments */
  while (i < argc) {
    char *arg = argv[i];

    if (arg[0] == '-' && arg[1]) {
      char *next_arg = (i+1 < argc) ? argv[i+1] : NULL;

      if (arg[1] == '-' && !arg[2]) {
        /* '--' signals end of options */
        i++;
        break;
      }

      if (arg[1] == '-') {
        /* Long option */
        result = parse_long_option(arg + 2, next_arg, opaque);
        if (!result) return 0;
        i += result;
      } else {
        /* Sequence of short options */
        size_t pos;
        for (pos = 1; arg[pos]; pos++) {
          char *next = (arg[pos+1]) ? arg + pos+1 : next_arg;
          result = parse_short_option(arg[pos], next, opaque);
          if (!result) return 0;
          if (result == 2) {
            if (next == next_arg) i++;
            break;
          }
        }
        i++;
      }
    } else {
      /* Argument */
      result = parse_argument(regular_args++, arg, 0, opaque);
      if (!result) return 0;
      i++;
    }
  }

  /* Parse rest as forced arguments */
  while (i < argc) {
    result = parse_argument(regular_args++, argv[i], 1, opaque);
    if (!result) return 0;
    i++;
  }

  return 1;
}



typedef struct {
  const char *basename;
  int done;

  // Emission
  float frequency;
  bool stereo;
  const char *rds_file;
  bool preemp;

  // Reflow
  int reflow_time;
  int calibration_reflows;

  // Resampling
  bool resample;
  size_t period_size;
  size_t ringsize;
  size_t resamp_quality;
  size_t resamp_squality;

  // JACK
  const char *name;
  const char *server_name;
  bool force_name;
  const char *target_ports[2];
} client_options;

static const client_options default_values = {
  "jackpifm", // basename
  0,     // done

  // Emission
  103.3, // frequency
  false, // stereo
  NULL,  // RDS blob file
  true,  // preemp

  // Reflow options
  40,    // reflow time
  5,     // calibration reflows

  // Resampling
  false, // resamp
  512,   // period_size
  16384, // ringsize
  5,     // resamp quality
  10,    // resamp squality

  // JACK
  "jackpifm", // client name
  NULL,  // server name
  false, // force name
  {NULL, NULL}, // target ports
};

static void print_help(const char *basename) {
  // Intro
  printf("Emits live audio from JACK over FM through the Raspberry GPIO.\n");
  printf("\n");

  // Usage
  printf("Usage:\n"
         "  %1$s [options] [PORT]\n"
         "  %1$s [options] [L_PORT R_PORT]\n"
         "  %1$s (--help | --version)\n",
         basename);
  printf("\n");

  // Emission options
  printf("Emission options:\n");
  print_option('f', "frequency=FREQ", "Set the FM carrier frequency in MHz. [default: 103.3]");
  print_option('s', "stereo", "Enable stereo emission.");
  print_option('R', "rds=FILE", "Encode an RDS blob with the stream.");
  print_option('e', "no-preemp", "Disable the pre-emphasis filter.");
  printf("\n");

  // Reflow options
  printf("Reflow options:\n");
  print_option('t', "reflow-time=T", "Time between reflows, in seconds. [default: 40]");
  print_option(  0, "calibration-reflows=N", "Number of reflows in the calibration phase. [default: 5]");
  printf("\n");

  // Sampling options
  printf("Sampling options:\n");
  print_option('r', "resamp", "Resample sound to 152kHz before emission.");
  print_option('p', "period=FRAMES", "Output (emission) period in frames. [default: 512]");
  print_option('r', "ringsize=FRAMES", "Size of the ringbuffer in frames. [default: 16384]");
  print_option(  0, "resamp-quality=N", "Resampling lookup table row size. [default: 5]");
  print_option(  0, "resamp-squality=N", "Resampling lookup table column size. [default: 10]");
  printf("\n");

  // JACK options
  printf("JACK options:\n");
  print_option('n', "name=NAME", "JACK client name. [default: jackpifm]");
  print_option(  0, "server-name=NAME", "Force a specific JACK server by name.");
  print_option(  0, "force-name", "Force the client to use the given name.");
  printf("\n");

  // Other options
  printf("Other options:\n");
  print_option('h', "help", "Print this help message.");
  print_option('v', "version", "Print program version.");
  printf("\n");

  // Ending
  printf("In the first form, jackpifm will connect its input (or both inputs if it's stereo) "
         "to the passed port. In the second form, the left input will be connected to the "
         "first passed port, and the right input to the second.\n\n");
  printf("If you use --stereo or --rds make sure to pass --resamp too, or it won't start.\n\n");
}

static void print_version() {
  printf("jackpifm %s\n", JACKPIFM_VERSION);
}

static int parse_short_option(char opt, char *next, void *opaque) {
  client_options *data = opaque;

  if (opt == 'f' && next) {
    double freq;
    if (parse_float(next, &freq) && freq > 0 && freq < 1e6) {
      data->frequency = freq;
      return 2;
    }
    fprintf(stderr, "Wrong frequency value.\n");
    return 0;
  }

  if (opt == 's') {
    data->stereo = true;
    return 1;
  }

  if (opt == 'R' && next) {
    data->rds_file = next;
    return 2;
  }

  if (opt == 'e') {
    data->preemp = false;
    return 1;
  }

  if (opt == 't' && next) {
    long time;
    if (parse_int(next, &time) && time > 0 && time < 1e6) {
      data->reflow_time = time;
      return 2;
    }
    fprintf(stderr, "Wrong reflow time value.\n");
    return 0;
  }

  if (opt == 'r') {
    data->resample = true;
    return 1;
  }

  if (opt == 'p' && next) {
    long frames;
    if (parse_int(next, &frames) && frames > 0 && frames < 1e6) {
      data->period_size = frames;
      return 2;
    }
    fprintf(stderr, "Wrong period size value.\n");
    return 0;
  }

  if (opt == 'b' && next) {
    long frames;
    if (parse_int(next, &frames) && frames > 0 && frames < 1e6) {
      data->ringsize = frames;
      return 2;
    }
    fprintf(stderr, "Wrong ringbuffer size value.\n");
    return 0;
  }

  if (opt == 'n' && next) {
    data->name = next;
    return 2;
  }

  if (opt == 'h') {
    print_help(data->basename);
    data->done = 1;
    return 0;
  }

  if (opt == 'v') {
    print_version();
    data->done = 1;
    return 0;
  }

  fprintf(stderr, "Wrong option '-%c' found.\n", opt);
  return 0;
}

static int parse_long_option(char *opt, char *next, void *opaque) {
  client_options *data = opaque;

  if (strcmp(opt, "frequency") == 0 && next) {
    double freq;
    if (parse_float(next, &freq) && freq > 0 && freq < 1e6) {
      data->frequency = freq;
      return 2;
    }
    fprintf(stderr, "Wrong frequency value.\n");
    return 0;
  }

  if (strcmp(opt, "stereo") == 0) {
    data->stereo = true;
    return 1;
  }

  if (strcmp(opt, "rds") == 0 && next) {
    data->rds_file = next;
    return 2;
  }

  if (strcmp(opt, "no-preemp") == 0) {
    data->preemp = false;
    return 1;
  }

  if (strcmp(opt, "reflow-time") == 0 && next) {
    long time;
    if (parse_int(next, &time) && time > 0 && time < 1e6) {
      data->reflow_time = time;
      return 2;
    }
    fprintf(stderr, "Wrong reflow time value.\n");
    return 0;
  }

  if (strcmp(opt, "calibration-reflows") == 0 && next) {
    long count;
    if (parse_int(next, &count) && count >= 0 && count < 1e6) {
      data->calibration_reflows = count;
      return 2;
    }
    fprintf(stderr, "Wrong calibration reflow count value.\n");
    return 0;
  }

  if (strcmp(opt, "resamp") == 0) {
    data->resample = true;
    return 1;
  }

  if (strcmp(opt, "period") == 0 && next) {
    long frames;
    if (parse_int(next, &frames) && frames > 0 && frames < 1e6) {
      data->period_size = frames;
      return 2;
    }
    fprintf(stderr, "Wrong period size value.\n");
    return 0;
  }

  if (strcmp(opt, "ringsize") == 0 && next) {
    long frames;
    if (parse_int(next, &frames) && frames > 1 && frames < 1e6) {
      data->ringsize = frames;
      return 2;
    }
    fprintf(stderr, "Wrong ringbuffer size value.\n");
    return 0;
  }

  if (strcmp(opt, "resamp-quality") == 0 && next) {
    long size;
    if (parse_int(next, &size) && size > 1 && size < 1e6) {
      data->resamp_quality = size;
      return 2;
    }
    fprintf(stderr, "Wrong resamp quality size value.\n");
    return 0;
  }

  if (strcmp(opt, "resamp-squality") == 0 && next) {
    long size;
    if (parse_int(next, &size) && size > 1 && size < 1e6) {
      data->resamp_squality = size;
      return 2;
    }
    fprintf(stderr, "Wrong resamp squality value.\n");
    return 0;
  }

  if (strcmp(opt, "name") == 0 && next) {
    data->name = next;
    return 2;
  }

  if (strcmp(opt, "server-name") == 0 && next) {
    data->server_name = next;
    return 2;
  }

  if (strcmp(opt, "force-name") == 0) {
    data->force_name = true;
    return 1;
  }

  if (strcmp(opt, "help") == 0) {
    print_help(data->basename);
    data->done = 1;
    return 0;
  }

  if (strcmp(opt, "version") == 0) {
    print_version();
    data->done = 1;
    return 0;
  }

  fprintf(stderr, "Wrong option '-%s' found.\n", opt);
  return 0;
}

static int parse_argument(int argn, char *arg, int is_forced, void *opaque) {
  client_options *data = opaque;

  if (argn < 2) {
    // Target port
    data->target_ports[argn] = arg;
    return 1;
  }

  fprintf(stderr, "Too many arguments.\n");
  return 0;
}

static void parse_jackpifm_options(client_options *data, int argc, char **argv) {
  memcpy(data, &default_values, sizeof(client_options));
  data->basename = argv[0];
  argc = parse_options(argc, argv, parse_short_option, parse_long_option, parse_argument, data);
  if (data->done) exit(0);
  if (!argc) exit(1);

  if (data->target_ports[1] && !data->stereo) {
    fprintf(stderr, "Two ports passed but stereo was not enabled.\n");
    exit(1);
  }
  if ((data->stereo || data->rds_file) && !data->resample) {
    fprintf(stderr, "To use --stereo or --rds you must also enable --resamp.\n");
    exit(1);
  }
  if (data->period_size >= data->ringsize) {
    fprintf(stderr, "Period size (%d) cannot be greater than ringsize (%d).\n", data->period_size, data->ringsize);
    exit(1);
  }

  if (data->stereo && !data->target_ports[1])
    data->target_ports[1] = data->target_ports[0];
}
