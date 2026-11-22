; CalcFront - hand-written eZ80 for the economy (src/sim/game.c)
;
; Every player grows troops and gold every tick, and gets a new troop cap
; every 16 ticks. In C both are chains of 32-bit library calls; here the
; common case is 24-bit math, MLT and short loops. Results are identical to
; the C versions in game.c (suffix _c), which the PC harness runs and the
; self-test (CF_SELFTEST) compares with these.
;
; void econ_grow(player_t *pl)            troop growth and gold for one tick
;   Troops and cap below 2^24 and no troop multiplier; anything else goes to
;   econ_grow_c (same arguments, so a plain jump).
; void calc_max(player_t *pl)             troop cap and large-territory factor
;   All cases; a troop multiplier calls troop_mul.
; void econ_fade(int8_t *r)               31 relations one point toward 0
; uint8_t econ_div8(uint24_t n, uint24_t d)   n / ((d >> 8) + 1), n <= d
; uint24_t econ_mulq8(uint24_t a, uint8_t g)  (a * g) >> 8, a < 2^16
;
; player_t offsets (checked by _Static_assert in game.c): type 0, troops 3,
; maxTroops 7, gold 11, growth 15, tiles 20, big8 47, cityLv 50.
; g_cfg: difficulty +2, goldMul +5, troopMul +6 (index 1 = x1).

	.assume	ADL = 1

PT_NATION	:= 2
PT_TRIBE	:= 3

	.section	.text._econ_grow,"ax",@progbits
	.globl	_econ_grow
	.type	_econ_grow,@function
_econ_grow:
	ld	iy, 0
	add	iy, sp
	ld	iy, (iy + 3)		; pl
	ld	a, (_g_cfg + 6)
	dec	a
	jp	nz, _econ_grow_c	; troop multiplier
	ld	a, (iy + 6)
	or	a, (iy + 10)
	jp	nz, _econ_grow_c	; troops or cap from 2^24
	ld	hl, (iy + 7)		; M
	ld	de, (iy + 3)		; T
	or	a, a
	sbc	hl, de			; n = M - T
	jp	z, .Lg_zero
	jp	c, .Lg_over
	push	hl
	; r8 = n / ((M >> 8) + 1): the high bytes of M, the top one 0
	ld	de, (iy + 8)
	call	div8_q
	push	af
	; T^0.73 / 4 = 2^(log2(T) * 187 / 256 - 2), 0 for T < 2
	lea	hl, iy + 3
	call	log2_m
	ld	c, 187
	call	mulq8_lg
	ld	a, h
	sub	a, 2
	jr	c, .Lg_small
	ld	h, a
	call	exp2_24			; < 2^16
	jr	.Lg_pow
.Lg_small:
	or	a, a
	sbc	hl, hl
.Lg_pow:
	ld	de, 10
	add	hl, de			; add
	pop	af
	ld	c, a
	call	mul16q8			; (add * r8) >> 8
	ld	a, (iy + 0)
	cp	a, PT_NATION
	jr	z, .Lg_nation
	cp	a, PT_TRIBE
	jr	nz, .Lg_cap
	srl	h			; tribes: half
	rr	l
	jr	.Lg_cap
.Lg_nation:				; (add * growMul) >> 8, growMul = 256 + lo from hard on
	ld	a, (_g_cfg + 2)
	ld	b, a
	push	hl
	ld	hl, _growLo
	ld	de, 0
	ld	e, a
	add	hl, de
	ld	c, (hl)
	pop	hl
	push	hl
	call	mul16q8			; keeps B
	pop	de
	ld	a, b
	cp	a, 2			; DIFF_HARD
	jr	c, .Lg_cap
	add	hl, de
.Lg_cap:				; a = add + 1, at most n
	inc	hl
	pop	de
	or	a, a
	sbc	hl, de
	jr	nc, .Lg_set		; DE = n
	add	hl, de
	ex	de, hl			; DE = a
.Lg_set:
	ld	hl, (iy + 3)
	add	hl, de
	ld	(iy + 3), hl		; troops += a (stays below M < 2^24)
	ld	(iy + 15), de
	xor	a, a
	ld	(iy + 18), a		; growth = a
	jr	.Lg_gold
