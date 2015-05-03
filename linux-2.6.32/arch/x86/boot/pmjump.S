/* ----------------------------------------------------------------------- *
 *
 *   Copyright (C) 1991, 1992 Linus Torvalds
 *   Copyright 2007 rPath, Inc. - All Rights Reserved
 *
 *   This file is part of the Linux kernel, and is made available under
 *   the terms of the GNU General Public License version 2.
 *
 * ----------------------------------------------------------------------- */

/*
 * The actual transition into protected mode
 */

#include <asm/boot.h>
#include <asm/processor-flags.h>
#include <asm/segment.h>
#include <linux/linkage.h>

	.text
	.code16

/*
 * void protected_mode_jump(u32 entrypoint, u32 bootparams);
 */
// linux fastcall : 3个参数,依次是EAX, EDX, ECX
GLOBAL(protected_mode_jump)
	# %edx，第二个参数， boot_parmas地址, 加载到%esi
	movl	%edx, %esi		# Pointer to boot_params table

	xorl	%ebx, %ebx
	movw	%cs, %bx
	shll	$4, %ebx	# %ebx的内容左移4位, %cs << 4
	addl	%ebx, 2f	# %ebx = (%cs * 16) + addr(2f); 即2f的线性地址
						# 将结果存到2f所指的变量里面
	jmp	1f			# Short jump to serialize on 386/486
1:

	# 见arch/x86/boot/pm.c中的setup_gdt();
	# 设置了进入""保护模式""以后的数据段段选择子
	movw	$__BOOT_DS, %cx
	movw	$__BOOT_TSS, %di

	movl	%cr0, %edx		# 保存%cr0
	orb	$X86_CR0_PE, %dl	# Protected mode, 设置进入保护模式的PE位
	movl	%edx, %cr0

	# 至此，已经进入保护模式,但此刻还没设置新的%cs
	# 直接指定段选择子和偏移量来进行长跳转
	# Transition to 32-bit mode
	.byte	0x66, 0xea		# ljmpl opcode, 据说这两个字节是ljmpl的码值
2:	.long	in_pm32			# offset
	.word	__BOOT_CS		# segment
ENDPROC(protected_mode_jump)

	.code32
	.section ".text32","ax"

	/* 
	 * 到这个函数时寄存器： 
	 * eax=压缩内核线性地址(protected_mode_jump的参数) 
	 * cs=__BOOT_CS代码段选择子 
	 * cx=__BOOT_DS段选择子 
	 * di=__BOOT_TSS段选择子          
	 */  
GLOBAL(in_pm32)
	# Set up data segments for flat 32-bit mode
	movl	%ecx, %ds	# 设置保护模式的数据段选择子
	movl	%ecx, %es	# 继续可以看到，%es,%fs,%gs,%ss都是来自%ecx
	movl	%ecx, %fs	# 说明了保护模式的平坦地址空间
	movl	%ecx, %gs
	movl	%ecx, %ss

	/* 
	 * bx存放实模式cs值(内核地址X所在段),ebx等于cs左移过4位 
	 * line 31: movw	%cs, %bx
	 * line 32: shll	$4, %ebx
	 * sp在header.S中被设置成了相对于X的段基址的偏移量, 
	 * 现在esp=ebx+esp=栈顶指针,其实esp的值现在是个32位线性 
	 * 地址,值与实模式下线性地址相同。 
	 */  
	# The 32-bit code sets up its own stack, but this way we do have
	# a valid stack if some debugging hack wants to use it.
	addl	%ebx, %esp	# //初始化堆栈,将原先实模式启动代码的空间全部用作堆栈

	# Set up TR to make Intel VT happy
	ltr	%di		# %di 来自于$__BOOT_TSS

	# Clear registers to allow for future extensions to the
	# 32-bit boot protocol
	xorl	%ecx, %ecx
	xorl	%edx, %edx
	xorl	%ebx, %ebx
	xorl	%ebp, %ebp
	xorl	%edi, %edi

	# Set up LDTR to make Intel VT happy
	lldt	%cx

	jmpl	%eax			# Jump to the 32-bit entrypoint
							# 跳转到code32_start吧,其实此前已经进入保护模式，但
							#		这儿之后才开始执行真正有意义的保护模式代码
							# %eax是 protected_mode_jump的第一个参数
							#	就是通过boot_param中code32_start的值
ENDPROC(in_pm32)
