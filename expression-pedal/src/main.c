/**
 * Author: Viacheslav Lotsmanov
 * License: GNU/GPLv3 https://raw.githubusercontent.com/unclechu/pi-pedalboard/master/LICENSE
 */

#define _POSIX_SOURCE // Fix a warning about implicit declaration of “fileno”
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
#  define LOG(msg, ...) fprintf(stderr, "JACK LOG: " msg "\n", ##__VA_ARGS__);
#else
#  define LOG(...) ((void)0)
#endif

#define ERR(msg, ...) fprintf(stderr, "JACK ERROR: " msg "\n", ##__VA_ARGS__);
#define MIN(a, b) ((a < b) ? (a) : (b))
#define MAX(a, b) ((a > b) ? (a) : (b))
#define EQ(a, b) (strcmp((a), (b)) == 0)
#define MAX_VALUE 255

#define MALLOC_CHECK(a) \
  if (a == NULL) { \
    fprintf(stderr, "Failed to allocate memory!\n"); \
    exit(EXIT_FAILURE); \
  }

char jack_client_name[128] = "pidalboard-expression-pedal";

// Optional calibration for the lowest and the biggest values
typedef struct { int32_t min_offset, max_offset; } Offset;

typedef struct Node Node;
typedef struct Queue Queue;

struct Node {
  uint32_t value;
  Node *next;
};

struct Queue {
  Node *head;
  Node *tail;
};

typedef struct {
  Offset offset;
  uint32_t sample_rate, buffer_size;
  jack_client_t *jack_client;
  jack_port_t *send_port, *return_port;
  uint8_t last_value;
  jack_nframes_t last_sample_idx;
  jack_nframes_t every_n_samples;
  bool binary_output;
  pthread_mutex_t queue_lock;
  Queue value_changes_queue;
  pthread_mutex_t thread_lock;
} State;

void* handle_value_updates(void *arg)
{
  State *state = (State *)arg;
  int stdout_fd = dup(fileno(stdout));

  for (;;) {
    uint32_t value = 0;
    pthread_mutex_lock(&state->thread_lock);

    next_queue_item:
      pthread_mutex_lock(&state->queue_lock);

      if (state->value_changes_queue.head == NULL) {
        pthread_mutex_unlock(&state->queue_lock);
        LOG("Value changes queue is empty.");
      } else {
        value = state->value_changes_queue.head->value;
        Node *tmp_node = state->value_changes_queue.head;
        tmp_node->next = NULL;
        state->value_changes_queue.head = state->value_changes_queue.head->next;

        if (state->value_changes_queue.head == NULL)
          state->value_changes_queue.tail = NULL;

        pthread_mutex_unlock(&state->queue_lock);
        free(tmp_node);

        if (state->binary_output) {
          if (write(stdout_fd, &value, sizeof(uint32_t)) == -1) {
            fprintf(stderr, "Failed to write binary data to stdout!\n");
            exit(EXIT_FAILURE);
          }
        } else {
          printf("New value: %d\n", value);
        }

        goto next_queue_item;
      }
  }
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
    state->last_sample_idx
      = (state->last_sample_idx + 1)
      % state->every_n_samples
  ) {
    send_buf[i] = 1; // Send just 1 as direct current
    if (state->last_sample_idx != 0) continue;

    uint8_t value = MIN(MAX(round(
      ((return_buf[i] * MAX_VALUE) - state->offset.min_offset)
        * MAX_VALUE / state->offset.max_offset
    ), 0), MAX_VALUE);

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

      pthread_mutex_unlock(&state->queue_lock);
      pthread_mutex_unlock(&state->thread_lock);
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
  Offset offset          = { 0, 0 };
  state->offset          = offset;
  state->sample_rate     = 0;
  state->buffer_size     = 0;
  state->jack_client     = NULL;
  state->send_port       = NULL;
  state->return_port     = NULL;
  state->last_value      = 0;
  state->last_sample_idx = 0;
  state->every_n_samples = 1;
  state->binary_output   = false;

  // cannot be initialized with NULL
  // state->queue_lock

  state->value_changes_queue.head = NULL;
  state->value_changes_queue.tail = NULL;

  // cannot be initialized with NULL
  // state->thread_lock
}