.Lg_over:				; T > M: troops -= ((T - M) >> 5) + 1
	ex	de, hl
	or	a, a
	sbc	hl, hl
	sbc	hl, de			; T - M
	xor	a, a
	add	hl, hl
	rla
	add	hl, hl
	rla
	add	hl, hl
	rla				; A:HL = (T - M) << 3
	ld	(iy + 15), hl		; growth as scratch, cleared below
	ld	(iy + 18), a
	ld	de, (iy + 16)		; (T - M) >> 5
	ld	hl, (iy + 3)
	scf
	sbc	hl, de
	ld	(iy + 3), hl
.Lg_zero:
	or	a, a
	sbc	hl, hl
	ld	(iy + 15), hl
	ld	(iy + 18), l		; growth = 0
.Lg_gold:				; gold += 100, 50 for tribes, times the gold multiplier
	ld	bc, 100
	ld	a, (iy + 0)
	cp	a, PT_TRIBE
	jr	nz, .Lg_g100
	ld	c, 50
.Lg_g100:
	ld	a, (_g_cfg + 5)
	dec	a
	jr	nz, .Lg_gmul
	ld	hl, (iy + 11)
	add	hl, bc
	ld	(iy + 11), hl
	ret	nc
	inc	(iy + 14)
	ret
.Lg_gmul:
	ld	de, 0
	push	de
	push	bc
	call	_gold_mul		; E:HL
	pop	bc
	pop	bc
	ld	iy, 0
	add	iy, sp
	ld	iy, (iy + 3)
	ld	bc, (iy + 11)
	add	hl, bc
	ld	(iy + 11), hl
	ld	a, e
	adc	a, (iy + 14)
	ld	(iy + 14), a
	ret
.Lfunc_end_grow:
	.size	_econ_grow, .Lfunc_end_grow-_econ_grow

	.section	.text._calc_max,"ax",@progbits
	.globl	_calc_max
	.type	_calc_max,@function
_calc_max:
	push	ix
	ld	ix, 0
	add	ix, sp
	ld	ix, (ix + 6)		; pl (IX survives the calls below)
	; n = tiles * S < 2^22, written to maxTroops (scratch until the end)
	ld	a, (_g_S)
	ld	d, (ix + 20)
	ld	e, a
	mlt	de
	ld	h, (ix + 21)
	ld	l, a
	mlt	hl
	ld	(ix + 7), e
	ld	a, d
	add	a, l
	ld	(ix + 8), a
	ld	a, h
	adc	a, 0
	ld	(ix + 9), a
	lea	hl, ix + 7
	call	log2_m			; lg = B:A
	ld	c, a
	push	bc
	ld	c, 154
	call	mulq8_lg
	ld	a, h
	add	a, 8
	ld	h, a
	call	exp2_24			; e = 2^((lg * 154 >> 8) / 256 + 8) < 2^22
	; (e >> 5) * 125 + (((e & 31) * 125) >> 5) = e * 1000 >> 8, and
	; e * 1000 = ((e << 7) - 3e) << 3 < 2^32
	push	hl
	pop	de
	add	hl, hl
	add	hl, de
	ex	de, hl			; DE = 3e, HL = e
	xor	a, a
	ld	b, 7
.Lc_sh7:
	add	hl, hl
	rla
	djnz	.Lc_sh7
	or	a, a
	sbc	hl, de
	sbc	a, 0
	ld	b, 3
.Lc_sh3:
	add	hl, hl
	rla
	djnz	.Lc_sh3
	ld	(ix + 7), hl
	ld	(ix + 10), a
	ld	hl, (ix + 8)		; e * 1000 >> 8
	ld	de, 50000
	add	hl, de
	xor	a, a
	add	hl, hl
	rla
	ld	e, a			; m = E:HL = 2 * (... + 50000)
	ld	a, (ix + 50)
	or	a, (ix + 51)
	jr	z, .Lc_nocity
	push	hl			; m += cityLv * 250000
	push	de
	or	a, a
	sbc	hl, hl
	ld	l, (ix + 50)
	ld	h, (ix + 51)
	ld	e, 0
	ld	bc, 250000
	xor	a, a
	call	__lmulu
	pop	bc
	ld	a, c
	pop	bc
	call	__ladd
.Lc_nocity:
	ld	a, (ix + 0)
	cp	a, PT_TRIBE
	jr	nz, .Lc_nat
	ld	bc, 3			; tribes: m / 3
	xor	a, a
	call	__ldivu
	jr	.Lc_mul
