#!/usr/bin/env python3
"""MDV cartridge / SD file manager for the MicroPicoDrive (mainline, Pico 2 W).

Command-line client for the Connect-mode JSON protocol served by
src/ble/ota_ble.c (process_command + the section-5b push machinery). Works on
Windows, Linux and macOS. The device must be in System Tools -> Connect;
first-time use from a PC needs 'pair' while the device shows Pair Device.

Requires: Python 3.10+ and `pip install bleak`.

Commands:
  scan                       list advertising MicroPicoDrive (MPD-*) devices
  pair      [-d NAME]        walk through OS pairing while device is in Pair mode
  info      [-d NAME]        device info (name, unit id, fw, SD status)
  health    [-d NAME]        uptime / temperature / heap / fw snapshot
  identify  [-d NAME]        flash the device screen for 3 s
  ls        [PATH]           list a directory (default "/")
  put LOCAL [REMOTE]         upload a file to the SD card (default: /<basename>)
  get REMOTE [LOCAL]         download a file (default: ./<basename>)
  rm PATH                    delete a file (its .thumb sidecar rides along)
  mv FROM TO                 rename/move a file (sidecar rides along)
  label PATH                 read an MDV cartridge's medium name
  setlabel PATH LABEL        rewrite the medium name (1-10 printable chars)

Wire protocol (keep in sync with ota_ble.c -- see the note there):
  CONTROL   raw JSON {"op":..,"reqId":N,..} written with response
  STATUS    notify frames [reqId][flags][seq16 LE][payload]; flags bit0 = last,
            bit1 = error ({"error":"CODE"}); 'read' streams raw bytes this way
  DATA      push frames [reqId][flags bit0=LAST][seq16 LE][payload<=240] written
            without response during a wbegin session; the device wacks progress
            on STATUS as {"nextOffset":N} (every 16 KB, on LAST, and after a
            150 ms gap-idle resync that adopts the next seq it sees)

Exit code 0 only on verified success.
"""
import argparse
import asyncio
import hashlib
import json
import platform
import sys
import time
from pathlib import Path

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("bleak is not installed - run: pip install bleak")

UUID_SERVICE = "7a0b0001-d5a9-4b7c-9f2a-5b1e6f0c4d10"
UUID_INFO    = "7a0b0002-d5a9-4b7c-9f2a-5b1e6f0c4d10"
UUID_CONTROL = "7a0b0003-d5a9-4b7c-9f2a-5b1e6f0c4d10"
UUID_DATA    = "7a0b0004-d5a9-4b7c-9f2a-5b1e6f0c4d10"
UUID_STATUS  = "7a0b0005-d5a9-4b7c-9f2a-5b1e6f0c4d10"

FLAG_LAST, FLAG_ERROR = 0x01, 0x02
PUSH_PAYLOAD_MAX = 240      # ota_ble.c caps at 512; 240 stays in one ATT fragment
PUSH_AHEAD_MAX = 32768      # bytes in flight past the last wack before pausing
WACK_FINAL_TIMEOUT = 6.0    # seconds to wait for the post-LAST wack per attempt
PUT_MAX_ATTEMPTS = 8        # gap-resume rewinds before giving up

ERR_HINTS = {
    "EBUSY": "eject the cartridge / stop QL access first",
    "ENOSPC": "SD card is full",
    "ECHECKSUM": "transfer corrupted - retry the upload",
    "ESTATE": "no write session open (wbegin missing or timed out)",
    "ENOENT": "no such file",
    "EEXIST": "destination already exists",
    "EISDIR": "path is a directory",
    "EINVAL": "not a valid MDV image / label (1-10 printable chars)",
    "ENOSYS": "firmware too old for this command",
}

NAME_PREFIXES = ("MPD-", "UIExt-")


def _is_ours(dev, adv) -> bool:
    if adv and UUID_SERVICE in (adv.service_uuids or []):
        return True
    return bool(dev.name) and dev.name.startswith(NAME_PREFIXES)


async def find_device(name: str | None, timeout: float = 10.0):
    print(f"Scanning{' for ' + name if name else ''}...")
    found = await BleakScanner.discover(timeout=timeout, return_adv=True)
    matches = [dev for dev, adv in found.values()
               if (dev.name == name if name else _is_ours(dev, adv))]
    if not matches:
        return None
    if len(matches) > 1 and not name:
        print("Multiple devices found, pass -d NAME:")
        for d in matches:
            print(f"  {d.name}  {d.address}")
        return None
    return matches[0]


