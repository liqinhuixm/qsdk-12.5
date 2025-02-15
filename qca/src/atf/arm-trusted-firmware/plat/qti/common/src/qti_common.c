/*
 * Copyright (c) 2018, ARM Limited and Contributors. All rights reserved.
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdbool.h>
#include <common/debug.h>
#include <assert.h>
#include <errno.h>
#include <lib/xlat_tables/xlat_tables_v2.h>
#include <plat_qti.h>
#include <platform_def.h>
#include <qtiseclib_interface.h>
#include <qtiseclib_cb_interface.h>

/*
 * Table of regions for various BL stages to map using the MMU.
 * This doesn't include TZRAM as the 'mem_layout' argument passed to
 * qti_configure_mmu_elx() will give the available subset of that,
 */

const mmap_region_t plat_qti_mmap[] = {
	MAP_REGION_FLAT(QTI_DEVICE_BASE, QTI_DEVICE_BASE + QTI_SHARED_IMEM_RO_BASE,
			MT_DEVICE | MT_RW | MT_SECURE),
	MAP_REGION_FLAT(QTI_SHARED_IMEM_RO_BASE, QTI_SHARED_IMEM_RO_SIZE,
			MT_CODE | MT_SECURE),
	MAP_REGION_FLAT(QTI_SHARED_IMEM_RO_BASE + QTI_SHARED_IMEM_RO_SIZE, (QTI_DEVICE_SIZE -(QTI_SHARED_IMEM_RO_BASE + QTI_SHARED_IMEM_RO_SIZE)),
			MT_DEVICE | MT_RW | MT_SECURE),
	{0}
};

CASSERT(ARRAY_SIZE(plat_qti_mmap) <= MAX_MMAP_REGIONS, assert_max_mmap_regions);
bool qti_is_overlap_atf_rg(unsigned long long addr, size_t size)
{
	if (addr > addr + size || ((BL31_BASE < addr + size) && (BL31_LIMIT > addr) && (BL31_LIMIT > (addr + size))))
		return true;
	return false;
}

/*----------------------------------------------------------------------------
 *  unsigned int plat_qti_my_cluster_pos(void)
 *  definition to get the cluster index of the calling CPU.
 *  - In ARM v8   (MPIDR_EL1[24]=0)
 *    ClusterId = MPIDR_EL1[15:8]
 *  - In ARM v8.1 & Later version (MPIDR_EL1[24]=1)
 *    ClusterId = MPIDR_EL1[23:15]
 * -------------------------------------------------------------------------*/
unsigned int plat_qti_my_cluster_pos(void)
{
	unsigned long mpidr, cluster_id;

	mpidr = read_mpidr_el1();
	if ((mpidr & MPIDR_MT_MASK) == 0) {	/* MT not supported */
		cluster_id = (mpidr >> MPIDR_AFF1_SHIFT) & MPIDR_AFFLVL_MASK;
	} else {		/* MT supported */

		cluster_id = (mpidr >> MPIDR_AFF2_SHIFT) & MPIDR_AFFLVL_MASK;
	}
	assert(cluster_id < PLAT_CLUSTER_COUNT);
	return cluster_id;
}

#define STRINGIFY(s) #s
#define FORM_STRING(s) STRINGIFY(s)
/*
 * Set up the page tables for the generic and platform-specific memory regions.
 * The extents of the generic memory regions are specified by the function
 * arguments and consist of:
 * - Trusted SRAM seen by the BL image;
 * - Code section;
 * - Read-only data section;
 * - Coherent memory region, if applicable.
 */
