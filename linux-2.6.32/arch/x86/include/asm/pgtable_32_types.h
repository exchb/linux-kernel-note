#ifndef _ASM_X86_PGTABLE_32_DEFS_H
#define _ASM_X86_PGTABLE_32_DEFS_H

/*
 * The Linux x86 paging architecture is 'compile-time dual-mode', it
 * implements both the traditional 2-level x86 page tables and the
 * newer 3-level PAE-mode page tables.
 */
#ifdef CONFIG_X86_PAE
# include <asm/pgtable-3level_types.h>
# define PMD_SIZE	(1UL << PMD_SHIFT) // x86 PMD_SHIFT == 22
# define PMD_MASK	(~(PMD_SIZE - 1))
#else
# include <asm/pgtable-2level_types.h>
#endif

#define PGDIR_SIZE	(1UL << PGDIR_SHIFT)
#define PGDIR_MASK	(~(PGDIR_SIZE - 1))

/* Just any arbitrary offset to the start of the vmalloc VM area: the
 * current 8MB value just means that there will be a 8MB "hole" after the
 * physical memory until the kernel virtual memory starts.  That means that
 * any out-of-bounds memory accesses will hopefully be caught.
 * The vmalloc() routines leaves a hole of 4kB between each vmalloced
 * area for the same reason. ;)
 */
#define VMALLOC_OFFSET	(8 * 1024 * 1024)   // gap

#ifndef __ASSEMBLER__
extern bool __vmalloc_start_set; /* set once high_memory is set */
#endif

// 和物理内存有关,如果物理内存过1G，high_memory = 896M
#define VMALLOC_START	((unsigned long)high_memory + VMALLOC_OFFSET)
#ifdef CONFIG_X86_PAE
#define LAST_PKMAP 512
#else
#define LAST_PKMAP 1024   // pkmap 永久映射，最多1024个页面
#endif

#define PKMAP_BASE ((FIXADDR_BOOT_START - PAGE_SIZE * (LAST_PKMAP + 1)) \
		    & PMD_MASK)

#ifdef CONFIG_HIGHMEM
# define VMALLOC_END	(PKMAP_BASE - 2 * PAGE_SIZE)    // 8M的gap防止越界
#else
# define VMALLOC_END	(FIXADDR_START - 2 * PAGE_SIZE)
#endif

#define MODULES_VADDR	VMALLOC_START
#define MODULES_END	VMALLOC_END
#define MODULES_LEN	(MODULES_VADDR - MODULES_END)

// 表达内核能直接映射的区域, 这几个值都是定值,倒着计算回来的
#define MAXMEM	(VMALLOC_END - PAGE_OFFSET - __VMALLOC_RESERVE)

#endif /* _ASM_X86_PGTABLE_32_DEFS_H */
