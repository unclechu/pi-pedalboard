/**
 * Author: Viacheslav Lotsmanov
 * License: GNU/GPLv3 https://raw.githubusercontent.com/unclechu/pi-pedalboard/master/LICENSE
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <jack/jack.h>

#ifdef DEBUG
# define LOG(msg, ...) fprintf(stderr, "JACK LOG: " msg "\n", ##__VA_ARGS__);
#else
# define LOG(...) ((void)0)
#endif

#define ERR(msg, ...) fprintf(stderr, "JACK ERROR: " msg "\n", ##__VA_ARGS__);

char jack_client_name[128] = "pi-pedalboard-expression-pedal";

typedef struct {
  uint32_t sample_rate, buffer_size;
  jack_client_t *jack_client;
  jack_port_t *send_port, *return_port;
  jack_default_audio_sample_t *send_buf, *return_buf;
} State;

int jack_process(jack_nframes_t nframes, void *arg)
{
  State *state = (State *)arg;
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

void bind_callbacks(State *state)
{
  LOG("Binding process callback…");
  jack_set_process_callback(state->jack_client, jack_process, (void *)state);
  LOG("Process callback is bound.");

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
  state->sample_rate = 0;
  state->buffer_size = 0;
  state->jack_client = NULL;
  state->send_port   = NULL;
  state->return_port = NULL;
  state->send_buf    = NULL;
  state->return_buf  = NULL;
}

void init()
{
  LOG("Initialization of state…");
  State *state = (State *)malloc(sizeof(State));

  if (!state) {
    ERR("State initialization failed!");
    exit(EXIT_FAILURE);
  }

  null_state(state);
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
  bind_callbacks(state);

  if (jack_activate(state->jack_client)) {
    ERR("Activating client failed!");
    exit(EXIT_FAILURE);
  }
}

int main()
{
  LOG("Starting of application…");
  init();
  sleep(-1);
  return EXIT_SUCCESS;
}
