#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# pedalboard server

import socket
from threading import Thread
from gpiozero  import Button
from signal    import pause
from time      import sleep, time

from pedalboard.radio import Radio

TCP_IP            = '0.0.0.0'
TCP_PORT          = 31415

ENC               = 'UTF-8'
NEW_PRESS_DELAY   = 0.3 # in seconds
CONNECTIONS_LIMIT = 5

buttons_map = [
  (1, 3),
  (2, 4),
  (3, 17),
  (4, 27),
  (5, 22)
]


class BtnsThread(Thread):
  
  def __init__(self, radio):
    self.radio = radio
    self.last_press_time = 0
    self.is_released = True
    Thread.__init__(self)
  
  def pressed(self, n):
    def f():
      if time() - (self.last_press_time + NEW_PRESS_DELAY) <= 0: return
      print('Pressed button #%d' % n)
      self.last_press_time = time()
      self.is_released = False
      self.radio.trigger('button pressed', n=n)
    return f
  
  def released(self, n):
    def f():
      if self.is_released: return
      print('Released button #%d' % n)
      self.is_released = True
      self.radio.trigger('button released', n=n)
    return f
  
  def run(self):
    for btn in [(x[0], Button(x[1])) for x in buttons_map]:
      btn[1].when_pressed = self.pressed(btn[0])
      btn[1].when_released = self.released(btn[0])
    print('Started buttons listening')


class SocketThread(Thread):
  
  is_dead = True
  
  def __init__(self, radio, conn, addr):
    self.is_dead = False
    self.radio   = radio
    self.conn    = conn
    self.addr    = addr
    self.radio.on('close connections', self.__del__)
    Thread.__init__(self)
  
  def __del__(self):
    if self.is_dead: return
    self.radio.off('close connections', self.__del__)
    self.radio.off('button pressed', self.send_pressed)
    self.radio.off('button released', self.send_released)
    self.conn.close()
    print('Connection lost for:', self.addr)
    del self.radio
    del self.conn
    del self.addr
    del self.is_dead
  
  def send_pressed(self, n):
    try:
      self.conn.send(bytes('button pressed|%d\n' % n, ENC))
      print('Sent about button pressed to', self.addr)
    except BrokenPipeError:
      self.__del__()
  
  def send_released(self, n):
    try:
      self.conn.send(bytes('button released|%d\n' % n, ENC))
      print('Sent about button released to', self.addr)
    except BrokenPipeError:
      self.__del__()
  
  def run(self):
    print('Address connected:', self.addr)
    self.radio.on('button pressed', self.send_pressed)
    self.radio.on('button released', self.send_released)


radio = Radio()
BtnsThread(radio).start()


s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((TCP_IP, TCP_PORT))
s.listen(CONNECTIONS_LIMIT)

try:
  print('Starting listening for socket connections...')
  while True:
    conn, addr = s.accept()
    SocketThread(radio, conn, addr).start()
except (KeyboardInterrupt, SystemExit):
  print('Exiting... Closing all connections...')
  radio.trigger('close connections')
  sleep(1)
  s.shutdown(socket.SHUT_RDWR)
  print('Done')
