/*
 *  linux/boot/head.S
 *
 *  Copyright (C) 1991, 1992, 1993  Linus Torvalds
 */

/*
 *  head.S contains the 32-bit startup code.
 *
 * NOTE!!! Startup happens at absolute address 0x00001000, which is also where
 * the page directory will exist. The startup code will be overwritten by
 * the page directory. [According to comments etc elsewhere on a compressed
 * kernel it will end up at 0x1000 + 1Mb I hope so as I assume this. - AC]
 *
 * Page 0 is deliberately kept safe, since System Management Mode code in 
 * laptops may need to access the BIOS data stored there.  This is also
 * useful for future device drivers that either access the BIOS via VM86 
 * mode.
 */

/*
 * High loaded stuff by Hans Lermen & Werner Almesberger, Feb. 1996
 */
	.code32
	.text

#include <linux/init.h>
#include <linux/linkage.h>
#include <asm/segment.h>
#include <asm/pgtable_types.h>
#include <asm/page_types.h>
#include <asm/boot.h>
#include <asm/msr.h>
#include <asm/processor-flags.h>
#include <asm/asm-offsets.h>

	__HEAD
	.code32
ENTRY(startup_32)
	cld
	/*
	 * Test KEEP_SEGMENTS(段保持标志) flag to see if the bootloader is asking
	 * us to not reload segments
	 */
	testb $(1<<6), BP_loadflags(%esi)
	jnz 1f

	/*
		需要重新设置段寄存器
	 */
	cli
	movl	$(__KERNEL_DS), %eax
	movl	%eax, %ds
	movl	%eax, %es
	movl	%eax, %ss
1:

/*
 * Calculate the delta between where we were compiled to run
 * at and where we were actually loaded at.  This can only be done
 * with a short local call on x86.  Nothing  else will tell us what
 * address we are running at.  The reserved chunk of the real-mode
 * data at 0x1e4 (defined as a scratch field) are used as the stack
 * for this calculation. Only 4 bytes are needed.
 */
/*
 * call 1f  : call会将ret ip压栈，而此时ret ip即标号1f的地址
 * call之后，即跳转到1f后，执行该函数，该函数先求得%ebp，%ebp = %eip
 * 然后执行 subl $1b, $ebp, $1b是编译时预期的1f的地址，即理论值
 * call放入栈中的(%ebp中的值)，是实际加载地址， %ebp = %ebp - $1b
 * 可得： %ebp中保存了理论值和实际值的差距,即内核加载的基地址
 * 即startup_32的实际地址
 */
	leal	(BP_scratch+4)(%esi), %esp	# boot_params.scratch的地址设置成为堆栈顶
	call	1f							# 想想函数调用的过程:压入%eip
1:	popl	%ebp						# 所以函数1: 第一条指令popl就能取到%eip的值
	subl	$1b, %ebp					# %ebp中包含的是startup_32的加载地址啦

/* setup a stack and make sure cpu supports long mode. */
	movl	$boot_stack_end, %eax
	addl	%ebp, %eax
	movl	%eax, %esp				# 设置一个堆栈区域

	call	verify_cpu
	testl	%eax, %eax
	jnz	no_longmode

/*
 * Compute the delta between where we were compiled to run at
 * and where the code will actually run at.
 *
 * %ebp contains the address we are loaded at by the boot loader and %ebx
 * contains the address where we should move the kernel image temporarily
 * for safe in-place decompression.
 */

#ifdef CONFIG_RELOCATABLE
	movl	%ebp, %ebx
	movl	BP_kernel_alignment(%esi), %eax
	decl	%eax
	addl	%eax, %ebx
	notl	%eax
	andl	%eax, %ebx
#else
	movl	$LOAD_PHYSICAL_ADDR, %ebx
#endif

	/* Target address to relocate to for decompression */
	/* 在对齐后的地址之上加一个解压偏移作为解压地址
	addl	$z_extract_offset, %ebx

/*
 * Prepare for entering 64 bit mode
 */

	/* Load new GDT with the 64bit segments using 32bit descriptor */
	leal	gdt(%ebp), %eax
	movl	%eax, gdt+2(%ebp)
	lgdt	gdt(%ebp)

	/* Enable PAE mode */
	xorl	%eax, %eax
	orl	$(X86_CR4_PAE), %eax
	movl	%eax, %cr4

 /*
  * Build early 4G boot pagetable
  */
	/* Initialize Page tables to 0 */
    // pagetable 在本文件末尾声明的空间
    // ebx = $LOAD_PHYSICAL_ADDR 这就是加载的物理地址.0x100000
