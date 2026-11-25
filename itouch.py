import argparse
import asyncio
import base64
import getpass
import hashlib
import math
import os
import re
import shlex
import socket
import sys
import threading
import time
from collections import deque
from contextlib import contextmanager
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PHONE_COMMAND = "/usr/local/bin/itouch"
DEFAULT_PACKAGE = ROOT / "dist" / "com.itouch.remote_1.1.1_iphoneos-arm.deb"
DEFAULT_TOOLS = (ROOT / "../../Games/ios/Clutch/_tools").resolve()
ORIENTATIONS = {
    "portrait": 1,
    "upside-down": 2,
    "landscape-home-right": 3,
    "landscape-home-left": 4,
}


def add_dependencies(tools):
    for folder in ("ios-python", "ssh-python"):
        path = tools / folder
        if path.is_dir():
            sys.path.insert(0, str(path))


async def select_usb(udid, port):
    from pymobiledevice3 import usbmux

    devices = [d for d in await usbmux.list_devices()
               if d.is_usb and (not udid or d.matches_udid(udid))]
    if not devices:
        raise RuntimeError("No matching USB iPhone. Check the cable and Apple Mobile Device Service.")
    if len(devices) > 1:
        raise RuntimeError("Multiple USB devices found. Use --udid to select the iPhone.")
    device = devices[0]
    try:
        connection = await asyncio.wait_for(device.connect(port), 8)
    except Exception as error:
        raise RuntimeError(f"Cannot open SSH port {port} over USB: {error}") from error
    connection.setblocking(True)
    return connection, device


async def usb_orientation(device):
    from pymobiledevice3.lockdown import create_using_usbmux
    from pymobiledevice3.services.springboard import SpringBoardServicesService

    phone = await create_using_usbmux(
        serial=device.serial, connection_type="USB", pair_timeout=8)
    try:
        async with SpringBoardServicesService(phone) as springboard:
            return int(await springboard.get_interface_orientation())
    finally:
        await phone.close()


def verify_host_key(paramiko, args, address, key):
    fingerprint = "SHA256:" + base64.b64encode(
        hashlib.sha256(key.asbytes()).digest()).decode("ascii").rstrip("=")
    if args.host_key:
        if args.host_key != fingerprint:
            raise RuntimeError(f"SSH host key mismatch for {address}. Received {fingerprint}.")
        return
    path = Path.home() / ".ssh" / "itouch_known_hosts"
    known = paramiko.HostKeys(str(path)) if path.is_file() else paramiko.HostKeys()
    saved = known.lookup(address)
    if saved is not None:
        if saved.get(key.get_name()) != key:
            raise RuntimeError(f"SSH host key changed for {address}. Received {fingerprint}.")
        return
    if not sys.stdin.isatty():
        raise RuntimeError(f"Unknown SSH host key for {address}. Verify {fingerprint} and use --host-key.")
    print(f"First SSH connection to {address}\n{key.get_name()} {fingerprint}", file=sys.stderr)
    if input("Trust this key for iTouch? [y/N] ").strip().lower() not in {"y", "yes"}:
        raise RuntimeError("SSH host key was not accepted.")
    path.parent.mkdir(parents=True, exist_ok=True)
    known.add(address, key.get_name(), key)
    known.save(str(path))


@contextmanager
def connect(args):
    import paramiko

    device = None
    if args.host:
        connection = socket.create_connection((args.host, args.port), timeout=8)
        address = f"[{args.host}]:{args.port}"
    else:
        connection, device = asyncio.run(select_usb(args.udid, args.port))
        address = f"[usb-{device.serial}]:{args.port}"
    with connection, paramiko.Transport(connection) as ssh:
        ssh.banner_timeout = 8
        ssh.auth_timeout = 15
        ssh.start_client(timeout=10)
        verify_host_key(paramiko, args, address, ssh.get_remote_server_key())
        if args.identity:
            try:
                key = paramiko.PKey.from_path(args.identity)
            except (paramiko.PasswordRequiredException, TypeError):
                key = paramiko.PKey.from_path(
                    args.identity, getpass.getpass("SSH private key passphrase: ").encode())
            ssh.auth_publickey(args.user, key)
        else:
            password = os.environ.get("ITOUCH_SSH_PASSWORD")
            if password is None:
                if not sys.stdin.isatty():
                    raise RuntimeError("Use --identity or ITOUCH_SSH_PASSWORD for noninteractive SSH.")
                password = getpass.getpass(f"{args.user} SSH password: ")
            ssh.auth_password(args.user, password)
        yield ssh, device


