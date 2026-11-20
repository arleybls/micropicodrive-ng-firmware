# mdvpatch: make a QL cartridge run from another drive

## What it is for

Most Sinclair QL games and programs expect to live in drive 1. The name
`mdv1_` is written inside them, usually in the boot program and sometimes
inside the game itself. If you mount such a cartridge in drive 2 and start it,
the game looks for its files in drive 1 and stops with "not found".

`mdvpatch` makes a copy of the cartridge image with those names changed, so
the copy runs from the drive you choose. It can change the names back again
too. Your original file is never touched.

It works on standard `.MDV` cartridge images, the same files the MicroPicoDrive
loads from its SD card.

If you only use the images in a MicroPicoDrive, you may not need this tool. Its
firmware has an **Auto Patch** setting that does the same change by itself each
time an image is mounted, without touching the file. Use `mdvpatch` when you
want a patched file to keep, for an emulator or another device.

## What you need

- A computer with Windows, Linux or macOS.
- Python 3.8 or newer. Nothing else has to be installed.
- The file `mdvpatch.py`, from the `tools` folder of the firmware repository.

## Install

1. Check whether you already have Python. Open a terminal (on Windows:
   PowerShell or Command Prompt) and type:

   ```
   python --version
   ```

   If it prints a version number of 3.8 or higher, you are done with this step.
   On Linux and macOS the command may be `python3` instead of `python`. If so,
   use `python3` everywhere this manual says `python`.

2. If Python is missing, download it from https://www.python.org/downloads/
   and run the installer. On Windows, tick the box "Add python.exe to PATH" on
   the first screen of the installer.

3. Copy `mdvpatch.py` into the folder where you keep your `.MDV` files. That
   keeps the commands short, because you do not have to type long paths.

There is nothing to set up after that.

## How to run it

Open a terminal in the folder that holds `mdvpatch.py` and your `.MDV` files.
On Windows you can do this from File Explorer: open the folder, click the
address bar, type `powershell` and press Enter.

Every command has the same shape:

```
python mdvpatch.py COMMAND FILE [options]
```

There are three commands:

| Command | What it does |
|---|---|
| `list` | Shows the files stored on the cartridge |
| `scan` | Shows every drive name it finds inside those files |
| `retarget` | Writes a copy of the cartridge that uses a different drive |

The examples below use a cartridge called `Bruce_Lee.mdv`. Put your own file
name in its place.

## Example 1: look before you change anything

See what is on the cartridge:

```
python mdvpatch.py list Bruce_Lee.mdv
```

```
  boot                                      35 bytes  data
  boot_flp                                  35 bytes  data
  bruceql                               118222 bytes  executable
  readme                                   549 bytes  data
```

Then see which drive names are written inside it:

```
python mdvpatch.py scan Bruce_Lee.mdv
```

```
  boot                 mdv1_  ...DE 4.110 EXEC_W mdv1_bruceql....
  boot_flp             flp1_  ...DE 4.110 EXEC_W flp1_bruceql....
  readme               mdv1_  ...menu..MDV: LRUN mdv1_boot; floppy: LR...
  readme               flp1_  ...t; floppy: LRUN flp1_boot_flp.Origina...
```

Each line shows the file, the drive name, and a little of the text around it.
Here the `boot` program starts the game with `EXEC_W mdv1_bruceql`. That is the
line that fails when the cartridge sits in drive 2.

## Example 2: make a copy that runs from drive 2

```
python mdvpatch.py retarget Bruce_Lee.mdv --to mdv2
```

```
  boot                 DE 4.110 EXEC_W mdv1_bruceql.
  readme               menu..MDV: LRUN mdv1_boot; floppy: LR
2 name(s) changed from mdv1_ to mdv2_
written: Bruce_Lee_mdv2.mdv
```

The tool lists each place it changed and writes a new file,
`Bruce_Lee_mdv2.mdv`, next to the original. Copy the new file to the SD card,
mount it in drive 2 and start it on the QL with:

```
LRUN mdv2_boot
```

The QL only starts a cartridge by itself from drive 1. In any other drive you
have to type the `LRUN` line.

Not sure which drive a MicroPicoDrive is? Once the QL has used it, the small
green box at the top-right of its screen shows the number: `1` for `mdv1_`, `2`
for `mdv2_`.

## Example 3: go back to drive 1

Keep your original file and you never need this. If you only have the patched
copy, you can turn it back:

```
python mdvpatch.py retarget Bruce_Lee_mdv2.mdv --from mdv2 --to mdv1
```

```
  boot                 DE 4.110 EXEC_W mdv2_bruceql.
  readme               menu..MDV: LRUN mdv2_boot; floppy: LR
2 name(s) changed from mdv2_ to mdv1_
written: Bruce_Lee_mdv1.mdv
```

`--from` says which name to look for and `--to` says what to put in its place.
When you leave `--from` out, the tool looks for `mdv1`.

One thing to know about going back: the tool changes every `mdv2_` it finds.
If the original program already mentioned `mdv2_` somewhere for its own
reasons, that mention becomes `mdv1_` as well, and the result is no longer the
same as the original. The original file is the safe way back, so keep it.

## Other things you can do

Try a change without writing a file. Add `--dry-run` and the tool only shows
what it would change:

```
python mdvpatch.py retarget Bruce_Lee.mdv --to mdv2 --dry-run
```

Choose the name of the new file with `-o`:

```
python mdvpatch.py retarget Bruce_Lee.mdv --to mdv2 -o BruceLee_drive2.mdv
```

Change only some files with `--files`, giving the names from `list` separated
by commas. This leaves the `readme` alone and patches only the boot program:

```
python mdvpatch.py retarget Bruce_Lee.mdv --to mdv2 --files boot
```

Move software written for floppy disk onto a Microdrive. Some cartridges carry
names such as `flp1_`:

```
python mdvpatch.py retarget Game.mdv --from flp1 --to mdv1
```

A drive name is always three letters and a number from 1 to 8, such as `mdv1`,
`mdv2` or `flp1`. You can type it with or without the underscore at the end.

## What the tool does to the file

It swaps one drive name for another of the same length, so nothing inside the
cartridge moves. Every part of the cartridge it changes carries a checksum, a
small number the QL uses to check that the data is good. The tool works out
the new checksum for you, so the copy is a valid cartridge for the
MicroPicoDrive and for emulators.

## Limits

- The tool searches for drive names as plain text. Most programs store them
  that way. A program that builds the name while it runs, piece by piece,
  cannot be patched like this.
- A long file full of game data could, by chance, contain five bytes that look
  like a drive name. This is very unlikely. `scan` shows the text around every
  match, so you can check that each one is a real drive name before you patch.
  If one looks wrong, use `--files` to leave that file out.
- A patched copy only works from the drive you chose. Keep one copy for each
  drive you use.
- Loading is not faster or slower after patching. A game with a large data
  file still takes a few minutes to load from a Microdrive.

## If something goes wrong

| Message | What it means |
|---|---|
| `not a standard .MDV image` | The file is not 174930 bytes long, so it is not a normal cartridge image. Check that you picked the `.MDV` file and not a `.zip` or a floppy `.img`. |
| `no 'mdv1_' found - nothing to do` | The cartridge does not contain the name you asked to replace. Run `scan` to see which names it does contain, then set `--from`. |
| `is not a drive name` | The name after `--to` or `--from` is not three letters and a number from 1 to 8. |
| `not on the cartridge` | A name given to `--files` does not match a file. Run `list` and copy the name exactly, including capital letters. |
| `refusing to overwrite the original image` | The output name is the same as the input. Choose a different name with `-o`. |
| `python` is not recognised | Python is not installed, or not on the PATH. See the Install section. |