.Lc_nat:
	cp	a, PT_NATION
	jr	nz, .Lc_mul
	push	hl			; nations: (m >> 2) * (2 + difficulty)
	pop	bc
	ld	a, e
	ld	l, 2
	call	__lshru
	push	bc
	pop	hl
	ld	e, a
	ld	a, (_g_cfg + 2)
	add	a, 2
	ld	bc, 0
	ld	c, a
	xor	a, a
	call	__lmulu
.Lc_mul:
	ld	a, (_g_cfg + 6)
	dec	a
	jr	z, .Lc_cap
	push	de
	push	hl
	call	_troop_mul
	pop	bc
	pop	bc
.Lc_cap:				; at most 60000000 = 0x3938700
	ld	a, e
	cp	a, 3
	jr	c, .Lc_store
	jr	nz, .Lc_capit
	ld	bc, 0x938701
	push	hl
	or	a, a
	sbc	hl, bc
	pop	hl
	jr	c, .Lc_store
.Lc_capit:
	ld	hl, 0x938700
	ld	e, 3
.Lc_store:
	ld	(ix + 7), hl
	ld	(ix + 10), e
	; big8 = 1 / (1 + (300000/n)^2.5) in 1/255: with l = (lg - 4658) * 5 / 2
	; (rounded toward 0), 0 below -2000 (lg < 3858), 255 above 2000
	; (lg >= 5459), else x8 * 255 / (256 + x8) with x8 = 2^(l/256 + 8)
	pop	bc
	or	a, a
	sbc	hl, hl
	ld	h, b
	ld	l, c			; lg
	ld	de, 3858
	sbc	hl, de			; t = lg - 3858
	ld	a, 0
	jr	c, .Lc_big
	ld	de, 5459 - 3858
	sbc	hl, de
	ld	a, 255
	jr	nc, .Lc_big
	add	hl, de			; t < 1601
	; l + 2048 = (5t + 96 + (l < 0)) >> 1, l < 0 when t < 800
	push	hl
	pop	de
	add	hl, hl
	add	hl, hl
	add	hl, de
	ld	bc, 96
	add	hl, bc
	ex	de, hl
	ld	bc, 800
	or	a, a
	sbc	hl, bc
	ex	de, hl
	ld	de, 0
	adc	hl, de
	srl	h
	rr	l
	call	exp2_24			; x8 < 2^16
	push	hl
	inc	h
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	ex	de, hl			; (256 + x8) << 7
	pop	bc
	push	bc
	pop	hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	or	a, a
	sbc	hl, bc			; x8 * 255
	call	div8_loop
.Lc_big:
	ld	(ix + 47), a
	pop	ix
	ret
.Lfunc_end_max:
	.size	_calc_max, .Lfunc_end_max-_calc_max

	.section	.text._econ_div8,"ax",@progbits
	.globl	_econ_div8
	.type	_econ_div8,@function
_econ_div8:
	ld	iy, 0
	add	iy, sp
	ld	hl, (iy + 3)
	ld	de, 0
	ld	e, (iy + 7)
	ld	d, (iy + 8)
	jp	div8_q
.Lfunc_end_div8:
	.size	_econ_div8, .Lfunc_end_div8-_econ_div8

	.section	.text._econ_mulq8,"ax",@progbits
	.globl	_econ_mulq8
	.type	_econ_mulq8,@function
_econ_mulq8:
	ld	iy, 0
	add	iy, sp
	ld	c, (iy + 6)
	or	a, a
	sbc	hl, hl
	ld	l, (iy + 3)
	ld	h, (iy + 4)
	jp	mul16q8
.Lfunc_end_mulq8:
	.size	_econ_mulq8, .Lfunc_end_mulq8-_econ_mulq8

;  void econ_fade(int8_t *r): r[0..30] one point toward 0 (the relations to
;  players 1..31, every 16 ticks)
	.section	.text._econ_fade,"ax",@progbits
	.globl	_econ_fade
	.type	_econ_fade,@function
_econ_fade:
	ld	hl, 3
	add	hl, sp
	ld	hl, (hl)
	ld	b, 31
.Lf_loop:
	ld	a, (hl)
	or	a, a
	jr	z, .Lf_next
	jp	p, .Lf_pos
	inc	a
	inc	a			; negative: +1 (after the dec below)
.Lf_pos:
	dec	a
	ld	(hl), a
.Lf_next:
	inc	hl
	djnz	.Lf_loop
	ret
.Lfunc_end_fade:
	.size	_econ_fade, .Lfunc_end_fade-_econ_fade

; ---- shared helpers (register arguments) ----
	.section	.text.econ_helpers,"ax",@progbits

; A = HL / (DE + 1) for HL < 256 * (DE + 1), DE < 2^16 (U = 0).
; Clobbers B, DE, HL.
div8_q:
	ex	de, hl
	inc	hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	ex	de, hl
; A = HL / (DE >> 7) for DE = q << 7 <= 2^23, HL < 2 * DE: 8 restoring steps
div8_loop:
	ld	b, 8
.Ld8:
	or	a, a
	sbc	hl, de
	jr	nc, .Ld8_fit
	add	hl, de			; undo, sets carry
.Ld8_fit:
	rla				; quotient bit, inverted
	add	hl, hl
	djnz	.Ld8
	cpl
	ret

; HL = (HL * C) >> 8 for HL < 2^16 (U = 0). Clobbers A, DE; keeps BC.
mul16q8:
	ld	d, l
	ld	e, c
	mlt	de
	ld	a, d			; (low * g) >> 8
	ld	l, c
	mlt	hl			; high * g
	add	a, l
	ld	l, a
	ret	nc
	inc	h
	ret

; log2 of the 24-bit value at (HL), Q8: B = integer part, A = fraction
; (0 for x < 2, as fx_log2). Clobbers DE, HL.
log2_m:
	inc	hl
	inc	hl
	ld	a, (hl)
	dec	hl
	ld	d, (hl)
	ld	b, 23
	or	a, a
	jr	nz, .Llg_norm
	ld	a, d
	dec	hl
	ld	d, (hl)
	ld	b, 15
	or	a, a
	jr	nz, .Llg_norm
	ld	a, d
	ld	d, 0
	ld	b, 7
	cp	a, 2
	jr	nc, .Llg_norm
	xor	a, a
	ld	b, a
	ret
.Llg_norm:				; leading one to bit 7 of A
	bit	7, a
	jr	nz, .Llg_frac
.Llg_shift:
	sla	d
	rla
	dec	b
	bit	7, a
	jr	z, .Llg_shift
.Llg_frac:
	sla	d			; A = the 8 bits after the leading one
	rla
	ld	hl, _fx_logtab
	ld	de, 0
	ld	e, a
	add	hl, de
	ld	a, (hl)
	ret

; HL = (lg * C) >> 8 for lg = B:A (Q8). Clobbers A, DE.
mulq8_lg:
	ld	d, a
	ld	e, c
	mlt	de			; D = (fraction * C) >> 8
	or	a, a
	sbc	hl, hl
	ld	h, b
	ld	l, c
	mlt	hl			; integer part * C
	ld	a, l
	add	a, d
	ld	l, a
	ret	nc
	inc	h
	ret

; HL = 2^(HL / 256) for H <= 23 (as fx_exp2: 16-bit mantissa, shifted).
; Clobbers A, B, DE.
exp2_24:
	ld	a, h
	ld	de, 0
	ld	e, l
	ld	hl, _fx_exptab
	add	hl, de
	add	hl, de
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	ex	de, hl			; mantissa, 2^15 .. 2^16 - 1
	sub	a, 15
	jr	nc, .Lex_left
	neg				; shift right by 15 - integer part
	cp	a, 8
	jr	c, .Lex_right
	ld	l, h
	ld	h, 0
	sub	a, 8
	ret	z
.Lex_right:
	ld	b, a
.Lex_r:
	srl	h
	rr	l
	djnz	.Lex_r
	ret
.Lex_left:
	ret	z
	ld	b, a
.Lex_l:
	add	hl, hl
	djnz	.Lex_l
	ret

	.extern	_g_cfg
	.extern	_g_S
	.extern	_growLo
	.extern	_fx_logtab
	.extern	_fx_exptab
	.extern	_econ_grow_c
	.extern	_gold_mul
	.extern	_troop_mul
	.extern	__lmulu
	.extern	__ladd
	.extern	__ldivu
	.extern	__lshru
