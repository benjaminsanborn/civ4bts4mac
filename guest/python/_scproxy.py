"""Stand-in for CPython's _scproxy C module (macOS proxy settings, used by
urllib on darwin): no proxies. See _socket.py."""


def _get_proxy_settings():
    return {"exclude_simple": False, "exceptions": []}


def _get_proxies():
    return {}
