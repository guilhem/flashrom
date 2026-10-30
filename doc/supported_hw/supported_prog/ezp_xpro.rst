EZP XPro
========

The ``ezp_xpro`` backend connects flashrom to an EZP XPro USB programmer
(USB ID ``0716:5018``) for SPI NOR and supported SPI EEPROM programming. It uses libusb 1.0 directly,
without the proprietary application or a device-specific kernel driver. The
backend selects SPI NOR profiles from flashrom's chip database and matches them
to the firmware selectors recovered from the manufacturer's application. It
accepts compatible 3.3 V SPI NOR profiles up to 64 MiB, including supported
legacy algorithms, and the M95320/M95M02 EEPROM definitions from flashrom.
Support follows exact firmware mappings across manufacturers.

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
  levels must be compatible with the selected chip's specification.
* ``voltage=3300`` is an explicit voltage request in millivolts, not evidence
  that the adapter supplies safe levels. SPI NOR profiles require 3300 mV.
  The supported EEPROM profiles also accept 5000 mV within their datasheet
  range. Native 1.8 V operation is not enabled.

Read and preserve a backup
--------------------------

Both programmer parameters are required: ``chip`` is the exact flashrom chip
name, and ``voltage`` must be ``3300`` for SPI NOR. Find the chip name with ``flashrom -L``
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

Use libflashrom in another application
-------------------------------------

Build libflashrom with ``ezp_xpro`` enabled and use its public API. Pass
``"ezp_xpro"`` as the programmer name and, for example,
``"chip=W25Q128.V,voltage=3300"`` as the parameter string to
``flashrom_programmer_init()``. Keep chip selection in that parameter string;
pass ``NULL`` as the chip-name filter to ``flashrom_flash_probe_v2()`` so the
opaque chip can be probed. Check every initialization and probe result before
reading or modifying memory.

For EEPROM, create the flash context with ``flashrom_create_context()`` and set
``FLASHROM_FLAG_FORCE`` on it before probing, accepting the target-assumption
limitations described below. Reuse ``flashrom_image_read()``,
``flashrom_image_write()`` and ``flashrom_image_verify()`` for image operations.
Release probe-name arrays, the flash context and the programmer using the
corresponding public cleanup functions. No separate EZP library is required.

The backend coalesces changes within each prepared writable region. Full-array
profiles program the array once per such region; replacement profiles program
only blocks or pages whose contents differ. Whole-array verification remains
mandatory. Separate included layout regions can still require separate
transactions. This API programs supported memories; it does not expose SWD,
JTAG or PIC ICSP for microcontroller programming.

SPI NOR erase, write, and verify
--------------------------------

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

SPI EEPROM: explicit selection and logical erase
------------------------------------------------

The supported EEPROM definitions are ``M95320`` (4096 bytes, 32-byte pages)
and ``M95M02`` (262144 bytes, 256-byte pages). Select the exact chip and either
``voltage=3300`` or ``voltage=5000``, as appropriate for its datasheet and wiring.
The driver rejects a voltage outside flashrom's declared range.

EEPROM probing requires ``--force``. M95320 has no electronic identification;
M95M02 has a signature, but reading it through the recovered EZP interface is
not established. The backend reports an assumed target and sends no NOR ID
query. This cannot detect a missing or incorrectly selected chip. Global
``--force`` also retains flashrom's other safety-check overrides; it is not
limited to target assumption. NOR identity mismatches and backend transfer
failures still cannot be overridden.

For an actual M95320, for example::

    programmer='ezp_xpro:chip=M95320,voltage=3300'
    flashrom --force -p "$programmer" -r eeprom-backup.bin
    flashrom --force -p "$programmer" -r eeprom-backup-second.bin
    cmp eeprom-backup.bin eeprom-backup-second.bin
    flashrom --force -p "$programmer" -w eeprom-image.bin
    flashrom --force -p "$programmer" -v eeprom-image.bin

Use a capacity-sized image and preserve the backup. Writes can change bits in
both directions without sending NOR chip erase. EEPROM ``-E`` implements
flashrom's logical erase convention by writing zeroes, rather than issuing a
physical erase command. Every write, including logical erase, preserves bytes
outside its range and verifies the complete array. Neither EEPROM has been
qualified on EZP hardware.

Limits and qualification evidence
---------------------------------

* flashrom remains authoritative for chip identity, capacity, physical page
  size, voltage limits and erase geometry. The generated vendor mapping adds
  algorithm selectors, algorithm-unit sizes, names and exact primary/alternate
  wire identities. For some legacy chips these differ from the ordinary
  JEDEC representation. An exact named vendor row can resolve the selected
  algorithm; otherwise incompatible matching profiles are rejected.
