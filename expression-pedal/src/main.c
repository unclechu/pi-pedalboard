/**
 * Author: Viacheslav Lotsmanov
 * License: GNU/GPLv3 https://raw.githubusercontent.com/unclechu/pi-pedalboard/master/LICENSE
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
#include <float.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
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

#define PERR(msg, ...) \
  { \
    char *str = malloc(sizeof("ERROR: ") + sizeof(msg) + 500); \
    MALLOC_CHECK(str); \
    sprintf(str, "ERROR: " msg, ##__VA_ARGS__); \
    perror(str); \
    free(str); \
    exit(EXIT_FAILURE); \
  }

#define MIN(a, b) ((a < b) ? (a) : (b))
#define MAX(a, b) ((a > b) ? (a) : (b))
#define EQ(a, b) (strcmp((a), (b)) == 0)

#define MALLOC_CHECK(a) if (a == NULL) ERR("Failed to allocate memory!");

#define AMP_TO_DB(amp) (20 * log10(amp))
#define DB_TO_AMP(dB) (pow(10, (dB / 20))

// Generic-ish hehe
#define DEFINE_QUEUE(prefix, item_type) \
  typedef struct prefix ## Node { \
    item_type             value; \
    struct prefix ## Node *next; \
  } prefix ## Node; \
  typedef struct prefix ## Queue { \
    prefix ## Node *head; \
    prefix ## Node *tail; \
  } prefix ## Queue;

#define QUEUE_PUSH(queue, new_node) \
  { \
    /* current last node (if there is one) now has pushed node going after it */ \
    if (queue.tail != NULL) queue.tail->next = new_node; \
    /* pushed node is the last item now */ \
    queue.tail = new_node; \
    /* first value in the queue is its head too */ \
    if (queue.head == NULL) queue.head = new_node; \
  }

// WARNING! It does not lock the mutex, only unlocks it!
#define QUEUE_SHIFT(queue, queue_lock_to_release) \
  ({ \
    __auto_type value = queue.head->value; \
    __auto_type tmp_node = queue.head; \
    queue.head = queue.head->next; \
    \
    /* end of the queue */ \
    if (queue.head == NULL) queue.tail = NULL; \
    \
    pthread_mutex_unlock(&queue_lock_to_release); \
    tmp_node->next = NULL; \
    free(tmp_node); \
    value; \
  })

char jack_client_name[] = "pidalboard-expression-pedal"; // TODO make customizable by command line args
int socket_port = 31416; // TODO make customizable by command line args

typedef jack_default_audio_sample_t sample_t; // shorter name
typedef struct { sample_t rms_min_bound, rms_max_bound; } RmsBounds; // in dB

DEFINE_QUEUE(Uint8,    uint8_t);
DEFINE_QUEUE(Decibels, sample_t);

typedef struct Connection {
  int                 socket_fd; // connection socket FD
  pthread_mutex_t     queue_lock;
  pthread_cond_t      queue_cond;
  Uint8Queue          value_changes_queue;
  struct Connection   *next;
} Connection;

typedef struct {
  jack_nframes_t      sample_rate, buffer_size;

  jack_client_t       *jack_client;
  jack_port_t         *send_port, *return_port;

  uint8_t             last_value;

  bool                binary_output;

  pthread_mutex_t     queue_lock;
  pthread_cond_t      queue_cond;
  Uint8Queue          value_changes_queue;
  DecibelsQueue       calibration_values_queue; // for calibration mode only

  int                 server_socket_fd;    // for socket mode only
  Connection          *socket_connections; // for socket mode only
  pthread_mutex_t     connections_lock;    // for socket mode only

  sample_t            sine_wave_freq;
  jack_nframes_t      sine_wave_sample_i;
  jack_nframes_t      sine_wave_one_rotation_samples;

  RmsBounds           rms_bounds;
  bool                use_default_rms_window_size;
  jack_nframes_t      rms_window_size;
  jack_nframes_t      rms_window_sample_i;
  sample_t            rms_sum;
  sample_t            last_rms_db;
} State;

