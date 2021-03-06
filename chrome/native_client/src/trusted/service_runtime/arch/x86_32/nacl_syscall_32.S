/*
 * Copyright 2008 The Native Client Authors.  All rights reserved.
 * Use of this source code is governed by a BSD-style license that can
 * be found in the LICENSE file.
 */

/*
 * NaCl service runtime context switch code.  NaClSyscallSeg is the
 * lcall target from the syscall trampoline code, and this code is
 * responsible for figuring out the identity of the thread, saving
 * the user registers, finish restoring the segment registers (and
 * getting out of the sandbox), and actually invoking the C system
 * call handler code.
 */

#include "native_client/src/trusted/service_runtime/nacl_config.h"

/*
 * on stack:
 *
 *  syscall-arg-N
 *  ...
 *  syscall-arg-1
 *  RA for caller to libc stub
 *  code seg from trampoline lcall
 *  RA for trampoline lcall (used to compute syscall number)
 */

        .text

#if __PIC__

        /*
         * The trampoline code calls NaClPcrelThunk using an lcall,
         * restoring %cs.  Next, NaClPcrelThunk invokes
         * NaClSyscallSeg, with nacl_user in %ecx and %ds restored.
         * The other segment registers remains to be restored, and we
         * use address differences to obtain nacl_sys (and any other
         * global variables), avoiding PIC address synthesis using the
         * usual sequence of
         *
         *     call 0f
         * 0:  pop %eax
         *
         * to get the address of the pop into a register.  Such
         * sequences are verboten in the context switch code, since we
         * are still using a stack in untrusted memory, and we need to
         * figure out the per-thread secure stack first.
         */
.globl  IDENTIFIER(NaClSyscallSeg)
        HIDDEN(NaClSyscallSeg)
IDENTIFIER(NaClSyscallSeg):
        cld
        xor     %eax, %eax
        mov     %gs, %ax
        shr     $3, %eax
        movl    0(%ecx), %edx
        movl    0(%edx,%eax,4), %edx

        /* check NaClThreadContext in sel_rt_32.h for the offsets */
        movl    %ebx, 0x0(%edx)
        movl    %esi, 0x4(%edx)
        movl    %edi, 0x8(%edx)
        movl    %ebp, 0x14(%edx)
        movl    %esp, 0x1c(%edx)

        /*
         * we do not save the registers (below) that the user is powerless
         * to change.  eip will be overwritten w/ the validated return
         * address pulled from the stack for the syscall return.
         *
         * these segment registers are restored by NaCl_springboard for
         * syscall return.
         */

#if 0
        movl    %eip, 0xc(%edx)
        mov     %cs, 0x3c(%edx)
        mov     %ds, 0x28(%edx)
        mov     %es, 0x2a(%edx)
        mov     %fs, 0x2c(%edx)
        mov     %gs, 0x2e(%edx)
        mov     %ss, 0x20(%edx)
#endif
        /*
         * load only the system segments; called code do not depend on
         * any registers having particular values.  we will clear/discard
         * caller-saved registers at system call return.
         */
        movl    4(%ecx), %edx
        movl    0(%edx,%eax,4), %edx
        /*
         * %cs and %ds already taken care of by NaCl_trampoline_seg_code
         *
         * %ebx need not be saved/restored even if -fpic were used, since
         * in that case %ebx is initialized on fn entry.
         */
        mov     0x2a(%edx), %es
        mov     0x2c(%edx), %fs
        mov     0x2e(%edx), %gs

        mov     0x20(%edx), %ecx  /* ss */
        movl    0x1c(%edx), %edx  /* esp */
        mov     %ecx, %ss  /* disable intr */
        movl    %edx, %esp
        push    %eax
        call    IDENTIFIER(NaClSyscallCSegHook)
        /*
         * If stack usage in the above code changes, modify initial %esp
         * computation -- see nacl_switch_to_app.c:NaClStartThreadInApp.
         */
        hlt
        /* noret */

#else

.globl  IDENTIFIER(NaClSyscallSeg)
        HIDDEN(NaClSyscallSeg)
IDENTIFIER(NaClSyscallSeg):
        cld
        xor     %eax, %eax
        mov     %gs, %ax
        shr     $3, %eax

        movl    IDENTIFIER(nacl_user)(,%eax,4), %edx

        /* check NaClThreadContext in sel_rt_32.h for the offsets */
        movl    %ebx, 0x0(%edx)
        movl    %esi, 0x4(%edx)
        movl    %edi, 0x8(%edx)
        movl    %ebp, 0x14(%edx)
        movl    %esp, 0x1c(%edx)

        /*
         * we do not save the registers (below) that the user is powerless
         * to change.  eip will be overwritten w/ the validated return
         * address pulled from the stack for the syscall return.
         *
         * these segment registers are restored by NaCl_springboard for
         * syscall return.
         */

#if 0
        movl    %eip, 0xc(%edx)
        mov     %cs, 0x3c(%edx)
        mov     %ds, 0x28(%edx)
        mov     %es, 0x2a(%edx)
        mov     %fs, 0x2c(%edx)
        mov     %gs, 0x2e(%edx)
        mov     %ss, 0x20(%edx)
#endif
        /*
         * load only the system segments; called code do not depend on
         * any registers having particular values.  we will clear/discard
         * caller-saved registers at system call return.
         */
        movl    IDENTIFIER(nacl_sys)(,%eax,4), %edx
        /*
         * %cs and %ds already taken care of by NaCl_trampoline_seg_code
         *
         * %ebx need not be saved/restored even if -fpic were used, since
         * in that case %ebx is initialized on fn entry.
         */
        mov     0x2a(%edx), %es
        mov     0x2c(%edx), %fs
        mov     0x2e(%edx), %gs

        mov     0x20(%edx), %ecx  /* ss */
        movl    0x1c(%edx), %edx  /* esp */
        mov     %ecx, %ss  /* disable intr */
        movl    %edx, %esp
        push    %eax
        call    IDENTIFIER(NaClSyscallCSegHook)
        /*
         * If stack usage in the above code changes, modify initial %esp
         * computation -- see nacl_switch_to_app.c:NaClStartThreadInApp.
         */
        hlt
        /* noret */
#endif