#endif
    // edi = pagetable + ebx,这就是pagetable实际的物理地址
	leal	pgtable(%ebx), %edi
	xorl	%eax, %eax
	movl	$((4096*6)/4), %ecx
	rep	stosl

	/* Build Level 4 */
    // edi = pgtable + ebx = pgtable + 0x100000 = pgtable的物理地址
	leal	pgtable + 0(%ebx), %edi
    // eax = pgtable + 0x1007
	leal	0x1007 (%edi), %eax
    // 设置PGD 0(pgtable) -> pgtable + 0x1007
    // 这0x1007就是给PGD留的位置
    // #define PTRS_PER_PGD 512
	movl	%eax, 0(%edi)

	/* Build Level 3 */
    // 这就只往下拉了0x1000,512个位置,同样的
    // #define PTRS_PER_PUD 512
    // edi = pgtable + 0x1000 + ebx = pgtable + 0x1000
	leal	pgtable + 0x1000(%ebx), %edi
    // 再往下拉了0x1007 -> eax
    // eax = pgtable + 0x2007
	leal	0x1007(%edi), %eax
	movl	$4, %ecx
    // 这就相当于edi又留了512空间给pud:
    // pud 0 -> eax = pgtable + 0x2007
1:	movl	%eax, 0x00(%edi)
	addl	$0x00001000, %eax
    // pud自增, eax也按0x1000自增
    // pud 1 -> pgtable + 0x3007 .. 
    // pud 3 -> pgtable + 0x5007
	addl	$8, %edi
    // ecx只有4
	decl	%ecx
	jnz	1b

	/* Build Level 2 */
    // pud 0开始,置上面的pud 0 - 3的pmd
	leal	pgtable + 0x2000(%ebx), %edi
    // 0x183是什么属性?
	movl	$0x00000183, %eax
    // 2048 = 512 * 4, 就是上面的4 pud
	movl	$2048, %ecx
1:	movl	%eax, 0(%edi)
    // 这个值就是一个PMD大小 1<<PMD_SHIFT = 1 << 21
    // eax从0开始,每次自增一个PMD
    // pmd 从0x00000183开始?
	addl	$0x00200000, %eax
	addl	$8, %edi
	decl	%ecx
	jnz	1b

    // pte没有设置?
	/* Enable the boot page tables */
	leal	pgtable(%ebx), %eax
	movl	%eax, %cr3

	/* Enable Long mode in EFER (Extended Feature Enable Register) */
	movl	$MSR_EFER, %ecx
	rdmsr
	btsl	$_EFER_LME, %eax
	wrmsr

	/*
	 * Setup for the jump to 64bit mode
	 *
	 * When the jump is performend we will be in long mode but
	 * in 32bit compatibility mode with EFER.LME = 1, CS.L = 0, CS.D = 1
	 * (and in turn EFER.LMA = 1).	To jump into 64bit mode we use
	 * the new gdt/idt that has __KERNEL_CS with CS.L = 1.
	 * We place all of the values on our mini stack so lret can
	 * used to perform that far jump.
	 */
	pushl	$__KERNEL_CS
	leal	startup_64(%ebp), %eax	// %ebp是内核加载基址, 将%ebp + startup_64这个绝对值加载到%eax
	pushl	%eax

	/* Enter paged protected Mode, activating Long Mode */
	movl	$(X86_CR0_PG | X86_CR0_PE), %eax /* Enable Paging and Protected mode */
	movl	%eax, %cr0

	/* Jump from 32bit compatibility mode into 64bit mode. */
	lret	// 返回后的地址就是startup_64，64bit模式下的startup_64
ENDPROC(startup_32)

no_longmode:
	/* This isn't an x86-64 CPU so hang */
1:
	hlt
	jmp     1b

#include "../../kernel/verify_cpu_64.S"

	/*
	 * Be careful here startup_64 needs to be at a predictable
	 * address so I can export it in an ELF header.  Bootloaders
	 * should look at the ELF header to find this address, as
	 * it may change in the future.
	 */
	.code64
	.org 0x200