void* handle_value_updates(void *arg)
{
  State *state = (State *)arg;

  int stdout_fd
    = (state->binary_output && state->server_socket_fd != -1)
    ? dup(fileno(stdout))
    : -1;

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
        uint8_t value =
          QUEUE_SHIFT(state->value_changes_queue, state->queue_lock);

        if (state->server_socket_fd != -1) {

          LOG("Sending value update (%d) to client socket connections…", value);
          pthread_mutex_lock(&state->connections_lock);
          Connection *connection = state->socket_connections;

          for (
            int i = 1;
            connection != NULL;
            connection = connection->next, ++i
          ) {
            LOG(
              "Sending value update (%d) to the client socket connection "
              "handler thread #%d (FD: %d)…",
              value,
              i,
              connection->socket_fd
            );

            Uint8Node *new_node = malloc(sizeof(Uint8Node));
            MALLOC_CHECK(new_node);
            new_node->value = value;
            new_node->next = NULL;
            pthread_mutex_lock(&connection->queue_lock);
            QUEUE_PUSH(connection->value_changes_queue, new_node);
            pthread_cond_signal(&connection->queue_cond);
            pthread_mutex_unlock(&connection->queue_lock);
          }

          pthread_mutex_unlock(&state->connections_lock);

        } else if (state->binary_output) {
          if (write(stdout_fd, &value, sizeof(uint8_t)) == -1)
            PERR("Failed to write binary data to stdout");
        } else {
          printf("%d\n", value);
        }

        // Don’t wait for a next notification yet,
        // handle whole queue before starting to wait again.
        pthread_mutex_lock(&state->queue_lock);
        goto handle_next_queue_item_without_waiting;
      }
  }
}

void* handle_calibrate_value_updates(void *arg)
{
  State *state = (State *)arg;

  for (;;) {
    pthread_mutex_lock(&state->queue_lock);
    LOG("Waiting for a notification of a new RMS dB value update…");
    pthread_cond_wait(&state->queue_cond, &state->queue_lock);
    LOG("Received a notification of a change of the RMS dB value.");

    handle_next_queue_item_without_waiting:
      if (state->calibration_values_queue.head == NULL) {
        pthread_mutex_unlock(&state->queue_lock);
        LOG("RMS dB value changes queue is empty.");
      } else {
        sample_t rms_db =
          QUEUE_SHIFT(state->calibration_values_queue, state->queue_lock);

        fprintf(stderr, "New RMS: %f dB\n", rms_db);

        // Don’t wait for a next notification yet,
        // handle whole queue before starting to wait again.
        pthread_mutex_lock(&state->queue_lock);
        goto handle_next_queue_item_without_waiting;
      }
  }
}

