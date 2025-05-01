#!/usr/bin/env python3
import os
import sys
import socket
import hashlib
import time
import signal
import requests
import threading
import json

from tqdm import tqdm

from http.server import SimpleHTTPRequestHandler
from socketserver import ThreadingMixIn, TCPServer


# ——— HTTP server that serves our .bin and logs progress ⌛s
class ThreadedTCPServer(ThreadingMixIn, TCPServer):
    allow_reuse_address = True
    daemon_threads = True


class OTARequestHandler(SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        # suppress default logging
        pass

    def copyfile(self, source, outputfile):
        """Stream the file to the client, with a tqdm progress bar."""
        filesize = os.fstat(source.fileno()).st_size
        with tqdm(
            total=filesize,
            unit="B",
            unit_scale=True,
            desc="[Server] Uploading",
            ncols=60,
            leave=True,
        ) as bar:
            while True:
                chunk = source.read(64 * 1024)
                if not chunk:
                    break
                outputfile.write(chunk)
                bar.update(len(chunk))


def get_local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    finally:
        s.close()


def calc_md5(path, chunk_size=8192):
    md5 = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(chunk_size), b""):
            md5.update(chunk)
    return md5.hexdigest()


# ——— read version from version.txt ———
def get_version(version_file):
    try:
        with open(version_file, "r") as vf:
            version = vf.read().strip()
    except Exception as e:
        print(
            f"Warning: could not read version from {version_file}: {e}", file=sys.stderr
        )
        version = "0.0.0"
    return version


def poll_ota_status(esp_addr, timeout=300):
    url = f"http://{esp_addr}/ota/status"
    deadline = time.time() + timeout
    last_status = None
    last_progress = -1

    while time.time() < deadline:
        try:
            r = requests.get(url, timeout=5)
            r.raise_for_status()
            data = r.json()

            status = data.get("status", "UNKNOWN").upper()
            progress = data.get("progress", 0)

            if status != last_status or progress != last_progress:
                print(f"📶 OTA status: {status} ({progress}%)", flush=True)
                last_status = status
                last_progress = progress

            if status in ("OTA_SUCCESS", "SUCCESS", "IDLE"):
                print("✅ OTA completed successfully!", flush=True)
                return 0
            if status in ("OTA_FAILED", "FAILED"):
                print("❌ OTA failed.", flush=True)
                return 1

        except requests.RequestException as e:
            print(f"⚠️  Polling error: {e}", flush=True)
        except json.JSONDecodeError as e:
            print(f"⚠️  JSON decode error: {e}", flush=True)

        time.sleep(10)

    print("⏳ OTA status polling timed out.", flush=True)
    return 1

def start_http_server(directory):
    os.chdir(directory)
    server = ThreadedTCPServer(("", 0), OTARequestHandler)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, port


def main():
    # catch Ctrl-C
    signal.signal(signal.SIGINT, lambda s, f: sys.exit(1))

    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <firmware.bin> <esp-ip[:port]>", file=sys.stderr)
        sys.exit(1)

    fw_path, esp_addr = sys.argv[1], sys.argv[2]
    if not os.path.isfile(fw_path):
        print(f"Error: firmware file '{fw_path}' not found.", file=sys.stderr)
        sys.exit(1)

    # compute MD5 and prepare URL
    md5sum = calc_md5(fw_path)
    fw_dir = os.path.dirname(fw_path) or "."
    fw_file = os.path.basename(fw_path)
    version = get_version(os.path.join(os.getcwd(), "version.txt"))

    server = None
    exit_code = 1
    try:
        # Start file‐server
        server, port = start_http_server(fw_dir)
        host_ip = get_local_ip()

        # Invite ESP to pull it
        ota_body = {
            "MD5": md5sum,
            "url": f"http://{host_ip}:{port}/{fw_file}",
            "version": version,
        }
        ota_url = f"http://{esp_addr}/ota"
        print(f"→ OTA Invite: POST {ota_url} with body {ota_body}", flush=True)

        try:
            r = requests.post(ota_url, json=ota_body, timeout=600)
            print(f"\n← HTTP {r.status_code}", flush=True)
            print(r.text.strip(), flush=True)
            if r.status_code != 200:
                return 1
        except requests.RequestException as e:
            print(f"\nError: OTA request failed: {e}", file=sys.stderr, flush=True)
            return 1

        # Poll for final status
        exit_code = poll_ota_status(esp_addr)

    finally:
        if server:
            print("Shutting down HTTP server...", flush=True)
            server.shutdown()

    sys.exit(exit_code)


if __name__ == "__main__":
    main()
