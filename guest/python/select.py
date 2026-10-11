"""Stand-in for CPython's select C module (needed to import subprocess, which
webbrowser uses). There are no sockets or child processes to wait on, so
select() raises select.error. See _socket.py."""


class error(Exception):
    pass


def select(rlist, wlist, xlist, timeout=None):
    raise error(78, "select is not available")  # ENOSYS
