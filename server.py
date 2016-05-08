#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# FIXME two connections

import socket
from threading import Thread
from gpiozero  import Button
from signal    import pause
from time      import sleep, time

TCP_IP            = '0.0.0.0'
TCP_PORT          = 31415
CONNECTIONS_LIMIT = 5
ENC               = 'UTF-8'
NEW_PRESS_DELAY   = 0.3 # in seconds

buttons_map = [
  (1, 3),
  (2, 4),
  (3, 17),
  (4, 27),
  (5, 22)
]


class Radio:
  
  def __init__(self):
    self.listeners = []
  
  def on(self, ev_name, func):
    self.listeners.append((ev_name, func))
    print("Bound callback on event '%s'" % ev_name)
  
  def off(self, ev_name, func):
    new_listeners = []
    for listener in self.listeners:
      if listener[0] != ev_name and listener[1] is not func:
        new_listeners.append(listener)
      else:
        print("Unbound callback on event '%s'" % ev_name)
    if len(new_listeners) is len(self.listeners) and len(new_listeners) > 0:
      raise Exception('Listener not found')
    else:
      self.listeners = new_listeners
  
  def trigger(self, ev_name, **kwargs):
    for listener in self.listeners:
      if listener[0] == ev_name:
        listener[1](**kwargs)
        print("Triggered event '%s' callback" % ev_name)


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
      conn.send(bytes('button pressed|%d\n' % n, ENC))
      print('Sent about button pressed to', self.addr)
    except BrokenPipeError:
      self.__del__()
  
  def send_released(self, n):
    try:
      conn.send(bytes('button released|%d\n' % n, ENC))
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
s.bind((TCP_IP, TCP_PORT))
s.listen(CONNECTIONS_LIMIT)

try:
  while True:
    conn, addr = s.accept()
    SocketThread(radio, conn, addr).start()
except (KeyboardInterrupt, SystemExit):
  print('Exiting... Closing all connections...')
  radio.trigger('close connections')
  sleep(1)
  print('Done')