def run_remote(ssh, command, data=None):
    with ssh.open_session(timeout=10) as channel:
        channel.settimeout(20)
        channel.set_combine_stderr(True)
        channel.exec_command(command)
        if data is not None:
            channel.sendall(data)
        channel.shutdown_write()
        output = channel.makefile("rb").read().decode("utf-8", errors="replace")
        return channel.recv_exit_status(), output


def phone_call(ssh, command, data=None):
    status, output = run_remote(ssh, command, data)
    if status:
        raise RuntimeError(output.strip() or f"Phone command exited with status {status}.")
    if output:
        print(output, end="", flush=True)


def ssh_orientation(ssh):
    """Read the current iOS interface orientation over an authenticated SSH connection."""
    status, output = run_remote(ssh, PHONE_COMMAND + " orientation")
    value = output.strip()
    if status or value not in {"1", "2", "3", "4"}:
        raise RuntimeError(f"Could not read interface orientation over SSH: "
                           f"{value or f'exit status {status}'}. "
                           "For automatic orientation over Wi-Fi, install iTouch 1.1.1 or later.")
    return int(value)


def size(value):
    match = re.fullmatch(r"([1-9][0-9]{0,4})[xX]([1-9][0-9]{0,4})", value)
    if not match:
        raise argparse.ArgumentTypeError("use WIDTHxHEIGHT, e.g. 2208x1242")
    return tuple(map(int, match.groups()))


def parse_action(tokens, dimensions):
    if not tokens:
        raise ValueError("Enter ping, tap X Y, hold X Y MS, or swipe X1 Y1 X2 Y2 [MS].")
    action, *values = tokens
    lengths = {"ping": (0,), "tap": (2,), "hold": (3,), "swipe": (4, 5)}
    if action not in lengths or len(values) not in lengths[action]:
        raise ValueError("Expected ping, tap X Y, hold X Y MS, or swipe X1 Y1 X2 Y2 [MS].")
    count = 4 if action == "swipe" else min(2, len(values))
    points = []
    for i in range(0, count, 2):
        x, y = map(float, values[i:i + 2])
        if dimensions:
            w, h = dimensions
            if not (0 <= x <= w - 1 and 0 <= y <= h - 1):
                raise ValueError(f"Pixel coordinates must fit within {w}x{h}.")
            x, y = (x + 0.5) / w, (y + 0.5) / h
        elif not (0 <= x <= 1 and 0 <= y <= 1):
            raise ValueError("Use normalized coordinates from 0 to 1 or supply --size WIDTHxHEIGHT.")
        points.append((x, y))
    duration = None
    if action in ("hold", "swipe"):
        duration = int(values[count]) if len(values) > count else 350
        if not 30 <= duration <= 5000:
            raise ValueError("Duration must be an integer from 30 to 5000 ms.")
    return action, points, duration


def native_point(x, y, orientation):
    """Convert upright display coordinates to the digitizer's portrait axes."""
    return ((x, y), (1 - x, 1 - y), (1 - y, x), (y, 1 - x))[orientation - 1]