// Waits for a connection, and when receives one spawns a new thread which will
// wait for another connection whilst in current thread it will receive value
// updates and send those values to the connected client.
void* socket_client_handle(void *arg)
{
  LOG("Running a new socket connection thread…");
  State *state = (State *)arg;

  struct sockaddr_in client_address;
  memset(&client_address, 0, sizeof(client_address));
  socklen_t client_address_length = sizeof(client_address);

  LOG("Waiting for a new client socket connection…");

  int client_socket_fd = accept(
    state->server_socket_fd,
    (struct sockaddr *)&client_address,
    &client_address_length
  );

  if (client_socket_fd < 0) PERR("Failed to accept socket client connection");

  fprintf(
    stderr,
    "Received a socket connection from “%s” client (client socket FD: %d).\n",
    inet_ntoa(client_address.sin_addr),
    client_socket_fd
  );

  Connection *this_connection = malloc(sizeof(Connection));
  MALLOC_CHECK(this_connection);
  memset(this_connection, 0, sizeof(Connection));
  this_connection->socket_fd = client_socket_fd;
  if (pthread_mutex_init(&this_connection->queue_lock, NULL) != 0)
    ERR("pthread_mutex_init() error!");
  if (pthread_cond_init(&this_connection->queue_cond, NULL) != 0)
    ERR("pthread_cond_init() error!");
  this_connection->value_changes_queue.head = NULL;
  this_connection->value_changes_queue.tail = NULL;
  this_connection->next = NULL;

  LOG(
    "Appending connection entity (socket FD: %d) to the socket connections list…",
    client_socket_fd
  );

  pthread_mutex_lock(&state->connections_lock);

  {
    Connection *current_connection = state->socket_connections;
    if (current_connection == NULL)
      state->socket_connections = this_connection;
    else {
      while (current_connection->next != NULL)
        current_connection = current_connection->next;
      current_connection->next = this_connection;
    }
  }

  pthread_mutex_unlock(&state->connections_lock);
  LOG("Spawning another thread to wait for another client socket connection…");

  {
    pthread_t tid = -1;
    int err = pthread_create(&tid, NULL, &socket_client_handle, (void *)state);
    if (err != 0) ERR("Failed to create a thread: [%s]", strerror(err));

    LOG(
      "Spawned another thread for handling client socket connection (thread id: %ld).",
      tid
    );
  }

  for (;;) {
    pthread_mutex_lock(&this_connection->queue_lock);

    LOG(
      "Waiting for a notification of a new value update "
      "for client socket connection (FD: %d)…",
      client_socket_fd
    );

    pthread_cond_wait(
      &this_connection->queue_cond,
      &this_connection->queue_lock
    );

    LOG(
      "Received a notification of a change of the value "
      "for client socket connection (FD: %d)…",
      client_socket_fd
    );

    handle_next_queue_item_without_waiting:
      if (this_connection->value_changes_queue.head == NULL) {
        pthread_mutex_unlock(&this_connection->queue_lock);

        LOG(
          "Value changes queue is empty "
          "for client socket connection (FD: %d).",
          client_socket_fd
        );
      } else {
        uint8_t value = QUEUE_SHIFT(
          this_connection->value_changes_queue,
          this_connection->queue_lock
        );

        if (state->binary_output) {
          LOG(
            "Sending value update (%d) directly to client socket connection "
            "as 8-bit binary unsigned integer (in range from 0 to %d, FD %d)…",
            value,
            UINT8_MAX,
            client_socket_fd
          );
        } else {
          LOG(
            "Sending value update (%d) directly to client socket connection "
            "as a line with human-readable text with the number (FD %d)…",
            value,
            client_socket_fd
          );
        }

        ssize_t write_result = 0;

        if (state->binary_output)
          write_result = write(client_socket_fd, &value, sizeof(uint8_t));
        else {
          char str[sizeof("255\n")];
          sprintf(str, "%u\n", value);
          write_result = write(client_socket_fd, str, strlen(str));
        }

        if (write_result == -1) {
          fprintf(
            stderr,
            "Failed to write to client socket connection "
            "(client socket FD: %d), taking it as lost connection…\n",
            client_socket_fd
          );

          LOG(
            "Removing the connection from the connections list "
            "and closing client socket connection (FD: %d) …",
            client_socket_fd
          );

          pthread_mutex_lock(&state->connections_lock);
          Connection *connection = state->socket_connections;

          if (connection == this_connection) {
            state->socket_connections = connection->next;
          } else {
            for (;; connection = connection->next) {
              if (connection->next == NULL) ERR(
                "Unexpectedly reached end of client socket connections list "
                "when trying to remove client socket connection from the list "
                "(FD: %d)!",
                client_socket_fd
              );

              if (connection->next == this_connection) {
                connection->next = connection->next->next;
                connection = NULL;
                break;
              }
            }
          }

          pthread_mutex_unlock(&state->connections_lock);

          LOG(
            "Removed client socket connection from the client socket "
            "connections list. Freeing memory and closing the client socket "
            "connection (just in case, FD: %d)…",
            client_socket_fd
          );

          this_connection->next = NULL;
          free(this_connection);

          if (close(client_socket_fd) < 0) PERR(
            "Failed to close client socket connection (FD: %d)",
            client_socket_fd
          );

          LOG(
            "The client socket connection (FD: %d) handler thread is done.",
            client_socket_fd
          );

          return NULL; // end of the thread
        } else {
          // Don’t wait for a next notification yet,
          // handle whole queue before starting to wait again.
          pthread_mutex_lock(&this_connection->queue_lock);
          goto handle_next_queue_item_without_waiting;
        }
      }
  }

  return NULL;
}

