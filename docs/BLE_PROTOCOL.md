# MicroPicoDrive BLE protocol

How to talk to a MicroPicoDrive (mainline, Pico 2 W) over Bluetooth LE: browse
the SD card, upload and download MDV cartridges, rename the unit, reboot it,
and hand it a firmware update. Read this if you are writing a client — an app,
a script, an integration. You should not need to read the firmware source.

A working client ships in this repo and doubles as the executable example:
[`tools/mdvtool.py`](../tools/mdvtool.py) (file operations; a further
maintainer-side tool covers firmware update). Every hard limit quoted along
the way is collected in one table near the end, "Numbers worth pinning".

> **Keep in sync (maintainers):** the protocol is implemented in
> `src/ble/ota_ble.c` — `process_command()` serves every JSON command. Any
> change to the op set, frame formats, error codes or limits there must be
> reflected here, in `tools/mdvtool.py`, in the local `tools/ota_bletest.py`,
> and in the companion app's mirror doc
> (`sinclair-mpd-companion/docs/BLE_PROTOCOL.md`). `process_command()` carries
> the same note.

## The one-minute picture

The device is a BLE peripheral with one primary service and four
characteristics. All four require an **encrypted, passkey-bonded link** — pair
once per client machine (see below).

| UUID (`7A0B000x-D5A9-4B7C-9F2A-5B1E6F0C4D10`) | Name here | Companion doc name | Properties | Carries |
|---|---|---|---|---|
| `…0002` | INFO | INFO | Read | device info (JSON in Connect mode) |
| `…0003` | CONTROL | CMD | Write | your JSON commands |
| `…0004` | DATA | DATA_IN | Write without response | file bytes you push |
| `…0005` | STATUS | DATA | Notify | replies, streamed file bytes, upload acks |

(The "companion doc name" column maps these to the companion app's protocol
doc for maintainers who have that repo; ignore it otherwise.)

The same four characteristics serve **two personalities**, and a connection is
always in exactly one:

- **Connect mode** (device menu: System Tools → Connect) — the JSON protocol
  this document describes.
- **Update Firmware mode** — a separate binary protocol for OTA firmware
  images, summarised at the end.

Everything below assumes Connect mode. The radio is only powered while the
device sits on one of three screens — **Connect**, **Pair Device**, or
**Update Firmware**. On any other screen (the main menu, a cartridge in use)
it does not advertise and a scan finds nothing. Pair Device and Update
Firmware mode do answer an INFO read but silently ignore JSON commands — your
request times out. There is a reliable tell: **in Connect mode an INFO read
returns JSON** (starts with `{`); in the other two modes it returns a legacy
semicolon string like `v2.8.0;slotA;max=1572864;proto=1`. Check that first
when a command gets no answer.

## Finding and pairing the device

Scan for the service UUID (`7A0B0001-…`), not the name — names are cached and
unreliable on both Android and Windows. The device advertises as `MPD-XXXX`
(last four hex digits of its unit id) or a user-chosen name.

Pairing happens once per client machine: connect, read INFO, and the OS runs a
6-digit passkey exchange — the passkey appears **on the device display**. The
**Pair Device** screen is the intended place for this, though the Connect
screen accepts a first-time bond too. After that, reconnections are
silent. `mdvtool.py pair` walks through it, including the platform quirks
(on Linux, run `bluetoothctl` alongside so an agent can take the passkey
prompt; on Windows, the Settings → Bluetooth → Add device path works as a
fallback).

## Sending a command, reading the reply

Write one JSON object to CONTROL (a normal write-with-response), then collect
notification frames from STATUS. Keep the whole JSON to at most **255 bytes**.

```json
{ "op": "list", "path": "/", "reqId": 7 }
```

`reqId` is a number 1–255 you pick; the reply echoes it, which is how you match
responses when frames interleave. Every STATUS notification is one binary
frame:

