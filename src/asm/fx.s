; CalcFront - hand-written eZ80 for the fixed-point helpers of the economy
;
; Every player's troop growth needs x^0.73 each tick. The C versions
; (src/sim/fx.c, used by the PC harness) compile to shift-library loops; these
; do the same work with byte moves, 8-bit shifts and MLT. Results are
; identical: the profiling build checks them against the C versions.
;
; uint16_t fx_log2(uint32_t x)        log2(x) in Q8, 0 for x < 2
;   (sp+3) low 24 bits of x, (sp+6) high byte. Result in HL.
; uint32_t fx_exp2(int16_t l)         2^(l/256), 0 for l < 0
;   (sp+3) l. Result in E:HL.
; uint32_t fx_pow(uint32_t x, uint8_t p8)   x^(p8/256), 0 for x = 0
;   (sp+3) low 24 bits of x, (sp+6) high byte, (sp+9) p8. Result in E:HL.
; Uses AF, BC, DE, HL, IY (all free for the callee in this ABI).

	.assume	ADL = 1

	.section	.text._fx_log2,"ax",@progbits
	.globl	_fx_log2
	.type	_fx_log2,@function
_fx_log2:
	ld	iy, 0
	add	iy, sp
	lea	hl, iy + 6		; the top byte of x
	ld	c, 31			; position of its bit 7
	ld	b, 3
.Llog_byte:				; find the top nonzero byte
	ld	a, (hl)
	or	a, a
	jr	nz, .Llog_found
	dec	hl
	ld	a, c
	sub	a, 8
	ld	c, a
	djnz	.Llog_byte
	ld	a, (hl)			; x < 256: only the low byte
	ld	d, 0
	cp	a, 2
	jr	nc, .Llog_norm
	or	a, a			; x < 2
	sbc	hl, hl
	ret
.Llog_found:
	dec	hl
	ld	d, (hl)			; the byte below it
.Llog_norm:
	bit	7, a			; move the leading one to bit 7 of A
	jr	nz, .Llog_frac
.Llog_shift:
	sla	d
	rla
	dec	c
	bit	7, a
	jr	z, .Llog_shift
.Llog_frac:
	sla	d			; A = the 8 bits after the leading one
	rla
	ld	de, 0
	ld	e, a
	ld	hl, _fx_logtab
	add	hl, de
	ld	a, (hl)
	or	a, a
	sbc	hl, hl
	ld	l, a			; fraction
	ld	h, c			; integer part
	ret
.Lfunc_end_log2:
	.size	_fx_log2, .Lfunc_end_log2-_fx_log2

	.section	.text._fx_exp2,"ax",@progbits
	.globl	_fx_exp2
	.type	_fx_exp2,@function
_fx_exp2:
	ld	iy, 0
	add	iy, sp
	ld	a, (iy + 4)		; integer part
	bit	7, a
	jr	nz, .Lexp_zero		; l < 0
	ld	de, 0
	ld	e, (iy + 3)		; fraction: index of the Q15 mantissa
	ld	hl, _fx_exptab
	add	hl, de
	add	hl, de
	ld	c, (hl)
	inc	hl
	ld	b, (hl)			; BC = mantissa, 2^15 .. 2^16 - 1
	cp	a, 15
	jr	nc, .Lexp_big
	neg				; shift right by 15 - integer part
	add	a, 15
.Lexp_right:
	srl	b
	rr	c
	dec	a
	jr	nz, .Lexp_right
	or	a, a
	sbc	hl, hl
	ld	h, b
	ld	l, c
	ld	e, 0
	ret
.Lexp_big:
	cp	a, 31
	jr	nc, .Lexp_max
	sub	a, 15			; shift left by integer part - 15
	ld	d, a
	or	a, a
	sbc	hl, hl
	ld	h, b
	ld	l, c
	ld	e, 0
	ld	a, d
	or	a, a
	ret	z
.Lexp_left:
	add	hl, hl			; 24-bit, carry into E
	rl	e
	dec	a
	jr	nz, .Lexp_left
	ret
.Lexp_max:
	scf
	sbc	hl, hl			; HL = all ones
	ld	e, 255
	ret
.Lexp_zero:
	or	a, a
	sbc	hl, hl
	ld	e, 0
	ret
.Lfunc_end_exp2:
	.size	_fx_exp2, .Lfunc_end_exp2-_fx_exp2

	.section	.text._fx_pow,"ax",@progbits
	.globl	_fx_pow
	.type	_fx_pow,@function
_fx_pow:
	ld	iy, 0
	add	iy, sp
	ld	hl, (iy + 3)
	ld	de, 0
	ld	e, (iy + 6)
	ld	a, e
	or	a, a
	jr	nz, .Lpow_go
	or	a, a
	sbc	hl, de			; DE = 0: Z if the low 24 bits are 0 too
	jr	nz, .Lpow_go2
	ret				; x = 0: E:HL = 0
.Lpow_go2:
	add	hl, de			; restore x (DE = 0)
.Lpow_go:
	push	de			; x for fx_log2: high byte
	push	hl			; low 24 bits
	call	_fx_log2
	pop	de
	pop	de
	ld	iy, 0
	add	iy, sp
	ld	a, (iy + 9)		; p8
	ld	b, h
	ld	c, a
	mlt	bc			; integer part * p8
	ld	d, l
	ld	e, a
	mlt	de			; fraction * p8: keep the high byte
	ld	a, d
	or	a, a
	sbc	hl, hl
	ld	h, b
	ld	l, c
	ld	de, 0
	ld	e, a
	add	hl, de			; (log2(x) * p8) >> 8, below 2^13
	push	hl
	call	_fx_exp2
	pop	bc
	ret
.Lfunc_end_pow:
	.size	_fx_pow, .Lfunc_end_pow-_fx_pow

	.extern	_fx_logtab
	.extern	_fx_exptab
