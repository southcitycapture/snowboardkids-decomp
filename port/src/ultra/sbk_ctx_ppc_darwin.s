; Stackful context switch for 32-bit PowerPC, Mac OS X ABI.
;
; struct sbk_ctx { r1 @0; lr @4; cr @8; r13..r31 @12..88; f14..f31 @88..232 }
;
; Callee-saved state under the Darwin PPC32 ABI: r1, r13-r31, f14-f31, CR2-CR4,
; and LR (as the return address). Everything else is clobbered by any call, so a
; cooperative switch made through a normal function call needs only these.
; (AltiVec v20-v31 are deliberately not saved: no code in this program keeps
; vector registers live across a scheduler call.)

	.text
	.align 2

; void sbk_ctx_switch(sbk_ctx *from /* r3 */, sbk_ctx *to /* r4 */)
	.globl _sbk_ctx_switch
_sbk_ctx_switch:
	mflr	r0
	stw	r1, 0(r3)
	stw	r0, 4(r3)
	mfcr	r0
	stw	r0, 8(r3)
	stmw	r13, 12(r3)
	stfd	f14, 88(r3)
	stfd	f15, 96(r3)
	stfd	f16, 104(r3)
	stfd	f17, 112(r3)
	stfd	f18, 120(r3)
	stfd	f19, 128(r3)
	stfd	f20, 136(r3)
	stfd	f21, 144(r3)
	stfd	f22, 152(r3)
	stfd	f23, 160(r3)
	stfd	f24, 168(r3)
	stfd	f25, 176(r3)
	stfd	f26, 184(r3)
	stfd	f27, 192(r3)
	stfd	f28, 200(r3)
	stfd	f29, 208(r3)
	stfd	f30, 216(r3)
	stfd	f31, 224(r3)

	lwz	r1, 0(r4)
	lwz	r0, 4(r4)
	mtlr	r0
	lwz	r0, 8(r4)
	mtcrf	0xff, r0
	lmw	r13, 12(r4)
	lfd	f14, 88(r4)
	lfd	f15, 96(r4)
	lfd	f16, 104(r4)
	lfd	f17, 112(r4)
	lfd	f18, 120(r4)
	lfd	f19, 128(r4)
	lfd	f20, 136(r4)
	lfd	f21, 144(r4)
	lfd	f22, 152(r4)
	lfd	f23, 160(r4)
	lfd	f24, 168(r4)
	lfd	f25, 176(r4)
	lfd	f26, 184(r4)
	lfd	f27, 192(r4)
	lfd	f28, 200(r4)
	lfd	f29, 208(r4)
	lfd	f30, 216(r4)
	lfd	f31, 224(r4)
	blr

; First entry into a fresh context. sbk_ctx_init() parks the thread entry
; point in r14 and its argument in r15 (both callee-saved, so they survive the
; switch) and sets LR to this trampoline.
	.globl _sbk_ctx_trampoline
_sbk_ctx_trampoline:
	mr	r3, r15
	mtctr	r14
	bctrl
	bl	_sbk_thread_exit
	trap