```
byte 0     reqId    echoes your request
byte 1     flags    bit0 = last frame, bit1 = error
bytes 2–3  seq      uint16 little-endian, counts from 0
bytes 4…   payload
```

Concatenate payloads for your `reqId` until you see the **last** flag, then
parse. A reply of up to 240 bytes fits one frame; longer replies (a big
directory listing, a file download) arrive as many. If the **error** bit is
set, the payload is `{"error":"CODE"}` — codes are listed at the end.

A minimal client, using [bleak](https://github.com/hbldh/bleak) (works on
Windows, Linux and macOS):

```python
import asyncio, json
from bleak import BleakClient, BleakScanner

SERVICE = "7a0b0001-d5a9-4b7c-9f2a-5b1e6f0c4d10"
CONTROL = "7a0b0003-d5a9-4b7c-9f2a-5b1e6f0c4d10"
STATUS  = "7a0b0005-d5a9-4b7c-9f2a-5b1e6f0c4d10"

async def main():
    # Assumes this machine is already paired with the device —
    # run "mdvtool.py pair" once first, or the connect will fail.
    dev = await BleakScanner.find_device_by_filter(
        lambda d, adv: SERVICE in (adv.service_uuids or []))
    async with BleakClient(dev) as client:
        queue = asyncio.Queue()
        await client.start_notify(STATUS, lambda _, d: queue.put_nowait(bytes(d)))

        await client.write_gatt_char(
            CONTROL, json.dumps({"op": "list", "path": "/", "reqId": 7}).encode(),
            response=True)
        payload = bytearray()
        while True:
            frame = await asyncio.wait_for(queue.get(), 10.0)
            if frame[0] != 7:
                continue                      # someone else's frame
            payload += frame[4:]
            if frame[1] & 0x01:               # last
                break
        print(json.loads(payload.decode()))   # [{"name":"BOOT.MDV","isDir":false,"size":174930}, …]

asyncio.run(main())
```

## Command reference

| Op | Request fields | Reply payload |
|---|---|---|
| `hello` | — | `{"ok":true}` |
| `ping` | — | `{"ok":true}` (see liveness, below) |
| `info` | — | `{"name","unitId","fw","otaCapable","sd":{"inserted","freeBytes","totalBytes"}}` |
| `health` | — | `{"uptimeS","tempC","freeHeapB","fw","slot","otaCapable"}` |
| `identify` | — | `{"ok":true}`; the device screen flashes for 3 s |
| `setname` | `name` (1–16 printable ASCII) | `{"ok":true}`; persists, shows in INFO at once |
| `list` | `path` (≤127 chars) | JSON array of `{"name","isDir","size"}` |
| `stat` | `path` | `{"size","sha256"}` (sha256 empty for directories) |
| `read` | `path`, `len`, optional `offset` | raw file bytes, streamed in frames |
| `wbegin` | `path` (≤95 chars), `size`, `sha256` (hex, case-insensitive) | `{"ok":true}`; opens an upload session |
| `wcommit` | — | `{"ok":true}` once the upload is verified |
| `delete` | `path` | `{"ok":true}`; files only, never directories |
| `rename` | `from`, `to` (full paths, cross-directory allowed) | `{"ok":true}` |
| `getlabel` | `path` (an MDV image — judged by size and headers, not extension) | `{"label":"CHESS"}` |
| `setlabel` | `path`, `label` (1–10 printable ASCII) | `{"ok":true}`; ~1 s atomic rewrite |
| `reboot` | — | `{"ok":true}`, then the device reboots ~500 ms later |
| `updatemode` | — | `{"ok":true}`, then it reboots into Update Firmware mode |

Notes that keep clients honest:

- **Mutations are refused while a cartridge is in use.** `wbegin`, `delete`,
  `rename`, `setlabel`, `reboot` and `updatemode` all answer `EBUSY` while
  *any* cartridge is mounted or the QL is using the drive — not only when the
  target file is the mounted one. Tell the user to eject first.
