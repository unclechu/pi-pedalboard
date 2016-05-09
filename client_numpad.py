#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# pedalboard client, numpad keys (useful for guitarix presets manipulating)

import socket
from sys        import argv
from subprocess import call

TCP_IP      = argv[1]
TCP_PORT    = 31415
BUFFER_SIZE = 1024
ENC         = 'UTF-8'

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect((TCP_IP, TCP_PORT))

btn_num_map = {
  '1': 'KP_End',
  '2': 'KP_Down',
  '3': 'KP_Next',
  '4': 'KP_Left',
  '5': 'KP_Begin'
}

try:
  while True:
    data = s.recv(BUFFER_SIZE)
    cols = data.decode(ENC).split('|')
    if cols[0] == 'button pressed':
      call(['xdotool', 'key', btn_num_map[cols[1]]])
      print('button pressed', cols[1])
except (KeyboardInterrupt, SystemExit):
  s.shutdown(socket.SHUT_RDWR)
  print('Done')