class TouchStream:
    """One live finger over SSH, with queued events and a held-touch heartbeat."""

    def __init__(self, ssh):
        self.ssh = ssh
        self.condition = threading.Condition()
        self.pending = deque()
        self.pressed = False
        self.sending = False
        self.closed = False
        self.error = None
        self.channel = None

    @staticmethod
    def reply(reader, expected):
        response = reader.readline(256).decode("utf-8", errors="replace").strip()
        if response != expected:
            raise RuntimeError(response or "The iTouch stream closed.")

    def __enter__(self):
        self.channel = self.ssh.open_session(timeout=10)
        try:
            self.channel.settimeout(8)
            self.channel.set_combine_stderr(True)
            self.channel.exec_command(PHONE_COMMAND + " stream")
            self.reader = self.channel.makefile("rb")
            self.reply(self.reader, "OK stream/1")
            self.thread = threading.Thread(target=self._send, name="iTouch stream", daemon=True)
            self.thread.start()
        except BaseException:
            self.channel.close()
            raise
        return self

    def __exit__(self, *exception):
        with self.condition:
            self.closed = True
            self.pending.clear()
            self.condition.notify_all()
        self.channel.close()
        self.thread.join(timeout=1)
        self.reader.close()

    def _send(self):
        held = False
        try:
            while True:
                with self.condition:
                    got_work = self.condition.wait_for(
                        lambda: self.pending or self.closed, timeout=0.75)
                    if self.closed:
                        return
                    if self.channel.closed:
                        raise RuntimeError("The iTouch stream closed.")
                    if got_work:
                        name, line = self.pending.popleft()
                    elif held:
                        name, line = "ping", b"ping\n"
                    else:
                        continue
                    self.sending = True
                self.channel.sendall(line)
                self.reply(self.reader, "OK")
                if name in ("down", "up"):
                    held = name == "down"
                with self.condition:
                    self.sending = False
                    self.condition.notify_all()
        except Exception as error:
            self.channel.close()
            with self.condition:
                self.error = error
                self.condition.notify_all()

    def _check(self):
        if self.error is not None:
            raise RuntimeError(f"iTouch stream: {self.error}") from self.error
        if self.closed or self.channel is None or self.channel.closed:
            raise RuntimeError("The iTouch stream is closed.")

    def _enqueue(self, name, x=None, y=None):
        with self.condition:
            self._check()
            if name == "up" and not self.pressed:
                return
            if (name == "down") == self.pressed:
                raise ValueError("A touch is already down." if self.pressed else "Call down first.")
            if name in ("down", "move"):
                if not all(math.isfinite(v) and 0 <= v <= 1 for v in (x, y)):
                    raise ValueError("Live touch coordinates must be between 0 and 1.")
                line = f"{name} {x:.9f} {y:.9f}\n".encode("ascii")
            else:
                line = b"up\n"
            if name == "move" and self.pending and self.pending[-1][0] == "move":
                self.pending[-1] = (name, line)
            else:
                self.pending.append((name, line))
            if name != "move":
                self.pressed = name == "down"
            self.condition.notify_all()

    def down(self, x, y):
        """Press at normalized native portrait coordinates."""
        self._enqueue("down", x, y)

    def move(self, x, y):
        """Move at the latest supplied coordinates while staying pressed."""
        self._enqueue("move", x, y)

    def up(self):
        """Lift at the last position."""
        self._enqueue("up")

    def flush(self, timeout=2):
        """Wait for pending input to be acknowledged. Keep this off the GUI thread."""
        with self.condition:
            done = self.condition.wait_for(
                lambda: self.error is not None or self.closed
                or (not self.pending and not self.sending), timeout=timeout)
            self._check()
            if not done:
                raise TimeoutError("Timed out waiting for the iTouch stream.")


def gesture(ssh, device, args, action):
    name, points, duration = action
    if points:
        if args.orientation == "auto":
            if device is None:
                orientation = ssh_orientation(ssh)
            else:
                try:
                    orientation = asyncio.run(asyncio.wait_for(usb_orientation(device), 12))
                except Exception as error:
                    raise RuntimeError(f"Could not read screen orientation: {error}. "
                                       "Unlock and trust the phone, or set --orientation.") from error
        else:
            orientation = ORIENTATIONS[args.orientation]
        if args.size and args.size[0] != args.size[1]:
            if (args.size[0] > args.size[1]) != (orientation in (3, 4)):
                raise ValueError("The current screen orientation does not match --size.")
    wire = [PHONE_COMMAND, name]
    for x, y in points:
        native = native_point(x, y, orientation)
        wire.extend(f"{coordinate:.9f}" for coordinate in native)
    if duration is not None:
        wire.append(str(duration))
    phone_call(ssh, " ".join(wire))


