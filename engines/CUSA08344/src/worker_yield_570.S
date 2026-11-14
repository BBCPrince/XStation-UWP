# SPDX-License-Identifier: GPL-2.0-or-later
.intel_syntax noprefix
.text
.globl yield570_begin, yield570_end, yield570_modeptr, yield570_callbackptr, yield570_normalptr, yield570_sleepptr
yield570_begin:
    cmp byte ptr [rdi+0x59],0
    jne .Loriginal_sleep
    # Preserve the SysV red zone, flags, all GPRs and PS4 x87/SSE/AVX state.
    lea rsp,[rsp-128]
    pushfq
    test r8d,1023
    jne .Lskip
    push rax
    mov rax,qword ptr [rip+yield570_modeptr]
    cmp dword ptr [rax],0
    pop rax
    je .Lskip
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rbx,rsp
    sub rsp,1024
    and rsp,-64
    # XSAVE's reserved header bytes must start at zero. Area is at rsp+64;
    # rsp[0..31] is Windows shadow space and cannot overlap saved state.
    mov qword ptr [rsp+576],0
    mov qword ptr [rsp+584],0
    mov qword ptr [rsp+592],0
    mov qword ptr [rsp+600],0
    mov qword ptr [rsp+608],0
    mov qword ptr [rsp+616],0
    mov qword ptr [rsp+624],0
    mov qword ptr [rsp+632],0
    mov eax,7
    xor edx,edx
    xsave64 [rsp+64]
    cld
    call qword ptr [rip+yield570_callbackptr]
    mov eax,7
    xor edx,edx
    xrstor64 [rsp+64]
    mov rsp,rbx
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
.Lskip:
    popfq
    lea rsp,[rsp+128]
    pause
    jmp qword ptr [rip+yield570_normalptr]
.Loriginal_sleep:
    jmp qword ptr [rip+yield570_sleepptr]
.balign 8
yield570_modeptr: .quad 0
yield570_callbackptr: .quad 0
yield570_normalptr: .quad 0
yield570_sleepptr: .quad 0
yield570_end:
