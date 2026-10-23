/*
 * SPDX-License-Identifier: GPL-2.0-only
 * SPDX-FileCopyrightText: 2020 Google LLC
 */

#ifndef _TESTS_TEST_H
#define _TESTS_TEST_H

/*
 * Standard test header that should be included in all tests. For now it just encapsulates the
 * include dependencies for Cmocka. Test-specific APIs that are so generic we would want them
 * available everywhere could also be added here.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#define NON_ZERO (0xf000baaa)

#define MOCK_FD (0x10ec)

#define SKIP_TEST(name) \
	void name (void **state) { skip(); }

/*
 * Having this as function allows to set a breakpoint on the address,
 * as it has a named symbol associated with the address number.
 */
void *not_null(void);

#define LOG_ME printf("%s is called\n", __func__)

/* ezp_xpro.c */
void ezp_xpro_invalid_params_test(void **state);
void ezp_xpro_profile_guards_test(void **state);
void ezp_xpro_profile_inventory_test(void **state);
void ezp_xpro_profile_status_test(void **state);
void ezp_xpro_profile_write_ranges_test(void **state);
void ezp_xpro_profile_transfer_error_test(void **state);
void ezp_xpro_ambiguous_params_test(void **state);
void ezp_xpro_basic_lifecycle_test_success(void **state);
void ezp_xpro_master_registration_failure_test(void **state);
void ezp_xpro_shutdown_registration_failure_test(void **state);
void ezp_xpro_firmware_transfer_error_test(void **state);
void ezp_xpro_firmware_invalid_test(void **state);
void ezp_xpro_probe_lifecycle_test_success(void **state);
void ezp_xpro_probe_mismatch_test(void **state);
void ezp_xpro_probe_transfer_error_test(void **state);
void ezp_xpro_shutdown_transfer_error_test(void **state);
void ezp_xpro_reprobe_mismatch_test(void **state);
void ezp_xpro_read_ranges_test(void **state);
void ezp_xpro_read_image_test(void **state);
void ezp_xpro_read_invalid_range_test(void **state);
void ezp_xpro_read_transfer_error_test(void **state);
void ezp_xpro_write_ranges_test(void **state);
void ezp_xpro_image_write_verify_erase_test(void **state);
void ezp_xpro_partial_layout_auto_erase_test(void **state);
void ezp_xpro_destructive_guards_test(void **state);
void ezp_xpro_write_transfer_error_test(void **state);
void ezp_xpro_write_corruption_test(void **state);
void ezp_xpro_erase_polling_test(void **state);
void ezp_xpro_erase_failure_test(void **state);
void ezp_xpro_erase_transfer_error_test(void **state);

#endif /* _TESTS_TEST_H */
