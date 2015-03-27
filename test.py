#!/usr/bin/env python3

import socket
from threading import Thread
import sys

port = 50006

try:
    port = int(sys.argv[1])
except:
    pass

def create_socket():
    global port
    s = socket.socket(socket.AF_INET)
    s.connect(('127.0.0.1', port))

for i in range(0, 65535):
    a = Thread(target=create_socket)
    a.run()