class Session:
    """One connected Connect-mode session: STATUS demux + JSON commands."""

    def __init__(self, client: BleakClient):
        self.client = client
        self.queue: asyncio.Queue[bytes] = asyncio.Queue()
        self._req_id = 0

    def next_req_id(self) -> int:
        self._req_id = self._req_id % 255 + 1
        return self._req_id

    def on_status(self, _h, data: bytearray):
        self.queue.put_nowait(bytes(data))

    async def start(self):
        await self.client.start_notify(UUID_STATUS, self.on_status)

    async def frame(self, req_id: int, timeout: float) -> bytes:
        """Next STATUS frame for req_id (other req_ids are dropped)."""
        t_end = time.monotonic() + timeout
        while True:
            remaining = t_end - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"no response to reqId {req_id} within {timeout}s")
            frame = await asyncio.wait_for(self.queue.get(), remaining)
            if len(frame) >= 4 and frame[0] == req_id:
                return frame

    async def cmd(self, obj: dict, timeout: float = 10.0) -> dict:
        """Send one JSON command, return the parsed response; raises on error."""
        req_id = self.next_req_id()
        obj = {**obj, "reqId": req_id}
        payload = bytearray()
        await self.client.write_gatt_char(UUID_CONTROL, json.dumps(obj).encode(),
                                          response=True)
        while True:
            frame = await self.frame(req_id, timeout)
            payload += frame[4:]
            if frame[1] & FLAG_LAST:
                resp = json.loads(payload.decode())
                if frame[1] & FLAG_ERROR or "error" in resp:
                    code = resp.get("error", "?")
                    hint = ERR_HINTS.get(code, "")
                    raise RuntimeError(f"device refused: {code}"
                                       + (f" ({hint})" if hint else ""))
                return resp


async def open_session(args) -> tuple[BleakClient, Session] | None:
    dev = await find_device(args.device)
    if not dev:
        print("Device not found. Put it in System Tools -> Connect first.")
        return None
    print(f"Connecting to {dev.name}...")
    client = BleakClient(dev)
    await client.connect()
    session = Session(client)
    await session.start()
    return client, session


def payload_max(client: BleakClient) -> int:
    try:
        mtu = client.mtu_size or 23
    except Exception:
        mtu = 23
    return max(20, min(PUSH_PAYLOAD_MAX, mtu - 3 - 4))


# ── commands ──────────────────────────────────────────────────────────────────

async def cmd_scan(_args):
    found = await BleakScanner.discover(timeout=10.0, return_adv=True)
    hits = False
    for dev, adv in found.values():
        if _is_ours(dev, adv):
            print(f"{dev.name}  {dev.address}")
            hits = True
    if not hits:
        print("No MicroPicoDrive found. Is it in 'Connect' or 'Pair Device' mode?")
        return 1
    return 0


async def cmd_pair(args):
    dev = await find_device(args.device)
    if not dev:
        print("Device not found. Put it in System Tools -> Pair Device first.")
        return 1
    print(f"Found {dev.name} ({dev.address}). Connecting...")
    async with BleakClient(dev) as client:
        os_name = platform.system()
        if os_name == "Windows":
            try:
                await client.pair(protection_level=2)
            except Exception:
                pass  # some backends pair implicitly below
        elif os_name == "Linux":
            print("If pairing stalls, run 'bluetoothctl' in another terminal so an")
            print("agent can answer the passkey prompt.")
            try:
                await client.pair()
            except Exception:
                pass
        # On macOS (and as a fallback everywhere) pairing is triggered by
        # touching an encrypted characteristic - the OS shows its own dialog.
        print("Reading a protected characteristic to trigger/verify pairing...")
        print("Type the 6-digit passkey shown on the DEVICE DISPLAY when prompted.")
        try:
            raw = await client.read_gatt_char(UUID_INFO)
        except Exception as e:
            print(f"Pairing not established ({e}).")
            print("Windows fallback: Settings > Bluetooth > Add device, while the")
            print("device shows the Pairing screen, then re-run this command.")
            return 1
        print(f"Paired. Device: {raw.decode(errors='replace')}")
        return 0


async def cmd_info(args):
    opened = await open_session(args)
    if not opened:
        return 1
    client, session = opened
    async with client:
        print(json.dumps(await session.cmd({"op": "info"}), indent=2))
    return 0


async def cmd_health(args):
    opened = await open_session(args)
    if not opened:
        return 1
    client, session = opened
    async with client:
        print(json.dumps(await session.cmd({"op": "health"}), indent=2))
    return 0


async def cmd_identify(args):
    opened = await open_session(args)
    if not opened:
        return 1
    client, session = opened
    async with client:
        await session.cmd({"op": "identify"})
        print("Device screen is flashing.")
    return 0


async def cmd_ls(args):
    opened = await open_session(args)
    if not opened:
        return 1
    client, session = opened
    async with client:
        entries = await session.cmd({"op": "list", "path": args.path})
        for e in sorted(entries, key=lambda x: (not x["isDir"], x["name"].lower())):
            kind = "<dir>" if e["isDir"] else f"{e['size']:>9}"
            print(f"{kind}  {e['name']}")
        if not entries:
            print("(empty)")
    return 0


async def cmd_rm(args):
    opened = await open_session(args)
    if not opened:
        return 1
    client, session = opened
    async with client:
        await session.cmd({"op": "delete", "path": args.path})
        print(f"Deleted {args.path}")
    return 0


async def cmd_mv(args):
    opened = await open_session(args)
    if not opened:
        return 1
    client, session = opened
    async with client:
        await session.cmd({"op": "rename", "from": args.src, "to": args.dst})
        print(f"Renamed {args.src} -> {args.dst}")
    return 0


async def cmd_label(args):
    opened = await open_session(args)
    if not opened:
        return 1
    client, session = opened
    async with client:
        resp = await session.cmd({"op": "getlabel", "path": args.path})
        print(resp["label"])
    return 0


async def cmd_setlabel(args):
    opened = await open_session(args)
    if not opened:
        return 1
    client, session = opened
    async with client:
        await session.cmd({"op": "setlabel", "path": args.path,
                           "label": args.label}, timeout=30.0)
        print(f"Label of {args.path} set to '{args.label}'")
    return 0


async def cmd_put(args):
    local = Path(args.local)
    if not local.is_file():
        print(f"Local file not found: {local}")
        return 1
    remote = args.remote or "/" + local.name
    if not remote.startswith("/"):
        remote = "/" + remote
    data = local.read_bytes()
    sha = hashlib.sha256(data).hexdigest()
    size = len(data)

    opened = await open_session(args)
    if not opened:
        return 1
    client, session = opened
    async with client:
        req_id = session.next_req_id()
        await client.write_gatt_char(
            UUID_CONTROL,
            json.dumps({"op": "wbegin", "reqId": req_id, "path": remote,
                        "size": size, "sha256": sha}).encode(),
            response=True)
        # wbegin's ok/error and every later wack arrive on this same reqId.
        frame = await session.frame(req_id, 10.0)
        resp = json.loads(frame[4:].decode())
        if frame[1] & FLAG_ERROR or "error" in resp:
            code = resp.get("error", "?")
            hint = ERR_HINTS.get(code, "")
            print(f"wbegin refused: {code}" + (f" ({hint})" if hint else ""))
            return 1

        pmax = payload_max(client)
        seq = 0
        offset = 0          # next byte to send
        acked = 0           # device's last wack nextOffset
        attempts = 0
        t0 = time.monotonic()
        print(f"Uploading {local.name} -> {remote} ({size} bytes, {pmax} B/frame)")
        while acked < size:
            # Stream from `offset`; the LAST flag rides on the final frame.
            while offset < size:
                # Drain wacks; pause if too far ahead of the acked watermark.
                while True:
                    try:
                        frame = session.queue.get_nowait()
                    except asyncio.QueueEmpty:
                        break
                    if len(frame) >= 4 and frame[0] == req_id:
                        acked = max(acked, json.loads(frame[4:]).get("nextOffset", 0))
                if offset - acked >= PUSH_AHEAD_MAX:
                    await asyncio.sleep(0.02)
                    continue
                chunk = data[offset:offset + pmax]
                last = offset + len(chunk) >= size
                hdr = bytes((req_id, 0x01 if last else 0x00,
                             seq & 0xFF, (seq >> 8) & 0xFF))
                await client.write_gatt_char(UUID_DATA, hdr + chunk, response=False)
                offset += len(chunk)
                seq += 1
                if seq % 16 == 0:
                    await asyncio.sleep(0)   # let notifications through
                    pct = acked * 100 // size
                    print(f"\r  {pct}%", end="", flush=True)
            # All bytes sent: wait for the wack that says the device has them.
            try:
                frame = await session.frame(req_id, WACK_FINAL_TIMEOUT)
                acked = max(acked, json.loads(frame[4:]).get("nextOffset", 0))
            except TimeoutError:
                pass
            if acked < size:
                # Frames were dropped; the gap-idle wack told us where to resume
                # (the device adopts our next seq, so seq just keeps counting).
                attempts += 1
                if attempts >= PUT_MAX_ATTEMPTS:
                    print("\nUpload stalled - too many resume attempts.")
                    return 1
                print(f"\r  resuming from byte {acked}...")
                offset = acked
        dt = time.monotonic() - t0
        print(f"\r  100% in {dt:.1f}s ({size / dt / 1024:.1f} KB/s)")

        await session.cmd({"op": "wcommit"}, timeout=30.0)
        print(f"Committed {remote} (sha256 verified on device)")
    return 0


