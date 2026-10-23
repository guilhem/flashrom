EZP XPro
========

The ``ezp_xpro`` backend connects flashrom to an EZP XPro USB programmer
(USB ID ``0716:5018``) for SPI NOR programming. It uses libusb 1.0 directly,
without the proprietary application or a device-specific kernel driver. The
backend selects SPI NOR profiles from flashrom's chip database and matches them
to the firmware selectors recovered from the manufacturer's application. It
accepts compatible 3.3 V profiles of at most 16 MiB across manufacturers; chip
support is not restricted to the model used for hardware qualification.

.. warning::

   Read, erase, write and verification were tested on one GD25Q80 using firmware
   1.3. This establishes main-array operation on that combination. Supply and
   signal voltages were not measured, and protection-register and OTP effects
   remain uncharacterized. Keep an independent recovery method and qualify a
   sacrificial chip before using another combination on valuable contents.

Use an existing flashrom installation
-------------------------------------

Run ``flashrom --help`` first and look for ``ezp_xpro`` in its programmer list.
If present, use that installation. If absent, build a checkout containing this
driver with the usual compiler, Meson, Ninja, pkg-config, and libusb 1.0
development files; see :doc:`/dev_guide/building_from_source`. From the source
directory::

    meson setup builddir -Dprogrammer=ezp_xpro
    meson compile -C builddir
    builddir/flashrom --help

Use ``builddir/flashrom`` in place of ``flashrom`` below when running that build.

USB access on Linux
-------------------

Grant your user access to USB device ``0716:5018``. The source tree's
``util/flashrom_udev.rules`` includes a rule using group ``plugdev``, following
the other USB programmers. Adapt that group to your distribution if necessary,
install the rules in ``/etc/udev/rules.d/``, reload udev rules, and reconnect the
programmer. Group membership changes require a new login. No custom kernel
driver is needed.

Before connecting a chip
------------------------

* Use an already-qualified flashrom programmer to read and save an independent
  backup first, when available. Preserve it outside the working directory.
* Check the chip marking and datasheet, adapter orientation, and wiring. Start
  with a socketed sacrificial chip, isolated from a powered target board.
* Measure VCC and the SPI signal levels at the adapter before attaching the
  chip, and check VCC under load during qualification. Both supply and logic
  levels must be compatible with the chip's 3.3 V specification.
* ``voltage=3300`` is an explicit voltage request in millivolts, not evidence
  that the adapter supplies safe levels. Do not connect a 1.8 V or 5 V chip.

Read and preserve a backup
--------------------------

Both programmer parameters are required: ``chip`` is the exact flashrom chip
name, and ``voltage`` must be ``3300``. Find the chip name with ``flashrom -L``
and select it according to its marking and datasheet. That list covers all
flashrom programmers; the EZP backend rejects profiles it cannot translate.
For example, a 3.3 V Winbond W25Q128 uses ``W25Q128.V`` and has a capacity of
16 MiB (128 Mbit). Set the programmer argument for your actual chip, then keep
it quoted in each command::

    programmer='ezp_xpro:chip=W25Q128.V,voltage=3300'

    flashrom -p "$programmer" -r backup.bin
    flashrom -p "$programmer" -r backup-second.bin
    cmp backup.bin backup-second.bin
    flashrom -p "$programmer" -v backup.bin

Use new filenames: ``-r`` overwrites an existing file. Matching reads and a
successful verification are necessary before proceeding. They do not replace
comparison with a backup from an independently qualified programmer. Stop on
any mismatch or error and preserve the logs and backups.

The backend uses flashrom's opaque master interface: the programmer firmware
performs flash operations rather than exposing arbitrary SPI commands. The
``chip`` parameter selects the intended flash definition. Omit the global ``-c``
option: selecting the physical chip there filters out the generic opaque chip
definition before this backend can probe it. An accepted chip name alone does not qualify that
chip and adapter combination.

Erase, write, and verify
------------------------

Erase and write require firmware 1.3. The following commands
describe the destructive qualification workflow using a sacrificial chip.
Use a nonuniform test image exactly matching the selected chip capacity
(16,777,216 bytes for W25Q128.V), saved as ``test-image.bin``. Keep the original
backup.
Erasing first is useful for qualification; normal ``-w`` operation handles
required erases automatically::

    flashrom -p "$programmer" -E
    flashrom -p "$programmer" -w test-image.bin
    flashrom -p "$programmer" -v test-image.bin
    flashrom -p "$programmer" -r readback.bin
    cmp test-image.bin readback.bin

Leave flashrom's automatic erase/write verification enabled. A successful
``-v`` checks the entire image, and a successful ``cmp`` produces no output.
For valuable contents, also verify the result with an independently qualified programmer. To
restore and check the original contents::

    flashrom -p "$programmer" -w backup.bin
    flashrom -p "$programmer" -v backup.bin

Limits and qualification evidence
---------------------------------

* Profile eligibility uses flashrom's JEDEC identity, capacity, page size,
  voltage limits and uniform erase geometry. A generated table contains the
  additional firmware selectors from the manufacturer's v2.8 database. Profiles
  must match unambiguously, use ordinary three-byte RDID and page programming,
  and have a supported block replacement size (4, 32, 64 or 256 KiB). Both the
  firmware profile and flashrom's geometry must agree.
* Examples include GigaDevice GD25Q64(B), Winbond W25Q128.V, Macronix
  MX25L5121E, and Micron/Numonyx/ST M25P128. These examples describe accepted
  profiles, not additional hardware qualification. New aliases in flashrom
  with matching identity and geometry reuse the same firmware mapping.
