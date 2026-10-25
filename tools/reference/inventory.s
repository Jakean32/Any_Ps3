.machine "cell"
.text
.global _start
_start:
	# D-forms
	li %r3, 65
	lis %r4, 0x100
	addi %r5, %r3, -8
	addis %r6, %r3, 0x10
	addic %r7, %r3, 4
	addic. %r7, %r3, 4
	subfic %r8, %r3, 0x100
	mulli %r9, %r3, 7
	cmpwi %r7, 131
	cmplwi %r7, 131
	cmpdi %r7, 131
	cmpldi %r7, 131
	ori %r10, %r3, 0x1234
	oris %r10, %r3, 0x1234
	xori %r10, %r3, 0x1234
	xoris %r10, %r3, 0x1234
	andi. %r10, %r3, 0x1234
	andis. %r10, %r3, 0x1234
	# X-form ALU
	add %r3, %r4, %r5
	add. %r3, %r4, %r5
	subf %r3, %r4, %r5
	subf. %r3, %r4, %r5
	neg %r3, %r4
	mullw %r3, %r4, %r5
	mulld %r3, %r4, %r5
	divw %r3, %r4, %r5
	divwu %r3, %r4, %r5
	divd %r3, %r4, %r5
	divdu %r3, %r4, %r5
	mulhw %r3, %r4, %r5
	mulhwu %r3, %r4, %r5
	or %r3, %r4, %r5
	or. %r3, %r4, %r5
	xor %r3, %r4, %r5
	and %r3, %r4, %r5
	nand %r3, %r4, %r5
	nor %r3, %r4, %r5
	andc %r3, %r4, %r5
	orc %r3, %r4, %r5
	eqv %r3, %r4, %r5
	extsb %r3, %r4
	extsh %r3, %r4
	extsw %r3, %r4
	cntlzw %r3, %r4
	cntlzd %r3, %r4
	slw %r3, %r4, %r5
	srw %r3, %r4, %r5
	sraw %r3, %r4, %r5
	srawi %r3, %r4, 7
	sld %r3, %r4, %r5
	srd %r3, %r4, %r5
	srad %r3, %r4, %r5
	sradi %r3, %r4, 7
	sradi %r3, %r4, 37
	cmpw %cr0, %r4, %r5
	cmpd %cr0, %r4, %r5
	cmplw %cr0, %r4, %r5
	cmpld %cr0, %r4, %r5
	# rotates
	rlwinm %r3, %r4, 5, 8, 21
	rlwimi %r3, %r4, 5, 8, 21
	rlwnm %r3, %r4, %r5, 8, 21
	rldicl %r3, %r4, 5, 8
	rldicr %r3, %r4, 5, 8
	rldic %r3, %r4, 5, 8
	rldimi %r3, %r4, 5, 8
	rldcl %r3, %r4, %r5, 8
	rldcr %r3, %r4, %r5, 8
	# CR ops
	crand 4, 5, 6
	crandc 4, 5, 6
	creqv 4, 5, 6
	crnand 4, 5, 6
	crnor 4, 5, 6
	cror 4, 5, 6
	crorc 4, 5, 6
	crxor 4, 5, 6
	mfcr %r3
	mtcrf 0xFF, %r3
	mcrf 3, 5
	# SPR
	mflr %r3
	mtctr %r3
	mfxer %r4
	mtlr %r5
	mfctr %r6
	mtspr 1, %r7
	# loads/stores
	lwz %r3, 4(%r4)
	lwzu %r3, 4(%r4)
	lbz %r3, 4(%r4)
	lbzu %r3, 4(%r4)
	lhz %r3, 4(%r4)
	lhzu %r3, 4(%r4)
	lha %r3, 4(%r4)
	lhau %r3, 4(%r4)
	stw %r3, 4(%r4)
	stwu %r3, 4(%r4)
	stb %r3, 4(%r4)
	stbu %r3, 4(%r4)
	sth %r3, 4(%r4)
	sthu %r3, 4(%r4)
	lwzx %r3, %r4, %r5
	lbzx %r3, %r4, %r5
	lhzx %r3, %r4, %r5
	lhax %r3, %r4, %r5
	stwx %r3, %r4, %r5
	stbx %r3, %r4, %r5
	sthx %r3, %r4, %r5
	lwax %r3, %r4, %r5
	lfs %f1, 4(%r4)
	lfd %f2, 4(%r4)
	stfs %f1, 4(%r4)
	stfd %f2, 4(%r4)
	ld %r3, 8(%r4)
	ldu %r3, 8(%r4)
	lwa %r3, 8(%r4)
	std %r3, 8(%r4)
	stdu %r3, 8(%r4)
	ldx %r3, %r4, %r5
	ldux %r3, %r4, %r5
	stdx %r3, %r4, %r5
	stdux %r3, %r4, %r5
	lwbrx %r3, %r4, %r5
	stwbrx %r3, %r4, %r5
	dcbz %r4, %r5
	dcbst %r4, %r5
	dcbf %r4, %r5
	dcbt %r4, %r5
	eieio
	sync
	lwsync
	isync
	# branches
	b 1f
	bl 1f
	ba 0x100
	bla 0x100
	beq 1f
	bne 1f
	blt 1f
	bgt 1f
	bctr
	bctrl
	blr
	blrl
	bdz 1f
	bdnz 1f
1:	nop
	sc
	tw 31, %r3, %r4
	twi 31, %r3, 4
	td 31, %r3, %r4
	tdi 31, %r3, 4
	b .
