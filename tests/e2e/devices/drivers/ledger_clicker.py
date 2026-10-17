#!/usr/bin/env python3
"""Approves every Ledger review on Speculos through its REST API.

Mirrors bwk-hwi's hwi/tests/common/speculos_clicker.rs (`Clicker`): every 0.3 s it
reads the current screen; a screen with an approve word (or the address check's last
screen) and no "Reject" is approved with both buttons, any other screen but the idle
one is walked right. Every distinct screen text is appended to the log, one line each
with a timestamp. Stdlib only; exits on SIGTERM.

Usage: ledger_clicker.py --api http://127.0.0.1:5000 --log <file>
"""

import argparse
import json
import signal
import sys
import time
import urllib.request

POLL_INTERVAL = 0.3
# The words of speculos_clicker.rs: "Sign transaction" is app 2.4.6's last review
# screen of a signature.
APPROVE_WORDS = ("Approve", "Accept", "Register account", "Sign transaction")
# Screens approved only when they read exactly so: the last screen of an address check
# on 2.4.6 is "Confirm", while "Confirm public key" opens an xpub export. Not in
# speculos_clicker.rs, whose tests never show an address.
APPROVE_SCREENS = ("Confirm",)
REQUEST_TIMEOUT = 5


def current_screen(api):
    """The texts of the current screen joined with " | ", None on any error."""
    try:
        with urllib.request.urlopen(
            api + "/events?currentscreenonly=true", timeout=REQUEST_TIMEOUT
        ) as answer:
            events = json.load(answer)
    except (OSError, ValueError):
        return None
    return " | ".join(
        event["text"] for event in events.get("events", []) if "text" in event
    )


def press(api, button):
    request = urllib.request.Request(
        api + "/button/" + button,
        data=b'{"action":"press-and-release"}',
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        urllib.request.urlopen(request, timeout=REQUEST_TIMEOUT).close()
    except OSError:
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--api", required=True)
    parser.add_argument("--log", required=True)
    args = parser.parse_args()
    api = args.api.rstrip("/")
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    last = None
    with open(args.log, "a", encoding="utf-8") as log:
        while True:
            text = current_screen(api)
            if text is not None:
                if text and text != last:
                    stamp = time.strftime("%H:%M:%S")
                    log.write(f"{stamp} {text}\n")
                    log.flush()
                    last = text
                approve = any(word in text for word in APPROVE_WORDS) or text in APPROVE_SCREENS
                if approve and "Reject" not in text:
                    press(api, "both")
                elif text and "ready" not in text.lower() and "Bitcoin Test" not in text:
                    press(api, "right")
            time.sleep(POLL_INTERVAL)


if __name__ == "__main__":
    main()
