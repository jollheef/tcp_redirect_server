#!/usr/bin/env python3
# @file test.py
# @author Михаил Клементьев < jollheef <AT> riseup.net >
# @date Март 2015
# @license GPLv3
# @brief tcp connection test application
#
# Создает большое количество соединений на указанный порт и не закрывает их.

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
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.connect(('127.0.0.1', port))

for i in range(0, 65535):
    a = Thread(target=create_socket)
    a.run()
