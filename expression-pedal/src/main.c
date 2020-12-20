/**
 * Author: Viacheslav Lotsmanov
 * License: GNU/GPLv3 https://raw.githubusercontent.com/unclechu/pi-pedalboard/master/LICENSE
 *
 * TODO Implement logarithmic value conversion
 * TODO Socket server
 */

#define _POSIX_SOURCE   // Fix a warning about implicit declaration of “fileno”
#define _DEFAULT_SOURCE // Fix a warning about implicit declaration of “usleep”

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <jack/jack.h>

#ifdef DEBUG
#  define LOG(msg, ...) fprintf(stderr, "DEBUG: " msg "\n", ##__VA_ARGS__);
#else
#  define LOG(...) ((void)0)
#endif

#define ERR(msg, ...) \
  { \
    fprintf(stderr, "ERROR: " msg "\n", ##__VA_ARGS__); \
    exit(EXIT_FAILURE); \
  }

#define ERRJACK(msg, ...) \
  { \
    fprintf(stderr, "JACK ERROR: " msg "\n", ##__VA_ARGS__); \
    exit(EXIT_FAILURE); \
  }

#define MIN(a, b) ((a < b) ? (a) : (b))
#define MAX(a, b) ((a > b) ? (a) : (b))
#define EQ(a, b) (strcmp((a), (b)) == 0)

#define MALLOC_CHECK(a) if (a == NULL) ERR("Failed to allocate memory!");

#define RMS_LOG "RMS (× 1e4)"
#define RMS_SHIFT(x) (x * 1e4)

char jack_client_name[128] = "pidalboard-expression-pedal";

// It’s really just a “uint32_t” (not “int64_t”) but it’s handled anyway as
// “int64_t” in calculations in order to overcome underflows and overflows.
// Also it is useful to set it to “-1” by default when parsing command line
// arguments which would indicate that the values weren’t provided.
typedef struct { int64_t rms_min_bound, rms_max_bound; } RmsBounds;

typedef struct Node  Node;
typedef struct Queue Queue;

struct Node {
  uint8_t value;
  Node *next;
};

struct Queue {
  Node *head;
  Node *tail;
};

typedef struct {
  jack_nframes_t      sample_rate, buffer_size;

  jack_client_t       *jack_client;
  jack_port_t         *send_port, *return_port;

  uint8_t             last_value;

  bool                binary_output;

  pthread_mutex_t     queue_lock;
  pthread_cond_t      queue_cond;
  Queue               value_changes_queue;

  jack_default_audio_sample_t
                      sine_wave_freq;
  jack_nframes_t      sine_wave_sample_i;
  jack_nframes_t      sine_wave_one_rotation_samples;

  RmsBounds           rms_bounds;
  bool                use_default_rms_window_size;
  jack_nframes_t      rms_window_size;
  jack_nframes_t      rms_window_sample_i;
  jack_default_audio_sample_t
                      rms_sum;
  int64_t             last_rms; // In reaility it’s just a “uint32_t”
} State;

void* handle_value_updates(void *arg)
{
  State *state = (State *)arg;
  int stdout_fd = dup(fileno(stdout));

  for (;;) {
    pthread_mutex_lock(&state->queue_lock);
    LOG("Waiting for a notification of a new value update…");
    pthread_cond_wait(&state->queue_cond, &state->queue_lock);
    LOG("Received a notification of a change of the value.");

    handle_next_queue_item_without_waiting:
      if (state->value_changes_queue.head == NULL) {
        pthread_mutex_unlock(&state->queue_lock);
        LOG("Value changes queue is empty.");
      } else {
        uint8_t value = state->value_changes_queue.head->value;
        Node *tmp_node = state->value_changes_queue.head;
        state->value_changes_queue.head = state->value_changes_queue.head->next;

        if (state->value_changes_queue.head == NULL)
          state->value_changes_queue.tail = NULL;

        pthread_mutex_unlock(&state->queue_lock);
        tmp_node->next = NULL;
        free(tmp_node);

        if (state->binary_output) {
          if (write(stdout_fd, &value, sizeof(uint8_t)) == -1)
            ERR("Failed to write binary data to stdout!");
        } else {
          printf("New value: %d\n", value);
        }

        // Don’t wait for a next notification yet,
        // handle whole queue before starting to wait again.
        pthread_mutex_lock(&state->queue_lock);
        goto handle_next_queue_item_without_waiting;
      }
  }
}

inline jack_default_audio_sample_t sample_radians
( jack_default_audio_sample_t hz
, jack_nframes_t              sample_n
, jack_nframes_t              sample_rate
)
{
  return
    (jack_default_audio_sample_t)sample_n * hz * 2 * M_PI
      / (jack_default_audio_sample_t)sample_rate;
}

// Should return “uint32_t” but in calculations everywhere “int64_t” is used.
inline int64_t finalize_rms
( jack_nframes_t              window_size
, jack_default_audio_sample_t sum
)
{
  return floor(RMS_SHIFT(
    (1.0f / (jack_default_audio_sample_t)window_size) * sum
  ));
}

int jack_process(jack_nframes_t nframes, void *arg)
{
  State *state = (State *)arg;
  jack_default_audio_sample_t *send_buf, *return_buf;
  send_buf = jack_port_get_buffer(state->send_port, nframes);
  return_buf = jack_port_get_buffer(state->return_port, nframes);

  for (
    jack_nframes_t i = 0;
    i < nframes;
    ++i,
    state->sine_wave_sample_i
      = (state->sine_wave_sample_i + 1)
      % state->sine_wave_one_rotation_samples
  ) {
    send_buf[i] = sin(sample_radians(
      state->sine_wave_freq,
      state->sine_wave_sample_i,
      state->sample_rate
    ));

    if (++state->rms_window_sample_i >= state->rms_window_size) {
      int64_t rms = finalize_rms(state->rms_window_size, state->rms_sum);

      if (rms != state->last_rms) {
        state->last_rms = rms;

        uint8_t value = MIN(MAX(round(
          (rms - state->rms_bounds.rms_min_bound)
            * UINT8_MAX / state->rms_bounds.rms_max_bound
        ), 0), UINT8_MAX);

        if (value != state->last_value) {
          Node *new_node = malloc(sizeof(Node));
          MALLOC_CHECK(new_node);
          new_node->value = value;
          new_node->next = NULL;
          pthread_mutex_lock(&state->queue_lock);

          if (state->value_changes_queue.tail != NULL)
            state->value_changes_queue.tail->next = new_node;

          state->value_changes_queue.tail = new_node;

          if (state->value_changes_queue.head == NULL) // first value
            state->value_changes_queue.head = new_node;

          pthread_cond_signal(&state->queue_cond);
          pthread_mutex_unlock(&state->queue_lock);
          state->last_value = value;
        }
      }

      state->rms_window_sample_i = 0;
      state->rms_sum = powf(return_buf[i], 2);
    } else {
      state->rms_sum += powf(return_buf[i], 2);
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

  for (
    jack_nframes_t i = 0;
    i < nframes;
    ++i,
    state->sine_wave_sample_i
      = (state->sine_wave_sample_i + 1)
      % state->sine_wave_one_rotation_samples
  ) {
    send_buf[i] = sin(sample_radians(
      state->sine_wave_freq,
      state->sine_wave_sample_i,
      state->sample_rate
    ));

    if (++state->rms_window_sample_i >= state->rms_window_size) {
      int64_t rms = finalize_rms(state->rms_window_size, state->rms_sum);

      if (rms != state->last_rms) {
        fprintf(stderr, "New "RMS_LOG": %li\n", rms);
        state->last_rms = rms;
      }

      state->rms_window_sample_i = 0;
      state->rms_sum = powf(return_buf[i], 2);
    } else {
      state->rms_sum += powf(return_buf[i], 2);
    }
  }

  return 0;
}

void register_ports(State *state)
{
  LOG("Registering JACK send port…");

  state->send_port = jack_port_register( state->jack_client
                                       , "send"
                                       , JACK_DEFAULT_AUDIO_TYPE
                                       , JackPortIsOutput
                                       , 0
                                       );

  if (state->send_port == NULL) ERRJACK("Registering send port failed!");

  LOG("Send JACK port is registered.");
  LOG("Registering JACK return port…");

  state->return_port = jack_port_register( state->jack_client
                                         , "return"
                                         , JACK_DEFAULT_AUDIO_TYPE
                                         , JackPortIsInput
                                         , 0
                                         );

  if (state->return_port == NULL) ERRJACK("Registering send port failed!");

  LOG("Return JACK port is registered.");
}

int set_sample_rate(jack_nframes_t nframes, void *arg)
{
  State *state = (State *)arg;
  state->sample_rate = nframes;
  LOG("New JACK sample rate received: %d", nframes);

  state->sine_wave_one_rotation_samples = round(
    (jack_default_audio_sample_t)state->sample_rate / state->sine_wave_freq
  );

  if (state->use_default_rms_window_size) {
    state->rms_window_size = state->sine_wave_one_rotation_samples;
    LOG("New RMS window size: %d samples", state->rms_window_size);
  }

  return 0;
}

int set_buffer_size(jack_nframes_t nframes, void *arg)
{
  State *state = (State *)arg;
  state->buffer_size = nframes;
  LOG("New JACK buffer size: %d", nframes);
  return 0;
}

void bind_callbacks(State *state, bool calibrate)
{
  if (calibrate) {
    LOG("Binding JACK process callback for calibration mode…");

    if (jack_set_process_callback(
      state->jack_client,
      jack_process_calibrate,
      (void *)state
    )) ERRJACK("jack_set_process_callback() error!");

    LOG("JACK process callback for calibration mode is bound.");
  } else {
    LOG("Binding JACK process callback…");

    if (jack_set_process_callback(
      state->jack_client,
      jack_process,
      (void *)state
    )) ERRJACK("jack_set_process_callback() error!");

    LOG("JACK process callback is bound.");
  }

  LOG("Binding JACK sample rate callback…");

  if (jack_set_sample_rate_callback(
    state->jack_client,
    set_sample_rate,
    (void *)state
  ) != 0) ERRJACK("jack_set_sample_rate_callback() error!");

  LOG("JACK sample rate callback is bound.");

  LOG("Binding JACK buffer size callback…");

  if (jack_set_buffer_size_callback(
    state->jack_client,
    set_buffer_size,
    (void *)state
  ) != 0) ERRJACK("jack_set_buffer_size_callback() error!");

  LOG("JACK buffer size callback is bound.");
}

typedef struct {
  pthread_t value_updates_handler_tid;
} JackShutdownPayload;

void jack_shutdown_callback(void *arg)
{
  LOG("Received JACK shutdown notification, terminating…");
  JackShutdownPayload *jack_shutdown_payload = (JackShutdownPayload *)arg;

  LOG("Cancelling value updates handling thread…");
  if (pthread_cancel(jack_shutdown_payload->value_updates_handler_tid) != 0)
    ERR("pthread_cancel() error!");
}

void null_state(State *state)
{
  state->sample_rate     = 0;
  state->buffer_size     = 0;
  state->jack_client     = NULL;
  state->send_port       = NULL;
  state->return_port     = NULL;
  state->last_value      = 0;
  state->binary_output   = false;

  // Cannot be initialized with NULL
  // state->queue_lock

  // Cannot be initialized with NULL
  // state->queue_cond

  state->value_changes_queue.head = NULL;
  state->value_changes_queue.tail = NULL;

  state->sine_wave_freq                 = 0;
  state->sine_wave_sample_i             = 0;
  state->sine_wave_one_rotation_samples = 0;

  RmsBounds rms_bounds               = { 0, 0 };
  state->rms_bounds                  = rms_bounds;
  state->use_default_rms_window_size = false;
  state->rms_window_size             = 0;
  state->rms_window_sample_i         = 0;
  state->rms_sum                     = 0.0f;
  state->last_rms                    = 0;
}

void run( RmsBounds                   rms_bounds
        , jack_default_audio_sample_t sine_wave_freq  // 0 for default value
        , jack_nframes_t              rms_window_size // 0 for default value
        , bool                        binary_output
        , bool                        calibrate
        )
{
  LOG("Initializing a queue mutex…");
  pthread_mutex_t queue_lock;
  pthread_cond_t  queue_cond;

  if (pthread_mutex_init(&queue_lock, NULL) != 0)
    ERR("pthread_mutex_init() error!");

  if (pthread_cond_init(&queue_cond, NULL) != 0)
    ERR("pthread_cond_init() error!");

  LOG("Initialization of state…");
  State *state = (State *)malloc(sizeof(State));
  MALLOC_CHECK(state);

  null_state(state);
  state->binary_output = binary_output;
  state->queue_lock = queue_lock;
  state->queue_cond = queue_cond;
  state->sine_wave_freq = (sine_wave_freq == 0) ? 440.0f : sine_wave_freq;
  state->rms_bounds = rms_bounds;
  state->rms_bounds.rms_max_bound -= state->rms_bounds.rms_min_bound; // Precalculate
  state->use_default_rms_window_size = rms_window_size == 0;
  if (rms_window_size != 0) state->rms_window_size = rms_window_size;
  LOG("State is initialized…");

  LOG("Opening JACK client…");
  jack_status_t status;

  state->jack_client = jack_client_open( jack_client_name
                                       , JackNullOption
                                       , &status
                                       , NULL
                                       );

  if (state->jack_client == NULL) ERRJACK("Opening client failed!");

  if (status & JackNameNotUnique)
    ERRJACK("Client name “%s” is already taken!", jack_client_name);

  LOG("JACK client is opened.");
  register_ports(state);
  bind_callbacks(state, calibrate);

  LOG("Running a thread for handing value updates queue…");
  pthread_t tid;
  int err = pthread_create(&tid, NULL, &handle_value_updates, (void *)state);

  if (err != 0) ERR("Failed to create a thread: [%s]", strerror(err));

  LOG("Setting JACK shutdown callback…");
  JackShutdownPayload jack_shutdown_payload = { tid };
  jack_on_shutdown( state->jack_client
                  , jack_shutdown_callback
                  , (void *)&jack_shutdown_payload
                  );

  if (jack_activate(state->jack_client)) ERRJACK("Client activation failed!");

  pthread_join(tid, NULL);
  pthread_mutex_destroy(&queue_lock);
  pthread_cond_destroy(&queue_cond);
  /* sleep(-1); */
}

void show_usage(FILE *out, char *app)
{
  size_t i = 0;
  char spaces[128];
  for (i = 0; i < strlen(app); ++i) spaces[i] = ' ';
  spaces[i] = '\0';
  fprintf(out, "Usage: %s -l|--lower UINT\n", app);
  fprintf(out, "       %s -u|--upper UINT\n", spaces);
  fprintf(out, "       %s [-c|--calibrate]\n", spaces);
  fprintf(out, "       %s [-b|--binary]\n", spaces);
  fprintf(out, "       %s [-f|--frequency UINT]\n", spaces);
  fprintf(out, "       %s [-w|--rms-window UINT]\n", spaces);
  fprintf(out, "\n");
  fprintf(out, "Available options:\n");
  fprintf(out, "  -l,--lower UINT       Set min "RMS_LOG" (see --calibrate).\n");
  fprintf(out, "  -u,--upper UINT       Set max "RMS_LOG" (see --calibrate).\n");
  fprintf(out, "  -c,--calibrate        Calibrate min and max RMS bounds.\n");
  fprintf(out, "                        Set your pedal to minimum position and record the value.\n");
  fprintf(out, "                        Then do the same for maximum position.\n");
  fprintf(out, "                        Use those values for --lower and --upper arguments.\n");
  fprintf(out, "  -b,--binary           Print binary unsigned 8-bit integers sequence\n");
  fprintf(out, "                        instead of human-readable lines.\n");
  fprintf(out, "  -f,--frequency UINT   Frequency in Hz of a sine wave to send\n");
  fprintf(out, "                        (default value is 440).\n");
  fprintf(out, "  -w,--rms-window UINT  RMS window size in amount of samples\n");
  fprintf(out, "                        (default value is one rotation of the sine wave,\n");
  fprintf(out, "                        so sample rate divided by --frequency,\n");
  fprintf(out, "                        so for 48000 sample rate and 440 Hz --frequency\n");
  fprintf(out, "                        it will be ≈109).\n");
  fprintf(out, "  -h,-?,--help          Show this help text.\n");
}

int main(int argc, char *argv[])
{
  LOG("Starting of application…");

  RmsBounds                   rms_bounds      = { -1, -1 };
  bool                        binary_output   = false;
  bool                        calibrate       = false;
  jack_nframes_t              rms_window_size = 0;
  jack_default_audio_sample_t sine_wave_freq  = 0;

  for (int i = 1; i < argc; ++i) {
    if (EQ(argv[i], "--help") || EQ(argv[i], "-h") || EQ(argv[i], "-?")) {
      show_usage(stdout, argv[0]);
      return EXIT_SUCCESS;
    } else if (
      EQ(argv[i], "-l") || EQ(argv[i], "--lower") ||
      EQ(argv[i], "-u") || EQ(argv[i], "--upper")
    ) {
      if (++i >= argc) {
        fprintf(stderr, "There must be a value after “%s” argument!\n\n", argv[--i]);
        show_usage(stderr, argv[0]);
        return EXIT_FAILURE;
      }

      long int x = atol(argv[i]);

      // Check for “UINT32_MAX” instead of “INT64_MAX” is intentional
      if (x < 0 || x > UINT32_MAX) {
        fprintf( stderr
               , "Incorrect unsigned integer value “%s” "
                 "argument provided for “%s”!\n\n"
               , argv[i]
               , argv[i-1]
               );
        show_usage(stderr, argv[0]);
        return EXIT_FAILURE;
      }

      if (EQ(argv[i-1], "-l") || EQ(argv[i-1], "--lower")) {
        // Cast to “uint32_t” instead of “int64_t” is intentional
        rms_bounds.rms_min_bound = (uint32_t)x;
        LOG("Setting min "RMS_LOG" to %li…", rms_bounds.rms_min_bound);
      } else {
        // Cast to “uint32_t” instead of “int64_t” is intentional
        rms_bounds.rms_max_bound = (uint32_t)x;
        LOG("Setting max "RMS_LOG" to %li…", rms_bounds.rms_max_bound);
      }
    } else if (EQ(argv[i], "-c") || EQ(argv[i], "--calibrate")) {
      calibrate = true;
      LOG("Turning calibration mode on…");
    } else if (EQ(argv[i], "-b") || EQ(argv[i], "--binary")) {
      binary_output = true;
      LOG("Setting stdout output format to binary mode…");
    } else if (EQ(argv[i], "-f") || EQ(argv[i], "--frequency")) {
      if (++i >= argc) {
        fprintf(stderr, "There must be a value after “%s” argument!\n\n", argv[--i]);
        show_usage(stderr, argv[0]);
        return EXIT_FAILURE;
      }

      long int x = atol(argv[i]);

      if (x < 1 || x > UINT32_MAX) {
        fprintf( stderr
               , "Incorrect unsigned integer (starting from 1) value “%s” "
                 "argument provided for “%s”!\n\n"
               , argv[i]
               , argv[i-1]
               );
        show_usage(stderr, argv[0]);
        return EXIT_FAILURE;
      }

      sine_wave_freq = (jack_default_audio_sample_t)x;
      LOG("Setting sine wave frequency to %f Hz…", sine_wave_freq);
    } else if (EQ(argv[i], "-w") || EQ(argv[i], "--rms-window")) {
      if (++i >= argc) {
        fprintf(stderr, "There must be a value after “%s” argument!\n\n", argv[--i]);
        show_usage(stderr, argv[0]);
        return EXIT_FAILURE;
      }

      long int x = atol(argv[i]);

      if (x < 1 || x > JACK_MAX_FRAMES) {
        fprintf( stderr
               , "Incorrect unsigned integer (starting from 1) value “%s” "
                 "argument provided for “%s”!\n\n"
               , argv[i]
               , argv[i-1]
               );
        show_usage(stderr, argv[0]);
        return EXIT_FAILURE;
      }

      rms_window_size = (jack_nframes_t)x;
      LOG("Setting RMS window size to %d samples…", rms_window_size);
    } else {
      fprintf(stderr, "Incorrect argument: “%s”!\n\n", argv[i]);
      show_usage(stderr, argv[0]);
      return EXIT_FAILURE;
    }
  }

  if (calibrate) {
    fprintf(stderr, "Running in calibration mode…\n");
  } else if (rms_bounds.rms_min_bound < 0 || rms_bounds.rms_max_bound < 0) {
    fprintf( stderr
           , "RMS bounds were not provided, run with --calibrate "
             "to get the values first!\n\n"
           );
    show_usage(stderr, argv[0]);
    return EXIT_FAILURE;
  } else if (rms_bounds.rms_max_bound <= rms_bounds.rms_min_bound) {
    fprintf(stderr, "RMS max bound must be higher than min bound!\n\n");
    show_usage(stderr, argv[0]);
    return EXIT_FAILURE;
  }

  run(rms_bounds, sine_wave_freq, rms_window_size, binary_output, calibrate);
  return EXIT_SUCCESS;
}

// Root Mean Square (RMS) calculation.
// This function isn’t used in the code, it’s inlined where needed.
// Just useful to see the reference implementation.
jack_default_audio_sample_t rms
( jack_default_audio_sample_t *samples_list
, jack_nframes_t               window_size
)
{
  jack_default_audio_sample_t sum = 0.0f;

  for (jack_nframes_t i = 0; i < window_size; ++i)
    sum += powf(samples_list[i], 2);

  return (1.0f / (jack_default_audio_sample_t)window_size) * sum;
}
