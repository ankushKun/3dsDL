#!/usr/bin/env python3
"""HTTP and HTTPS fixture server for the download engine integration tests.

Bodies are deterministic so the tests can verify every byte: byte i of a file
is pattern_byte(i). The main endpoint is /file/<size>, whose behaviour is
controlled by query parameters:

  cd=<value>        send this Content-Disposition header
  etag=stable|changing|weak|none   ETag behaviour (default stable)
  lm=0              omit Last-Modified
  ranges=0          ignore Range requests and say Accept-Ranges: none
  range_ignore=1    say Accept-Ranges: bytes but ignore Range requests anyway
  refuse_ranged=<n> answer the first n ranged requests for key with 503
  drop_at=<n>       first request for key: close the connection after n bytes
  stall_at=<n>      first request for key: stop sending after n bytes
  slow=1            send the body slowly
  key=<name>        identifies a test for drop_at / stall_at / etag=changing

Other endpoints: /chunked/<size>, /redirect/<count>, /redirect-ftp,
/status/<code>, /empty, /huge, /release/<tag> (a GitHub-style release) and
/headers (the request's headers as JSON).

It also runs three fake NTP servers on UDP: "good" answers with
NTP_FIXED_UNIX, "unsynced" claims stratum 0, and "mismatched" answers with
the wrong originate timestamp.
"""

import argparse
import json
import os
import socket
import ssl
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

LAST_MODIFIED = "Wed, 21 Oct 2015 07:28:00 GMT"
NTP_FIXED_UNIX = 1791784800  # 2026-10-12 06:00:00 UTC
NTP_TO_UNIX = 2208988800

_state_lock = threading.Lock()
_first_request_done = set()
_request_counts = {}