def install(ssh, path):
    package = path.read_bytes()
    if len(package) > 1024 * 1024 or not package.startswith(b"!<arch>\n"):
        raise ValueError("Expected an iTouch Debian package smaller than 1 MiB.")
    phone_call(ssh, """
set -e
umask 077
pkg=$(mktemp /var/tmp/itouch.XXXXXX)
trap 'rm -f "$pkg"' EXIT
cat > "$pkg"
dpkg -i "$pkg"
""", package)
    for _ in range(12):
        status, output = run_remote(ssh, PHONE_COMMAND + " ping")
        if status == 0 and output.strip() == "OK iTouch/1":
            print(output, end="")
            return
        time.sleep(0.25)
    raise RuntimeError(f"Installed package, but its service did not start. {output.strip()}")


def shell(ssh, device, args):
    interactive = sys.stdin.isatty()
    while True:
        if interactive:
            print("itouch> ", end="", flush=True)
        line = sys.stdin.readline()
        if not line or line.strip() in {"exit", "quit"}:
            return
        if not line.strip():
            continue
        try:
            gesture(ssh, device, args, parse_action(shlex.split(line), args.size))
        except (RuntimeError, ValueError) as error:
            if not interactive:
                raise
            print(f"iTouch: {error}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description="Control an arm64 iOS 10 touchscreen over SSH.")
    parser.add_argument("--tools", type=Path,
                        default=Path(os.environ.get("ITOUCH_TOOLS", DEFAULT_TOOLS)))
    target = parser.add_mutually_exclusive_group()
    target.add_argument("--host", help="SSH host over TCP. USB is used by default.")
    target.add_argument("--udid", help="Select a specific USB iPhone.")
    parser.add_argument("--port", type=int, default=2222, help="Phone SSH port (default: 2222).")
    parser.add_argument("--user", default="root")
    parser.add_argument("--identity", type=Path, help="SSH private key.")
    parser.add_argument("--host-key", help="Verified SSH host key fingerprint in SHA256: format.")
    parser.add_argument("--orientation", choices=("auto", *ORIENTATIONS), default="auto")
    parser.add_argument("--size", type=size, help="Use screenshot pixels at WIDTHxHEIGHT.")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("ping")
    sub.add_parser("orientation", help="Read the phone's interface orientation over SSH.")
    tap = sub.add_parser("tap")
    hold = sub.add_parser("hold")
    for command in (tap, hold):
        command.add_argument("x")
        command.add_argument("y")
    hold.add_argument("ms")
    swipe = sub.add_parser("swipe")
    for coordinate in ("x1", "y1", "x2", "y2"):
        swipe.add_argument(coordinate)
    swipe.add_argument("ms", nargs="?", default="350")
    sub.add_parser("shell", help="Read one gesture per line over a single SSH connection.")
    installer = sub.add_parser("install")
    installer.add_argument("package", nargs="?", type=Path, default=DEFAULT_PACKAGE)
    sub.add_parser("uninstall")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be in the range 1 through 65535")
    add_dependencies(args.tools)

    try:
        if args.command in ("ping", "tap", "hold", "swipe"):
            values = [getattr(args, name) for name in ("x", "y", "x1", "y1", "x2", "y2", "ms")
                      if hasattr(args, name)]
            action = parse_action([args.command, *values], args.size)
        with connect(args) as (ssh, device):
            if args.command == "install":
                install(ssh, args.package)
            elif args.command == "uninstall":
                phone_call(ssh, "dpkg -P com.itouch.remote")
            elif args.command == "shell":
                shell(ssh, device, args)
            elif args.command == "orientation":
                print(ssh_orientation(ssh))
            else:
                gesture(ssh, device, args, action)
    except KeyboardInterrupt:
        return 130
    except Exception as error:
        print(f"iTouch: {error or type(error).__name__}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
