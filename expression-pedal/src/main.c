/**
 * Author: Viacheslav Lotsmanov
 * License: GNU/GPLv3 https://raw.githubusercontent.com/unclechu/pi-pedalboard/master/LICENSE
 *
 * TODO implement sample step limitation to reduce cpu load
 * TODO implement binary & human-readable output
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <jack/jack.h>

#ifdef DEBUG
#  define LOG(msg, ...) fprintf(stderr, "JACK LOG: " msg "\n", ##__VA_ARGS__);
#else
#  define LOG(...) ((void)0)
#endif

#define ERR(msg, ...) fprintf(stderr, "JACK ERROR: " msg "\n", ##__VA_ARGS__);
#define MIN(a, b) ((a < b) ? (a) : (b))
#define MAX(a, b) ((a > b) ? (a) : (b))
#define EQ(a, b) (strcmp((a), (b)) == 0)
#define MAX_VALUE 255

char jack_client_name[128] = "pidalboard-expression-pedal";

// Optional calibration for the lowest and the biggest values
typedef struct { int32_t min_offset, max_offset; } Offset;

typedef struct {
  Offset offset;
  uint32_t sample_rate, buffer_size;
  jack_client_t *jack_client;
  jack_port_t *send_port, *return_port;
  uint8_t last_value;
} State;

int jack_process(jack_nframes_t nframes, void *arg)
{
  State *state = (State *)arg;
  jack_default_audio_sample_t *send_buf, *return_buf;
  send_buf = jack_port_get_buffer(state->send_port, nframes);
  return_buf = jack_port_get_buffer(state->return_port, nframes);

  for (jack_nframes_t i = 0; i < nframes; ++i) {
    uint8_t value = MIN(MAX(round(
      ((return_buf[i] * MAX_VALUE) - state->offset.min_offset)
        * MAX_VALUE / state->offset.max_offset
    ), 0), MAX_VALUE);

    send_buf[i] = 1; // Send just 1 as direct current

    if (value != state->last_value) {

      // TODO binary output
      ERR("%d → %d \n", state->last_value, value);

      state->last_value = value;
    }
  }

  return 0;
}

int jack_process_calibrate(jack_nframes_t nframes, void *arg)
{
  State *state = (State *)arg;
  jack_default_audio_sample_t *send_buf, *return_buf;

  send_buf = jack_port_get_buffer(state->send_port, nframes);

  // One sample per buffer is enough to debug it.
  // And it’s not very useful to write to the log more often.
  return_buf = jack_port_get_buffer(state->return_port, 1);

  for (jack_nframes_t i = 0; i < nframes; ++i) send_buf[i] = 1;

  int32_t value = round(MAX_VALUE * return_buf[0]);
  uint8_t bounded_value = MIN(MAX(value, 0), MAX_VALUE);

  fprintf( stderr
         , "Bounded value: %d; min offset: %d; max offset: %d;\n"
         , bounded_value
         , (value < MAX_VALUE) ? value         : 0
         , (value > MAX_VALUE) ? (value - MAX_VALUE) : 0
         );

  return 0;
}

void register_ports(State *state)
{
  LOG("Registering send port…");

  state->send_port = jack_port_register( state->jack_client
                                       , "send"
                                       , JACK_DEFAULT_AUDIO_TYPE
                                       , JackPortIsOutput
                                       , 0
                                       );

  if (state->send_port == NULL) {
    ERR("Registering send port failed!");
    exit(EXIT_FAILURE);
  }

  LOG("Send port is registered.");
  LOG("Registering return port…");

  state->return_port = jack_port_register( state->jack_client
                                         , "return"
                                         , JACK_DEFAULT_AUDIO_TYPE
                                         , JackPortIsInput
                                         , 0
                                         );

  if (state->return_port == NULL) {
    ERR("Registering send port failed!");
    exit(EXIT_FAILURE);
  }

  LOG("Return port is registered.");
}

int set_sample_rate(jack_nframes_t nframes, void *arg)
{
  State *state = (State *)arg;
  state->sample_rate = nframes;
  LOG("New sample rate: %d", nframes);
  return 0;
}

int set_buffer_size(jack_nframes_t nframes, void *arg)
{
  State *state = (State *)arg;
  state->buffer_size = nframes;
  LOG("New buffer size: %d", nframes);
  return 0;
}

void bind_callbacks(State *state, bool calibrate)
{
  if (calibrate) {
    LOG("Binding process callback for calibration mode…");
    jack_set_process_callback( state->jack_client
                             , jack_process_calibrate
                             , (void *)state
                             );
    LOG("Process callback for calibration mode is bound.");
  } else {
    LOG("Binding process callback…");
    jack_set_process_callback(state->jack_client, jack_process, (void *)state);
    LOG("Process callback is bound.");
  }

  LOG("Binding sample rate callback…");

  jack_set_sample_rate_callback( state->jack_client
                               , set_sample_rate
                               , (void *)state
                               );

  LOG("Sample rate callback is bound.");

  LOG("Binding buffer size callback…");

  jack_set_buffer_size_callback( state->jack_client
                               , set_buffer_size
                               , (void *)state
                               );

  LOG("Buffer size callback is bound.");
}

void null_state(State *state)
{
  Offset offset      = { 0, 0 };
  state->offset      = offset;
  state->sample_rate = 0;
  state->buffer_size = 0;
  state->jack_client = NULL;
  state->send_port   = NULL;
  state->return_port = NULL;
  state->last_value  = 0;
}

void init(Offset offset, bool calibrate)
{
  LOG("Initialization of state…");
  State *state = (State *)malloc(sizeof(State));

  if (!state) {
    ERR("State initialization failed!");
    exit(EXIT_FAILURE);
  }

  null_state(state);
  offset.max_offset += MAX_VALUE; // precalculate
  state->offset = offset;
  LOG("State is initialized…");

  LOG("Opening client…");
  jack_status_t status;

  state->jack_client = jack_client_open( jack_client_name
                                       , JackNullOption
                                       , &status
                                       , NULL
                                       );

  if (state->jack_client == NULL) {
    ERR("Opening client failed!");
    exit(EXIT_FAILURE);
  }

  if (status & JackNameNotUnique) {
    ERR("Client name '%s' is already taken!", jack_client_name);
    exit(EXIT_FAILURE);
  }

  LOG("Client is opened.");
  register_ports(state);
  bind_callbacks(state, calibrate);

  if (jack_activate(state->jack_client)) {
    ERR("Activating client failed!");
    exit(EXIT_FAILURE);
  }
}

void show_usage(FILE *out, char *app)
{
  size_t i = 0;
  char spaces[128];
  for (i = 0; i < strlen(app); ++i) spaces[i] = ' ';
  spaces[i] = '\0';
  fprintf(out, "Usage: %s [-c|--calibrate]\n", app);
  fprintf(out, "       %s [-l|--lower INT]\n", spaces);
  fprintf(out, "       %s [-u|--upper INT]\n\n", spaces);
  fprintf(out, "Available options:\n");
  fprintf(out, "  -c,--calibrate  Calibrate min and max bounds.\n");
  fprintf(out, "                  Provide minimum value and see what offset you\n");
  fprintf(out, "                  need to set for minimum value. And then provide\n");
  fprintf(out, "                  maximum value and look at the maximum value offset.\n");
  fprintf(out, "  -l,--lower      Set min bound correction (see --calibrate)\n");
  fprintf(out, "  -u,--upper      Set max bound correction (see --calibrate)\n");
  fprintf(out, "  -h,-?,--help    Show this help text\n");
}

int main(int argc, char *argv[])
{
  LOG("Starting of application…");
  Offset offset = { 0, 0 };
  bool calibrate = false;

  for (int i = 1; i < argc; ++i) {
    if (EQ(argv[i], "--help") || EQ(argv[i], "-h") || EQ(argv[i], "-?")) {
      show_usage(stdout, argv[0]);
      return EXIT_SUCCESS;
    } else if (EQ(argv[i], "-c") || EQ(argv[i], "--calibrate")) {
      calibrate = true;
      fprintf(stderr, "Running in calibration mode…\n");
    } else if (
      EQ(argv[i], "-l") || EQ(argv[i], "--lower") ||
      EQ(argv[i], "-u") || EQ(argv[i], "--upper")
    ) {
      if (++i >= argc) {
        fprintf(stderr, "There must be a value after “%s” argument!\n\n", argv[--i]);
        return EXIT_FAILURE;
      }

      char *ptr;
      long int x = strtol(argv[i], &ptr, 10);

      if ( ! EQ(ptr, "") || x < SHRT_MIN || x > SHRT_MAX) {
        fprintf( stderr
               , "Incorrect integer value “%s” argument provided for “%s”!\n\n"
               , argv[i]
               , argv[i-1]
               );
        return EXIT_FAILURE;
      }

      if (EQ(argv[i-1], "-l") || EQ(argv[i-1], "--lower")) {
        offset.min_offset = (short int)x;
        LOG("Set min offset to: %ld", x);
      } else {
        offset.max_offset = (short int)x;
        LOG("Set max offset to: %ld", x);
      }
    } else {
      fprintf(stderr, "Incorrect argument: “%s”!\n\n", argv[i]);
      show_usage(stderr, argv[0]);
      return EXIT_FAILURE;
    }
  }

  init(offset, calibrate);
  sleep(-1);
  return EXIT_SUCCESS;
}