def pattern_byte(index):
    return (index * 7 + index // 256) % 251


def pattern(start, end):
    return bytes(pattern_byte(i) for i in range(start, end))


def first_request(key):
    with _state_lock:
        if key in _first_request_done:
            return False
        _first_request_done.add(key)
        return True


def next_count(key):
    with _state_lock:
        _request_counts[key] = _request_counts.get(key, 0) + 1
        return _request_counts[key]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def send_body(self, start, end, slow=False, limit=None, stall=False):
        position = start
        chunk = 4096 if not slow else 1024
        while position < end:
            if limit is not None and position - start >= limit:
                if stall:
                    time.sleep(30)
                return False
            stop = min(end, position + chunk)
            if limit is not None:
                stop = min(stop, start + limit)
            try:
                self.wfile.write(pattern(position, stop))
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                return False
            position = stop
            if slow:
                time.sleep(0.02)
        return True

    def do_GET(self):
        parsed = urlparse(self.path)
        query = {k: v[-1] for k, v in parse_qs(parsed.query).items()}
        parts = [p for p in parsed.path.split("/") if p]

        if parts[:1] == ["file"] and len(parts) == 2:
            return self.file(int(parts[1]), query)
        if parts[:1] == ["chunked"] and len(parts) == 2:
            return self.chunked(int(parts[1]))
        if parts[:1] == ["redirect"] and len(parts) == 2:
            count = int(parts[1])
            target = "/file/1000" if count <= 1 else "/redirect/%d" % (count - 1)
            return self.redirect(target)
        if parts == ["redirect-ftp"]:
            return self.redirect("ftp://127.0.0.1/file.bin")
        if parts == ["redirect-host"]:
            port = self.server.server_address[1]
            return self.redirect("http://localhost:%d/file/1000" % port)
        if parts[:1] == ["status"] and len(parts) == 2:
            return self.status(int(parts[1]))
        if parts == ["empty"]:
            self.send_response(200)
            self.disposition(query)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if parts[:1] == ["release"] and len(parts) == 2:
            return self.release(parts[1])
        if parts == ["headers"]:
            return self.json_body({k.lower(): v for k, v in self.headers.items()})
        if parts == ["huge"]:
            self.send_response(200)
            self.send_header("Content-Length", str(5 * 1024 ** 3))
            self.end_headers()
            self.close_connection = True
            return
        self.status(404)

    def disposition(self, query):
        # Line breaks would let a request add headers of its own.
        if "cd" in query:
            value = query["cd"].replace("\r", "").replace("\n", "")
            self.send_header("Content-Disposition", value)

    def json_body(self, value):
        body = json.dumps(value).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def release(self, tag):
        # Shaped like GitHub's releases/latest answer, with assets served
        # from /file/ so an update can be downloaded end to end.
        base = "http://%s:%d" % self.server.server_address[:2]
        self.json_body({
            "tag_name": tag,
            "name": "3dsDL " + tag,
            "html_url": base + "/release/" + tag,
            "published_at": "2026-10-20T08:15:00Z",
            "body": "## What's new\n\n- Updates.",
            "assets": [
                {"name": "3dsDL.cia", "size": 4000,
                 "browser_download_url": base + "/file/4000"},
            ],
        })

    def redirect(self, location):
        self.send_response(302)
        self.send_header("Location", location)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def status(self, code):
        body = b"error page that must never be saved\n"
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def chunked(self, size):
        self.send_response(200)
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        position = 0
        while position < size:
            stop = min(size, position + 1000)
            data = pattern(position, stop)
            self.wfile.write(b"%x\r\n" % len(data) + data + b"\r\n")
            position = stop
        self.wfile.write(b"0\r\n\r\n")

    def file(self, size, query):
        key = query.get("key", self.path)
        etag_mode = query.get("etag", "stable")
        ranges = query.get("ranges", "1") != "0"

        if etag_mode == "stable":
            etag = '"file-%d"' % size
        elif etag_mode == "changing":
            etag = '"v%d"' % next_count(key)
        elif etag_mode == "weak":
            etag = 'W/"weak-%d"' % size
        else:
            etag = None

        start = 0
        partial = False
        requested = self.headers.get("Range")
        if (requested and "refuse_ranged" in query
                and next_count(key + "#ranged") <= int(query["refuse_ranged"])):
            return self.status(503)
        honour = ranges and query.get("range_ignore") != "1"
        if honour and requested and requested.startswith("bytes="):
            if_range = self.headers.get("If-Range")
            validator_ok = (
                if_range is None
                or (etag is not None and if_range == etag)
                or (query.get("lm", "1") != "0" and if_range == LAST_MODIFIED)
            )
            if validator_ok:
                first = int(requested[6:].split("-")[0])
                if first >= size:
                    self.send_response(416)
                    self.send_header("Content-Range", "bytes */%d" % size)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                start = first
                partial = True

        self.send_response(206 if partial else 200)
        self.send_header("Content-Length", str(size - start))
        self.send_header("Accept-Ranges", "bytes" if ranges else "none")
        if partial:
            self.send_header("Content-Range",
                             "bytes %d-%d/%d" % (start, size - 1, size))
        if etag is not None:
            self.send_header("ETag", etag)
        if query.get("lm", "1") != "0":
            self.send_header("Last-Modified", LAST_MODIFIED)
        self.disposition(query)
        self.end_headers()

        limit = None
        stall = False
        if "drop_at" in query and first_request(key):
            limit = int(query["drop_at"])
        elif "stall_at" in query and first_request(key):
            limit = int(query["stall_at"])
            stall = True

        complete = self.send_body(start, size, slow=query.get("slow") == "1",
                                  limit=limit, stall=stall)
        if not complete:
            self.close_connection = True


class QuietServer(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        # Split downloads hang up on purpose once a piece is full.
        if isinstance(sys.exc_info()[1], (ConnectionError, ssl.SSLError)):
            return
        super().handle_error(request, client_address)


def serve(server):
    server.serve_forever(poll_interval=0.1)


def serve_ntp(sock, behaviour):
    while True:
        request, address = sock.recvfrom(512)
        if len(request) < 48:
            continue
        origin = request[40:48]
        stratum = 0 if behaviour == "unsynced" else 2
        if behaviour == "mismatched":
            origin = bytes(8)
        seconds = NTP_FIXED_UNIX + NTP_TO_UNIX
        transmit = struct.pack(">II", seconds, 0x80000000)  # +0.5 s
        # header, root delay, root dispersion, reference ID and reference
        # timestamp (20 bytes), then originate, receive and transmit.
        reply = (bytes([(0 << 6) | (4 << 3) | 4, stratum, 6, 0xEC])
                 + bytes(20) + origin + transmit + transmit)
        sock.sendto(reply, address)


def start_ntp(behaviour):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    threading.Thread(target=serve_ntp, args=(sock, behaviour),
                     daemon=True).start()
    return sock.getsockname()[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--ready", required=True,
                        help="file to write the chosen ports to")
    args = parser.parse_args()

    http_server = QuietServer(("127.0.0.1", 0), Handler)
    https_server = QuietServer(("127.0.0.1", 0), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(args.cert, args.key)
    https_server.socket = context.wrap_socket(https_server.socket,
                                              server_side=True)

    for server in (http_server, https_server):
        threading.Thread(target=serve, args=(server,), daemon=True).start()

    ports = {"http": http_server.server_address[1],
             "https": https_server.server_address[1],
             "ntp_good": start_ntp("good"),
             "ntp_unsynced": start_ntp("unsynced"),
             "ntp_mismatched": start_ntp("mismatched")}
    temporary = args.ready + ".tmp"
    with open(temporary, "w") as handle:
        json.dump(ports, handle)
    os.replace(temporary, args.ready)

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