- **Liveness is opt-in.** If you never send a `ping`, the device keeps an idle
  connection alive indefinitely. Your first `ping` arms a watchdog for the
  rest of the connection: 30 s with no inbound write of any kind drops the
  link. Long-running clients should ping every ~10 s; short scripts can skip
  pinging entirely.
- **Paths** are absolute, `/NAME.MDV` style, on a FAT filesystem — case is
  preserved but not significant. Length limits: 127 characters for `list`,
  `stat` and `read`; 95 for every other op. Enforce them client-side — the
  firmware silently truncates longer strings and then operates on the
  truncated path.
- `list` on a very large directory truncates near 8 KB of JSON — hundreds of
  entries — and there is **no flag telling you it happened**. If complete
  listings matter to your client, keep directories comfortably below that.
- `delete` and `rename` of an `.mdv` take its `.mdv.thumb` sidecar along,
  best-effort — a raw 160×80 RGB565 big-endian thumbnail, exactly 25,600
  bytes, moved with the ordinary file ops (no dedicated commands).
- `getlabel`/`setlabel` work on QLay-format MDV images (exactly 174,930 bytes)
  and read/rewrite the medium name in the sector headers, fixing each header
  checksum. Changing the label changes the file's SHA-256.
- `read` honours `offset`/`len` exactly, so you can page a download in windows
  and retry a window that dropped frames. Reading past the end of the file is
  not an error — you get fewer bytes than asked, possibly none, with the last
  flag set. `stat` first, then verify the bytes you got against its `sha256`.
  One caveat on `stat`: an empty `sha256` usually means a directory, but the
  device also leaves it empty when hashing fails — use `isDir` from `list` as
  the directory test, never the empty hash.

## Uploading a file

Uploads run through a session: announce the file, stream the bytes, commit.
The device writes to a temp file (`<path>.part`) and only renames it over the
destination after the SHA-256 matches — an interrupted upload never leaves a
half-written file, and stale `.part` files are cleaned on the next Connect
mode entry.

1. **`wbegin`** with `path`, `size` and the file's SHA-256 (hex, either
   case). `ENOSPC` here means the card is full; `EBUSY` means eject the
   cartridge. Be aware that a `wbegin` while another upload session is open
   silently discards that session and its temp file — the device does not
   refuse it.
2. **Stream the bytes to DATA** as write-without-response frames with the same
   4-byte header as STATUS frames — `[reqId][flags][seq16 LE][payload]` —
   where `reqId` is the one from `wbegin`, `seq` counts every frame from 0,
   and the payload is up to **240 bytes** of file data (the firmware accepts
   up to 512, but 240 always fits one radio packet). Set flags bit0 (**last**)
   on the final frame.
3. **Watch STATUS for acks.** The device sends `{"nextOffset":N}` frames on
   your `reqId` — after every 16 KB written, and once more after your last
   frame. `nextOffset` is the count of contiguous bytes safely on the card.
4. **If frames got dropped** (radio hiccup, buffer overrun), progress stalls:
   after 150 ms of silence the device sends an ack repeating the last good
   `nextOffset` and resyncs to whatever `seq` you send next. Seek your file to
   `nextOffset` and resume from there, `seq` continuing to count up.
5. When `nextOffset` equals the file size, send **`wcommit`** — with a fresh
   `reqId`, so its reply is distinguishable from the acks still tagged with
   the `wbegin` one. `{"ok":true}` means the device verified the SHA-256 and
   the file is in place. `ECHECKSUM` means it did not survive the trip —
   start over.

The upload loop from `mdvtool.py`, trimmed to its shape (the full working
versions of `cmd`, `drain_acks` and `wait_for_ack` are in that file):