ENTRY(startup_64)
	/*
	 * We come here either from startup_32 or directly from a
	 * 64bit bootloader.  If we come here from a bootloader we depend on
	 * an identity mapped page table being provied that maps our
	 * entire text+data+bss and hopefully all of memory.
	 */

	/* Setup data segments. */
	xorl	%eax, %eax
	movl	%eax, %ds
	movl	%eax, %es
	movl	%eax, %ss
	movl	%eax, %fs
	movl	%eax, %gs
	lldt	%ax
	movl    $0x20, %eax
	ltr	%ax

	/*
	 * Compute the decompressed kernel start address.  It is where
	 * we were loaded at aligned to a 2M boundary. %rbp contains the
	 * decompressed kernel start address.
	 *
	 * If it is a relocatable kernel then decompress and run the kernel
	 * from load address aligned to 2MB addr, otherwise decompress and
	 * run the kernel from LOAD_PHYSICAL_ADDR
	 *
	 * We cannot rely on the calculation done in 32-bit mode, since we
	 * may have been invoked via the 64-bit entry point.
	 */

	/* Start with the delta to where the kernel will run at. */
#ifdef CONFIG_RELOCATABLE
	/*
	 * see : http://stackoverflow.com/questions/9354941/retrieving-data-in-x86-64
	 * The leaq instruction gets the address of startup_32 using an offset of the instruction pointer(%rip)
	 * which can be calculated at link time. Then the movl instruction uses that address as the base for loading the data
	 */
	leaq	startup_32(%rip) /* - $startup_32 */, %rbp	# leaq startup_32(%rip) , %rbp，表示加载startup_32的地址到%rbp中
	movl	BP_kernel_alignment(%rsi), %eax
	decl	%eax
	addq	%rax, %rbp
	notq	%rax
	andq	%rax, %rbp
#else
	movq	$LOAD_PHYSICAL_ADDR, %rbp
#endif

	/* Target address to relocate to for decompression */
	leaq	z_extract_offset(%rbp), %rbx

	/* Set up the stack */
	leaq	boot_stack_end(%rbx), %rsp

	/* Zero EFLAGS */
	pushq	$0
	popfq

/*
 * Copy the compressed kernel to the end of our buffer
 * where decompression in place becomes safe.
 */
	pushq	%rsi
	leaq	(_bss-8)(%rip), %rsi
	leaq	(_bss-8)(%rbx), %rdi
	movq	$_bss /* - $startup_32 */, %rcx	// move的数量, startup_32到_bss的内容都搬到%rbx的地方
	shrq	$3, %rcx
	std
	rep	movsq
	cld
	popq	%rsi

/*
 * Jump to the relocated address.
 */
	leaq	relocated(%rbx), %rax	// 跳转到搬移后的代码：相对于%rbx的relocated偏移的地址,执行解压缩代码
	jmp	*%rax

	.text
relocated:

/*
 * Clear BSS (stack is currently empty)
 */
	xorl	%eax, %eax
	leaq    _bss(%rip), %rdi
	leaq    _ebss(%rip), %rcx
	subq	%rdi, %rcx
	shrq	$3, %rcx
	rep	stosq

/*
 * Do the decompression, and jump to the new kernel..
 */
	pushq	%rsi			/* Save the real mode argument */
	movq	%rsi, %rdi		/* real mode address */
	leaq	boot_heap(%rip), %rsi	/* malloc area for uncompression */
	leaq	input_data(%rip), %rdx  /* input_data */
	movl	$z_input_len, %ecx	/* input_len */
	movq	%rbp, %r8		/* output target address */
	call	decompress_kernel
	popq	%rsi

/*
 * Jump to the decompressed kernel.
 */
	jmp	*%rbp

	.data
gdt:
	.word	gdt_end - gdt
	.long	gdt
	.word	0
	.quad	0x0000000000000000	/* NULL descriptor */
	.quad	0x00af9a000000ffff	/* __KERNEL_CS */
	.quad	0x00cf92000000ffff	/* __KERNEL_DS */
	.quad	0x0080890000000000	/* TS descriptor */
	.quad   0x0000000000000000	/* TS continued */
gdt_end:

/*
 * Stack and heap for uncompression
 */
	.bss
	.balign 4
boot_heap:
	.fill BOOT_HEAP_SIZE, 1, 0
boot_stack:
	.fill BOOT_STACK_SIZE, 1, 0
boot_stack_end:

/*
 * Space for page tables (not in .bss so not zeroed)
 */
	.section ".pgtable","a",@nobits
	.balign 4096
pgtable:
	.fill 6*4096, 1, 0