* Supported NOR mappings include ordinary page programming, the established
  SST byte/AAI, ESMT and Atmel algorithms, and compatible PMC/AMIC identification
  encodings. Missing mappings, unresolved variants, unknown voltages, DataFlash,
  NAND and parallel/LPC/FWH buses remain unsupported. ``flashrom -L`` covers
  other programmers too; it is not an EZP compatibility list.
* The exact ``4B7`` mappings permit 32/64 MiB chips with compatible flashrom
  addressing metadata. They are distinct from the unsupported ``400`` family.
  These paths are tested with mocked USB, not large physical chips. Physical
  qualification must use nonuniform contents that differ between the 0, 16,
  32 MiB and final regions; repeated or all-FF reads cannot rule out address
  aliasing. Target address-mode restoration for the next consumer is unproven.
* Native 1.8 V operation remains disabled. The Windows host has a socket-mode
  detector for an alternate socket, which the vendor manual identifies as
  the copy-source/master position. This does not establish native 1.8 V
  selection or safe supply/signal levels for a low-voltage chip.
* Firmware block replacement uses the mapped block size (4, 32, 64 or 256 KiB
  for NOR; a page for these EEPROMs). For a compatible NOR profile marked ``F``,
  the driver instead preserves the whole image, erases if the chip is nonblank,
  and programs the complete array. Even a small update can therefore erase and
  rewrite all contents. Loss of power can lose the whole array; keep the backup.
* Only whole-chip erase geometry is advertised. Where the NOR metadata has a
  compatible chip-erase command, erase uses firmware ``05``; otherwise an
  established block-replacement profile fills the array with FF and verifies
  it. The core may erase the complete chip during a partial-layout update and
  rewrite excluded contents. No physical whole-chip erase substitutes for a
  smaller erase callback request.
* Eligibility does not mark a chip as tested. Only the documented GD25Q80 and
  firmware 1.3 combination has completed hardware qualification. Other NOR and
  EEPROM read/erase/write paths remain untested on hardware.
* Normal ``--wp-*`` operations remain unavailable. Do not assume protection,
  QE, configuration or OTP settings can be inspected or preserved. Complete
  array verification establishes only array contents.
* Every write callback performs a fresh complete-array comparison, including
  excluded bytes, even if normal CLI verification was disabled. A failed
  transfer, completion or comparison blocks subsequent operations in that
  programmer session. Preserve the backup and logs before reconnecting and
  investigating a failure.

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

Further qualification exercised the whole-array ``01``/``02`` programming path
while retaining the exact GD25Q80 wire profile. A nonuniform 1 MiB image and
complete readback matched. Logical erase through ``13`` block replacement,
without ``05``, restored the original all-FF array, confirmed by a fresh normal
CLI process. This qualifies those paths on GD25Q80, not other F profiles,
large chips or EEPROMs.

The initial nonuniform pattern and readback SHA-256 was
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

The driver uses configuration ``00``, firmware query ``12``, chip-ID query
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
Names disambiguate explicit selections, and linked IDs preserve each original
primary/alternate pair. Family 2 retains EEPROM rows without inventing IDs.
The mapping does not replace flashrom's chip definitions. To regenerate it from a decoded
manufacturer v2.8 database, without running the proprietary application::

    python3 util/ezp_xpro_profiles.py /path/to/database.decoded.txt --output programmers/ezp_xpro_profiles.h

Keep conflicting entries, including unsupported algorithms and ``F`` block
sizes: filtering them out during generation could make an ambiguous identity
appear safe. No vendor database or Python generator is needed at build or run
time. The read-modify-write path receives blocks in at most 1024-byte transfers
and sends replacement data in at most 4096-byte transfers, following the
Windows host's transfer limits. The firmware's internal erase opcode is not
inferred from its subtype.

Whole-image programming sends ``01`` (5000 ms command timeout), waits 50 ms,
streams the entire image in at most 4096-byte OUT01 transfers, waits 30 ms,
and sends ``02`` with the BE32 transmitted byte count. Success requires byte
``02`` within ten 6000 ms reads, followed by stop ``09`` and complete readback.
This count has a different meaning from the retained block address in the
``13`` replacement path. SPI image data is not padded past the intended length.
EEPROM secondary field ``N`` encodes byte 5 as zero and uses one algorithm page
as the replacement unit; each transfer is bounded by the remaining page bytes.
