bits 32 ;想开辟个新天地...

section .multiboot ;可是啊
align 4 ;我仍感到迷茫
dd 0x1BADB002 ;老师可否为我指明人生方向
dd 0x00 ;此刻的我
dd - (0x1BADB002 + 0x00) ;刚从零点起步

section .bss ;无法排列你
align 16 ;七百颗透明的心脏
stack_bottom: ;对
resb 16384 ;沾满泥水的我来说
stack_top: ;你的描绘

section .text ;似乎有些
global _start ;遥不可望
_start: ;请告诉我
    mov esp, stack_top ;这些希冀
    
    extern kernel_main ;并非
    call kernel_main ;只是
    
    cli ;某些
.hang:
    hlt
    jmp .hang ;痴心妄想