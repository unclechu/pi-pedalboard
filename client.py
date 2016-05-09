#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# pedalboard client (trigget numpad keys for guitarix)
# TODO connect to guitarix remote control instead of triggering numpad keys
# work in progress...

import socket
from threading import Thread
from gpiozero  import Button
from signal    import pause
from time      import sleep, time

from pedalboard.radio import Radio

TCP_IP   = ''
TCP_PORT = 31415
ENC      = 'UTF-8'


class SocketThread(Thread):
  
  def __init__(self, radio):
    self.radio = radio
    Thread.__init__(self)
  
  def run(self):
    print('test')


radio = Radio()

try:
  pause()
except (KeyboardInterrupt, SystemExit):
  print('Exiting... Closing connection...')
  radio.trigger('close connection')
  sleep(1)
  print('Done')
