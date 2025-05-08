#!/usr/bin/env python3

import os
import json
import click

Import("env")


def main() -> None:
    board_name = env["PIOENV"]

    # Load secrets
    if os.path.exists("secrets.json"):
        with open("secrets.json", "r") as f:
            config = json.load(f)
            secrets = config.get("default", {}).copy()
            click.secho(
                f"Checking secrets for board: {board_name}", fg="blue", bold=True
            )
            secrets.update(config.get(board_name, {}))
    else:
        click.secho(
            "secrets.json not found. Using hardcoded placeholders.",
            fg="yellow",
            bold=True,
        )
        secrets = {
            "WIFI_SSID": "MyWiFi",
            "WIFI_PASSWORD": "sharedpass",
            "REMOTE_URL": "https://raw.githubusercontent.com/tronbyt/apps/refs/heads/main/apps/coffee/coffee.webp",
            "DEFAULT_TIMEZONE": "UTC",
            "DEFAULT_BRIGHTNESS": 30,
        }

    # Format and apply build flags
    click.secho("Applied build flags:", fg="cyan", bold=True)
    defines = []
    for key, value in secrets.items():
        define = (
            f"-D{key}={env.StringifyMacro(value)}"
            if isinstance(value, str)
            else f"-D{key}={value}"
        )
        defines.append(define)
        click.secho(f"  {define}", fg="green")
    
    print()
    env.Append(CCFLAGS=defines)


main()
