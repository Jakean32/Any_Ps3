.machine "cell"
.text
.globl _start
_start:
	lis %r4, 1
	ori %r4, %r4, 0x1000    # r4 = 0x11000
	li %r7, 0               # sum
	li %r8, 5               # i
1:	add %r7, %r7, %r8       # sum += i
	addi %r8, %r8, -1        # i--
	cmpwi %r8, 0
	bgt 1b                  # while (i > 0)
	stw %r7, 0(%r4)         # expect 15
	li %r9, 4
	mtctr %r9
2:	addi %r7, %r7, 1         # add 1, four times (bdnz)
	bdnz 2b
	stw %r7, 4(%r4)         # expect 19
	li %r11, 22
	li %r3, 3                # exit code 3
	sc
	b .
.section .data