async def cmd_get(args):
    remote = args.remote if args.remote.startswith("/") else "/" + args.remote
    local = Path(args.local) if args.local else Path(remote.rsplit("/", 1)[-1])

    opened = await open_session(args)
    if not opened:
        return 1
    client, session = opened
    async with client:
        st = await session.cmd({"op": "stat", "path": remote}, timeout=30.0)
        size, sha = st["size"], st["sha256"]
        print(f"Downloading {remote} -> {local} ({size} bytes)")

        req_id = session.next_req_id()
        await client.write_gatt_char(
            UUID_CONTROL,
            json.dumps({"op": "read", "reqId": req_id, "path": remote,
                        "offset": 0, "len": size}).encode(),
            response=True)
        buf = bytearray()
        expected_seq = 0
        while True:
            frame = await session.frame(req_id, 15.0)
            if frame[1] & FLAG_ERROR:
                resp = json.loads(frame[4:].decode())
                code = resp.get("error", "?")
                print(f"read refused: {code} ({ERR_HINTS.get(code, '')})")
                return 1
            seq = frame[2] | (frame[3] << 8)
            if seq != expected_seq:
                print(f"\nMissed frame (got seq {seq}, wanted {expected_seq}).")
                return 1
            expected_seq += 1
            buf += frame[4:]
            print(f"\r  {len(buf) * 100 // size if size else 100}%", end="", flush=True)
            if frame[1] & FLAG_LAST:
                break
        print()
        if len(buf) != size:
            print(f"Short read: {len(buf)} of {size} bytes.")
            return 1
        if sha and hashlib.sha256(buf).hexdigest() != sha.lower():
            print("Checksum mismatch - file changed mid-read or transfer corrupted.")
            return 1
        local.write_bytes(bytes(buf))
        print(f"Saved {local} (sha256 verified)")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("scan", "pair", "info", "health", "identify"):
        sp = sub.add_parser(name)
        sp.add_argument("-d", "--device", help="device name, e.g. MPD-1A2B")
    ls = sub.add_parser("ls")
    ls.add_argument("path", nargs="?", default="/")
    ls.add_argument("-d", "--device")
    put = sub.add_parser("put")
    put.add_argument("local", help="local file to upload")
    put.add_argument("remote", nargs="?", help="destination path (default /<name>)")
    put.add_argument("-d", "--device")
    get = sub.add_parser("get")
    get.add_argument("remote", help="file on the SD card")
    get.add_argument("local", nargs="?", help="local destination (default ./<name>)")
    get.add_argument("-d", "--device")
    rm = sub.add_parser("rm")
    rm.add_argument("path")
    rm.add_argument("-d", "--device")
    mv = sub.add_parser("mv")
    mv.add_argument("src")
    mv.add_argument("dst")
    mv.add_argument("-d", "--device")
    lb = sub.add_parser("label")
    lb.add_argument("path")
    lb.add_argument("-d", "--device")
    sl = sub.add_parser("setlabel")
    sl.add_argument("path")
    sl.add_argument("label")
    sl.add_argument("-d", "--device")
    args = p.parse_args()

    handler = {"scan": cmd_scan, "pair": cmd_pair, "info": cmd_info,
               "health": cmd_health, "identify": cmd_identify, "ls": cmd_ls,
               "put": cmd_put, "get": cmd_get, "rm": cmd_rm, "mv": cmd_mv,
               "label": cmd_label, "setlabel": cmd_setlabel}[args.cmd]
    try:
        rc = asyncio.run(handler(args))
    except (RuntimeError, TimeoutError) as e:
        print(f"\nFAILED: {e}")
        rc = 1
    except KeyboardInterrupt:
        rc = 130
    sys.exit(rc)


if __name__ == "__main__":
    main()
