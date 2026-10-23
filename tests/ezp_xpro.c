/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "lifecycle.h"

#if CONFIG_EZP_XPRO == 1
#include <limits.h>
#include <libusb.h>
#include "helpers.h"

#define EZP_PARAMS "chip=GD25Q80(B),voltage=3300"
#define EZP_CHIP_SIZE (1024 * KiB)
#define EZP_READ_BLOCK 4096
#define EZP_READ_TRANSFERS (5 + 1 + EZP_CHIP_SIZE / EZP_READ_BLOCK + 2)

struct ezp_example {
	const char *name;
	unsigned int size, page, block;
	uint8_t id[3];
	uint8_t packet[20];
};

/* Independent vendor v2.8 rows and complete frames, not the generated driver table. */
static const struct ezp_example ezp_examples[] = {
	/* @GD25Q80:256:4KB:1MB:N:324:C84014:N:3:000 */
	{ "GD25Q80(B)", 1 * MiB, 256, 4 * KiB, { 0xc8, 0x40, 0x14 },
	  { 0xa5, 0, 0, 0, 8, 2, 0, 0x10, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* @GD25Q64:256:4KB:8MB:N:324:C84017:N:3:000 */
	{ "GD25Q64(B)", 8 * MiB, 256, 4 * KiB, { 0xc8, 0x40, 0x17 },
	  { 0xa5, 0, 0, 0, 8, 2, 0, 0x80, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* @W25Q64:256:4KB:8MB:N:324:EF4017:N:3:000 */
	{ "W25Q64BV/W25Q64CV/W25Q64FV", 8 * MiB, 256, 4 * KiB, { 0xef, 0x40, 0x17 },
	  { 0xa5, 0, 0, 0, 8, 2, 0, 0x80, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* @W25Q128:256:4KB:16MB:N:324:EF4018:N:3:000 */
	{ "W25Q128.V", 16 * MiB, 256, 4 * KiB, { 0xef, 0x40, 0x18 },
	  { 0xa5, 0, 0, 0, 8, 2, 1, 0, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* @MX25L5121E:32:4KB:64KB:N:324:C22210:N:3:000 */
	{ "MX25L5121E", 64 * KiB, 32, 4 * KiB, { 0xc2, 0x22, 0x10 },
	  { 0xa5, 0, 0, 0, 5, 2, 0, 1, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* @M25P10A:256:32KB:128KB:N:324:202011:101010:3:010 */
	{ "M25P10-A", 128 * KiB, 256, 32 * KiB, { 0x20, 0x20, 0x11 },
	  { 0xa5, 0, 0, 0x10, 8, 5, 0, 2, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* @S25FL016A:256:64KB:2MB:N:324:010214:N:3:010 */
	{ "S25FL016A", 2 * MiB, 256, 64 * KiB, { 1, 2, 0x14 },
	  { 0xa5, 0, 0, 0x10, 8, 6, 0, 0x20, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* @M25P128:256:256KB:16MB:N:324:202018:171717:3:010 */
	{ "M25P128", 16 * MiB, 256, 256 * KiB, { 0x20, 0x20, 0x18 },
	  { 0xa5, 0, 0, 0x10, 8, 8, 1, 0, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* @F25L32PA:256:4KB:4MB:N:324:8C2016:N:3:050 */
	{ "F25L32PA", 4 * MiB, 256, 4 * KiB, { 0x8c, 0x20, 0x16 },
	  { 0xa5, 0, 0, 0x50, 8, 2, 0, 0x40, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
};

/* Independent identity layouts and algorithm encodings from the decoded vendor rows. */
static const struct ezp_example ezp_legacy_examples[] = {
	{ "Pm25LD010(C)", 128 * KiB, 256, 4 * KiB, { 0x7f, 0x9d, 0x21 },
	  { 0xa5, 0, 0, 0, 8, 2, 0, 2, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	{ "Pm25LD020(C)", 256 * KiB, 256, 4 * KiB, { 0x7f, 0x9d, 0x22 },
	  { 0xa5, 0, 0, 0, 8, 2, 0, 4, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	{ "Pm25LD256C", 32 * KiB, 256, 4 * KiB, { 0x7f, 0x9d, 0x2f },
	  { 0xa5, 0, 0, 0, 8, 2, 0, 0, 0x80, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	{ "Pm25LD512(C)", 64 * KiB, 256, 4 * KiB, { 0x7f, 0x9d, 0x20 },
	  { 0xa5, 0, 0, 0, 8, 2, 0, 1, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	{ "Pm25LQ016", 2 * MiB, 256, 4 * KiB, { 0x7f, 0x9d, 0x45 },
	  { 0xa5, 0, 0, 0, 8, 2, 0, 0x20, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	{ "Pm25LQ032C", 4 * MiB, 256, 4 * KiB, { 0x7f, 0x9d, 0x46 },
	  { 0xa5, 0, 0, 0, 8, 2, 0, 0x40, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	{ "Pm25LV080B", 1 * MiB, 256, 4 * KiB, { 0x7f, 0x9d, 0x13 },
	  { 0xa5, 0, 0, 0, 8, 2, 0, 0x10, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	{ "Pm25LV016B", 2 * MiB, 256, 4 * KiB, { 0x7f, 0x9d, 0x14 },
	  { 0xa5, 0, 0, 0, 8, 2, 0, 0x20, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* SST080 uses a padded REMS/RES2 identity and vendor page N (FF). */
	{ "SST25LF020A", 256 * KiB, 256, 4 * KiB, { 0, 0xbf, 0x43 },
	  { 0xa5, 0, 0, 0x80, 0xff, 2, 0, 4, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	{ "SST25LF040A", 512 * KiB, 256, 4 * KiB, { 0, 0xbf, 0x44 },
	  { 0xa5, 0, 0, 0x80, 0xff, 2, 0, 8, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* SST090's primary/alternate IDs retain the same AAI selector. */
	{ "SST25VF016B", 2 * MiB, 256, 4 * KiB, { 0xbf, 0x41, 0 },
	  { 0xa5, 0, 0, 0x90, 0xff, 2, 0, 0x20, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	{ "SST25VF040B", 512 * KiB, 256, 4 * KiB, { 0xbf, 0x8d, 0 },
	  { 0xa5, 0, 0, 0x90, 0xff, 2, 0, 8, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	{ "SST25VF040B.REMS", 512 * KiB, 256, 4 * KiB, { 0xbf, 0x8d, 0 },
	  { 0xa5, 0, 0, 0x90, 0xff, 2, 0, 8, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* Vendor spelling is SST25PF020B. */
	{ "SST25VF020B", 256 * KiB, 256, 4 * KiB, { 0xbf, 0x25, 0x8c },
	  { 0xa5, 0, 0, 0x90, 0xff, 2, 0, 4, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	{ "SST25VF080B", 1 * MiB, 256, 4 * KiB, { 0xbf, 0x8e, 0 },
	  { 0xa5, 0, 0, 0x90, 0xff, 2, 0, 0x10, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	{ "SST25VF032B", 4 * MiB, 256, 4 * KiB, { 0xbf, 0x4a, 0 },
	  { 0xa5, 0, 0, 0x90, 0xff, 2, 0, 0x40, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* The algorithm page is two bytes; flashrom's physical page remains 256. */
	{ "F25L008A", 1 * MiB, 256, 4 * KiB, { 0x8c, 0x21, 0x14 },
	  { 0xa5, 0, 0, 0x30, 1, 2, 0, 0x10, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	{ "AT25F512A", 64 * KiB, 128, 32 * KiB, { 0, 0x1f, 0x65 },
	  { 0xa5, 0, 0, 0x20, 7, 5, 0, 1, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	{ "AT25F2048", 256 * KiB, 256, 64 * KiB, { 0, 0x1f, 0x63 },
	  { 0xa5, 0, 0, 0x20, 8, 6, 0, 4, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	{ "AT25F4096", 512 * KiB, 256, 64 * KiB, { 0, 0x1f, 0x64 },
	  { 0xa5, 0, 0, 0x20, 8, 6, 0, 8, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
};

static const struct ezp_example ezp_array_examples[] = {
	/* Block F is encoded FF and must never enter command 13. */
	{ "AT25DF021", 256 * KiB, 256, 0, { 0x1f, 0x43, 0 },
	  { 0xa5, 0, 0, 0, 8, 0xff, 0, 4, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	{ "AT25DF041A", 512 * KiB, 256, 0, { 0x1f, 0x44, 1 },
	  { 0xa5, 0, 0, 0, 8, 0xff, 0, 8, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	{ "EN25B16", 2 * MiB, 256, 0, { 0x1c, 0x20, 0x15 },
	  { 0xa5, 0, 0, 0x10, 8, 0xff, 0, 0x20, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* AMIC continuation RDID4 retains the entire two-byte model on the vendor wire. */
	{ "A25L05PU", 64 * KiB, 256, 0, { 0x37, 0x20, 0x10 },
	  { 0xa5, 0, 0, 0, 8, 0xff, 0, 1, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	{ "A25L05PT", 64 * KiB, 256, 0, { 0x37, 0x20, 0x20 },
	  { 0xa5, 0, 0, 0, 8, 0xff, 0, 1, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	{ "A25L10PU", 128 * KiB, 256, 0, { 0x37, 0x20, 0x11 },
	  { 0xa5, 0, 0, 0x10, 8, 0xff, 0, 2, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	{ "A25L10PT", 128 * KiB, 256, 0, { 0x37, 0x20, 0x21 },
	  { 0xa5, 0, 0, 0x10, 8, 0xff, 0, 2, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
};

static const struct ezp_example ezp_eeprom_examples[] = {
	/* @M95320:32:N:4KB:N:216:N:N:5:200; no identification transaction. */
	{ "M95320", 4 * KiB, 32, 32, { 0 },
	  { 0xa5, 0, 2, 0, 5, 0, 0, 0, 0x10, 0, 0, 2, 0x16, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* @M95M02:256:N:256KB:N:324:N:N:5:200; ST95 identification unavailable through EZP. */
	{ "M95M02", 256 * KiB, 256, 256, { 0 },
	  { 0xa5, 0, 2, 0, 8, 0, 0, 4, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
};

static const struct ezp_example ezp_no_chip_erase_example = {
	"W25X05", 64 * KiB, 256, 4 * KiB, { 0xef, 0x30, 0x10 },
	{ 0xa5, 0, 0, 0, 8, 2, 0, 1, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a },
};

static const struct ezp_example ezp_large_examples[] = {
	/* @W25Q256F/JV:256:4KB:32MB:N:4B7:EF4019:N:3:000 */
	{ "W25Q256FV", 32 * MiB, 256, 4 * KiB, { 0xef, 0x40, 0x19 },
	  { 0xa5, 0, 0, 0, 8, 2, 2, 0, 0, 0, 0, 4, 0xb7, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* @W25Q512JV:256:4KB:64MB:N:4B7:EF4020:N:3:000 */
	{ "W25Q512JV", 64 * MiB, 256, 4 * KiB, { 0xef, 0x40, 0x20 },
	  { 0xa5, 0, 0, 0, 8, 2, 4, 0, 0, 0, 0, 4, 0xb7, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* @W25Q512JV-IM:256:4KB:64MB:N:4B7:EF7020:N:3:000 */
	{ "W25Q512JV_M", 64 * MiB, 256, 4 * KiB, { 0xef, 0x70, 0x20 },
	  { 0xa5, 0, 0, 0, 8, 2, 4, 0, 0, 0, 0, 4, 0xb7, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* @GD25Q256:256:4KB:32MB:N:4B7:C84019:N:3:000 */
	{ "GD25Q256E/GD25B256E/GD25R256E/GD25Q256D", 32 * MiB, 256, 4 * KiB, { 0xc8, 0x40, 0x19 },
	  { 0xa5, 0, 0, 0, 8, 2, 2, 0, 0, 0, 0, 4, 0xb7, 0, 0, 0, 0, 0, 0, 0x5a } },
};

struct ezp_transfer {
	uint8_t endpoint;
	uint8_t command;
	int length;
	uint8_t response[3];
	int error;
	int missing;
	unsigned int offset;
};

struct ezp_io_state {
	const struct ezp_transfer *transfers;
	size_t count;
	size_t next;
	unsigned int usb_inits;
};

static const struct ezp_transfer ezp_probe_transfers[] = {
	{ .endpoint = 0x02, .command = 0x12, .length = 20 },
	{ .endpoint = 0x83, .length = 1, .response = { 0x10 } },
	{ .endpoint = 0x02, .command = 0x00, .length = 20 },
	{ .endpoint = 0x02, .command = 0x07, .length = 20 },
	{ .endpoint = 0x83, .length = 3, .response = { 0xc8, 0x40, 0x14 } },
	{ .endpoint = 0x02, .command = 0x09, .length = 20 },
};

static int ezp_libusb_init(void *state, libusb_context **ctx)
{
	struct ezp_io_state *s = state;
	s->usb_inits++;
	*ctx = not_null();
	return 0;
}

static uint8_t ezp_read_byte(unsigned int offset)
{
	return (offset ^ (offset >> 8) ^ (offset >> 16) ^ (offset >> 24)) * 37 + 11;
}

static void ezp_check_example_packet(const struct ezp_example *example,
		const uint8_t *data, uint8_t command, unsigned int address)
{
	uint8_t packet[20];
	memcpy(packet, example->packet, sizeof(packet));
	packet[1] = (packet[1] & 0xc0) | command;
	for (unsigned int i = 0; i < 4; i++)
		packet[13 + i] = address >> (24 - i * 8);
	assert_memory_equal(data, packet, sizeof(packet));
}

static void ezp_check_packet(const uint8_t *data, uint8_t command, unsigned int address)
{
	ezp_check_example_packet(&ezp_examples[0], data, command, address);
}

static int ezp_libusb_bulk_transfer(void *state, libusb_device_handle *devh,
		unsigned char endpoint, unsigned char *data, int length,
		int *actual_length, unsigned int timeout)
{
	struct ezp_io_state *s = state;
	assert_true(s->next < s->count);
	const struct ezp_transfer *t = &s->transfers[s->next++];
	assert_ptr_equal(devh, not_null());
	assert_int_equal(endpoint, t->endpoint);
	assert_int_equal(length, t->length);
	assert_true(timeout > 0);
	if (endpoint == 0x02) {
		ezp_check_packet(data, t->command, 0);
	} else if (length == EZP_READ_BLOCK) {
		for (int i = 0; i < length - t->missing; i++)
			data[i] = ezp_read_byte(t->offset + i);
	} else {
		assert_true((size_t)length <= sizeof(t->response));
		memcpy(data, t->response, length - t->missing);
	}
	*actual_length = length - t->missing;
	return t->error;
}

static struct io_mock ezp_io(struct ezp_io_state *state)
{
	return (struct io_mock) {
		.state = state,
		.libusb_init = ezp_libusb_init,
		.libusb_bulk_transfer = ezp_libusb_bulk_transfer,
	};
}

void ezp_xpro_invalid_params_test(void **state)
{
	const char *params[] = {
		NULL, "", "chip=GD25Q80(B)", "voltage=3300",
		"chip=,voltage=3300", "chip=GD25Q80(B),voltage=",
		"chip=GD25Q80(B),voltage=1800", "chip=GD25Q80(B),voltage=3.3V",
		"chip=GD25Q80(B),voltage=5000", "chip=GD25Q80(B),voltage=3301",
		"chip=GD25Q80(B),voltage=3300x",
		"chip=not-a-flash-chip,voltage=3300",
		"chip=GD25Q80,voltage=3300",
		"chip=GD25LQ80,voltage=3300",
		EZP_PARAMS ",unexpected=yes",
	};
	for (size_t i = 0; i < ARRAY_SIZE(params); i++) {
		struct ezp_io_state s = { 0 };
		const struct io_mock io = ezp_io(&s);
		run_init_error_path(state, &io, &programmer_ezp_xpro, params[i], 1);
		assert_int_equal(s.usb_inits, 0);
		assert_int_equal(registered_master_count, 0);
	}
}

void ezp_xpro_profile_guards_test(void **state)
{
	const char *names[] = {
		"GD25LQ80", /* 1.8 V. */
		"F49B002UA", /* Parallel bus. */
		"unknown Macronix SPI chip", /* Generic identity. */
		"M25P10", /* RES probe and byte writes, not the vendor's 128-byte RDID row. */
		"AT25F512", /* Vendor identity has the wrong capacity. */
		"Pm25LD040(C)", "Pm25LV040", /* Conflicting 000/070 algorithms. */
		"Pm25LQ020", /* Vendor 9D1142 is not flashrom's 7F9D42. */
		"SST25WF040", /* 1.8 V AAI remains unsupported. */
		"AT45DB041D", /* DataFlash command set. */
		"W25Q01JV", /* More than 64 MiB. */
		"S25FL256S Large Sectors", "S25FL256S Small Sectors", /* Extended ID probe and 1.8 V metadata. */
		"S25FL256S......0", /* Vendor 400 subtype and hybrid geometry. */
		"S25FL512S", /* Selector 400, not 4B7. */
		"W25Q256JW", /* Large 1.8 V profile. */
		"S25FL032A/P", "S25FL064A/P", /* Same ID, conflicting A/P geometry and subtype. */
		"AT25F512B", /* Vendor page 128 disagrees with flashrom page 256. */
	};
	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		bool exists = false;
		for (unsigned int j = 0; j < flashchips_size && flashchips[j].name; j++)
			exists |= !strcmp(names[i], flashchips[j].name);
		assert_true(exists);
		char params[128];
		snprintf(params, sizeof(params), "chip=%s,voltage=3300", names[i]);
		struct ezp_io_state s = { 0 };
		const struct io_mock io = ezp_io(&s);
		run_init_error_path(state, &io, &programmer_ezp_xpro, params, 1);
		assert_int_equal(s.usb_inits, 0);
		assert_int_equal(registered_master_count, 0);
	}
}

struct ezp_inventory {
	unsigned int transfers, usb_inits;
};

static int ezp_inventory_init(void *state, libusb_context **ctx)
{
	struct ezp_inventory *s = state;
	s->usb_inits++;
	*ctx = not_null();
	return 0;
}

static int ezp_inventory_transfer(void *state, libusb_device_handle *devh,
		unsigned char endpoint, unsigned char *data, int length,
		int *actual_length, unsigned int timeout)
{
	struct ezp_inventory *s = state;
	assert_ptr_equal(devh, not_null());
	assert_int_equal(timeout, 1000);
	if (s->transfers++ == 0) {
		assert_int_equal(endpoint, 0x02);
		assert_int_equal(length, 20);
		assert_int_equal(data[0], 0xa5);
		assert_int_equal(data[1], 0x12);
		assert_int_equal(data[19], 0x5a);
	} else {
		assert_int_equal(s->transfers, 2);
		assert_int_equal(endpoint, 0x83);
		assert_int_equal(length, 1);
		data[0] = 0x03;
	}
	*actual_length = length;
	return 0;
}

void ezp_xpro_profile_inventory_test(void **state)
{
	(void)state;
	unsigned int accepted = 0, rejected = 0, duplicate_names = 0;
	for (unsigned int i = 0; i < flashchips_size && flashchips[i].name; i++) {
		bool duplicate = false;
		for (unsigned int j = 0; j < i; j++)
			duplicate |= !strcmp(flashchips[i].name, flashchips[j].name);
		if (duplicate) {
			duplicate_names++;
			continue;
		}
		char params[256];
		int len = snprintf(params, sizeof(params), "chip=%s,voltage=3300", flashchips[i].name);
		assert_true(len > 0 && (size_t)len < sizeof(params));
		struct ezp_inventory s = { 0 };
		const struct io_mock io = {
			.state = &s, .libusb_init = ezp_inventory_init,
			.libusb_bulk_transfer = ezp_inventory_transfer,
		};
		io_mock_register(&io);
		int ret = programmer_init(&programmer_ezp_xpro, params);
		if (!ret) {
			accepted++;
			printf("EZP accepted flashrom profile: %s\n", flashchips[i].name);
			assert_int_equal(s.usb_inits, 1);
			assert_int_equal(s.transfers, 2);
			assert_int_equal(registered_master_count, 1);
		} else {
			rejected++;
			assert_int_equal(s.usb_inits, 0);
			assert_int_equal(s.transfers, 0);
			assert_int_equal(registered_master_count, 0);
		}
		assert_int_equal(programmer_shutdown(), 0);
		assert_int_equal(registered_master_count, 0);
		io_mock_register(NULL);
	}
	assert_true(accepted >= ARRAY_SIZE(ezp_examples) + ARRAY_SIZE(ezp_large_examples) +
		    ARRAY_SIZE(ezp_legacy_examples) + ARRAY_SIZE(ezp_array_examples) +
		    ARRAY_SIZE(ezp_eeprom_examples) + 1);
	assert_true(rejected > 0);
	printf("EZP mocked runtime inventory: %u accepted, %u rejected unique names, %u duplicate rows skipped; no hardware.\n",
	       accepted, rejected, duplicate_names);
}

void ezp_xpro_ambiguous_params_test(void **state)
{
	const char *params[] = {
		EZP_PARAMS ",chip=GD25Q80(B)",
		EZP_PARAMS ",chip=GD25LQ80",
		EZP_PARAMS ",voltage=3300",
		EZP_PARAMS ",voltage=1800",
	};
	for (size_t i = 0; i < ARRAY_SIZE(params); i++) {
		struct ezp_io_state s = { 0 };
		const struct io_mock io = ezp_io(&s);
		run_init_error_path(state, &io, &programmer_ezp_xpro, params[i], 1);
		assert_int_equal(s.usb_inits, 0);
	}
}

void ezp_xpro_basic_lifecycle_test_success(void **state)
{
	struct ezp_io_state s = { .transfers = ezp_probe_transfers, .count = 2 };
	const struct io_mock io = ezp_io(&s);
	run_basic_lifecycle(state, &io, &programmer_ezp_xpro, EZP_PARAMS);
	assert_int_equal(s.usb_inits, 1);
	assert_int_equal(s.next, s.count);
}

void ezp_xpro_master_registration_failure_test(void **state)
{
	struct registered_master master = { 0 };
	int ret;
	assert_int_equal(registered_master_count, 0);
	while ((ret = register_master(&master)) == 0)
		;
	assert_int_equal(ret, ERROR_FLASHROM_LIMIT);
	assert_true(registered_master_count > 0);
	struct ezp_io_state s = { .transfers = ezp_probe_transfers, .count = 2 };
	const struct io_mock io = ezp_io(&s);
	/* The framework retains cleanup ownership when the master table is full. */
	run_init_error_path(state, &io, &programmer_ezp_xpro, EZP_PARAMS, ERROR_FLASHROM_LIMIT);
	assert_int_equal(s.next, s.count);
	assert_int_equal(registered_master_count, 0);
	assert_int_equal(programmer_shutdown(), 0);
}

static int ezp_count_shutdown(void *data)
{
	unsigned int *count = data;
	(*count)++;
	return 0;
}

void ezp_xpro_shutdown_registration_failure_test(void **state)
{
	(void)state;
	struct ezp_io_state s = { .transfers = ezp_probe_transfers, .count = 2 };
	const struct io_mock io = ezp_io(&s);
	io_mock_register(&io);
	/* A rejected init enables shutdown registration without opening USB. */
	assert_int_equal(programmer_init(&programmer_ezp_xpro, NULL), 1);
	assert_int_equal(s.usb_inits, 0);
	unsigned int slots = 0, shutdowns = 0;
	while (register_shutdown(ezp_count_shutdown, &shutdowns) == 0)
		slots++;
	assert_true(slots > 0);
	/* With no shutdown slot, the framework cleans up immediately instead. */
	assert_int_equal(programmer_init(&programmer_ezp_xpro, EZP_PARAMS), 1);
	assert_int_equal(registered_master_count, 0);
	assert_int_equal(s.next, s.count);
	assert_int_equal(programmer_shutdown(), 0);
	assert_int_equal(shutdowns, slots);
	assert_int_equal(programmer_shutdown(), 0);
	assert_int_equal(shutdowns, slots);
	io_mock_register(NULL);
}

void ezp_xpro_firmware_transfer_error_test(void **state)
{
	for (size_t failed = 0; failed < 2; failed++) {
		for (int short_transfer = 0; short_transfer < 2; short_transfer++) {
			struct ezp_transfer transfers[2];
			memcpy(transfers, ezp_probe_transfers, sizeof(transfers));
			transfers[failed].error = short_transfer ? 0 : LIBUSB_ERROR_IO;
			transfers[failed].missing = short_transfer ? 1 : 0;
			struct ezp_io_state s = { .transfers = transfers, .count = failed + 1 };
			const struct io_mock io = ezp_io(&s);
			run_init_error_path(state, &io, &programmer_ezp_xpro, EZP_PARAMS, 1);
			assert_int_equal(s.next, s.count);
			assert_int_equal(registered_master_count, 0);
		}
	}
}

void ezp_xpro_firmware_invalid_test(void **state)
{
	struct ezp_transfer transfers[2];
	memcpy(transfers, ezp_probe_transfers, sizeof(transfers));
	transfers[1].response[0] = 0xff;
	struct ezp_io_state s = { .transfers = transfers, .count = ARRAY_SIZE(transfers) };
	const struct io_mock io = ezp_io(&s);
	run_init_error_path(state, &io, &programmer_ezp_xpro, EZP_PARAMS, 1);
	assert_int_equal(s.next, s.count);
	assert_int_equal(registered_master_count, 0);
}

void ezp_xpro_probe_lifecycle_test_success(void **state)
{
	struct ezp_io_state s = {
		.transfers = ezp_probe_transfers, .count = ARRAY_SIZE(ezp_probe_transfers),
	};
	const struct io_mock io = ezp_io(&s);
	const char *names[] = { "GD25Q80(B)" };
	run_probe_v2_lifecycle(state, &io, &programmer_ezp_xpro, EZP_PARAMS,
			       NULL, names, ARRAY_SIZE(names));
	assert_int_equal(s.next, s.count);
}

static void ezp_probe_rejected(struct ezp_transfer *transfers, size_t count)
{
	struct ezp_io_state s = { .transfers = transfers, .count = count };
	const struct io_mock io = ezp_io(&s);
	io_mock_register(&io);
	assert_int_equal(programmer_init(&programmer_ezp_xpro, EZP_PARAMS), 0);
	assert_int_equal(registered_master_count, 1);
	struct flashchip chip = { 0 };
	struct flashctx flash = { .chip = &chip, .mst = &registered_masters[0] };
	const struct opaque_master *master = &flash.mst->opaque;
	assert_int_equal(master->probe(&flash), 0);
	uint8_t byte = 0;
	size_t after_probe = s.next;
	assert_int_not_equal(master->read(&flash, &byte, 0, 1), 0);
	assert_int_not_equal(master->write(&flash, &byte, 0, 1), 0);
	assert_int_not_equal(master->erase(&flash, 0, 1024 * KiB), 0);
	assert_int_equal(s.next, after_probe);
	assert_int_equal(programmer_shutdown(), 0);
	assert_int_equal(s.next, s.count);
	io_mock_register(NULL);
}

void ezp_xpro_probe_mismatch_test(void **state)
{
	(void)state;
	const uint8_t ids[][3] = {
		{ 0xef, 0x40, 0x14 }, { 0xc8, 0x60, 0x14 }, { 0xc8, 0x40, 0x15 },
		{ 0xff, 0xff, 0xff }, { 0x00, 0x00, 0x00 },
	};
	for (size_t i = 0; i < ARRAY_SIZE(ids); i++) {
		struct ezp_transfer transfers[ARRAY_SIZE(ezp_probe_transfers)];
		memcpy(transfers, ezp_probe_transfers, sizeof(transfers));
		memcpy(transfers[4].response, ids[i], sizeof(ids[i]));
		ezp_probe_rejected(transfers, ARRAY_SIZE(transfers));
	}
}

void ezp_xpro_probe_transfer_error_test(void **state)
{
	(void)state;
	for (size_t failed = 2; failed <= 4; failed++) {
		for (int short_transfer = 0; short_transfer < 2; short_transfer++) {
			struct ezp_transfer transfers[ARRAY_SIZE(ezp_probe_transfers)];
			memcpy(transfers, ezp_probe_transfers, sizeof(transfers));
			transfers[failed].error = short_transfer ? 0 : LIBUSB_ERROR_IO;
			transfers[failed].missing = short_transfer ? 1 : 0;
			transfers[failed + 1] = ezp_probe_transfers[5];
			ezp_probe_rejected(transfers, failed + 2);
		}
	}
}

static size_t ezp_read_transfers(struct ezp_transfer *transfers)
{
	memcpy(transfers, ezp_probe_transfers, 5 * sizeof(*transfers));
	size_t n = 5;
	transfers[n++] = (struct ezp_transfer) { .endpoint = 0x02, .command = 0x0d, .length = 20 };
	for (unsigned int offset = 0; offset < EZP_CHIP_SIZE; offset += EZP_READ_BLOCK)
		transfers[n++] = (struct ezp_transfer) {
			.endpoint = 0x83, .length = EZP_READ_BLOCK, .offset = offset,
		};
	transfers[n++] = ezp_probe_transfers[5];
	transfers[n++] = ezp_probe_transfers[5];
	return n;
}

static void ezp_init_probe(struct flashctx *flash)
{
	assert_int_equal(programmer_init(&programmer_ezp_xpro, EZP_PARAMS), 0);
	assert_int_equal(registered_master_count, 1);
	flash->mst = &registered_masters[0];
	assert_int_equal(flash->mst->opaque.probe(flash), 1);
	assert_string_equal(flash->chip->name, "GD25Q80(B)");
	assert_int_equal(flash->chip->total_size * KiB, EZP_CHIP_SIZE);
	assert_int_equal(flash->chip->page_size, 256);
	assert_int_equal(flash->chip->gran, WRITE_GRAN_256BYTES);
	assert_int_equal(flash->chip->block_erasers[0].eraseblocks[0].size, EZP_CHIP_SIZE);
	assert_int_equal(flash->chip->block_erasers[0].eraseblocks[0].count, 1);
}

void ezp_xpro_shutdown_transfer_error_test(void **state)
{
	(void)state;
	for (int short_transfer = 0; short_transfer < 2; short_transfer++) {
		struct ezp_transfer transfers[ARRAY_SIZE(ezp_probe_transfers)];
		memcpy(transfers, ezp_probe_transfers, sizeof(transfers));
		transfers[5].error = short_transfer ? 0 : LIBUSB_ERROR_IO;
		transfers[5].missing = short_transfer ? 1 : 0;
		struct ezp_io_state s = { .transfers = transfers, .count = ARRAY_SIZE(transfers) };
		const struct io_mock io = ezp_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		ezp_init_probe(&flash);
		assert_int_not_equal(programmer_shutdown(), 0);
		assert_int_equal(s.next, s.count);
		assert_int_equal(registered_master_count, 0);
		io_mock_register(NULL);
	}
}

void ezp_xpro_reprobe_mismatch_test(void **state)
{
	(void)state;
	struct ezp_transfer transfers[9];
	memcpy(transfers, ezp_probe_transfers, 5 * sizeof(*transfers));
	memcpy(transfers + 5, ezp_probe_transfers + 2, 3 * sizeof(*transfers));
	memset(transfers[7].response, 0xff, sizeof(transfers[7].response));
	transfers[8] = ezp_probe_transfers[5];
	struct ezp_io_state s = { .transfers = transfers, .count = ARRAY_SIZE(transfers) };
	const struct io_mock io = ezp_io(&s);
	io_mock_register(&io);
	struct flashchip chip = { 0 };
	struct flashctx flash = { .chip = &chip };
	ezp_init_probe(&flash);
	assert_int_equal(flash.mst->opaque.probe(&flash), 0);
	uint8_t byte = 0;
	assert_int_not_equal(flash.mst->opaque.read(&flash, &byte, 0, 1), 0);
	assert_int_not_equal(flash.mst->opaque.write(&flash, &byte, 0, 1), 0);
	assert_int_not_equal(flash.mst->opaque.erase(&flash, 0, EZP_CHIP_SIZE), 0);
	assert_int_equal(s.next, 8);
	assert_int_equal(programmer_shutdown(), 0);
	assert_int_equal(s.next, s.count);
	io_mock_register(NULL);
}

void ezp_xpro_read_ranges_test(void **state)
{
	(void)state;
	const unsigned int ranges[][2] = {
		{ 0, 1 }, { EZP_CHIP_SIZE - 1, 1 }, { EZP_READ_BLOCK - 1, 3 },
		{ EZP_READ_BLOCK, EZP_READ_BLOCK }, { 17, 8211 },
	};
	for (size_t i = 0; i < ARRAY_SIZE(ranges); i++) {
		struct ezp_transfer transfers[EZP_READ_TRANSFERS];
		struct ezp_io_state s = { .transfers = transfers, .count = ezp_read_transfers(transfers) };
		const struct io_mock io = ezp_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		ezp_init_probe(&flash);
		unsigned int start = ranges[i][0], len = ranges[i][1];
		uint8_t *buf = malloc(len + 2);
		assert_non_null(buf);
		memset(buf, 0xa7, len + 2);
		assert_int_equal(flash.mst->opaque.read(&flash, buf + 1, start, len), 0);
		assert_int_equal(buf[0], 0xa7);
		assert_int_equal(buf[len + 1], 0xa7);
		for (unsigned int j = 0; j < len; j++)
			assert_int_equal(buf[j + 1], ezp_read_byte(start + j));
		free(buf);
		assert_int_equal(programmer_shutdown(), 0);
		assert_int_equal(s.next, s.count);
		io_mock_register(NULL);
	}
}

void ezp_xpro_read_image_test(void **state)
{
	(void)state;
	struct ezp_transfer transfers[EZP_READ_TRANSFERS];
	struct ezp_io_state s = { .transfers = transfers, .count = ezp_read_transfers(transfers) };
	const struct io_mock io = ezp_io(&s);
	io_mock_register(&io);
	struct flashrom_programmer *prog = NULL;
	struct flashctx *flash = NULL;
	const char **names = NULL;
	assert_int_equal(flashrom_programmer_init(&prog, programmer_ezp_xpro.name, EZP_PARAMS), 0);
	assert_int_equal(flashrom_create_context(&flash), 0);
	assert_int_equal(flashrom_flash_probe_v2(flash, &names, prog, NULL), 1);
	assert_string_equal(names[0], "GD25Q80(B)");
	flashrom_data_free(names);
	uint8_t *buf = malloc(EZP_CHIP_SIZE);
	assert_non_null(buf);
	assert_int_equal(flashrom_image_read(flash, buf, EZP_CHIP_SIZE), 0);
	for (unsigned int i = 0; i < EZP_CHIP_SIZE; i++)
		assert_int_equal(buf[i], ezp_read_byte(i));
	free(buf);
	flashrom_flash_release(flash);
	assert_int_equal(flashrom_programmer_shutdown(prog), 0);
	assert_int_equal(s.next, s.count);
	io_mock_register(NULL);
}

void ezp_xpro_read_invalid_range_test(void **state)
{
	(void)state;
	struct ezp_io_state s = {
		.transfers = ezp_probe_transfers, .count = ARRAY_SIZE(ezp_probe_transfers),
	};
	const struct io_mock io = ezp_io(&s);
	io_mock_register(&io);
	struct flashchip chip = { 0 };
	struct flashctx flash = { .chip = &chip };
	assert_int_equal(programmer_init(&programmer_ezp_xpro, EZP_PARAMS), 0);
	assert_int_equal(registered_master_count, 1);
	flash.mst = &registered_masters[0];
	uint8_t byte = 0xa7;
	assert_int_not_equal(flash.mst->opaque.read(&flash, &byte, 0, 1), 0);
	assert_int_equal(s.next, 2);
	assert_int_equal(flash.mst->opaque.probe(&flash), 1);
	const unsigned int ranges[][2] = {
		{ EZP_CHIP_SIZE, 1 }, { EZP_CHIP_SIZE - 1, 2 }, { EZP_CHIP_SIZE + 1, 0 },
		{ UINT_MAX, 1 }, { 1, UINT_MAX },
	};
	for (size_t i = 0; i < ARRAY_SIZE(ranges); i++)
		assert_int_not_equal(flash.mst->opaque.read(&flash, &byte, ranges[i][0], ranges[i][1]), 0);
	assert_int_equal(flash.mst->opaque.read(&flash, &byte, 0, 0), 0);
	assert_int_equal(flash.mst->opaque.read(&flash, &byte, EZP_CHIP_SIZE, 0), 0);
	assert_int_equal(byte, 0xa7);
	assert_int_equal(s.next, 5);
	assert_int_equal(programmer_shutdown(), 0);
	assert_int_equal(s.next, s.count);
	io_mock_register(NULL);
}

void ezp_xpro_read_transfer_error_test(void **state)
{
	(void)state;
	const size_t failures[] = { 5, 6, 6 + 128, 6 + 255, EZP_READ_TRANSFERS - 2 };
	for (size_t i = 0; i < ARRAY_SIZE(failures); i++) {
		for (int short_transfer = 0; short_transfer < 2; short_transfer++) {
			struct ezp_transfer transfers[EZP_READ_TRANSFERS];
			ezp_read_transfers(transfers);
			size_t failed = failures[i];
			transfers[failed].error = short_transfer ? 0 : LIBUSB_ERROR_TIMEOUT;
			transfers[failed].missing = short_transfer ? 1 : 0;
			transfers[failed + 1] = ezp_probe_transfers[5];
			struct ezp_io_state s = { .transfers = transfers, .count = failed + 2 };
			const struct io_mock io = ezp_io(&s);
			io_mock_register(&io);
			struct flashchip chip = { 0 };
			struct flashctx flash = { .chip = &chip };
			ezp_init_probe(&flash);
			uint8_t buf[3] = { 0xa7, 0xa7, 0xa7 };
			assert_int_not_equal(flash.mst->opaque.read(&flash, buf + 1, 0, 1), 0);
			assert_int_equal(buf[0], 0xa7);
			assert_int_equal(buf[2], 0xa7);
			assert_int_equal(programmer_shutdown(), 0);
			assert_int_equal(s.next, s.count);
			io_mock_register(NULL);
		}
	}
}

enum ezp_memory_phase {
	EZP_IDLE, EZP_VERSION, EZP_ID, EZP_STREAM, EZP_SECTOR, EZP_READ_ACK,
	EZP_PAYLOAD, EZP_COMMIT, EZP_PROGRAM_ACK, EZP_ERASE_POLL,
	EZP_ARRAY_PAYLOAD, EZP_ARRAY_COMMIT, EZP_ARRAY_ACK,
};

struct ezp_memory {
	const struct ezp_example *example;
	uint8_t *contents;
	uint8_t *pending;
	enum ezp_memory_phase phase;
	uint8_t firmware, read_ack, program_ack;
	unsigned int address, stream_offset, sector_offset, payload_offset;
	unsigned int streams, calls, programs, sectors, payloads, erases, polls;
	unsigned int configures, identifications;
	uint8_t last_command;
	int fail_command;
	enum ezp_memory_phase fail_phase;
	unsigned int fail_stream;
	unsigned int fail_programs;
	unsigned int skip_fail_matches;
	bool short_transfer, faulted;
	int transfer_error;
	unsigned int corrupt_at;
	bool corrupt_sector, erase_nonblank, erase_stuck;
	bool alias_sector, alias_commit, check_address;
	unsigned int expected_address;
	unsigned int erase_timeouts, erase_busy;
	unsigned int array_starts, array_polls, array_timeouts, array_busy;
	bool array_stuck;
	bool forbid_erase;
};

static struct ezp_memory ezp_memory_new_example(const struct ezp_example *example)
{
	struct ezp_memory s = {
		.example = example,
		.forbid_erase = example->packet[2] == 2 || !strcmp(example->name, "W25X05"),
		.firmware = 0x03, .read_ack = 0xa5, .program_ack = 0xff,
		.fail_command = -1, .corrupt_at = UINT_MAX, .transfer_error = LIBUSB_ERROR_IO,
	};
	s.contents = malloc(example->size);
	s.pending = malloc(example->block ? example->block : example->size);
	assert_non_null(s.contents);
	assert_non_null(s.pending);
	for (unsigned int i = 0; i < example->size; i++)
		s.contents[i] = ezp_read_byte(i);
	return s;
}

static struct ezp_memory ezp_memory_new(void)
{
	return ezp_memory_new_example(&ezp_examples[0]);
}

static int ezp_memory_transfer(void *state, libusb_device_handle *devh,
		unsigned char endpoint, unsigned char *data, int length,
		int *actual_length, unsigned int timeout)
{
	struct ezp_memory *s = state;
	const struct ezp_example *example = s->example;
	s->calls++;
	assert_ptr_equal(devh, not_null());
	*actual_length = length;
	bool command = endpoint == 0x02;
	uint8_t opcode = command ? data[1] & 0x3f : 0;
	if (command) {
		assert_int_equal(length, 20);
		assert_int_equal(timeout, opcode == 0x01 ? 5000 : 1000);
	} else if (endpoint == 0x01) {
		unsigned int remaining = (s->phase == EZP_ARRAY_PAYLOAD ? example->size : example->block) -
			s->payload_offset;
		assert_int_equal(length, min(EZP_READ_BLOCK, remaining));
		assert_int_equal(timeout, 60000);
	} else {
		assert_int_equal(endpoint, 0x83);
		unsigned int expected_timeout = 1000;
		int expected_length = 1;
		switch (s->phase) {
		case EZP_ID: expected_length = 3; break;
		case EZP_STREAM: expected_length = EZP_READ_BLOCK; expected_timeout = 5000; break;
		case EZP_SECTOR:
			expected_length = min(1024, example->block - s->sector_offset);
			expected_timeout = 5000;
			break;
		case EZP_READ_ACK: expected_timeout = 50000; break;
		case EZP_PROGRAM_ACK: expected_timeout = 60000; break;
		case EZP_ARRAY_ACK: expected_timeout = 6000; break;
		default: break;
		}
		assert_int_equal(length, expected_length);
		assert_int_equal(timeout, expected_timeout);
	}
	bool fault_matches = (command && opcode == s->fail_command) ||
	    (!command && s->fail_phase != EZP_IDLE && s->phase == s->fail_phase &&
	     (!s->fail_stream || s->streams == s->fail_stream) &&
	     (!s->fail_programs || s->programs >= s->fail_programs));
	if (fault_matches && s->skip_fail_matches) {
		s->skip_fail_matches--;
		fault_matches = false;
	}
	if (!s->faulted && fault_matches) {
		s->faulted = true;
		*actual_length = s->short_transfer ? length - 1 : 0;
		return s->short_transfer ? 0 : s->transfer_error;
	}
	if (command) {
		assert_int_equal(length, 20);
		unsigned int address = 0;
		for (unsigned int i = 13; i < 17; i++)
			address = (address << 8) | data[i];
		ezp_check_example_packet(example, data, opcode, address);
		if (opcode != 0x13 && opcode != 0x02)
			assert_int_equal(address, 0);
		uint8_t previous_command = s->last_command;
		s->last_command = opcode;
		switch (opcode) {
		case 0x12:
			assert_int_equal(s->phase, EZP_IDLE);
			s->phase = EZP_VERSION;
			break;
		case 0x00:
			assert_int_equal(s->phase, EZP_IDLE);
			s->configures++;
			break;
		case 0x07:
			assert_int_equal(s->phase, EZP_IDLE);
			assert_int_not_equal(example->packet[2], 2);
			s->identifications++;
			s->phase = EZP_ID;
			break;
		case 0x0d:
			assert_int_equal(s->phase, EZP_IDLE);
			if (example->packet[2] == 2) {
				assert_int_equal(previous_command, 0x00);
				assert_true(s->configures > 1);
			}
			s->phase = EZP_STREAM;
			s->stream_offset = 0;
			s->streams++;
			break;
		case 0x13:
			assert_int_equal(s->phase, EZP_IDLE);
			if (example->packet[2] == 2)
				assert_true(previous_command == 0x00 || previous_command == 0x02);
			assert_true(example->block > 0);
			assert_int_equal(address % example->block, 0);
			assert_true(address <= example->size - example->block);
			if (s->check_address)
				assert_int_equal(address, s->expected_address);
			s->address = address;
			s->sector_offset = 0;
			s->payload_offset = 0;
			s->phase = EZP_SECTOR;
			break;
		case 0x02:
			if (s->phase == EZP_ARRAY_COMMIT) {
				assert_int_equal(address, example->size);
				memcpy(s->contents, s->pending, example->size);
				if (s->corrupt_at < example->size)
					s->contents[s->corrupt_at] ^= 1;
				s->programs++;
				s->phase = EZP_ARRAY_ACK;
				break;
			}
			assert_int_equal(s->phase, EZP_COMMIT);
			assert_int_equal(address, s->address);
			/* The simulated device applies only the received payload, independently of the requested range. */
			memcpy(s->contents + (s->alias_commit ? address & 0xffffff : address),
			       s->pending, example->block);
			if (s->check_address)
				s->expected_address += example->block;
			if (s->corrupt_at < example->size)
				s->contents[s->corrupt_at] ^= 1;
			s->programs++;
			s->phase = EZP_PROGRAM_ACK;
			break;
		case 0x05:
			assert_int_equal(s->phase, EZP_IDLE);
			assert_false(s->forbid_erase);
			s->erases++;
			s->phase = EZP_ERASE_POLL;
			break;
		case 0x01:
			assert_int_equal(s->phase, EZP_IDLE);
			assert_int_equal(previous_command, 0x00);
			assert_int_equal(example->packet[5], 0xff);
			for (unsigned int i = 0; i < example->size; i++)
				assert_int_equal(s->contents[i], 0xff);
			s->array_starts++;
			s->payload_offset = 0;
			s->phase = EZP_ARRAY_PAYLOAD;
			break;
		case 0x09:
			if (!s->faulted && s->phase == EZP_STREAM)
				assert_int_equal(s->stream_offset, example->size);
			s->phase = EZP_IDLE;
			break;
		default:
			fail_msg("Unexpected EZP command 0x%02x", data[1]);
		}
		return 0;
	}
	if (endpoint == 0x01) {
		if (s->phase == EZP_ARRAY_PAYLOAD) {
			assert_int_equal(length, min(EZP_READ_BLOCK, example->size - s->payload_offset));
			memcpy(s->pending + s->payload_offset, data, length);
			s->payload_offset += length;
			s->payloads++;
			if (s->payload_offset == example->size)
				s->phase = EZP_ARRAY_COMMIT;
			return 0;
		}
		assert_int_equal(s->phase, EZP_PAYLOAD);
		assert_int_equal(length, min(EZP_READ_BLOCK, example->block - s->payload_offset));
		assert_true(s->payload_offset <= example->block - length);
		memcpy(s->pending + s->payload_offset, data, length);
		s->payload_offset += length;
		s->payloads++;
		if (s->payload_offset == example->block)
			s->phase = EZP_COMMIT;
		return 0;
	}
	assert_int_equal(endpoint, 0x83);
	switch (s->phase) {
	case EZP_VERSION:
		assert_int_equal(length, 1);
		data[0] = s->firmware;
		s->phase = EZP_IDLE;
		break;
	case EZP_ID: {
		assert_int_equal(length, sizeof(example->id));
		memcpy(data, example->id, sizeof(example->id));
		s->phase = EZP_IDLE;
		break;
	}
	case EZP_STREAM:
		assert_int_equal(length, EZP_READ_BLOCK);
		assert_true(s->stream_offset <= example->size - EZP_READ_BLOCK);
		memcpy(data, s->contents + s->stream_offset, length);
		s->stream_offset += length;
		break;
	case EZP_SECTOR:
		assert_int_equal(length, min(1024, example->block - s->sector_offset));
		assert_true(s->sector_offset <= example->block - length);
		memcpy(data, s->contents + (s->alias_sector ? s->address & 0xffffff : s->address) +
		       s->sector_offset, length);
		if (s->corrupt_sector)
			data[0] ^= 1;
		s->sector_offset += length;
		s->sectors++;
		if (s->sector_offset == example->block)
			s->phase = EZP_READ_ACK;
		break;
	case EZP_READ_ACK:
		assert_int_equal(length, 1);
		data[0] = s->read_ack;
		s->phase = EZP_PAYLOAD;
		break;
	case EZP_PROGRAM_ACK:
		assert_int_equal(length, 1);
		data[0] = s->program_ack;
		s->phase = EZP_IDLE;
		break;
	case EZP_ERASE_POLL:
		assert_int_equal(length, 1);
		assert_int_equal(timeout, 1000);
		assert_true(++s->polls <= 600);
		if (s->erase_timeouts) {
			s->erase_timeouts--;
			*actual_length = 0;
			return LIBUSB_ERROR_TIMEOUT;
		}
		if (s->erase_stuck || s->erase_busy) {
			if (s->erase_busy)
				s->erase_busy--;
			data[0] = 0xa5;
			break;
		}
		data[0] = 0x06;
		memset(s->contents, 0xff, example->size);
		if (s->erase_nonblank)
			s->contents[example->size - 1] = 0;
		s->phase = EZP_IDLE;
		break;
	case EZP_ARRAY_ACK:
		assert_true(++s->array_polls <= 10);
		if (s->array_timeouts) {
			s->array_timeouts--;
			*actual_length = 0;
			return LIBUSB_ERROR_TIMEOUT;
		}
		if (s->array_stuck || s->array_busy) {
			if (s->array_busy)
				s->array_busy--;
			data[0] = 0x06;
			break;
		}
		data[0] = 0x02;
		s->phase = EZP_IDLE;
		break;
	default:
		fail_msg("Unexpected EZP receive phase %d", s->phase);
	}
	return 0;
}

static struct io_mock ezp_memory_io(struct ezp_memory *s)
{
	return (struct io_mock) { .state = s, .libusb_bulk_transfer = ezp_memory_transfer };
}

static void ezp_assert_locked(struct flashctx *flash, const struct ezp_memory *s)
{
	unsigned int calls = s->calls;
	uint8_t byte = 0;
	assert_int_not_equal(flash->mst->opaque.read(flash, &byte, 0, 1), 0);
	assert_int_not_equal(flash->mst->opaque.write(flash, &byte, 0, 1), 0);
	assert_int_not_equal(flash->mst->opaque.erase(flash, 0, s->example->size), 0);
	assert_int_equal(flash->mst->opaque.probe(flash), 0);
	assert_int_equal(s->calls, calls);
}

static void ezp_memory_finish(struct ezp_memory *s)
{
	assert_int_equal(programmer_shutdown(), 0);
	io_mock_register(NULL);
	free(s->contents);
	free(s->pending);
}

static void ezp_init_example(struct flashctx *flash, const struct ezp_example *example)
{
	char params[128];
	snprintf(params, sizeof(params), "chip=%s,voltage=%u", example->name,
		 example->packet[1] & 0x80 ? 5000 : 3300);
	assert_int_equal(programmer_init(&programmer_ezp_xpro, params), 0);
	assert_int_equal(registered_master_count, 1);
	flash->mst = &registered_masters[0];
	if (example->packet[2] == 2)
		flashrom_flag_set(flash, FLASHROM_FLAG_FORCE, true);
	assert_int_equal(flash->mst->opaque.probe(flash), 1);
	assert_string_equal(flash->chip->name, example->name);
	assert_int_equal(flash->chip->total_size * KiB, example->size);
	assert_int_equal(flash->chip->page_size, example->page);
	assert_int_equal(flash->chip->block_erasers[0].eraseblocks[0].size, example->size);
	assert_int_equal(flash->chip->block_erasers[0].eraseblocks[0].count, 1);
}

void ezp_xpro_profile_status_test(void **state)
{
	(void)state;
	for (size_t i = 0; i < 2 * (ARRAY_SIZE(ezp_examples) + ARRAY_SIZE(ezp_large_examples)); i++) {
		const struct ezp_example *example = i / 2 < ARRAY_SIZE(ezp_examples) ?
			&ezp_examples[i / 2] : &ezp_large_examples[i / 2 - ARRAY_SIZE(ezp_examples)];
		struct ezp_memory s = ezp_memory_new_example(example);
		s.firmware = i % 2 ? 0x04 : 0x03;
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		ezp_init_example(&flash, s.example);
		ezp_memory_finish(&s);
		assert_int_equal(chip.tested.probe, i / 2 == 0 ? OK : NT);
		assert_int_equal(chip.tested.read, i == 0 ? OK : NT);
		assert_int_equal(chip.tested.erase, i == 0 ? OK : NT);
		assert_int_equal(chip.tested.write, i == 0 ? OK : NT);
		assert_int_equal(chip.tested.wp, NA);
	}
}

static void ezp_assert_untested(const struct flashchip *chip)
{
	assert_int_equal(chip->tested.probe, NT);
	assert_int_equal(chip->tested.read, NT);
	assert_int_equal(chip->tested.erase, NT);
	assert_int_equal(chip->tested.write, NT);
	assert_int_equal(chip->tested.wp, NA);
}

void ezp_xpro_legacy_profiles_test(void **state)
{
	(void)state;
	for (size_t i = 0; i < ARRAY_SIZE(ezp_legacy_examples); i++) {
		struct ezp_memory s = ezp_memory_new_example(&ezp_legacy_examples[i]);
		const struct ezp_example *example = s.example;
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		ezp_init_example(&flash, example);
		ezp_assert_untested(&chip);
		uint8_t *expected = malloc(example->size);
		assert_non_null(expected);
		memcpy(expected, s.contents, example->size);
		const unsigned int ranges[][2] = { { example->block - 1, 3 }, { example->size - 1, 1 } };
		for (size_t j = 0; j < ARRAY_SIZE(ranges); j++) {
			unsigned int start = ranges[j][0], len = ranges[j][1];
			uint8_t bytes[3] = { 0x59, 0x12, 0x34 };
			memcpy(expected + start, bytes, len);
			s.check_address = true;
			s.expected_address = start / example->block * example->block;
			assert_int_equal(flash.mst->opaque.write(&flash, bytes, start, len), 0);
			assert_memory_equal(s.contents, expected, example->size);
		}
		assert_int_equal(s.programs, 3);
		assert_int_equal(s.array_starts, 0);
		uint8_t bytes[3];
		assert_int_equal(flash.mst->opaque.read(&flash, bytes, example->page - 1, sizeof(bytes)), 0);
		assert_memory_equal(bytes, expected + example->page - 1, sizeof(bytes));
		assert_int_equal(flash.mst->opaque.erase(&flash, 0, example->size), 0);
		memset(expected, 0xff, example->size);
		assert_memory_equal(s.contents, expected, example->size);
		assert_int_equal(s.erases, 1);
		assert_int_equal(s.configures, 1);
		free(expected);
		ezp_memory_finish(&s);
	}
}

void ezp_xpro_legacy_peer_ids_test(void **state)
{
	(void)state;
	const struct { const char *name; uint8_t id[3]; } peers[] = {
		{ "SST25VF016B", { 0xbf, 0x25, 0x41 } },
		{ "SST25VF040B", { 0xbf, 0x25, 0x8d } },
		{ "SST25VF040B.REMS", { 0xbf, 0x25, 0x8d } },
		{ "SST25VF080B", { 0xbf, 0x25, 0x8e } },
		{ "SST25VF032B", { 0xbf, 0x25, 0x4a } },
		{ "F25L008A", { 0x8c, 0x20, 0x14 } },
	};
	for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
		const struct ezp_example *source = NULL;
		for (size_t j = 0; j < ARRAY_SIZE(ezp_legacy_examples); j++)
			if (!strcmp(ezp_legacy_examples[j].name, peers[i].name))
				source = &ezp_legacy_examples[j];
		assert_non_null(source);
		struct ezp_example example = *source;
		memcpy(example.id, peers[i].id, sizeof(example.id));
		struct ezp_memory s = ezp_memory_new_example(&example);
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		ezp_init_example(&flash, &example);
		ezp_assert_untested(&chip);
		uint8_t byte = s.contents[123] ^ 0xff;
		assert_int_equal(flash.mst->opaque.write(&flash, &byte, 123, 1), 0);
		assert_int_equal(s.contents[123], byte);
		assert_int_equal(s.programs, 1);
		ezp_memory_finish(&s);
	}
}

void ezp_xpro_legacy_identity_guards_test(void **state)
{
	(void)state;
	for (size_t i = 0; i < ARRAY_SIZE(ezp_legacy_examples) + ARRAY_SIZE(ezp_array_examples); i++) {
		for (size_t byte = 0; byte < 3; byte++) {
			struct ezp_example example = i < ARRAY_SIZE(ezp_legacy_examples) ?
				ezp_legacy_examples[i] : ezp_array_examples[i - ARRAY_SIZE(ezp_legacy_examples)];
			example.id[byte] ^= 0x80;
			struct ezp_memory s = ezp_memory_new_example(&example);
			const struct io_mock io = ezp_memory_io(&s);
			io_mock_register(&io);
			char params[128];
			snprintf(params, sizeof(params), "chip=%s,voltage=3300", example.name);
			assert_int_equal(programmer_init(&programmer_ezp_xpro, params), 0);
			struct flashchip chip = { 0 };
			struct flashctx flash = { .chip = &chip, .mst = &registered_masters[0] };
			flashrom_flag_set(&flash, FLASHROM_FLAG_FORCE, true);
			assert_int_equal(flash.mst->opaque.probe(&flash), 0);
			unsigned int calls = s.calls;
			uint8_t data = 0;
			assert_int_not_equal(flash.mst->opaque.read(&flash, &data, 0, 1), 0);
			assert_int_not_equal(flash.mst->opaque.write(&flash, &data, 0, 1), 0);
			assert_int_not_equal(flash.mst->opaque.erase(&flash, 0, example.size), 0);
			assert_int_equal(s.calls, calls);
			ezp_memory_finish(&s);
		}
	}
	/* Force must also retain a real transport failure, rather than assuming a NOR identity. */
	for (unsigned int short_transfer = 0; short_transfer < 2; short_transfer++) {
		struct ezp_memory s = ezp_memory_new_example(&ezp_legacy_examples[0]);
		s.fail_phase = EZP_ID;
		s.short_transfer = short_transfer;
		s.transfer_error = LIBUSB_ERROR_TIMEOUT;
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		assert_int_equal(programmer_init(&programmer_ezp_xpro, "chip=Pm25LD010(C),voltage=3300"), 0);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip, .mst = &registered_masters[0] };
		flashrom_flag_set(&flash, FLASHROM_FLAG_FORCE, true);
		assert_int_equal(flash.mst->opaque.probe(&flash), 0);
		assert_true(s.faulted);
		ezp_assert_locked(&flash, &s);
		ezp_memory_finish(&s);
	}
}

void ezp_xpro_array_write_test(void **state)
{
	(void)state;
	for (size_t i = 0; i < ARRAY_SIZE(ezp_array_examples); i++) {
		for (unsigned int blank = 0; blank < 2; blank++) {
			struct ezp_memory s = ezp_memory_new_example(&ezp_array_examples[i]);
			if (blank)
				memset(s.contents, 0xff, s.example->size);
			s.array_timeouts = 2;
			s.array_busy = 2;
			const struct io_mock io = ezp_memory_io(&s);
			io_mock_register(&io);
			struct flashchip chip = { 0 };
			struct flashctx flash = { .chip = &chip };
			ezp_init_example(&flash, s.example);
			ezp_assert_untested(&chip);
			unsigned int calls = s.calls;
			uint8_t byte = 0;
			assert_int_equal(flash.mst->opaque.write(&flash, &byte, s.example->size, 0), 0);
			assert_int_not_equal(flash.mst->opaque.write(&flash, &byte, s.example->size, 1), 0);
			assert_int_not_equal(flash.mst->opaque.erase(&flash, 1, s.example->size - 1), 0);
			assert_int_equal(s.calls, calls);
			uint8_t *expected = malloc(s.example->size);
			assert_non_null(expected);
			memcpy(expected, s.contents, s.example->size);
			uint8_t bytes[7] = { 0x59, 0x12, 0x34, 0xff, 0x80, 0, 0xa5 };
			unsigned int start = EZP_READ_BLOCK - 3;
			memcpy(expected + start, bytes, sizeof(bytes));
			assert_int_equal(flash.mst->opaque.write(&flash, bytes, start, sizeof(bytes)), 0);
			assert_memory_equal(s.contents, expected, s.example->size);
			assert_int_equal(s.array_starts, 1);
			assert_int_equal(s.configures, 2);
			assert_int_equal(s.array_polls, 5);
			assert_int_equal(s.programs, 1);
			assert_int_equal(s.payloads, s.example->size / EZP_READ_BLOCK);
			assert_int_equal(s.payload_offset, s.example->size);
			assert_int_equal(s.sectors, 0);
			assert_int_equal(s.erases, blank ? 0 : 1);
			free(expected);
			ezp_memory_finish(&s);
		}
	}
}

void ezp_xpro_array_completion_test(void **state)
{
	(void)state;
	for (unsigned int mode = 0; mode < 4; mode++) {
		struct ezp_memory s = ezp_memory_new_example(&ezp_array_examples[0]);
		memset(s.contents, 0xff, s.example->size);
		s.array_busy = mode == 0 ? 9 : 0;
		s.array_timeouts = mode == 1 ? 10 : 0;
		s.array_stuck = mode == 2;
		s.corrupt_at = mode == 3 ? s.example->size - 1 : UINT_MAX;
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		ezp_init_example(&flash, s.example);
		uint8_t byte = 0x59;
		int ret = flash.mst->opaque.write(&flash, &byte, 123, 1);
		assert_int_equal(s.programs, 1);
		assert_int_equal(s.array_polls, mode == 3 ? 1 : 10);
		if (mode == 0) {
			assert_int_equal(ret, 0);
			assert_int_equal(s.contents[123], byte);
		} else {
			assert_int_not_equal(ret, 0);
			ezp_assert_locked(&flash, &s);
		}
		ezp_memory_finish(&s);
	}
}

void ezp_xpro_array_transfer_error_test(void **state)
{
	(void)state;
	const struct { int command; enum ezp_memory_phase phase; unsigned int skip, programs; } faults[] = {
		{ -1, EZP_STREAM, 0, 0 }, { 0x05, EZP_IDLE, 0, 0 },
		{ -1, EZP_ERASE_POLL, 0, 0 }, { 0x01, EZP_IDLE, 0, 0 },
		{ 0x00, EZP_IDLE, 1, 0 },
		{ -1, EZP_ARRAY_PAYLOAD, 0, 0 },
		{ -1, EZP_ARRAY_PAYLOAD, 256 * KiB / EZP_READ_BLOCK - 1, 0 },
		{ 0x02, EZP_IDLE, 0, 0 }, { -1, EZP_ARRAY_ACK, 0, 0 },
		{ -1, EZP_STREAM, 0, 1 },
	};
	for (size_t i = 0; i < ARRAY_SIZE(faults); i++) {
		for (unsigned int short_transfer = 0; short_transfer < 2; short_transfer++) {
			struct ezp_memory s = ezp_memory_new_example(&ezp_array_examples[0]);
			s.fail_command = faults[i].command;
			s.fail_phase = faults[i].phase;
			s.skip_fail_matches = faults[i].skip;
			s.fail_programs = faults[i].programs;
			s.short_transfer = short_transfer;
			const struct io_mock io = ezp_memory_io(&s);
			io_mock_register(&io);
			struct flashchip chip = { 0 };
			struct flashctx flash = { .chip = &chip };
			ezp_init_example(&flash, s.example);
			uint8_t byte = 0x59;
			assert_int_not_equal(flash.mst->opaque.write(&flash, &byte, 123, 1), 0);
			assert_true(s.faulted);
			assert_int_equal(s.sectors, 0);
			ezp_assert_locked(&flash, &s);
			ezp_memory_finish(&s);
		}
	}
}

void ezp_xpro_array_public_api_test(void **state)
{
	(void)state;
	struct ezp_memory s = ezp_memory_new_example(&ezp_array_examples[0]);
	const struct io_mock io = ezp_memory_io(&s);
	io_mock_register(&io);
	struct flashctx *flash = NULL;
	const char **names = NULL;
	assert_int_equal(programmer_init(&programmer_ezp_xpro, "chip=AT25DF021,voltage=3300"), 0);
	assert_int_equal(flashrom_create_context(&flash), 0);
	assert_int_equal(flashrom_flash_probe_v2(flash, &names, NULL, NULL), 1);
	flashrom_data_free(names);
	struct flashrom_layout *layout = NULL;
	assert_int_equal(flashrom_layout_new(&layout), 0);
	unsigned int start = EZP_READ_BLOCK - 1;
	assert_int_equal(flashrom_layout_add_region(layout, start, start + 2, "partial-array"), 0);
	assert_int_equal(flashrom_layout_include_region(layout, "partial-array"), 0);
	flashrom_layout_set(flash, layout);
	flashrom_flag_set(flash, FLASHROM_FLAG_VERIFY_AFTER_WRITE, true);
	flashrom_flag_set(flash, FLASHROM_FLAG_VERIFY_WHOLE_CHIP, true);
	uint8_t *image = malloc(s.example->size), *expected = malloc(s.example->size);
	assert_non_null(image);
	assert_non_null(expected);
	memcpy(expected, s.contents, s.example->size);
	memset(image, 0x96, s.example->size);
	/* Only 1->0 changes are requested; the fallback must erase its nonblank array itself. */
	for (unsigned int i = 0; i < 3; i++) {
		image[start + i] = s.contents[start + i] & 0x42;
		expected[start + i] = image[start + i];
	}
	assert_int_equal(flashrom_image_write(flash, image, s.example->size, NULL), 0);
	assert_true(s.array_starts > 0);
	assert_int_equal(s.configures, 1 + s.array_starts);
	assert_int_equal(s.erases, s.array_starts);
	assert_int_equal(s.sectors, 0);
	assert_memory_equal(s.contents, expected, s.example->size);
	free(expected);
	free(image);
	flashrom_layout_set(flash, NULL);
	flashrom_layout_release(layout);
	flashrom_flash_release(flash);
	ezp_memory_finish(&s);
}

static void ezp_assert_eeprom(const struct flashchip *chip, const struct ezp_example *example)
{
	assert_int_equal(chip->tested.probe, !strcmp(example->name, "M95320") ? NA : NT);
	assert_int_equal(chip->tested.read, NT);
	assert_int_equal(chip->tested.write, NT);
	assert_int_equal(chip->tested.erase, NT);
	assert_int_equal(chip->tested.wp, NA);
	assert_int_equal(chip->feature_bits, FEATURE_NO_ERASE | FEATURE_ERASED_ZERO);
	assert_int_equal(chip->gran, WRITE_GRAN_1BYTE_IMPLICIT_ERASE);
}

void ezp_xpro_eeprom_policy_test(void **state)
{
	(void)state;
	for (size_t i = 0; i < 2 * ARRAY_SIZE(ezp_eeprom_examples); i++) {
		struct ezp_example example = ezp_eeprom_examples[i / 2];
		example.packet[1] = i % 2 ? 0x80 : 0;
		struct ezp_memory s = ezp_memory_new_example(&example);
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		char params[128];
		snprintf(params, sizeof(params), "chip=%s,voltage=%u", example.name, i % 2 ? 5000 : 3300);
		assert_int_equal(programmer_init(&programmer_ezp_xpro, params), 0);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip, .mst = &registered_masters[0] };
		unsigned int calls = s.calls;
		assert_int_equal(flash.mst->opaque.probe(&flash), 0);
		uint8_t byte = 0;
		assert_int_not_equal(flash.mst->opaque.read(&flash, &byte, 0, 1), 0);
		assert_int_not_equal(flash.mst->opaque.write(&flash, &byte, 0, 1), 0);
		assert_int_not_equal(flash.mst->opaque.erase(&flash, 0, example.size), 0);
		assert_int_equal(s.calls, calls);
		assert_int_equal(s.configures, 0);
		flashrom_flag_set(&flash, FLASHROM_FLAG_FORCE, true);
		assert_int_equal(flash.mst->opaque.probe(&flash), 1);
		assert_string_equal(chip.name, example.name);
		assert_int_equal(chip.total_size * KiB, example.size);
		assert_int_equal(chip.page_size, example.page);
		ezp_assert_eeprom(&chip, &example);
		assert_int_equal(s.configures, 1);
		assert_int_equal(s.identifications, 0);
		assert_int_equal(flash.mst->opaque.read(&flash, &byte, example.size - 1, 1), 0);
		assert_int_equal(byte, ezp_read_byte(example.size - 1));
		assert_int_equal(s.configures, 2);
		ezp_memory_finish(&s);
	}
	for (size_t i = 0; i < ARRAY_SIZE(ezp_eeprom_examples); i++) {
		for (unsigned int short_transfer = 0; short_transfer < 2; short_transfer++) {
			struct ezp_memory s = ezp_memory_new_example(&ezp_eeprom_examples[i]);
			s.fail_command = 0x00;
			s.short_transfer = short_transfer;
			s.transfer_error = LIBUSB_ERROR_TIMEOUT;
			const struct io_mock io = ezp_memory_io(&s);
			io_mock_register(&io);
			char params[128];
			snprintf(params, sizeof(params), "chip=%s,voltage=3300", s.example->name);
			assert_int_equal(programmer_init(&programmer_ezp_xpro, params), 0);
			struct flashchip chip = { 0 };
			struct flashctx flash = { .chip = &chip, .mst = &registered_masters[0] };
			flashrom_flag_set(&flash, FLASHROM_FLAG_FORCE, true);
			assert_int_equal(flash.mst->opaque.probe(&flash), 0);
			assert_true(s.faulted);
			ezp_assert_locked(&flash, &s);
			ezp_memory_finish(&s);
		}
	}
}

void ezp_xpro_eeprom_ranges_test(void **state)
{
	(void)state;
	for (size_t i = 0; i < 2 * ARRAY_SIZE(ezp_eeprom_examples); i++) {
		struct ezp_example example = ezp_eeprom_examples[i / 2];
		example.packet[1] = i % 2 ? 0x80 : 0;
		struct ezp_memory s = ezp_memory_new_example(&example);
		unsigned int edge = example.page - 1;
		memset(s.contents + edge, 0, 3);
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		ezp_init_example(&flash, &example);
		ezp_assert_eeprom(&chip, &example);
		uint8_t *expected = malloc(example.size);
		assert_non_null(expected);
		memcpy(expected, s.contents, example.size);
		const unsigned int ranges[][2] = {
			{ edge, 3 }, { edge, 3 }, { 0, example.page }, { example.size - 1, 1 },
		};
		unsigned int programs = 0;
		for (size_t j = 0; j < ARRAY_SIZE(ranges); j++) {
			uint8_t bytes[256];
			unsigned int start = ranges[j][0], len = ranges[j][1];
			memset(bytes, j == 0 ? 0xff : j == 1 ? 0 : 0x96, len);
			memcpy(expected + start, bytes, len);
			s.check_address = true;
			s.expected_address = start / example.page * example.page;
			assert_int_equal(flash.mst->opaque.write(&flash, bytes, start, len), 0);
			assert_memory_equal(s.contents, expected, example.size);
			programs += (start + len - 1) / example.page - start / example.page + 1;
			assert_int_equal(s.programs, programs);
			assert_int_equal(s.sectors, programs);
			assert_int_equal(s.payloads, programs);
			uint8_t readback[258];
			memset(readback, 0xa7, sizeof(readback));
			assert_int_equal(flash.mst->opaque.read(&flash, readback + 1, start, len), 0);
			assert_int_equal(readback[0], 0xa7);
			assert_int_equal(readback[len + 1], 0xa7);
			assert_memory_equal(readback + 1, expected + start, len);
			assert_int_equal(s.configures, 1 + 4 * (j + 1));
		}
		unsigned int start = 2 * example.page - 1, len = example.page + 2;
		s.expected_address = start / example.page * example.page;
		assert_int_equal(flash.mst->opaque.erase(&flash, start, len), 0);
		memset(expected + start, 0, len);
		assert_memory_equal(s.contents, expected, example.size);
		assert_int_equal(s.programs, programs + 3);
		assert_int_equal(s.erases, 0);
		assert_int_equal(s.array_starts, 0);
		assert_int_equal(s.identifications, 0);
		assert_int_equal(s.configures, 4 * ARRAY_SIZE(ranges) + 4);
		unsigned int calls = s.calls;
		uint8_t byte;
		assert_int_equal(flash.mst->opaque.read(&flash, &byte, example.size, 0), 0);
		assert_int_equal(flash.mst->opaque.write(&flash, &byte, example.size, 0), 0);
		assert_int_equal(flash.mst->opaque.erase(&flash, example.size, 0), 0);
		const unsigned int invalid[][2] = {
			{ example.size, 1 }, { example.size - 1, 2 }, { UINT_MAX, 1 }, { 1, UINT_MAX },
		};
		for (size_t j = 0; j < ARRAY_SIZE(invalid); j++) {
			assert_int_not_equal(flash.mst->opaque.read(&flash, &byte, invalid[j][0], invalid[j][1]), 0);
			assert_int_not_equal(flash.mst->opaque.write(&flash, &byte, invalid[j][0], invalid[j][1]), 0);
			assert_int_not_equal(flash.mst->opaque.erase(&flash, invalid[j][0], invalid[j][1]), 0);
		}
		assert_int_equal(s.calls, calls);
		free(expected);
		ezp_memory_finish(&s);
	}
}

void ezp_xpro_eeprom_transfer_error_test(void **state)
{
	(void)state;
	const struct { int command; enum ezp_memory_phase phase; unsigned int skip, programs; } faults[] = {
		{ 0x0d, EZP_IDLE, 0, 0 }, { -1, EZP_STREAM, 0, 0 }, { 0x13, EZP_IDLE, 1, 0 },
		{ 0x00, EZP_IDLE, 1, 0 }, { 0x00, EZP_IDLE, 2, 0 }, { 0x00, EZP_IDLE, 3, 0 },
		{ -1, EZP_SECTOR, 1, 0 }, { -1, EZP_READ_ACK, 1, 0 },
		{ -1, EZP_PAYLOAD, 1, 0 }, { 0x02, EZP_IDLE, 1, 0 },
		{ -1, EZP_PROGRAM_ACK, 1, 0 }, { -1, EZP_STREAM, 0, 1 },
	};
	for (size_t i = 0; i < ARRAY_SIZE(ezp_eeprom_examples); i++) {
		for (size_t j = 0; j < ARRAY_SIZE(faults); j++) {
			for (unsigned int short_transfer = 0; short_transfer < 2; short_transfer++) {
				struct ezp_memory s = ezp_memory_new_example(&ezp_eeprom_examples[i]);
				s.fail_command = faults[j].command;
				s.fail_phase = faults[j].phase;
				s.skip_fail_matches = faults[j].skip;
				s.fail_programs = faults[j].programs;
				s.short_transfer = short_transfer;
				s.transfer_error = LIBUSB_ERROR_TIMEOUT;
				const struct io_mock io = ezp_memory_io(&s);
				io_mock_register(&io);
				struct flashchip chip = { 0 };
				struct flashctx flash = { .chip = &chip };
				ezp_init_example(&flash, s.example);
				uint8_t bytes[3] = { 0x59, 0x12, 0x34 };
				assert_int_not_equal(flash.mst->opaque.write(&flash, bytes, s.example->page - 1, 3), 0);
				assert_true(s.faulted);
				assert_int_equal(s.erases, 0);
				ezp_assert_locked(&flash, &s);
				ezp_memory_finish(&s);
			}
		}
	}
}

void ezp_xpro_eeprom_erase_failure_test(void **state)
{
	(void)state;
	for (size_t i = 0; i < ARRAY_SIZE(ezp_eeprom_examples); i++) {
		for (unsigned int mode = 0; mode < 3; mode++) {
			struct ezp_memory s = ezp_memory_new_example(&ezp_eeprom_examples[i]);
			s.corrupt_at = mode == 0 ? s.example->size - 1 : UINT_MAX;
			if (mode) {
				s.fail_phase = EZP_PAYLOAD;
				s.skip_fail_matches = 1;
				s.short_transfer = mode == 1;
			}
			const struct io_mock io = ezp_memory_io(&s);
			io_mock_register(&io);
			struct flashchip chip = { 0 };
			struct flashctx flash = { .chip = &chip };
			ezp_init_example(&flash, s.example);
			assert_int_not_equal(flash.mst->opaque.erase(&flash,
				mode == 0 ? s.example->page + 1 : s.example->page - 1, mode == 0 ? 1 : 3), 0);
			assert_int_equal(s.erases, 0);
			ezp_assert_locked(&flash, &s);
			ezp_memory_finish(&s);
		}
	}
}

void ezp_xpro_eeprom_public_api_test(void **state)
{
	(void)state;
	for (size_t i = 0; i < ARRAY_SIZE(ezp_eeprom_examples); i++) {
		struct ezp_memory s = ezp_memory_new_example(&ezp_eeprom_examples[i]);
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		struct flashctx *flash = NULL;
		const char **names = NULL;
		char params[128];
		snprintf(params, sizeof(params), "chip=%s,voltage=3300", s.example->name);
		assert_int_equal(programmer_init(&programmer_ezp_xpro, params), 0);
		assert_int_equal(flashrom_create_context(&flash), 0);
		flashrom_flag_set(flash, FLASHROM_FLAG_FORCE, true);
		assert_int_equal(flashrom_flash_probe_v2(flash, &names, NULL, NULL), 1);
		flashrom_data_free(names);
		ezp_assert_eeprom(flash->chip, s.example);
		assert_int_equal(flash->chip->read, READ_OPAQUE);
		assert_int_equal(flash->chip->write, WRITE_OPAQUE);
		assert_int_equal(flash->chip->block_erasers[0].block_erase, OPAQUE_ERASE);
		struct flashrom_layout *layout = NULL;
		assert_int_equal(flashrom_layout_new(&layout), 0);
		unsigned int start = s.example->page - 1;
		assert_int_equal(flashrom_layout_add_region(layout, start, start + 2, "eeprom-page-edge"), 0);
		assert_int_equal(flashrom_layout_include_region(layout, "eeprom-page-edge"), 0);
		flashrom_layout_set(flash, layout);
		flashrom_flag_set(flash, FLASHROM_FLAG_VERIFY_AFTER_WRITE, true);
		flashrom_flag_set(flash, FLASHROM_FLAG_VERIFY_WHOLE_CHIP, true);
		uint8_t *image = malloc(s.example->size), *expected = malloc(s.example->size);
		assert_non_null(image);
		assert_non_null(expected);
		memcpy(expected, s.contents, s.example->size);
		memset(image, 0x96, s.example->size);
		for (unsigned int j = 0; j < 3; j++) {
			expected[start + j] ^= 0xff;
			image[start + j] = expected[start + j];
		}
		assert_int_equal(flashrom_image_write(flash, image, s.example->size, NULL), 0);
		assert_memory_equal(s.contents, expected, s.example->size);
		assert_int_equal(flashrom_image_verify(flash, image, s.example->size), 0);
		flashrom_layout_set(flash, NULL);
		assert_int_equal(flashrom_flash_erase(flash), 0);
		memset(expected, 0, s.example->size);
		assert_memory_equal(s.contents, expected, s.example->size);
		assert_int_equal(s.erases, 0);
		assert_int_equal(s.array_starts, 0);
		assert_int_equal(s.identifications, 0);
		free(expected);
		free(image);
		flashrom_layout_release(layout);
		flashrom_flash_release(flash);
		ezp_memory_finish(&s);
	}
}

void ezp_xpro_no_chip_erase_test(void **state)
{
	(void)state;
	struct ezp_memory s = ezp_memory_new_example(&ezp_no_chip_erase_example);
	const struct io_mock io = ezp_memory_io(&s);
	io_mock_register(&io);
	struct flashchip chip = { 0 };
	struct flashctx flash = { .chip = &chip };
	ezp_init_example(&flash, s.example);
	ezp_assert_untested(&chip);
	uint8_t *expected = malloc(s.example->size);
	assert_non_null(expected);
	memcpy(expected, s.contents, s.example->size);
	uint8_t bytes[3] = { 0x59, 0x12, 0x34 }, readback[3];
	unsigned int start = s.example->block - 1;
	assert_int_equal(flash.mst->opaque.read(&flash, readback, start, 3), 0);
	assert_memory_equal(readback, expected + start, 3);
	assert_int_equal(flash.mst->opaque.write(&flash, bytes, start, 3), 0);
	memcpy(expected + start, bytes, 3);
	assert_memory_equal(s.contents, expected, s.example->size);
	assert_int_equal(flash.mst->opaque.erase(&flash, start, 3), 0);
	memset(expected + start, 0xff, 3);
	assert_memory_equal(s.contents, expected, s.example->size);
	assert_int_equal(flash.mst->opaque.erase(&flash, 0, s.example->size), 0);
	memset(expected, 0xff, s.example->size);
	assert_memory_equal(s.contents, expected, s.example->size);
	assert_int_equal(s.erases, 0);
	assert_int_equal(s.array_starts, 0);
	assert_int_equal(s.programs, 4 + s.example->size / s.example->block);
	free(expected);
	ezp_memory_finish(&s);
}

void ezp_xpro_large_read_ranges_test(void **state)
{
	(void)state;
	/* Both high address bits must affect the data; a repeating 16 MiB pattern hides aliases. */
	assert_int_not_equal(ezp_read_byte(123), ezp_read_byte(16 * MiB + 123));
	assert_int_not_equal(ezp_read_byte(123), ezp_read_byte(32 * MiB + 123));
	assert_int_not_equal(ezp_read_byte(16 * MiB + 123), ezp_read_byte(32 * MiB + 123));
	for (size_t i = 0; i < ARRAY_SIZE(ezp_large_examples); i++) {
		struct ezp_memory s = ezp_memory_new_example(&ezp_large_examples[i]);
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		ezp_init_example(&flash, s.example);
		assert_int_equal(chip.tested.probe, NT);
		assert_int_equal(chip.tested.read, NT);
		assert_int_equal(chip.tested.erase, NT);
		assert_int_equal(chip.tested.write, NT);
		assert_int_equal(chip.tested.wp, NA);
		const unsigned int ranges[][2] = {
			{ 16 * MiB - 257, 515 },
			{ s.example->size > 32 * MiB ? 32 * MiB - 257 : 16 * MiB + 123, 515 },
			{ s.example->size - 1, 1 },
		};
		for (size_t j = 0; j < ARRAY_SIZE(ranges); j++) {
			uint8_t buf[517];
			memset(buf, 0xa7, sizeof(buf));
			unsigned int start = ranges[j][0], len = ranges[j][1];
			assert_int_equal(flash.mst->opaque.read(&flash, buf + 1, start, len), 0);
			assert_int_equal(buf[0], 0xa7);
			assert_int_equal(buf[len + 1], 0xa7);
			for (unsigned int k = 0; k < len; k++)
				assert_int_equal(buf[k + 1], ezp_read_byte(start + k));
			assert_int_equal(s.stream_offset, s.example->size);
			assert_int_equal(s.streams, j + 1);
		}
		unsigned int calls = s.calls;
		uint8_t byte;
		assert_int_not_equal(flash.mst->opaque.read(&flash, &byte, s.example->size, 1), 0);
		assert_int_not_equal(flash.mst->opaque.read(&flash, &byte, s.example->size - 1, 2), 0);
		assert_int_equal(s.calls, calls);
		ezp_memory_finish(&s);
	}
	/* flashrom's GD25Q512 is C84010, 64 KiB, not the vendor's C84020, 64 MiB row. */
	struct ezp_example small = {
		"GD25Q512", 64 * KiB, 256, 4 * KiB, { 0xc8, 0x40, 0x10 },
		{ 0xa5, 0, 0, 0, 8, 2, 0, 1, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a },
	};
	struct ezp_memory s = ezp_memory_new_example(&small);
	const struct io_mock io = ezp_memory_io(&s);
	io_mock_register(&io);
	struct flashchip chip = { 0 };
	struct flashctx flash = { .chip = &chip };
	ezp_init_example(&flash, &small);
	uint8_t byte;
	assert_int_equal(flash.mst->opaque.read(&flash, &byte, small.size - 1, 1), 0);
	assert_int_equal(byte, ezp_read_byte(small.size - 1));
	ezp_memory_finish(&s);
	/* The similarly named vendor large chip must not pass this exact-ID probe. */
	small.id[2] = 0x20;
	s = ezp_memory_new_example(&small);
	const struct io_mock mismatch_io = ezp_memory_io(&s);
	io_mock_register(&mismatch_io);
	assert_int_equal(programmer_init(&programmer_ezp_xpro, "chip=GD25Q512,voltage=3300"), 0);
	flash.mst = &registered_masters[0];
	assert_int_equal(flash.mst->opaque.probe(&flash), 0);
	unsigned int calls = s.calls;
	assert_int_not_equal(flash.mst->opaque.read(&flash, &byte, 0, 1), 0);
	assert_int_not_equal(flash.mst->opaque.write(&flash, &byte, 0, 1), 0);
	assert_int_not_equal(flash.mst->opaque.erase(&flash, 0, small.size), 0);
	assert_int_equal(s.calls, calls);
	ezp_memory_finish(&s);
}

void ezp_xpro_large_write_ranges_test(void **state)
{
	(void)state;
	for (size_t i = 0; i < ARRAY_SIZE(ezp_large_examples); i++) {
		struct ezp_memory s = ezp_memory_new_example(&ezp_large_examples[i]);
		const struct ezp_example *example = s.example;
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		ezp_init_example(&flash, example);
		const unsigned int ranges[][2] = {
			{ 0, 256 }, { 16 * MiB - 17, 33 },
			{ example->size > 32 * MiB ? 32 * MiB - 17 : 16 * MiB + 255, 33 },
			{ example->size - 256, 256 },
		};
		uint8_t *expected = malloc(example->size);
		assert_non_null(expected);
		memcpy(expected, s.contents, example->size);
		unsigned int programs = 0;
		for (size_t j = 0; j < ARRAY_SIZE(ranges); j++) {
			unsigned int start = ranges[j][0], len = ranges[j][1];
			uint8_t buf[256];
			for (unsigned int k = 0; k < len; k++)
				buf[k] = expected[start + k] ^ 0xff;
			memcpy(expected + start, buf, len);
			s.check_address = true;
			s.expected_address = start / example->block * example->block;
			assert_int_equal(flash.mst->opaque.write(&flash, buf, start, len), 0);
			assert_memory_equal(s.contents, expected, example->size);
			programs += (start + len - 1) / example->block - start / example->block + 1;
			assert_int_equal(s.programs, programs);
			assert_int_equal(s.sectors, programs * 4);
			assert_int_equal(s.payloads, programs);
			assert_int_equal(s.streams, 2 * (j + 1));
		}
		free(expected);
		ezp_memory_finish(&s);
	}
}

void ezp_xpro_large_corruption_test(void **state)
{
	(void)state;
	for (size_t i = 0; i < 2; i++) {
		for (unsigned int mode = 0; mode < 4; mode++) {
			struct ezp_memory s = ezp_memory_new_example(&ezp_large_examples[i]);
			s.alias_sector = mode == 0;
			s.alias_commit = mode == 1;
			s.corrupt_at = mode == 2 ? s.example->size - 1 : UINT_MAX;
			s.erase_nonblank = mode == 3;
			const struct io_mock io = ezp_memory_io(&s);
			io_mock_register(&io);
			struct flashchip chip = { 0 };
			struct flashctx flash = { .chip = &chip };
			ezp_init_example(&flash, s.example);
			unsigned int start = (i ? 32 : 16) * MiB + 123;
			uint8_t before = s.contents[start], byte = before ^ 0xff;
			if (mode == 3) {
				assert_int_not_equal(flash.mst->opaque.erase(&flash, 0, s.example->size), 0);
				assert_int_equal(s.polls, 1);
				assert_int_equal(s.stream_offset, s.example->size);
				assert_int_equal(s.contents[s.example->size - 1], 0);
			} else {
				assert_int_not_equal(flash.mst->opaque.write(&flash, &byte, start, 1), 0);
				assert_int_equal(s.programs, mode == 0 ? 0 : 1);
				assert_int_equal(s.streams, mode == 0 ? 1 : 2);
				if (mode == 1) {
					assert_int_equal(s.contents[start], before);
					assert_int_equal(s.contents[start & 0xffffff], byte);
				}
			}
			ezp_assert_locked(&flash, &s);
			ezp_memory_finish(&s);
		}
	}
}

void ezp_xpro_large_transfer_error_test(void **state)
{
	(void)state;
	for (size_t i = 0; i < 2; i++) {
		unsigned int boundary = (i ? 32 : 16) * MiB;
		const struct { int command; enum ezp_memory_phase phase; unsigned int stream, skip; } faults[] = {
			{ -1, EZP_STREAM, 1, boundary / EZP_READ_BLOCK },
			{ -1, EZP_STREAM, 2, boundary / EZP_READ_BLOCK },
			{ -1, EZP_SECTOR, 0, 4 },
			{ -1, EZP_PAYLOAD, 0, 1 },
			{ 0x02, EZP_IDLE, 0, 1 },
		};
		for (size_t j = 0; j < ARRAY_SIZE(faults); j++) {
			for (unsigned int short_transfer = 0; short_transfer < 2; short_transfer++) {
				struct ezp_memory s = ezp_memory_new_example(&ezp_large_examples[i]);
				s.fail_command = faults[j].command;
				s.fail_phase = faults[j].phase;
				s.fail_stream = faults[j].stream;
				s.skip_fail_matches = faults[j].skip;
				s.short_transfer = short_transfer;
				s.transfer_error = LIBUSB_ERROR_TIMEOUT;
				const struct io_mock io = ezp_memory_io(&s);
				io_mock_register(&io);
				struct flashchip chip = { 0 };
				struct flashctx flash = { .chip = &chip };
				ezp_init_example(&flash, s.example);
				uint8_t buf[3] = { 0x59, 0x12, 0x34 };
				if (j == 0) {
					assert_int_not_equal(flash.mst->opaque.read(&flash, buf, boundary - 1, 3), 0);
					assert_int_equal(s.programs, 0);
				} else {
					assert_int_not_equal(flash.mst->opaque.write(&flash, buf, boundary - 1, 3), 0);
					assert_int_equal(s.programs, j == 1 ? 2 : 1);
				}
				assert_true(s.faulted);
				if (j < 2)
					assert_int_equal(s.stream_offset, boundary);
				ezp_assert_locked(&flash, &s);
				ezp_memory_finish(&s);
			}
		}
	}
}

void ezp_xpro_large_public_api_test(void **state)
{
	(void)state;
	struct ezp_memory s = ezp_memory_new_example(&ezp_large_examples[1]);
	memset(s.contents, 0xff, s.example->size);
	const struct io_mock io = ezp_memory_io(&s);
	io_mock_register(&io);
	struct flashctx *flash = NULL;
	const char **names = NULL;
	assert_int_equal(programmer_init(&programmer_ezp_xpro, "chip=W25Q512JV,voltage=3300"), 0);
	assert_int_equal(flashrom_create_context(&flash), 0);
	assert_int_equal(flashrom_flash_probe_v2(flash, &names, NULL, NULL), 1);
	flashrom_data_free(names);
	assert_int_equal(flash->chip->tested.write, NT);
	struct flashrom_layout *layout = NULL;
	assert_int_equal(flashrom_layout_new(&layout), 0);
	unsigned int start = 32 * MiB - 1;
	assert_int_equal(flashrom_layout_add_region(layout, start, start + 2, "upper-boundary"), 0);
	assert_int_equal(flashrom_layout_include_region(layout, "upper-boundary"), 0);
	flashrom_layout_set(flash, layout);
	flashrom_flag_set(flash, FLASHROM_FLAG_VERIFY_AFTER_WRITE, true);
	flashrom_flag_set(flash, FLASHROM_FLAG_VERIFY_WHOLE_CHIP, true);
	uint8_t *image = malloc(s.example->size);
	assert_non_null(image);
	memcpy(image, s.contents, s.example->size);
	memset(image + start, 0x42, 3);
	assert_int_equal(flashrom_image_write(flash, image, s.example->size, NULL), 0);
	assert_int_equal(s.programs, 2);
	assert_int_equal(s.erases, 0);
	assert_memory_equal(s.contents, image, s.example->size);
	assert_int_equal(flashrom_image_verify(flash, image, s.example->size), 0);
	s.contents[start + 1] ^= 1;
	assert_int_not_equal(flashrom_image_verify(flash, image, s.example->size), 0);
	flashrom_layout_set(flash, NULL);
	assert_int_equal(flashrom_flash_erase(flash), 0);
	assert_int_equal(s.erases, 1);
	memset(image, 0xff, s.example->size);
	assert_memory_equal(s.contents, image, s.example->size);
	free(image);
	flashrom_layout_release(layout);
	flashrom_flash_release(flash);
	ezp_memory_finish(&s);
}

void ezp_xpro_profile_transfer_error_test(void **state)
{
	(void)state;
	for (size_t i = 0; i < ARRAY_SIZE(ezp_examples); i++) {
		for (unsigned int phase = 0; phase < 2; phase++) {
			for (unsigned int error = 0; error < 3; error++) {
				struct ezp_memory s = ezp_memory_new_example(&ezp_examples[i]);
				s.fail_phase = phase ? EZP_PAYLOAD : EZP_SECTOR;
				s.skip_fail_matches = s.example->block / (phase ? 4096 : 1024) - 1;
				s.short_transfer = error == 0;
				s.transfer_error = error == 1 ? LIBUSB_ERROR_TIMEOUT : LIBUSB_ERROR_IO;
				const struct io_mock io = ezp_memory_io(&s);
				io_mock_register(&io);
				struct flashchip chip = { 0 };
				struct flashctx flash = { .chip = &chip };
				ezp_init_example(&flash, s.example);
				uint8_t *before = malloc(s.example->size);
				assert_non_null(before);
				memcpy(before, s.contents, s.example->size);
				uint8_t byte = 0x59;
				assert_int_not_equal(flash.mst->opaque.write(&flash, &byte, s.example->size - 1, 1), 0);
				assert_true(s.faulted);
				assert_int_equal(s.programs, 0);
				assert_int_equal(s.sectors, s.example->block / 1024 - (phase ? 0 : 1));
				assert_int_equal(s.payloads, phase ? s.example->block / 4096 - 1 : 0);
				assert_memory_equal(before, s.contents, s.example->size);
				free(before);
				ezp_assert_locked(&flash, &s);
				ezp_memory_finish(&s);
			}
		}
	}
}

void ezp_xpro_profile_write_ranges_test(void **state)
{
	(void)state;
	for (size_t i = 0; i < ARRAY_SIZE(ezp_examples); i++) {
		struct ezp_memory s = ezp_memory_new_example(&ezp_examples[i]);
		const struct ezp_example *example = s.example;
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		ezp_init_example(&flash, example);
		const unsigned int ranges[][2] = {
			{ example->page - 1, 3 },
			{ example->block - 7, 2 * example->block + 19 },
			{ example->size - 1, 1 },
		};
		uint8_t *expected = malloc(example->size);
		uint8_t *buf = malloc(2 * example->block + 19);
		assert_non_null(expected);
		assert_non_null(buf);
		memcpy(expected, s.contents, example->size);
		unsigned int programs = 0;
		for (size_t j = 0; j < ARRAY_SIZE(ranges); j++) {
			unsigned int start = ranges[j][0], len = ranges[j][1];
			for (unsigned int k = 0; k < len; k++)
				buf[k] = 0xff ^ (k * 23 + j);
			memcpy(expected + start, buf, len);
			assert_int_equal(flash.mst->opaque.write(&flash, buf, start, len), 0);
			/* Check the entire chip so preserved bytes and later payload chunks cannot be lost. */
			assert_memory_equal(s.contents, expected, example->size);
			programs += (start + len - 1) / example->block - start / example->block + 1;
			assert_int_equal(s.programs, programs);
			assert_int_equal(s.sectors, programs * example->block / 1024);
			assert_int_equal(s.payloads, programs * example->block / EZP_READ_BLOCK);
			assert_int_equal(s.streams, 2 * (j + 1));
		}
		free(buf);
		free(expected);
		ezp_memory_finish(&s);
	}
}

void ezp_xpro_write_ranges_test(void **state)
{
	(void)state;
	const unsigned int ranges[][2] = {
		{ 0, 1 }, { EZP_CHIP_SIZE - 1, 1 }, { 255, 3 },
		{ EZP_READ_BLOCK - 1, 3 }, { EZP_READ_BLOCK - 7, 2 * EZP_READ_BLOCK + 19 },
		{ 123, 1 },
	};
	for (size_t i = 0; i < ARRAY_SIZE(ranges); i++) {
		struct ezp_memory s = ezp_memory_new();
		s.read_ack = i;
		s.program_ack = 0xff - i;
		if (i == ARRAY_SIZE(ranges) - 1)
			memset(s.contents, 0, EZP_CHIP_SIZE);
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		ezp_init_probe(&flash);
		unsigned int start = ranges[i][0], len = ranges[i][1];
		uint8_t *expected = malloc(EZP_CHIP_SIZE), *buf = malloc(len);
		assert_non_null(expected);
		assert_non_null(buf);
		memcpy(expected, s.contents, EZP_CHIP_SIZE);
		for (unsigned int j = 0; j < len; j++)
			buf[j] = 0xff ^ (j * 23);
		memcpy(expected + start, buf, len);
		assert_int_equal(flash.mst->opaque.write(&flash, buf, start, len), 0);
		assert_memory_equal(s.contents, expected, EZP_CHIP_SIZE);
		assert_int_equal(s.streams, 2);
		assert_int_equal(s.programs, (start + len - 1) / EZP_READ_BLOCK - start / EZP_READ_BLOCK + 1);
		assert_int_equal(s.payloads, s.programs);
		free(buf);
		free(expected);
		ezp_memory_finish(&s);
	}
}

void ezp_xpro_image_write_verify_erase_test(void **state)
{
	(void)state;
	struct ezp_memory s = ezp_memory_new();
	memset(s.contents, 0xff, EZP_CHIP_SIZE);
	const struct io_mock io = ezp_memory_io(&s);
	io_mock_register(&io);
	struct flashctx *flash = NULL;
	const char **names = NULL;
	assert_int_equal(programmer_init(&programmer_ezp_xpro, EZP_PARAMS), 0);
	assert_int_equal(flashrom_create_context(&flash), 0);
	assert_int_equal(flashrom_flash_probe_v2(flash, &names, NULL, NULL), 1);
	flashrom_data_free(names);
	assert_int_equal(flash->chip->gran, WRITE_GRAN_256BYTES);
	uint8_t *image = malloc(EZP_CHIP_SIZE);
	assert_non_null(image);
	memcpy(image, s.contents, EZP_CHIP_SIZE);
	image[17] = 0xa5;
	image[19] = 0x5a;
	image[23] = 0;
	image[2 * 256 + 9] = 0x34;
	image[EZP_READ_BLOCK + 5] = 0x12;
	image[EZP_CHIP_SIZE - 17] = 0x55;
	flashrom_flag_set(flash, FLASHROM_FLAG_VERIFY_AFTER_WRITE, true);
	flashrom_flag_set(flash, FLASHROM_FLAG_VERIFY_WHOLE_CHIP, true);
	assert_int_equal(flashrom_image_write(flash, image, EZP_CHIP_SIZE, NULL), 0);
	assert_memory_equal(s.contents, image, EZP_CHIP_SIZE);
	assert_int_equal(s.programs, 4);
	assert_int_equal(s.erases, 0);
	assert_int_equal(flashrom_image_verify(flash, image, EZP_CHIP_SIZE), 0);
	s.contents[EZP_CHIP_SIZE - 1] ^= 1;
	assert_int_not_equal(flashrom_image_verify(flash, image, EZP_CHIP_SIZE), 0);
	assert_int_equal(flashrom_flash_erase(flash), 0);
	memset(image, 0xff, EZP_CHIP_SIZE);
	assert_memory_equal(s.contents, image, EZP_CHIP_SIZE);
	assert_int_equal(s.erases, 1);
	free(image);
	flashrom_flash_release(flash);
	ezp_memory_finish(&s);
}

void ezp_xpro_partial_layout_auto_erase_test(void **state)
{
	(void)state;
	struct ezp_memory s = ezp_memory_new();
	const unsigned int start = EZP_READ_BLOCK - 1, len = 3;
	s.contents[start] = 0;
	const struct io_mock io = ezp_memory_io(&s);
	io_mock_register(&io);
	struct flashctx *flash = NULL;
	const char **names = NULL;
	assert_int_equal(programmer_init(&programmer_ezp_xpro, EZP_PARAMS), 0);
	assert_int_equal(flashrom_create_context(&flash), 0);
	assert_int_equal(flashrom_flash_probe_v2(flash, &names, NULL, NULL), 1);
	flashrom_data_free(names);
	struct flashrom_layout *layout = NULL;
	assert_int_equal(flashrom_layout_new(&layout), 0);
	assert_int_equal(flashrom_layout_add_region(layout, start, start + len - 1, "update"), 0);
	assert_int_equal(flashrom_layout_include_region(layout, "update"), 0);
	flashrom_layout_set(flash, layout);
	flashrom_flag_set(flash, FLASHROM_FLAG_VERIFY_AFTER_WRITE, true);
	flashrom_flag_set(flash, FLASHROM_FLAG_VERIFY_WHOLE_CHIP, true);
	uint8_t *image = malloc(EZP_CHIP_SIZE), *expected = malloc(EZP_CHIP_SIZE);
	assert_non_null(image);
	assert_non_null(expected);
	memcpy(expected, s.contents, EZP_CHIP_SIZE);
	/* Excluded image bytes deliberately disagree with the chip; only the selected range may change. */
	memset(image, 0x96, EZP_CHIP_SIZE);
	memset(image + start, 0xff, len);
	memset(expected + start, 0xff, len);
	assert_int_equal(flashrom_image_write(flash, image, EZP_CHIP_SIZE, NULL), 0);
	assert_int_equal(s.erases, 1);
	assert_true(s.programs > 0);
	assert_memory_equal(s.contents, expected, EZP_CHIP_SIZE);
	free(expected);
	free(image);
	flashrom_layout_set(flash, NULL);
	flashrom_layout_release(layout);
	flashrom_flash_release(flash);
	ezp_memory_finish(&s);
}

void ezp_xpro_destructive_guards_test(void **state)
{
	(void)state;
	const uint8_t firmware[] = { 0x03, 0x00, 0x02, 0x04, 0x10, 0xfe };
	for (size_t i = 0; i < ARRAY_SIZE(firmware); i++) {
		struct ezp_memory s = ezp_memory_new();
		s.firmware = firmware[i];
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		assert_int_equal(programmer_init(&programmer_ezp_xpro, EZP_PARAMS), 0);
		assert_int_equal(registered_master_count, 1);
		flash.mst = &registered_masters[0];
		uint8_t byte = 0;
		unsigned int calls = s.calls;
		assert_int_not_equal(flash.mst->opaque.write(&flash, &byte, 0, 1), 0);
		assert_int_not_equal(flash.mst->opaque.erase(&flash, 0, EZP_CHIP_SIZE), 0);
		assert_int_equal(s.calls, calls);
		assert_int_equal(flash.mst->opaque.probe(&flash), 1);
		calls = s.calls;
		const unsigned int ranges[][2] = {
			{ EZP_CHIP_SIZE, 1 }, { EZP_CHIP_SIZE - 1, 2 }, { EZP_CHIP_SIZE + 1, 0 },
			{ UINT_MAX, 1 }, { 1, UINT_MAX },
		};
		for (size_t j = 0; j < ARRAY_SIZE(ranges); j++)
			assert_int_not_equal(flash.mst->opaque.write(&flash, &byte, ranges[j][0], ranges[j][1]), 0);
		assert_int_not_equal(flash.mst->opaque.erase(&flash, 0, EZP_READ_BLOCK), 0);
		assert_int_not_equal(flash.mst->opaque.erase(&flash, 1, EZP_CHIP_SIZE - 1), 0);
		assert_int_not_equal(flash.mst->opaque.erase(&flash, 0, 0), 0);
		if (s.firmware == 0x03) {
			assert_int_equal(flash.mst->opaque.write(&flash, &byte, 0, 0), 0);
			assert_int_equal(flash.mst->opaque.write(&flash, &byte, EZP_CHIP_SIZE, 0), 0);
		} else {
			assert_int_not_equal(flash.mst->opaque.write(&flash, &byte, 0, 1), 0);
			assert_int_not_equal(flash.mst->opaque.erase(&flash, 0, EZP_CHIP_SIZE), 0);
		}
		assert_int_equal(s.calls, calls);
		assert_int_equal(flash.mst->opaque.read(&flash, &byte, 0, 1), 0);
		assert_int_equal(byte, ezp_read_byte(0));
		ezp_memory_finish(&s);
	}
}

void ezp_xpro_write_transfer_error_test(void **state)
{
	(void)state;
	const struct { int command; enum ezp_memory_phase phase; unsigned int stream, skip; } failures[] = {
		{ 0x0d, EZP_IDLE, 0, 0 }, { -1, EZP_STREAM, 1, 0 }, { 0x09, EZP_IDLE, 0, 0 },
		{ 0x13, EZP_IDLE, 0, 0 }, { -1, EZP_SECTOR, 0, 0 }, { -1, EZP_READ_ACK, 0, 0 },
		{ -1, EZP_PAYLOAD, 0, 0 }, { 0x02, EZP_IDLE, 0, 0 }, { -1, EZP_PROGRAM_ACK, 0, 0 },
		{ -1, EZP_STREAM, 2, 0 }, { 0x0d, EZP_IDLE, 0, 1 },
		{ 0x09, EZP_IDLE, 0, 1 }, { 0x09, EZP_IDLE, 0, 2 },
	};
	for (size_t i = 0; i < ARRAY_SIZE(failures); i++) {
		for (int short_transfer = 0; short_transfer < 2; short_transfer++) {
			struct ezp_memory s = ezp_memory_new();
			s.fail_command = failures[i].command;
			s.fail_phase = failures[i].phase;
			s.fail_stream = failures[i].stream;
			s.skip_fail_matches = failures[i].skip;
			s.short_transfer = short_transfer;
			const struct io_mock io = ezp_memory_io(&s);
			io_mock_register(&io);
			struct flashchip chip = { 0 };
			struct flashctx flash = { .chip = &chip };
			ezp_init_probe(&flash);
			uint8_t byte = 0x59;
			assert_int_not_equal(flash.mst->opaque.write(&flash, &byte, 123, 1), 0);
			assert_true(s.faulted);
			ezp_assert_locked(&flash, &s);
			ezp_memory_finish(&s);
		}
	}
}

void ezp_xpro_write_corruption_test(void **state)
{
	(void)state;
	const unsigned int corruptions[] = { UINT_MAX, 123, 124, EZP_CHIP_SIZE - 1 };
	for (size_t i = 0; i < ARRAY_SIZE(corruptions); i++) {
		struct ezp_memory s = ezp_memory_new();
		s.corrupt_sector = i == 0;
		s.corrupt_at = corruptions[i];
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		ezp_init_probe(&flash);
		uint8_t byte = 0x59;
		assert_int_not_equal(flash.mst->opaque.write(&flash, &byte, 123, 1), 0);
		assert_int_equal(s.payloads, i == 0 ? 0 : 1);
		assert_int_equal(s.programs, i == 0 ? 0 : 1);
		assert_int_equal(s.streams, i == 0 ? 1 : 2);
		ezp_assert_locked(&flash, &s);
		ezp_memory_finish(&s);
	}
}

void ezp_xpro_erase_polling_test(void **state)
{
	(void)state;
	struct ezp_memory s = ezp_memory_new();
	s.erase_timeouts = 2;
	s.erase_busy = 2;
	const struct io_mock io = ezp_memory_io(&s);
	io_mock_register(&io);
	struct flashchip chip = { 0 };
	struct flashctx flash = { .chip = &chip };
	ezp_init_probe(&flash);
	assert_int_equal(flash.mst->opaque.erase(&flash, 0, EZP_CHIP_SIZE), 0);
	assert_int_equal(s.polls, 5);
	assert_int_equal(s.streams, 1);
	for (unsigned int i = 0; i < EZP_CHIP_SIZE; i++)
		assert_int_equal(s.contents[i], 0xff);
	ezp_memory_finish(&s);
}

void ezp_xpro_erase_failure_test(void **state)
{
	(void)state;
	for (unsigned int mode = 0; mode < 3; mode++) {
		struct ezp_memory s = ezp_memory_new();
		s.erase_stuck = mode == 0;
		s.erase_timeouts = mode == 1 ? 600 : 0;
		s.erase_nonblank = mode == 2;
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		ezp_init_probe(&flash);
		assert_int_not_equal(flash.mst->opaque.erase(&flash, 0, EZP_CHIP_SIZE), 0);
		assert_int_equal(s.polls, mode == 2 ? 1 : 600);
		assert_int_equal(s.streams, mode == 2 ? 1 : 0);
		if (mode == 2)
			assert_int_equal(s.stream_offset, EZP_CHIP_SIZE);
		ezp_assert_locked(&flash, &s);
		ezp_memory_finish(&s);
	}
}

void ezp_xpro_erase_transfer_error_test(void **state)
{
	(void)state;
	const struct { int command; enum ezp_memory_phase phase; unsigned int skip; } failures[] = {
		{ 0x05, EZP_IDLE, 0 }, { -1, EZP_ERASE_POLL, 0 }, { 0x09, EZP_IDLE, 0 },
		{ 0x0d, EZP_IDLE, 0 }, { -1, EZP_STREAM, 0 }, { 0x09, EZP_IDLE, 1 },
	};
	for (size_t i = 0; i < ARRAY_SIZE(failures); i++) {
		for (int short_transfer = 0; short_transfer < 2; short_transfer++) {
			struct ezp_memory s = ezp_memory_new();
			s.fail_command = failures[i].command;
			s.fail_phase = failures[i].phase;
			s.skip_fail_matches = failures[i].skip;
			s.short_transfer = short_transfer;
			const struct io_mock io = ezp_memory_io(&s);
			io_mock_register(&io);
			struct flashchip chip = { 0 };
			struct flashctx flash = { .chip = &chip };
			ezp_init_probe(&flash);
			assert_int_not_equal(flash.mst->opaque.erase(&flash, 0, EZP_CHIP_SIZE), 0);
			assert_true(s.faulted);
			ezp_assert_locked(&flash, &s);
			ezp_memory_finish(&s);
		}
	}
}

#else
SKIP_TEST(ezp_xpro_invalid_params_test)
SKIP_TEST(ezp_xpro_profile_guards_test)
SKIP_TEST(ezp_xpro_profile_inventory_test)
SKIP_TEST(ezp_xpro_profile_status_test)
SKIP_TEST(ezp_xpro_legacy_profiles_test)
SKIP_TEST(ezp_xpro_legacy_peer_ids_test)
SKIP_TEST(ezp_xpro_legacy_identity_guards_test)
SKIP_TEST(ezp_xpro_array_write_test)
SKIP_TEST(ezp_xpro_array_completion_test)
SKIP_TEST(ezp_xpro_array_transfer_error_test)
SKIP_TEST(ezp_xpro_array_public_api_test)
SKIP_TEST(ezp_xpro_eeprom_policy_test)
SKIP_TEST(ezp_xpro_eeprom_ranges_test)
SKIP_TEST(ezp_xpro_eeprom_transfer_error_test)
SKIP_TEST(ezp_xpro_eeprom_erase_failure_test)
SKIP_TEST(ezp_xpro_eeprom_public_api_test)
SKIP_TEST(ezp_xpro_no_chip_erase_test)
SKIP_TEST(ezp_xpro_large_read_ranges_test)
SKIP_TEST(ezp_xpro_large_write_ranges_test)
SKIP_TEST(ezp_xpro_large_corruption_test)
SKIP_TEST(ezp_xpro_large_transfer_error_test)
SKIP_TEST(ezp_xpro_large_public_api_test)
SKIP_TEST(ezp_xpro_profile_write_ranges_test)
SKIP_TEST(ezp_xpro_profile_transfer_error_test)
SKIP_TEST(ezp_xpro_ambiguous_params_test)
SKIP_TEST(ezp_xpro_basic_lifecycle_test_success)
SKIP_TEST(ezp_xpro_master_registration_failure_test)
SKIP_TEST(ezp_xpro_shutdown_registration_failure_test)
SKIP_TEST(ezp_xpro_firmware_transfer_error_test)
SKIP_TEST(ezp_xpro_firmware_invalid_test)
SKIP_TEST(ezp_xpro_probe_lifecycle_test_success)
SKIP_TEST(ezp_xpro_probe_mismatch_test)
SKIP_TEST(ezp_xpro_probe_transfer_error_test)
SKIP_TEST(ezp_xpro_shutdown_transfer_error_test)
SKIP_TEST(ezp_xpro_reprobe_mismatch_test)
SKIP_TEST(ezp_xpro_read_ranges_test)
SKIP_TEST(ezp_xpro_read_image_test)
SKIP_TEST(ezp_xpro_read_invalid_range_test)
SKIP_TEST(ezp_xpro_read_transfer_error_test)
SKIP_TEST(ezp_xpro_write_ranges_test)
SKIP_TEST(ezp_xpro_image_write_verify_erase_test)
SKIP_TEST(ezp_xpro_partial_layout_auto_erase_test)
SKIP_TEST(ezp_xpro_destructive_guards_test)
SKIP_TEST(ezp_xpro_write_transfer_error_test)
SKIP_TEST(ezp_xpro_write_corruption_test)
SKIP_TEST(ezp_xpro_erase_polling_test)
SKIP_TEST(ezp_xpro_erase_failure_test)
SKIP_TEST(ezp_xpro_erase_transfer_error_test)
#endif
