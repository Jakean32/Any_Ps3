.machine "cell"
.text
.globl _start
_start:
	lis %r4, 1              # 0x10000 data base
	ori %r4, %r4, 0x1000    # r4 = 0x11000
	li %r3, 100
	addi %r3, %r3, 23       # 123
	stw %r3, 0(%r4)
	li %r3, 10
	li %r5, 3
	mullw %r6, %r3, %r5     # 30
	stw %r6, 4(%r4)
	li %r3, 1
	srawi %r3, %r3, 1       # 0 (arith shift of 1>>1)
	stw %r3, 8(%r4)
	li %r3, -1              # 0xFFFFFFFF
	srwi %r3, %r3, 28       # 15
	stw %r3, 12(%r4)
	li %r3, 0x10
	slwi %r3, %r3, 4        # 256
	stw %r3, 16(%r4)
	lis %r3, 0x1234
	ori %r3, %r3, 0x5678     # 0x12345678
	sradi %r3, %r3, 8        # 0x00123456
	stw %r3, 20(%r4)
	li %r3, 0x7F
	rldicl %r3, %r3, 40, 0   # 0x7F rot-left 40 in 64-bit
	std %r3, 24(%r4)
	li %r3, 0x66
	li %r5, 0x99
	subf %r6, %r5, %r3      # 0x66 - 0x99 = -0x33
	stw %r6, 32(%r4)
	li %r11, 22
	li %r3, 7               # exit code 7
	sc
	b .
.section .data
