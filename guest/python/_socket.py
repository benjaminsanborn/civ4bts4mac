"""Stand-in for CPython's _socket C module, which the guest Python doesn't have
(the runtime only implements the few socket calls the game itself makes).

It lets socket, urllib, urllib2, webbrowser and friends import, as mods and
map scripts expect (BUG's update checker, some map scripts' "visit website"
links); creating a socket or resolving a name raises socket.error.
"""

AF_UNSPEC, AF_UNIX, AF_INET, AF_INET6 = 0, 1, 2, 30
SOCK_STREAM, SOCK_DGRAM, SOCK_RAW, SOCK_RDM, SOCK_SEQPACKET = 1, 2, 3, 4, 5
IPPROTO_IP, IPPROTO_TCP, IPPROTO_UDP = 0, 6, 17
SOL_SOCKET, SO_REUSEADDR, SO_KEEPALIVE, SO_BROADCAST = 0xffff, 0x4, 0x8, 0x20
SO_SNDBUF, SO_RCVBUF, SO_ERROR, SO_TYPE = 0x1001, 0x1002, 0x1007, 0x1008
TCP_NODELAY = 1
SHUT_RD, SHUT_WR, SHUT_RDWR = 0, 1, 2
MSG_OOB, MSG_PEEK, MSG_DONTWAIT = 0x1, 0x2, 0x80
AI_PASSIVE, AI_CANONNAME, AI_NUMERICHOST = 0x1, 0x2, 0x4
NI_NUMERICHOST, NI_NUMERICSERV = 0x2, 0x8
INADDR_ANY, INADDR_BROADCAST, INADDR_LOOPBACK = 0, 0xffffffff, 0x7f000001
SOMAXCONN = 128
has_ipv6 = False


class error(IOError):
    pass


class herror(error):
    pass


class gaierror(error):
    pass


class timeout(error):
    pass


def _unavailable(*args, **kwargs):
    raise error(50, "networking is not available")  # ENETDOWN


class socket(object):
    """Network sockets are not available in this build."""

    def __init__(self, family=AF_INET, type=SOCK_STREAM, proto=0):
        _unavailable()

    close = accept = bind = connect = connect_ex = fileno = listen = _unavailable
    getpeername = getsockname = getsockopt = setsockopt = sendall = _unavailable
    setblocking = settimeout = gettimeout = shutdown = _unavailable
    recv = recvfrom = recv_into = recvfrom_into = send = sendto = _unavailable


SocketType = socket

getaddrinfo = gethostbyname = gethostbyname_ex = gethostbyaddr = _unavailable
getnameinfo = getservbyname = getservbyport = getprotobyname = fromfd = _unavailable


def gethostname():
    return "localhost"


_default_timeout = None


def getdefaulttimeout():
    return _default_timeout


def setdefaulttimeout(t):
    global _default_timeout
    _default_timeout = t


def ntohs(x):
    return ((x & 0xff) << 8) | ((x >> 8) & 0xff)


def ntohl(x):
    return (((x & 0xff) << 24) | ((x & 0xff00) << 8) | ((x >> 8) & 0xff00) | ((x >> 24) & 0xff)) & 0xffffffffL


htons, htonl = ntohs, ntohl


def inet_aton(s):
    parts = [int(p) for p in s.split(".")]
    if len(parts) != 4 or [p for p in parts if not 0 <= p <= 255]:
        raise error("illegal IP address string passed to inet_aton")
    return "".join([chr(p) for p in parts])


def inet_ntoa(b):
    if len(b) != 4:
        raise error("packed IP wrong length for inet_ntoa")
    return ".".join([str(ord(c)) for c in b])
