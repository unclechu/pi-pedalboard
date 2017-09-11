#!/usr/bin/env python3
# pedalboard server

import socket
from threading import Thread
from gpiozero  import Button
from signal    import pause
from time      import sleep, time
from radio     import Radio


TCP_IP            = '0.0.0.0'
TCP_PORT          = 31415

ENC               = 'UTF-8'
NEW_PRESS_DELAY   = 0.3 # in seconds
CONNECTIONS_LIMIT = 5

buttons_map = [
  (1, 2),
  (2, 3),
  (3, 4),
  (4, 17),
  (5, 27),
  (6, 22),
  (7, 10),
  (8, 9),
  (9, 11)
]


class BtnsThread(Thread):

  is_dead = True
  buttons = None

  def __init__(self, radio):
    self.is_dead = False
    self.radio = radio
    self.last_press_time = 0
    self.is_released = True
    super().__init__()

  def __del__(self):

    if self.is_dead: return
    print('Stopping listening for buttons…')

    if self.buttons is not None:
      for btn in self.buttons:
        btn[1].when_pressed = None
        btn[1].when_released = None
      del self.buttons

    del self.radio
    del self.last_press_time
    del self.is_released
    del self.is_dead

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
    self.buttons = [(x[0], Button(x[1])) for x in buttons_map]
    for btn in self.buttons:
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
    self.radio.trigger('add connection', connection=self)
    self.radio.on('close connections', self.__del__)
    super().__init__()

  def __del__(self):
    if self.is_dead: return
    self.radio.off('close connections', self.__del__)
    self.radio.off('button pressed', self.send_pressed, soft=True)
    self.radio.off('button released', self.send_released, soft=True)
    self.conn.close()
    self.radio.trigger('remove connection', connection=self)
    print('Connection lost for:', self.addr)
    del self.radio
    del self.conn
    del self.addr
    del self.is_dead

  def send_pressed(self, n):
    try:
      self.conn.send(bytes('button pressed|%d' % n, ENC))
      print('Sent about button pressed to', self.addr)
    except BrokenPipeError:
      self.__del__()

  def send_released(self, n):
    try:
      self.conn.send(bytes('button released|%d' % n, ENC))
      print('Sent about button released to', self.addr)
    except BrokenPipeError:
      self.__del__()

  def run(self):
    print('Address connected:', self.addr)
    self.radio.on('button pressed', self.send_pressed)
    self.radio.on('button released', self.send_released)


class ConnectionsHandler:

  is_dead = True

  def __init__(self, radio):
    self.is_dead = False
    self.connections = []
    self.radio = radio
    self.radio.reply('opened connections count', self.get_connections_count)
    self.radio.on('add connection', self.register_connection)
    self.radio.on('remove connection', self.unregister_connection)
    print('Started connections handling')

  def __del__(self):

    if self.is_dead: return

    self.radio.stopReplying(
      'opened connections count',
      self.get_connections_count
    )
    self.radio.off('add connection', self.register_connection)
    self.radio.off('remove connection', self.unregister_connection)

    for conn in self.connections:
      conn.__del__()
      del conn

    print('Stopped connections handling')
    del self.connections
    del self.radio
    del self.is_dead

  def register_connection(self, connection):

    for conn in self.connections:
      if conn == connection:
        raise Exception('Connection already registered')

    self.connections.append(connection)

  def unregister_connection(self, connection):

    new_connections = []

    for conn in self.connections:
      if conn != connection:
        new_connections.append(conn)

    if len(new_connections) == len(self.connections):
      raise Exception('Connection not found to unregister')
    elif len(new_connections) != len(self.connections) - 1:
      raise Exception('More than one connection to unregister')
    else:
      self.connections = new_connections

  def get_connections_count(self):
    return len(self.connections)


radio = Radio()

btns = BtnsThread(radio)
btns.start()

conn_handler = ConnectionsHandler(radio)

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((TCP_IP, TCP_PORT))
s.listen(CONNECTIONS_LIMIT)

try:
  print('Starting listening for socket connections…')
  while True:
    conn, addr = s.accept()
    SocketThread(radio, conn, addr).start()
except (KeyboardInterrupt, SystemExit):

  print('Exiting… Closing all connections…')
  radio.trigger('close connections')

  while True:
    conns_count = radio.request('opened connections count')
    if conns_count == 0: break
    sleep(0.1)

  conn_handler.__del__()
  del conn_handler
  btns.__del__()
  del btns
  radio.__del__()
  del radio

  s.shutdown(socket.SHUT_RDWR)

  print('Done')
