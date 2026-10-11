"""Stand-in for CPython's fcntl C module (needed to import subprocess, which
webbrowser uses). The calls raise IOError. See _socket.py."""

F_GETFD, F_SETFD, F_GETFL, F_SETFL = 1, 2, 3, 4
FD_CLOEXEC = 1
LOCK_SH, LOCK_EX, LOCK_NB, LOCK_UN = 1, 2, 4, 8


def _unavailable(*args):
    raise IOError(78, "fcntl is not available")  # ENOSYS


fcntl = ioctl = flock = lockf = _unavailable