```python
sha = hashlib.sha256(data).hexdigest()
await cmd({"op": "wbegin", "path": "/GAME.MDV", "size": len(data),
           "sha256": sha, "reqId": rid})

seq, offset, acked = 0, 0, 0
while acked < len(data):
    while offset < len(data):
        drain_acks()                          # update `acked` from {"nextOffset":N}
        if offset - acked >= 32768:           # don't outrun the card
            await asyncio.sleep(0.02); continue
        chunk = data[offset:offset + 240]
        last = offset + len(chunk) >= len(data)
        hdr = bytes((rid, 1 if last else 0, seq & 0xFF, seq >> 8 & 0xFF))
        await client.write_gatt_char(DATA, hdr + chunk, response=False)
        offset += len(chunk); seq += 1
    acked = await wait_for_ack()              # post-last ack
    if acked < len(data):                     # frames dropped: rewind
        offset = acked

await cmd({"op": "wcommit", "reqId": new_rid})
```

Expect roughly 10–35 s for a full 174 KB cartridge — BLE is the bottleneck.
One more timeout to know about: an upload session with **no inbound data for
30 s is aborted** and the temp file deleted; `wcommit` after that answers
`ESTATE`.

## Downloading a file

`stat` for size and checksum, then `read`. The bytes arrive on STATUS in
order, `seq` counting from 0, last flag on the final frame. Verify what you
reassembled against the `stat` sha256. If `seq` skips — a dropped notification
— re-request the missing region with `offset`/`len` rather than the whole
file.

```python
st = await cmd({"op": "stat", "path": "/GAME.MDV", "reqId": rid})
await write_control({"op": "read", "path": "/GAME.MDV",
                     "offset": 0, "len": st["size"], "reqId": rid2})
buf = bytearray()
while True:
    frame = await next_frame(rid2)
    buf += frame[4:]
    if frame[1] & 0x01:
        break
assert hashlib.sha256(buf).hexdigest() == st["sha256"]
```

## Error codes

Every error is a STATUS frame with the error bit set and payload
`{"error":"CODE"}`.

| Code | Meaning | What to do |
|---|---|---|
| `EBUSY` | cartridge mounted / QL using the drive | eject, retry |
| `ENOENT` | no such file | — |
| `EEXIST` | rename target already exists | pick another name |
| `EISDIR` | path is a directory | — |
| `EINVAL` | bad label or device name; or the file is not a valid MDV image (wrong size, or no sector with a valid header) | — |
| `ENOSPC` | SD card full | free space first |
| `ECHECKSUM` | uploaded bytes did not match the announced SHA-256 | retry the upload |
| `ESTATE` | `wcommit` with no open upload session | `wbegin` again |
| `EPROTO` | malformed request (missing or wrongly-typed field) | fix the request |
| `ESEEK` / `EIO` | SD trouble on the device | retry; check the card |
| `ENOTSUP` | `updatemode` without A/B partitions | install over USB BOOTSEL instead (`tools/install_mainline.ps1`) |
| `ENOSYS` | unknown op (older firmware) | check `info.fw` |

## Numbers worth pinning

| Limit | Value |
|---|---|
| JSON command size | ≤ 255 bytes |
| `path` length | ≤ 127 (`list`, `stat`, `read`), ≤ 95 (everything else) |
| Push frame payload | ≤ 512 accepted, 240 recommended |
| Upload ack cadence | every 16 KB, plus after the last frame |
| Gap-resync ack | after 150 ms of upload silence |
| Upload session timeout | 30 s without data |
| Liveness watchdog | 30 s, armed by your first `ping` |
| Reply frame payload | ≤ 240 bytes |

## Firmware update mode

`updatemode` reboots the device into a different, binary protocol on the same
four characteristics: CONTROL takes a 41-byte START (size, version,
SHA-256), DATA takes `[offset u32][≤240 bytes]` chunks under an ACK window,
STATUS notifies READY/ACK/ERROR/VERIFIED/COMMITTED/DECLINED, and the user
confirms the install on the device screen. Client authors who only manage
files never need this mode — but if a `read` of INFO returns the legacy
semicolon string, you may be talking to a device sitting in it. Firmware
delivery for end users goes through the companion app or a USB install.
