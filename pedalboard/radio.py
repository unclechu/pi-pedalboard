# -*- coding: utf-8 -*-


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
    if len(new_listeners) == len(self.listeners) and len(new_listeners) > 0:
      raise Exception('Listener not found')
    else:
      self.listeners = new_listeners
  
  def trigger(self, ev_name, **kwargs):
    for listener in self.listeners:
      if listener[0] == ev_name:
        listener[1](**kwargs)
        print("Triggered event '%s' callback" % ev_name)
