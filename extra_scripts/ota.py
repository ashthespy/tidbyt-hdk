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
import click  # already used in pio
from tqdm import tqdm
from http.server import SimpleHTTPRequestHandler
from socketserver import ThreadingMixIn, TCPServer


class ThreadedTCPServer(ThreadingMixIn, TCPServer):
    allow_reuse_address = True
    daemon_threads = True


class OTARequestHandler(SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def copyfile(self, source, outputfile):
        filesize = os.fstat(source.fileno()).st_size
        with tqdm(
            total=filesize,
            unit="B",
            unit_scale=True,
            desc="[OTA]",
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


def get_version(version_file):
    try:
        with open(version_file, "r") as vf:
            return vf.read().strip()
    except Exception as e:
        click.secho(
            f"Warning: could not read version from {version_file}: {e}", fg="yellow"
        )
        return "0.0.0"


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
                # print(f"📶 OTA status: {status} ({progress}%)", flush=True)
                last_status = status
                last_progress = progress

            if status in ("OTA_SUCCESS", "SUCCESS", "IDLE"):
                click.secho("OTA completed successfully.", fg="green", bold=True)
                return 0
            if status in ("OTA_FAILED", "FAILED"):
                click.secho("OTA failed.", fg="red", bold=True)
                return 1

        except requests.RequestException as e:
            tqdm.write(f"{' ' * 75}| Error: {e}")
        except json.JSONDecodeError as e:
            tqdm.write(f"{' ' * 75}| JSON decode error: {e}")

        time.sleep(5)

    click.secho("OTA status polling timed out.", fg="red")
    return 1


def start_http_server(directory):
    os.chdir(directory)
    server = ThreadedTCPServer(("", 0), OTARequestHandler)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, port


@click.command()
@click.argument("firmware_path")
@click.argument("esp_address")
def main(firmware_path, esp_address):
    signal.signal(signal.SIGINT, lambda s, f: sys.exit(1))

    if not os.path.isfile(firmware_path):
        click.secho(
            f"Error: firmware file '{firmware_path}' not found.", fg="red", bold=True
        )
        sys.exit(1)

    md5sum = calc_md5(firmware_path)
    fw_dir = os.path.dirname(firmware_path) or "."
    fw_file = os.path.basename(firmware_path)
    version = get_version(os.path.join(os.getcwd(), "version.txt"))

    server = None
    exit_code = 1
    try:
        server, port = start_http_server(fw_dir)
        host_ip = get_local_ip()

        ota_body = {
            "MD5": md5sum,
            "url": f"http://{host_ip}:{port}/{fw_file}",
            "version": version,
        }
        ota_url = f"http://{esp_address}/ota"

        click.secho(f"→ OTA Invite: POST {ota_url}", fg="blue", bold=True)
        click.secho(f"   Body: {json.dumps(ota_body)}", fg="white")

        try:
            r = requests.post(ota_url, json=ota_body, timeout=600)
            click.secho(f"\n← HTTP {r.status_code}", fg="magenta")
            click.secho(r.text.strip(), fg="magenta")
            if r.status_code != 200:
                return 1
        except requests.RequestException as e:
            click.secho(f"\nOTA request failed: {e}", fg="red", bold=True)
            return 1

        exit_code = poll_ota_status(esp_address)

    finally:
        if server:
            click.secho("Shutting down HTTP server...", fg="white", dim=True)
            server.shutdown()

    sys.exit(exit_code)


if __name__ == "__main__":
    main()
