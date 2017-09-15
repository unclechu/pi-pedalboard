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
uint32_t sample_rate = 0, buffer_size = 0;
jack_client_t *jack_client = NULL;
jack_port_t *send_port = NULL, *return_port = NULL;
jack_default_audio_sample_t *send_buf = NULL, *return_buf = NULL;

int jack_process(jack_nframes_t nframes, void *arg)
{
  // TODO
  return 0;
}

void register_ports()
{
  LOG("Registering send port…");

  send_port = jack_port_register( jack_client
                                , "send"
                                , JACK_DEFAULT_AUDIO_TYPE
                                , JackPortIsOutput
                                , 0
                                );

  if (send_port == NULL) {
    ERR("Registering send port failed!");
    exit(EXIT_FAILURE);
  }

  LOG("Send port is registered.");
  LOG("Registering return port…");

  return_port = jack_port_register( jack_client
                                  , "return"
                                  , JACK_DEFAULT_AUDIO_TYPE
                                  , JackPortIsInput
                                  , 0
                                  );

  if (return_port == NULL) {
    ERR("Registering send port failed!");
    exit(EXIT_FAILURE);
  }

  LOG("Return port is registered.");
}

int set_sample_rate(jack_nframes_t nframes, void *arg)
{
  sample_rate = nframes;
  LOG("New sample rate: %d", sample_rate);
  return 0;
}

int set_buffer_size(jack_nframes_t nframes, void *arg)
{
  buffer_size = nframes;
  LOG("New buffer size: %d", nframes);
  return 0;
}

int main()
{
  jack_status_t status;
  LOG("Opening client…");

  jack_client = jack_client_open( jack_client_name
                                , JackNullOption
                                , &status
                                , NULL
                                );

  if (jack_client == NULL) {
    ERR("Opening client failed!");
    return EXIT_FAILURE;
  }

  if (status & JackNameNotUnique) {
    ERR("Client name '%s' is already taken!", jack_client_name);
    return EXIT_FAILURE;
  }

  LOG("Client is opened.");
  register_ports();

  LOG("Binding process callback…");
  jack_set_process_callback(jack_client, jack_process, 0);
  LOG("Process callback is bound.");

  LOG("Binding sample rate callback…");
  jack_set_sample_rate_callback(jack_client, set_sample_rate, 0);
  LOG("Sample rate callback is bound.");

  LOG("Binding buffer size callback…");
  jack_set_buffer_size_callback(jack_client, set_buffer_size, 0);
  LOG("Buffer size callback is bound.");

  if (jack_activate(jack_client)) {
    ERR("Activating client failed!");
    return EXIT_FAILURE;
  }

  sleep(-1);
  return EXIT_SUCCESS;
}
