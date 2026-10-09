# Native Disk Profile Checkpoints

`tools/profile_manager.py` creates private named NAND/OTP checkpoints using APFS
file clones on macOS, Linux reflinks where supported or verified ordinary copies.
Restore boots a fresh writable workspace. It does not resume CPU/RAM/device
state, guarantee guest filesystem consistency or change audio timing.

Keep the store outside Git in private storage. File clones are not independent
backups. Keep an external backup of important images and manifests. No firmware
or device image is included in the tool or its tests.

## First Use

Use Python 3.9 or newer on macOS/Linux. Stop every program writing the external
source pair before importing. `--offline` is your assertion of that condition;
the tool cannot prevent uncooperative writers outside its managed store.

```sh
python3 tools/profile_manager.py --store /private/path/profiles init
python3 tools/profile_manager.py --store /private/path/profiles import baseline \
  --nand /private/path/base/nand.bin --otp /private/path/base/otp.bin \
  --offline --compatibility board-profile-v1 --build-id tested-emulator-build
python3 tools/profile_manager.py --store /private/path/profiles restore baseline work \
  --compatibility board-profile-v1
```

The compatibility identifier is an explicit operator policy, not automatic
firmware analysis. Use a new identifier when a profile format or board contract
changes. The build label records the baseline; managed runs additionally hash
the actual executable and record argv. That run provenance is retained in saved
checkpoints. These manifests can contain private local paths. Do not publish them.

## Run And Save

For upstream op1emu, use the public source's GUI assets directory. Running from
the workspace isolates the default `otp.bin`; `{nand}` expands to the workspace
NAND. Optional `{otp}` supports other foreground runners. All script/file paths
in argv should be absolute because cwd changes to the workspace.

Options to `run` precede its workspace name; everything following the name is
the command. No shell parsing or implicit shell is performed.

```sh
python3 tools/profile_manager.py --store /private/path/profiles run \
  --compatibility board-profile-v1 --assets /path/to/op1emu/gui work -- \
  /path/to/op1emu/build/op1emu '{nand}' --nand-rw --headless \
  --input-script /private/path/inputs.txt
python3 tools/profile_manager.py --store /private/path/profiles checkpoint work after-test \
  --compatibility board-profile-v1
python3 tools/profile_manager.py --store /private/path/profiles restore after-test next-test \
  --compatibility board-profile-v1
python3 tools/profile_manager.py --store /private/path/profiles list
python3 tools/profile_manager.py --store /private/path/profiles verify checkpoints after-test
```

`--nand-snapshot` is refused: its dirty pages are discarded, so there would be
nothing persistent to save. The managed workspace already protects its parent
through copy-on-write. Checkpoints copy exactly NAND and OTP, not the public GUI
asset link. Write screenshots/logs outside the workspace. Unknown workspace
entries are refused before launch and after shutdown so extra persistent state
cannot silently disappear from a checkpoint.

## Shutdown And Recovery

The first Ctrl-C/SIGTERM/SIGHUP forwards SIGINT to the separate child process
group and waits for normal shutdown. A second request kills the child and marks
the run FAILED. Only normal exit 0 qualifies managed STOPPED status. It does not
prove the guest filesystem was clean. The child must be a foreground program,
must not daemonise and must retain the inherited profile lock descriptor.
Console stdin is disabled; use an explicit headless input script.

Checkpoint and rerun refuse FAILED/stale-RUNNING profiles. Restore a previous
checkpoint into a new workspace instead. If the manager dies, a surviving child
retains the lock, and its run stays incomplete even after that child exits.
Pre-existing unknown entries refuse launch without tainting the workspace.
Only reserved interrupted manager manifest temporary files are cleaned up.
No user-profile deletion, pruning or in-place restore command is provided.

Every named object is built in `.pending` and published by a final directory
rename. Incomplete staging is never listed as a usable profile. A post-rename
sync error explicitly reports uncertain durability; the complete name may exist,
so verify it rather than overwriting it. Files and directories are fsynced; on
macOS this is not F_FULLFSYNC and no universal power-loss guarantee is claimed.
Read-only checkpoints still belong to you and can be deliberately tampered with;
verification checks their recorded byte identities, not authenticity.

`--copy` on import/restore/checkpoint forces a normal copy. Automatic fallback
is limited to unsupported/cross-filesystem clone errors. Permission, full-disk
and I/O failures stop the operation. A clone may still need free disk space when
later modified. External source bytes, modes and immutable flags are untouched.

## Firmware-Free Tests

```sh
python3 -m unittest discover -s tests -p test_profile_manager.py -v
```

The tests generate their own tiny NAND/OOB/erase patterns and OTP bytes. They
exercise actual files and foreground processes, including Ctrl-C, manager death
with a live child, clone isolation, copy fallback, publication faults and invalid
inputs. Native macOS CoW is checked when running on macOS. Live Linux reflink
qualification requires running the suite on a suitable Linux filesystem.