void run( Offset offset
        , jack_nframes_t every_n_samples
        , bool binary_output
        , bool calibrate
        )
{
  LOG("Initializing a mutex…");
  pthread_mutex_t queue_lock;
  pthread_mutex_t thread_lock;

  if (
    pthread_mutex_init(&queue_lock, NULL) != 0 ||
    pthread_mutex_init(&thread_lock, NULL) != 0
  ) {
    fprintf(stderr, "Mutex initialization failed!\n");
    exit(EXIT_FAILURE);
  }

  pthread_mutex_lock(&thread_lock);

  LOG("Initialization of state…");
  State *state = (State *)malloc(sizeof(State));
  MALLOC_CHECK(state);

  null_state(state);
  offset.max_offset += MAX_VALUE; // precalculate
  state->offset = offset;
  state->every_n_samples = every_n_samples;
  state->binary_output = binary_output;
  state->queue_lock = queue_lock;
  state->thread_lock = thread_lock;
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

  LOG("Running a thread for handing value updates queue…");
  pthread_t tid;
  int err = pthread_create(&tid, NULL, &handle_value_updates, (void *)state);

  if (err != 0) {
    fprintf(stderr, "Failed to create a thread: [%s]\n", strerror(err));
    exit(EXIT_FAILURE);
  }

  pthread_join(tid, NULL);
  pthread_mutex_destroy(&queue_lock);
  /* sleep(-1); */
}

void show_usage(FILE *out, char *app)
{
  size_t i = 0;
  char spaces[128];
  for (i = 0; i < strlen(app); ++i) spaces[i] = ' ';
  spaces[i] = '\0';
  fprintf(out, "Usage: %s [-b|--binary]\n", app);
  fprintf(out, "       %s [-c|--calibrate]\n", spaces);
  fprintf(out, "       %s [-l|--lower INT]\n", spaces);
  fprintf(out, "       %s [-u|--upper INT]\n", spaces);
  fprintf(out, "       %s [-n|--nsamples UINT]\n", spaces);
  fprintf(out, "\n");
  fprintf(out, "Available options:\n");
  fprintf(out, "  -b,--binary         Print binary unsigned 8-bit integers sequence\n");
  fprintf(out, "                      instead of human-readable lines\n");
  fprintf(out, "  -c,--calibrate      Calibrate min and max bounds.\n");
  fprintf(out, "                      Provide minimum value and see what offset you\n");
  fprintf(out, "                      need to set for minimum value. And then provide\n");
  fprintf(out, "                      maximum value and look at the maximum value offset.\n");
  fprintf(out, "  -l,--lower INT      Set min bound correction (see --calibrate)\n");
  fprintf(out, "  -u,--upper INT      Set max bound correction (see --calibrate)\n");
  fprintf(out, "  -n,--nsamples UINT  Calculate the value each N samples\n");
  fprintf(out, "                      (for optimization purposes)\n");
  fprintf(out, "  -h,-?,--help        Show this help text\n");
}

int main(int argc, char *argv[])
{
  LOG("Starting of application…");
  Offset offset = { 0, 0 };
  bool binary_output = false;
  bool calibrate = false;
  jack_nframes_t every_n_samples = 1;

  for (int i = 1; i < argc; ++i) {
    if (EQ(argv[i], "--help") || EQ(argv[i], "-h") || EQ(argv[i], "-?")) {
      show_usage(stdout, argv[0]);
      return EXIT_SUCCESS;
    } else if (EQ(argv[i], "-b") || EQ(argv[i], "--binary")) {
      binary_output = true;
      fprintf(stderr, "Setting stdout output format to binary mode…\n");
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
    } else if (EQ(argv[i], "-n") || EQ(argv[i], "--nsamples")) {
      if (++i >= argc) {
        fprintf(stderr, "There must be a value after “%s” argument!\n\n", argv[--i]);
        return EXIT_FAILURE;
      }

      char *ptr;
      long int x = strtol(argv[i], &ptr, 10);

      if ( ! EQ(ptr, "") || x < 1 || x > UINT32_MAX) {
        fprintf( stderr
               , "Incorrect positive unsigned integer value “%s” argument "
                 "provided for “%s”!\n\n"
               , argv[i]
               , argv[i-1]
               );
        return EXIT_FAILURE;
      }

      every_n_samples = x;
      LOG("Set amount of samples as steps between next value calculation to: %ld", x);
    } else {
      fprintf(stderr, "Incorrect argument: “%s”!\n\n", argv[i]);
      show_usage(stderr, argv[0]);
      return EXIT_FAILURE;
    }
  }

  run(offset, every_n_samples, binary_output, calibrate);
  return EXIT_SUCCESS;
}
