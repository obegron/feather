#!/usr/bin/env python3
"""Private test helper: JSON events only; never opens sockets or transfers files."""
import json
import sys

def emit(value):
    print(json.dumps(value), flush=True)

emit({'event': 'ready'})
for line in sys.stdin:
    command = json.loads(line)
    kind = command['command']
    if kind == 'shutdown':
        emit({'event': 'stopped'})
        break
    if kind == 'browse':
        emit({'event': 'peer', 'id': 'test-phone', 'name': 'Test phone'})
    if kind == 'send':
        emit({'event': 'started', 'id': command['id'], 'direction': 'outgoing'})
        emit({'event': 'progress', 'id': command['id'], 'done': 64, 'total': 128})
    if kind == 'cancel':
        emit({'event': 'cancelled', 'id': command['id']})