void qti_setup_page_tables(uintptr_t total_base,
						   size_t total_size,
						   uintptr_t code_start,
						   uintptr_t code_limit,
						   uintptr_t rodata_start,
						   uintptr_t rodata_limit,
						   uintptr_t coh_start, uintptr_t coh_limit)
{

	static uint64_t total_ddr_size = 0;
#if QTI_5018_PLATFORM || QTI_9574_PLATFORM
	uint64_t smem_base_pa = 0;
	uint64_t smem_targ_pa = 0;
	uint64_t smem_base_size = 0;
	uint64_t smem_targ_size = 0;
#endif
	/*
	 * Map the entire RAM with appropriate memory attributes.
	 * Subsequent mappings will adjust the attributes for specific regions.
	 */
	total_ddr_size = qtiseclib_get_ddr_size();
        mmap_add_region(QTI_DDR_BASE, QTI_DDR_BASE,
                        (total_ddr_size), MT_MEMORY | MT_RW | MT_SECURE);

	VERBOSE("Trusted SRAM seen by this BL image: %p - %p\n",
		(void *)total_base, (void *)(total_base + total_size));
	mmap_add_region(total_base, total_base,
			total_size, MT_MEMORY | MT_RW | MT_SECURE);

#if QTI_5018_PLATFORM || QTI_9574_PLATFORM
	qtiseclib_get_smem_targ_info(&smem_targ_pa, &smem_targ_size);
	VERBOSE("smem targ info region: %p - %p\n",
		(void *)smem_targ_pa, (void *)(smem_targ_pa + smem_targ_size));
	qtiseclib_get_smem_base_addr(&smem_base_pa, &smem_base_size);
	VERBOSE("smem region: %p - %p\n",
		(void *)smem_base_pa, (void *)(smem_base_pa + smem_base_size));
	mmap_add_region(smem_base_pa, smem_base_pa,
			smem_base_size, MT_NON_CACHEABLE | MT_RW | MT_SECURE | MT_EXECUTE_NEVER);
	qtiseclib_set_image_version(FORM_STRING(ATF_MAJOR), FORM_STRING(ATF_MINOR), FORM_STRING(ATF_COMMIT));
#endif
	/* Re-map the code section */
	VERBOSE("Code region: %p - %p\n",
		(void *)code_start, (void *)code_limit);
	mmap_add_region(code_start, code_start,
			code_limit - code_start, MT_CODE | MT_SECURE);

	/* Re-map the read-only data section */
	VERBOSE("Read-only data region: %p - %p\n",
		(void *)rodata_start, (void *)rodata_limit);
	mmap_add_region(rodata_start, rodata_start,
			rodata_limit - rodata_start, MT_RO_DATA | MT_SECURE);

	/* Re-map the coherent memory region */
	VERBOSE("Coherent region: %p - %p\n",
		(void *)coh_start, (void *)coh_limit);
	mmap_add_region(coh_start, coh_start,
			coh_limit - coh_start, MT_DEVICE | MT_RW | MT_SECURE);

#if ENABLE_SPM && defined(IMAGE_BL31)
	/* The address of the following region is calculated by the linker. */
	mmap_add_region(SP_IMAGE_XLAT_TABLES_START,
			SP_IMAGE_XLAT_TABLES_START,
			SP_IMAGE_XLAT_TABLES_SIZE,
			MT_MEMORY | MT_RW | MT_SECURE);
#endif
	/*Remap the IMEM RW region to make it uncacheable*/
	mmap_add_region(QTI_SHARED_IMEM_RW_BASE,QTI_SHARED_IMEM_RW_BASE,
					QTI_SHARED_IMEM_RW_SIZE, MT_NON_CACHEABLE | MT_RW | MT_SECURE);

	/* Remap the region beyond BL31_END for making it accessible for any other secure operations */
	if ((BL31_LIMIT > BL31_END) && (BL31_LIMIT - BL31_END) > 0) {
	    mmap_add_region(BL31_END, BL31_END, (BL31_LIMIT - BL31_END),
#ifdef QTI_53XX_PLATFORM
			    MT_DEVICE | MT_RW | MT_SECURE);
#else
			    MT_MEMORY | MT_RW | MT_SECURE);
#endif
	}
	/* Now (re-)map the platform-specific memory regions */
	mmap_add(plat_qti_mmap);

	/* Create the page tables to reflect the above mappings */
	init_xlat_tables();
}

int qti_mmap_add_dynamic_region(unsigned long long base_pa, uintptr_t base_va,
								size_t size, unsigned int attr)
{
	base_pa = qti_page_align(base_pa, DOWN);
	base_va = qti_page_align(base_va, DOWN);
	size = qti_page_align(size, UP);

	if(qti_is_overlap_atf_rg(base_pa, size))
	{
		/* Memory shouldn't overlap with ATF range.*/
		return -EPERM;
	}

	return mmap_add_dynamic_region(base_pa, base_va, size, attr);
}

int qti_mmap_remove_dynamic_region(uintptr_t base_va, size_t size)
{
	base_va = qti_page_align(base_va, DOWN);
	size = qti_page_align(size, UP);
	return mmap_remove_dynamic_region(base_va, size);
}
