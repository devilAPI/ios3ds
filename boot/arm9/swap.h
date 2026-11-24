/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
void firm_swap_restore(void);
/* true: we are not the build in sd:/ios3ds/ios3ds.firm (stale chainload copy,
   refreshed if it is luma/payloads/ios3ds.firm); do not boot */
bool firm_is_stale(void);