* EEPROM, DataFlash, NAND, parallel/LPC/FWH buses, other voltages, extended
  identification and addresses beyond 24 bits are unsupported. A chip in
  ``flashrom -L`` is not necessarily usable with this programmer. Profiles
  absent from the recovered firmware mapping, ambiguous variants, and firmware
  algorithms without a supported transfer path are rejected before USB access.
  In particular, the shared JEDEC IDs of S25FL032A/P and S25FL064A/P do not
  distinguish the conflicting vendor profiles.
* Eligibility does not mark a chip as tested. Only the documented GD25Q80 and
  firmware 1.3 combination has completed hardware qualification; other profiles
  retain flashrom's untested read/erase/write status.
* Normal flashrom ``--wp-status``, ``--wp-list``, ``--wp-enable``,
  ``--wp-disable``, ``--wp-range``, and ``--wp-region`` operations are unsupported
  through this opaque backend. Do not assume existing protection settings can
  be inspected or preserved; keep initial destructive tests to sacrificial chips.
* The erase callback accepts only a whole-chip range. The core may erase the
  complete chip when updating an existing image, including a partial layout;
  flashrom preserves the excluded contents by rewriting them. This backend
  does not expose a sector-only erase operation.
* Programming uses firmware block replacement while preserving the other bytes
  from an initial complete read. Each write callback performs a fresh complete
  readback comparison, even if normal CLI verification was disabled. This
  checks array contents and does not establish register or OTP behavior.
* A failed transfer or comparison prevents further operations in that programmer
  session. Preserve logs and backups, disconnect and reconnect the programmer,
  and investigate the failure before retrying a write.
* USB enumeration, a recognized chip, unit tests, and repeated reads alone do
  not prove erase/write reliability or electrical safety.

Linux hardware qualification on 23 October 2026 used USB ``0716:5018``,
``bcdDevice=0200``, firmware 1.3, and a GD25Q80 reporting JEDEC ``C8 40 14``.
Repeated 1 MiB reads agreed. A one-byte write, another page in the same sector,
and a sector-boundary write preserved every other array byte. A partial-layout
update requiring 0-to-1 transitions exercised automatic whole-chip erase and
preserved excluded data. A nonuniform 1 MiB pattern was programmed and matched
a complete readback from a fresh process. Explicit erase then returned the
array to its original all-``FF`` contents.

After expanding profile matching, a follow-up hardware test changed bytes at
offsets 0, 4095, 4096 and 1048575 using the new 1024-byte sector-read transfers.
A fresh complete read matched the expected image, including every preserved
byte. Restoring the backup exercised whole-chip erase, and another fresh read
matched the original contents exactly.

The nonuniform pattern and readback SHA-256 was
``e923d47bb0bbb9171b7bd92ade6012cb0ff9e95a4732a3370c9ef141654ba5f1``.
These comparisons used this backend, not an independent programmer. Socket and
signal voltages, register/OTP contents, other chips, and other firmware versions
were not qualified. Future qualification should record the adapter revision,
chip marking, software revision, voltages, command logs and readback results.

Protocol reference for contributors
----------------------------------

The transport was recovered statically from the GZUT Windows application
versions 2.3 and 2.8, available from the
`manufacturer's product page <http://www.dream-e.com/Pshow.asp?Pid=45>`_.
The English executable SHA-256 values are respectively
``7fa99c371cb1d6ad792df292400ebe0c059fb461cd341571fccf271d79b94a04`` and
``31b298c748149fd4da4fab1b7166a827024829ea5762b724489f4ef5bb64bc29``.
No vendor executable or firmware update is needed to use the driver.

Interface 0 uses bulk OUT endpoints ``02`` (commands) and ``01`` (data),
and bulk IN endpoint ``83``. Commands are 20-byte frames with markers
``A5`` and ``5A``. Byte 1 contains the operation and voltage flags; bytes
6--9 contain big-endian chip capacity, and bytes 13--16 carry an optional
big-endian argument. The GD25Q80 row is identical in both application versions::

    @GD25Q80:256:4KB:1MB:N:324:C84014:N:3:000
    A5 00 00 00 08 02 00 10 00 00 00 03 24 00 00 00 00 00 00 5A

The driver uses configuration ``00``, firmware query ``12``, JEDEC query
``07``, whole-array read ``0D``, whole-chip erase ``05`` and stop ``09``.
Erase waits for byte ``06`` and then independently checks every array byte.
Block replacement ``13`` takes an aligned sector address, returns the existing
sector and one handshake byte, accepts the replacement sector, and is finalized
by ``02`` retaining that address. A second handshake follows. Their values are
not interpreted as successful programming; a fresh complete-array comparison
is mandatory. The application firmware's internal erase, register and OTP
implementation has not been recovered.

The generated ``programmers/ezp_xpro_profiles.h`` stores only the extra firmware
profile information and the identity/geometry keys needed to disambiguate it.
It does not replace flashrom's chip definitions. To regenerate it from a decoded
manufacturer v2.8 database, without running the proprietary application::

    python3 util/ezp_xpro_profiles.py /path/to/database.decoded.txt --output programmers/ezp_xpro_profiles.h

Keep conflicting entries, including unsupported algorithms and ``F`` block
sizes: filtering them out during generation could make an ambiguous JEDEC ID
appear safe. No vendor database or Python generator is needed at build or run
time. The read-modify-write path receives blocks in at most 1024-byte transfers
and sends replacement data in at most 4096-byte transfers, following the
Windows host's transfer limits. The firmware's internal erase opcode is not
inferred from its subtype.