inline sample_t sample_radians
( sample_t       hz
, jack_nframes_t sample_n
, jack_nframes_t sample_rate
)
{
  return (sample_t)sample_n * hz * 2 * M_PI / (sample_t)sample_rate;
}

// Should return “uint32_t” but in calculations everywhere “int64_t” is used.
inline sample_t finalize_rms_db(jack_nframes_t window_size, sample_t sum)
{
  return AMP_TO_DB(1.0f / (sample_t)window_size * sum);
}

int jack_process(jack_nframes_t nframes, void *arg)
{
  State    *state      = (State *)arg;
  sample_t *send_buf   = jack_port_get_buffer(state->send_port,   nframes);
  sample_t *return_buf = jack_port_get_buffer(state->return_port, nframes);

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
      sample_t rms_db = finalize_rms_db(state->rms_window_size, state->rms_sum);

      if (rms_db != state->last_rms_db) {
        state->last_rms_db = rms_db;

        uint8_t value = MIN(MAX(round(
          (rms_db - state->rms_bounds.rms_min_bound)
            * UINT8_MAX / state->rms_bounds.rms_max_bound
        ), 0), UINT8_MAX);

        if (value != state->last_value) {
          Uint8Node *new_node = malloc(sizeof(Uint8Node));
          MALLOC_CHECK(new_node);
          new_node->value = value;
          new_node->next = NULL;
          pthread_mutex_lock(&state->queue_lock);
          QUEUE_PUSH(state->value_changes_queue, new_node);
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
  State    *state      = (State *)arg;
  sample_t *send_buf   = jack_port_get_buffer(state->send_port,   nframes);
  sample_t *return_buf = jack_port_get_buffer(state->return_port, nframes);

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
      sample_t rms_db = finalize_rms_db(state->rms_window_size, state->rms_sum);

      if (rms_db != state->last_rms_db) {
        DecibelsNode *new_node = malloc(sizeof(DecibelsNode));
        MALLOC_CHECK(new_node);
        new_node->value = rms_db;
        new_node->next = NULL;
        pthread_mutex_lock(&state->queue_lock);
        QUEUE_PUSH(state->calibration_values_queue, new_node);
        pthread_cond_signal(&state->queue_cond);
        pthread_mutex_unlock(&state->queue_lock);
        state->last_rms_db = rms_db;
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
    (sample_t)state->sample_rate / state->sine_wave_freq
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
  pthread_t           value_updates_handler_tid;
  State               *state;
} ShutdownPayload;

ShutdownPayload shutdown_payload = { -1, NULL };

void terminate_app(bool jack_is_down)
{
  fprintf(stderr, "Terminating the app…\n");

  LOG("Cancelling value updates handling thread…");

  if (pthread_cancel(shutdown_payload.value_updates_handler_tid) != 0)
    ERR("pthread_cancel() error!");

  LOG("Destroying value queue lock…");
  pthread_mutex_destroy(&shutdown_payload.state->queue_lock);
  LOG("Destroying value condition variable…");
  pthread_cond_destroy(&shutdown_payload.state->queue_cond);

  if (shutdown_payload.state->server_socket_fd != -1) {
    LOG("Destroying connections lock…");
    pthread_mutex_destroy(&shutdown_payload.state->connections_lock);

    LOG("Closing opened client socket connections…");
    Connection *current_connection = shutdown_payload.state->socket_connections;
    shutdown_payload.state->socket_connections = NULL;
    Connection *tmp_connection = NULL;

    for (
      int i = 1;
      current_connection != NULL;
      ++i,
      tmp_connection = current_connection,
      current_connection = current_connection->next,
      tmp_connection->next = NULL,
      free(tmp_connection)
    ) {
      LOG(
        "Closing client socket connection #%d (FD: %d)…",
        i,
        current_connection->socket_fd
      );

      if (close(current_connection->socket_fd) < 0) PERR(
        "Failed to close client socket connection (FD: %d)",
        current_connection->socket_fd
      );
    }

    LOG(
      "Closing socket server (FD: %d)…",
      shutdown_payload.state->server_socket_fd
    );

    if (close(shutdown_payload.state->server_socket_fd) < 0) PERR(
      "Failed to close socket server (FD: %d)",
      shutdown_payload.state->server_socket_fd
    );
  }

  if ( ! jack_is_down) {
    LOG("Deactivating JACK client…");

    if (jack_deactivate(shutdown_payload.state->jack_client) != 0)
      ERRJACK("JACK client deactivation failed!");

    LOG("Closing JACK client…");

    if (jack_client_close(shutdown_payload.state->jack_client) != 0)
      ERRJACK("Closing JACK client failed!");
  }

  LOG("DONE!");
}

void jack_shutdown_callback()
{
  LOG("Received JACK shutdown notification, terminating…");
  terminate_app(true);
}

void sig_handler(int signum)
{
  LOG("Received “%s” signal, terminating the app…", strsignal(signum));
  terminate_app(false);
}

void null_state(State *state)
{
  state->sample_rate = 0;
  state->buffer_size = 0;

  state->jack_client = NULL;
  state->send_port   = NULL;
  state->return_port = NULL;

  state->last_value = 0;

  state->binary_output = false;

  memset(&state->queue_lock, 0, sizeof(pthread_mutex_t));
  memset(&state->queue_cond, 0, sizeof(pthread_cond_t));
  state->value_changes_queue.head = NULL;
  state->value_changes_queue.tail = NULL;
  state->calibration_values_queue.head = NULL;
  state->calibration_values_queue.tail = NULL;

  state->server_socket_fd = -1;
  state->socket_connections = NULL;
  memset(&state->connections_lock, 0, sizeof(pthread_mutex_t));

  state->sine_wave_freq                 = 0.0f;
  state->sine_wave_sample_i             = 0;
  state->sine_wave_one_rotation_samples = 0;

  RmsBounds rms_bounds               = { 0.0f, 0.0f };
  state->rms_bounds                  = rms_bounds;
  state->use_default_rms_window_size = false;
  state->rms_window_size             = 0;
  state->rms_window_sample_i         = 0;
  state->rms_sum                     = 0.0f;
  state->last_rms_db                 = 0.0f;
}

void init_socket_server(State *state)
{
  LOG("Initializing socket server…");
  struct sockaddr_in server_address;
  memset(&server_address, 0, sizeof(server_address));

  LOG("Opening a socket…");
  state->server_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (state->server_socket_fd < 0) PERR("Failed to open a socket");

  {
    LOG("Setting socket server address option as reusable…");
    int enable = 1;
    if (setsockopt(
      state->server_socket_fd,
      SOL_SOCKET,
      SO_REUSEADDR,
      &enable,
      sizeof(enable)
    ) < 0)
      PERR("Failed to set socket server address option as reusable");
  }

  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = INADDR_ANY;
  server_address.sin_port = htons(socket_port);

  LOG("Binding socket on %d port…", socket_port);
  if (bind(
    state->server_socket_fd,
    (struct sockaddr *)&server_address,
    sizeof(server_address)
  ) < 0)
    PERR("Failed to bind socket to %d port", socket_port);

  LOG("Starting to listen for the socket…");
  if (listen(state->server_socket_fd, 5) < 0)
    PERR("Failed to start listening to a socket on %d port", socket_port);

  LOG(
    "Socket server is initialized (server socket FD: %d).",
    state->server_socket_fd
  );
}

void run
( RmsBounds      rms_bounds
, sample_t       sine_wave_freq  // 0 for default value
, jack_nframes_t rms_window_size // 0 for default value
, bool           binary_output
, bool           socket_server
, bool           calibrate
)
{
  LOG("Initializing a queue mutex…");

  LOG("Initialization of state…");
  State *state = (State *)malloc(sizeof(State));
  MALLOC_CHECK(state);
  null_state(state);
  state->binary_output = binary_output;
  if (pthread_mutex_init(&state->queue_lock, NULL) != 0)
    ERR("pthread_mutex_init() error!");
  if (pthread_cond_init(&state->queue_cond, NULL) != 0)
    ERR("pthread_cond_init() error!");
  if (socket_server)
    if (pthread_mutex_init(&state->connections_lock, NULL) != 0)
      ERR("pthread_mutex_init() error!");
  state->sine_wave_freq = (sine_wave_freq == 0) ? 440.0f : sine_wave_freq;
  state->rms_bounds = rms_bounds;
  state->rms_bounds.rms_max_bound -= state->rms_bounds.rms_min_bound; // Precalculate
  state->use_default_rms_window_size = rms_window_size == 0;
  if (rms_window_size != 0) state->rms_window_size = rms_window_size;
  LOG("State is initialized…");

  LOG("Opening JACK client…");
  jack_status_t status;

  state->jack_client = jack_client_open(
    jack_client_name,
    JackNullOption,
    &status,
    NULL
  );

  if (state->jack_client == NULL) ERRJACK("Opening client failed!");

  if (status & JackNameNotUnique)
    ERRJACK("Client name “%s” is already taken!", jack_client_name);

  LOG("JACK client is opened.");
  register_ports(state);
  bind_callbacks(state, calibrate);

  LOG("Running a thread for handing value updates queue…");
  pthread_t value_updates_handler_tid = -1;

  {
    int err = pthread_create(
      &value_updates_handler_tid,
      NULL,
      calibrate ? &handle_calibrate_value_updates : &handle_value_updates,
      (void *)state
    );

    if (err != 0) ERR("Failed to create a thread: [%s]", strerror(err));

    LOG(
      "Value updates handling thread is spawned (thread id: %ld).",
      value_updates_handler_tid
    );
  }

  if (socket_server) {
    init_socket_server(state);
    pthread_t socket_connection_handler_tid = -1;

    int err = pthread_create(
      &socket_connection_handler_tid,
      NULL,
      &socket_client_handle,
      (void *)state
    );

    if (err != 0) ERR("Failed to create a thread: [%s]", strerror(err));

    LOG(
      "Spawned a thread for handling client socket connection (thread id: %ld).",
      socket_connection_handler_tid
    );
  }

  LOG("Setting shutdown callbacks…");
  shutdown_payload.value_updates_handler_tid = value_updates_handler_tid;
  shutdown_payload.state = state;
  jack_on_shutdown(state->jack_client, jack_shutdown_callback, NULL);
  signal(SIGABRT, sig_handler);
  signal(SIGHUP,  sig_handler);
  signal(SIGINT,  sig_handler);
  signal(SIGQUIT, sig_handler);
  signal(SIGTERM, sig_handler);

  if (binary_output)
    fprintf(
      stderr,
      "Playing sine wave, analyzing returned signal and %s "
      "as 8-bit binary unsigned integers (in range from 0 to %d)…\n",
      socket_server
        ? "sending detected values to socket server clients"
        : "printing detected values to stdout",
      UINT8_MAX
    );
  else
    fprintf(
      stderr,
      "Playing sine wave, analyzing returned signal and %s "
      "as lines with human-readable text with numbers "
      "(in range from 0 to %d)…\n",
      socket_server
        ? "sending detected values to socket server clients"
        : "printing detected values to stdout",
      UINT8_MAX
    );

  if (jack_activate(state->jack_client) != 0)
    ERRJACK("Client activation failed!");

  pthread_join(value_updates_handler_tid, NULL);
  /* sleep(-1); */
}

void show_usage(FILE *out, char *app)
{
  size_t i = 0;
  char spaces[128];
  for (i = 0; i < strlen(app); ++i) spaces[i] = ' ';
  spaces[i] = '\0';
  fprintf(out, "Usage: %s -l|--lower FLOAT\n", app);
  fprintf(out, "       %s -u|--upper FLOAT\n", spaces);
  fprintf(out, "       %s [-c|--calibrate]\n", spaces);
  fprintf(out, "       %s [-b|--binary]\n", spaces);
  fprintf(out, "       %s [-s|--socket]\n", spaces);
  fprintf(out, "       %s [-f|--frequency UINT]\n", spaces);
  fprintf(out, "       %s [-w|--rms-window UINT]\n", spaces);
  fprintf(out, "\n");
  fprintf(out, "For me (the author of the program) the range between -90 dB and -6 dB works well:\n");
  fprintf(out, "  %s -l -90 -u -6\n", app);
  fprintf(out, "\n");
  fprintf(out, "Available options:\n");
  fprintf(out, "  -l,--lower FLOAT      Set min RMS in dB (see --calibrate).\n");
  fprintf(out, "  -u,--upper FLOAT      Set max RMS in dB (see --calibrate).\n");
  fprintf(out, "  -c,--calibrate        Calibrate min and max RMS bounds.\n");
  fprintf(out, "                        Set your pedal to minimum position and record the value.\n");
  fprintf(out, "                        Then do the same for maximum position.\n");
  fprintf(out, "                        Use those values for --lower and --upper arguments.\n");
  fprintf(out, "  -b,--binary           Print binary unsigned 8-bit integers sequence\n");
  fprintf(out, "                        instead of human-readable lines.\n");
  fprintf(out, "  -s,--socket           Start socket server on port %d and send\n", socket_port);
  fprintf(out, "                        8-bit integers sequence to connected clients\n");
  fprintf(out, "                        (as human-readable lines by default and\n");
  fprintf(out, "                        as binary stream with --binary).\n");
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

  RmsBounds      rms_bounds      = { -1, -1 };
  bool           has_rms_min     = false;
  bool           has_rms_max     = false;
  bool           binary_output   = false;
  bool           socket_server   = false;
  bool           calibrate       = false;
  jack_nframes_t rms_window_size = 0;
  sample_t       sine_wave_freq  = 0;

  LOG("Parsing command-line arguments…");

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

      double x = atof(argv[i]);

      // “float”’s range must be enough, it’s in decibels after all.
      if (x < -FLT_MAX || x > FLT_MAX) {
        fprintf( stderr
               , "Incorrect floating point value “%s” "
                 "argument provided for “%s”!\n\n"
               , argv[i]
               , argv[i-1]
               );
        show_usage(stderr, argv[0]);
        return EXIT_FAILURE;
      }

      if (EQ(argv[i-1], "-l") || EQ(argv[i-1], "--lower")) {
        rms_bounds.rms_min_bound = (float)x;
        has_rms_min = true;
        LOG("Setting min RMS to %f dB…", rms_bounds.rms_min_bound);
      } else {
        rms_bounds.rms_max_bound = (float)x;
        has_rms_max = true;
        LOG("Setting max RMS to %f dB…", rms_bounds.rms_max_bound);
      }
    } else if (EQ(argv[i], "-c") || EQ(argv[i], "--calibrate")) {
      calibrate = true;
      LOG("Turning calibration mode on…");
    } else if (EQ(argv[i], "-b") || EQ(argv[i], "--binary")) {
      binary_output = true;
      LOG("Setting stdout output format to binary mode…");
    } else if (EQ(argv[i], "-s") || EQ(argv[i], "--socket")) {
      socket_server = true;
      LOG("Turning on socket server on…");
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

      sine_wave_freq = (sample_t)x;
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
  } else if ( ! has_rms_min || ! has_rms_max) {
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

  run(
    rms_bounds,
    sine_wave_freq,
    rms_window_size,
    binary_output,
    socket_server,
    calibrate
  );

  return EXIT_SUCCESS;
}

// Root Mean Square (RMS) calculation.
// This function isn’t used in the code, it’s inlined where needed.
// Just useful to see the reference implementation.
inline sample_t rms(sample_t *samples_list, jack_nframes_t window_size)
{
  sample_t sum = 0.0f;

  for (jack_nframes_t i = 0; i < window_size; ++i)
    sum += powf(samples_list[i], 2);

  return (1.0f / (sample_t)window_size) * sum;
}
