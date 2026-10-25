.machine "cell"
.text
.globl _start
_start:
	li %r11, 403            # sys_tty_write
	li %r3, 0               # channel
	lis %r4, msg@ha
	addi %r4, %r4, msg@l    # r4 = &msg
	li %r5, msgend-msg      # len
	li %r6, 0               # nwritten = NULL
	sc
	li %r11, 22             # _sys_process_exit
	li %r3, 42              # exit code 42
	sc
	b .
.section .data
msg:	.ascii "Hello from Any_Ps3!\n"
msgend:
