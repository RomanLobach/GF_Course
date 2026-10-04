#!/usr/bin/env python3
"""Local OTA server for development: serves manifest.json + images over plain HTTP.

The manifest has the same shape as a release one (scripts/make_manifest.py), built from the
local `pio run` outputs, so a device can be updated without tagging a release:

    pio run -e base -e rover
    python scripts/ota_server.py --version 0.3.1
    # on the device:  config set ota_url http://<this PC>:8000/manifest.json
    #                 ota update            (or --force for the same/older version)

Rollback demo: build an image whose self-test always fails and serve it to one role:

    PLATFORMIO_BUILD_FLAGS="-D FW_FORCE_POST_FAIL" pio run -e rover
    cp .pio/build/rover/firmware.bin /tmp/rover-fail.bin && pio run -e rover
    python scripts/ota_server.py --version 9.9.9 --image rover=/tmp/rover-fail.bin
"""
import argparse
import hashlib
import http.server
import json
import os
import socket

PROJECT = "ttgo-lora-bench"
ROLES = ("base", "rover")


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))  # no packet is sent; picks the outgoing interface
        return s.getsockname()[0]
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--version", default="9.9.9", help="version announced in the manifest")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--image", action="append", default=[], metavar="ROLE=PATH",
                    help="serve this file for ROLE instead of .pio/build/ROLE/firmware.bin")
    args = ap.parse_args()

    images = {role: os.path.join(".pio", "build", role, "firmware.bin") for role in ROLES}
    for item in args.image:
        role, path = item.split("=", 1)
        if role not in ROLES:
            raise SystemExit("unknown role %r" % role)
        images[role] = path

    host = lan_ip()
    files = {}
    manifest = {"project": PROJECT, "version": args.version, "tag": "local", "images": {}}
    for role, path in images.items():
        if not os.path.exists(path):
            print("skip %s: %s not found" % (role, path))
            continue
        with open(path, "rb") as f:
            data = f.read()
        name = "%s-%s-%s.bin" % (PROJECT, role, args.version)
        files["/" + name] = data
        manifest["images"][role] = {
            "url": "http://%s:%d/%s" % (host, args.port, name),
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        }
    files["/manifest.json"] = (json.dumps(manifest, indent=2) + "\n").encode()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            body = files.get(self.path)
            if body is None:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    print(json.dumps(manifest, indent=2))
    print("\non the device:  config set ota_url http://%s:%d/manifest.json" % (host, args.port))
    http.server.ThreadingHTTPServer(("", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
