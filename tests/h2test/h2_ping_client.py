#!/usr/bin/env python3
"""
Minimal HTTP/2 client: TLS + ALPN h2, SETTINGS handshake, PING, print events.
Used to debug PING ACK vs server connection teardown (h2spec http2/6.7/1).

Always assign conn.data_to_send() to a variable before tls.sendall(...).
Calling tls.sendall(conn.data_to_send()) in one expression has been observed to
hang the SETTINGS exchange with CPython + hyper-h2 (buffer lifetime).
"""
import socket
import ssl
import sys

import h2.config
import h2.connection
import h2.settings


def main():
    """
    Run TLS+ALPN h2, complete SETTINGS exchange, send one PING, expect matching ACK.
    Returns 0 on success, 1 on failure.
    """
    host = "127.0.0.1"
    port = 8443
    if len(sys.argv) >= 2:
        port = int(sys.argv[1])

    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.set_alpn_protocols(["h2"])

    raw = socket.create_connection((host, port))
    raw.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    tls = ctx.wrap_socket(raw, server_hostname=host)
    print("alpn", tls.selected_alpn_protocol())

    conn = h2.connection.H2Connection(
        config=h2.config.H2Configuration(client_side=True))
    # RFC 7540 §6.5.2: clients must not send ENABLE_PUSH != 0; h2 defaults to 1.
    conn.local_settings[h2.settings.SettingCodes.ENABLE_PUSH] = 0
    conn.initiate_connection()
    preface = conn.data_to_send()
    tls.sendall(preface)

    local_ack = False
    remote_settings = False

    try:
        chunk = tls.recv(65536)
    except ssl.SSLError as e:
        print("recv SSLError", e)
        return 1
    if not chunk:
        print("recv EOF (TCP closed by peer)")
        return 1
    print("recv tls bytes", len(chunk))
    for ev in conn.receive_data(chunk):
        print(" ", type(ev).__name__, ev)
        if type(ev).__name__ == "SettingsAcknowledged":
            local_ack = True
        if type(ev).__name__ == "RemoteSettingsChanged":
            remote_settings = True
    out1 = conn.data_to_send()
    tls.sendall(out1)

    try:
        chunk = tls.recv(65536)
    except ssl.SSLError as e:
        print("recv SSLError", e)
        return 1
    if not chunk:
        print("recv EOF (TCP closed by peer)")
        return 1
    print("recv tls bytes", len(chunk))
    for ev in conn.receive_data(chunk):
        print(" ", type(ev).__name__, ev)
        if type(ev).__name__ == "SettingsAcknowledged":
            local_ack = True
        if type(ev).__name__ == "RemoteSettingsChanged":
            remote_settings = True
    out = conn.data_to_send()
    if out:
        tls.sendall(out)

    if not (local_ack and remote_settings):
        print("handshake incomplete: local_ack=", local_ack, "remote_settings=", remote_settings)
        return 1

    ping_data = b"h2spec\x00\x00"
    conn.ping(ping_data)
    ping_out = conn.data_to_send()
    tls.sendall(ping_out)
    print("sent PING", ping_data)

    for _ in range(20):
        chunk = tls.recv(65536)
        if not chunk:
            print("recv EOF after PING")
            return 1
        print("recv after ping", len(chunk))
        events = conn.receive_data(chunk)
        for ev in events:
            print(" ", type(ev).__name__, ev)
            if type(ev).__name__ == "PingAckReceived":
                if ev.ping_data == ping_data:
                    print("OK: PING ACK payload matches")
                    return 0
                print("BAD: ping payload", ev.ping_data)
                return 1
        out = conn.data_to_send()
        if out:
            tls.sendall(out)

    print("no PING ACK seen")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
