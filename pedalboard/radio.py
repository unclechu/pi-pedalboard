# -*- coding: utf-8 -*-


# exceptions
class RadioException(Exception):                     pass
class ListenerNotFound(RadioException):              pass
class HandlerAlreadyBound(RadioException):           pass
class OnHandlerAlreadyBound(HandlerAlreadyBound):    pass
class ReplyHandlerAlreadyBound(HandlerAlreadyBound): pass
class UnexpectedBehavior(RadioException):            pass


class Radio:
  
  is_dead = True
  
  def __init__(self):
    self.is_dead = False
    # (listener_type, event_name, handler)
    self.listeners = []
  
  def __del__(self):
    if self.is_dead: return
    del self.listeners
    del self.is_dead
  
  def on(self, ev_name, func):
    
    for listener in self.listeners:
      listener_type, listener_ev_name, listener_handler = listener
      # one event name can have infinite listeners
      if listener_type    == 'on' and \
         listener_ev_name == ev_name and \
         listener_handler == func:
        raise OnHandlerAlreadyBound(
          "Handler already bound on event '%s'" % ev_name
        )
    
    self.listeners.append(('on', ev_name, func))
    print("Bound 'on'-handler on event '%s'" % ev_name)
  
  def off(self, ev_name, func):
    
    new_listeners = []
    
    for listener in self.listeners:
      listener_type, listener_ev_name, listener_handler = listener
      if listener_type    != 'on' or \
         listener_ev_name != ev_name or \
         listener_handler != func:
        new_listeners.append(listener)
      else:
        print("Unbound 'on'-handler on event '%s'" % ev_name)
    
    if len(new_listeners) == len(self.listeners):
      raise ListenerNotFound("Listener for event '%s' not found" % ev_name)
    if len(new_listeners) != len(self.listeners) - 1:
      raise UnexpectedBehavior(
        "Removed listeners of event '%s' more than expected" % ev_name
      )
    else:
      self.listeners = new_listeners
  
  def trigger(self, ev_name, **kwargs):
    
    for listener in self.listeners:
      listener_type, listener_ev_name, listener_handler = listener
      if listener_type == 'on' and listener_ev_name == ev_name:
        listener_handler(**kwargs)
        print("Triggered event '%s' handler" % ev_name)
  
  def reply(self, req_name, func):
    
    for listener in self.listeners:
      listener_type, listener_ev_name, listener_handler = listener
      # only one handler can be replier for one request name
      if listener_type    == 'reply' and \
         listener_ev_name == req_name:
        raise ReplyHandlerAlreadyBound(
          "Handler already bound on event '%s'" % req_name
        )
    
    self.listeners.append(('reply', req_name, func))
    print("Bound 'reply'-handler on event '%s'" % req_name)
  
  # 'func' argument is optional because one request name
  # can have only one listener.
  def stopReplying(self, req_name, func=None):
    
    new_listeners = []
    
    for listener in self.listeners:
      listener_type, listener_req_name, listener_handler = listener
      if listener_type != 'reply' or listener_req_name != req_name:
        new_listeners.append(listener)
      elif func is not None and func != listener_handler:
        raise UnexpectedBehavior(
          "Replier handler and handler to stop replying must be" +
            (" same for request name ('%s')" % req_name)
        )
      else:
        print("Unbound 'reply'-handler on request name '%s'" % req_name)
    
    if len(new_listeners) == len(self.listeners):
      raise ListenerNotFound("Replier for request name '%s' not found" % req_name)
    if len(new_listeners) != len(self.listeners) - 1:
      raise UnexpectedBehavior(
        "Removed repliers of request name '%s' more than expected" % req_name
      )
    else:
      self.listeners = new_listeners
  
  def request(self, req_name, **kwargs):
    
    found = None
    
    for listener in self.listeners:
      listener_type, listener_req_name, listener_handler = listener
      if listener_type == 'reply' and listener_req_name == req_name:
        if found is None:
          found = listener_handler
        else:
          raise UnexpectedBehavior(
            "Found more than one replier for request name '%s'" % req_name
          )
    
    if found is None:
      raise ListenerNotFound(
        "Replier for request name '%s' not found" % req_name
      )
    
    return found(**kwargs)
