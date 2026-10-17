#!/usr/bin/env python3
"""Answers the Coldcard simulator's stories through its own key input.

The mechanism of bwk-hwi's `Approver` (hwi/tests/common/coldcard_sim.rs, phase 28):
a second, unencrypted link next to silent's encrypted session reads the story on
screen with `EXEC` and presses keys with `XKEY`. Starting encryption from this
client would replace silent's session key, so the client and every request are
unencrypted.

The simulator answers the last sender, so a request landing inside a multi-packet
upload of silent's link (enroll payload, PSBT) breaks both links: like the
`Approver`, the first request waits UPLOAD_GRACE.

Usage:
  coldcard_driver.py [--socket PATH] approve --timeout S
      press "y" on the first story shown, print its title and first line;
      exit 0 after the press, 1 when none showed within S seconds
  coldcard_driver.py [--socket PATH] dismiss
      press "x" if a story is still shown
  coldcard_driver.py [--socket PATH] story
      print the story shown, nothing when none
"""

import argparse
import sys
import time

from ckcc.client import ColdcardDevice
from ckcc.protocol import CCProtocolPacker

DEFAULT_SOCKET = "/tmp/ckcc-simulator.sock"
POLL_INTERVAL = 0.3
UPLOAD_GRACE = 1.0
REQUEST_TIMEOUT_MS = 5000
# ckcc-protocol's `cap_story`, short enough for one packet.
CAP_STORY = "RV.write('\\0'.join(sim_display.story or[]).encode())"


def connect(socket):
    time.sleep(UPLOAD_GRACE)
    return ColdcardDevice(sn=socket, encrypt=False)


def story(device):
    """(title, body) of the story shown, None when none."""
    raw = device.send_recv(
        b"EXEC" + CAP_STORY.encode(), timeout=REQUEST_TIMEOUT_MS, encrypt=False
    )
    if not raw:
        return None
    text = raw.decode(errors="replace") if isinstance(raw, bytes) else str(raw)
    title, _, body = text.partition("\0")
    return title, body


def press(device, key):
    device.send_recv(
        CCProtocolPacker.sim_keypress(key.encode()),
        timeout=REQUEST_TIMEOUT_MS,
        encrypt=False,
    )


def first_line(body):
    for line in body.splitlines():
        if line.strip():
            return line.strip()
    return ""


def approve(device, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        shown = story(device)
        if shown is not None:
            press(device, "y")
            title, body = shown
            print(f"approved: {title}: {first_line(body)}")
            return 0
        time.sleep(POLL_INTERVAL)
    print(f"no story within {timeout} s")
    return 1


def dismiss(device):
    shown = story(device)
    if shown is None:
        print("no story")
        return 0
    press(device, "x")
    title, body = shown
    print(f"dismissed: {title}: {first_line(body)}")
    return 0


def show(device):
    shown = story(device)
    if shown is not None:
        title, body = shown
        print(title)
        print(body)
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", default=DEFAULT_SOCKET)
    commands = parser.add_subparsers(dest="command", required=True)
    approve_parser = commands.add_parser("approve")
    approve_parser.add_argument("--timeout", type=float, required=True)
    commands.add_parser("dismiss")
    commands.add_parser("story")
    args = parser.parse_args()
    device = connect(args.socket)
    try:
        if args.command == "approve":
            return approve(device, args.timeout)
        if args.command == "dismiss":
            return dismiss(device)
        return show(device)
    finally:
        device.close()


if __name__ == "__main__":
    sys.exit(main())
