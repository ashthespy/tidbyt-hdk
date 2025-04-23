#!/usr/bin/env python3
import sys
import os
import hashlib
import socket
import threading
import http.server
import socketserver
import requests

# ——— MD5 helper ———
def calc_md5(path, chunk_size=8192):
    md5 = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(chunk_size), b""):
            md5.update(chunk)
    return md5.hexdigest()

# ——— get local LAN IP ———
def get_local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # doesn't actually send packets
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    finally:
        s.close()

# ——— tiny HTTP server in a background thread ———
class _SilentHandler(http.server.SimpleHTTPRequestHandler):
    # suppress console logging
    def log_message(self, format, *args):
        pass

def start_http_server(directory):
    # serve 'directory' on an ephemeral port
    os.chdir(directory)
    httpd = socketserver.TCPServer(("", 0), _SilentHandler)
    port = httpd.server_address[1]
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    return httpd, port

# ——— OTA upload logic ———
def ota_upload(fw_path, esp_addr):
    fw_dir  = os.path.dirname(fw_path) or "."
    fw_file = os.path.basename(fw_path)

    # 1) MD5
    md5sum = calc_md5(fw_path)

    # 2) HTTP server
    httpd, host_port = start_http_server(fw_dir)

    # 3) discover IP
    host_ip = get_local_ip()

    # 4) build invite URL
    #    your esp_http_server GET handler expects these four params:
    #      MD5, host, port, path
    invite = (
        f"http://{esp_addr}/"
        f"?MD5={md5sum}"
        f"&host={host_ip}"
        f"&port={host_port}"
        f"&path=/{fw_file}"
    )
    print(f"→ OTA Invite: GET {invite}", flush=True)

    # 5) fire the GET and wait for the ESP’s response
    try:
        r = requests.get(invite, timeout=600)   # give it up to 10 minutes
        print(f"← HTTP {r.status_code}", flush=True)
        print(r.text.strip(), flush=True)
        code = 0 if r.status_code == 200 else 1
    except requests.exceptions.RequestException as e:
        print(f"Error: OTA invite failed: {e}", flush=True)
        code = 1

    # 6) clean up
    httpd.shutdown()
    sys.exit(code)

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: ota_upload.py <firmware.bin> <esp-ip[:port]>", file=sys.stderr)
        sys.exit(1)
    ota_upload(sys.argv[1], sys.argv[2])
