/*
 * Copyright (c) 2015, Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/cpu-pd.h>

static int __init arm64_cpu_pd_init(void)
{
	return of_cpu_pd_init("arm,cpu-pd");
}
device_initcall(arm64_cpu_pd_init);
