; CalcFront - hand-written eZ80 helpers for land attacks (src/sim/attack.c)
;
; The C versions (suffix _c in attack.c) serve the PC harness; the profiling
; build checks these against them (attack_selftest). They replace library
; calls: a 24-bit multiply and a 16-step shift loop in rnd_n, 32-bit
; multiplies and shift loops in the tile cost, a 24-bit division.
;
; uint8_t rnd_below(uint8_t n)          rnd_n(n) for n <= 255: (rnd() * n) >> 16
;   (sp+3) n. Result in A. n < 2: 0, without drawing (as rnd_n).
; uint24_t mul_q8(uint24_t x, uint16_t k)   (x * k) >> 8
;   (sp+3) x, (sp+6) k. Result in HL; exact when x * k < 2^32.
; uint16_t free_cost(uint24_t x, uint24_t d)  cost of a tile of free land:
;   w = x / d clamped to 5..100 (5 when the quotient is below 6), times
;   2 * g_L16. (sp+3) x <= 200000, (sp+6) d, 1 <= d <= 200001. Result in HL.
; uint24_t udiv24(uint24_t x, uint24_t d)   x / d, d >= 1; see below.
; Uses AF, BC, DE, HL, IY (all free for the callee in this ABI).

	.assume	ADL = 1

; Offsets in attack_t (checked by _Static_assert in attack.c)
A_ATT	= 1
A_DEF	= 2
A_HEAD	= 14
A_COUNT	= 15
A_RING	= 18

; atk_ctx_t (src/sim/attack.c), attack_t and player_t offsets (checked there)
X_A	= 0
X_DP	= 3
X_CONQ	= 6
X_LOSS	= 9
X_T	= 12
X_DLOSS	= 15
X_DEF	= 19
X_ATT	= 20
X_CACHE	= 21
X_TRIBE	= 22
X_TAKEN	= 23
X_POPS	= 24
X_KEY	= 25
A_ACTIVE = 0
A_TROOPS = 4
A_BUDGET = 8
P_ALIVE	= 1
P_TROOPS = 3
P_TILES	= 20
P_NPOST	= 53
BUDGET_MAX = 60000
A_BORDER = 12
P_TYPE	= 0
P_BIG8	= 47
P_TRAIT	= 48
PT_TRIBE = 3
; atk_K_t
K_LOSSR	= 0
K_TERM	= 2
K_BAS	= 4
K_BD	= 6
K_SC8	= 8
K_TRAIT	= 11
K_L16	= 12
K_TRIBE	= 13
K_TAGLEN = 9			; bAs .. L16, the cost tag

	.section	.text._rnd_below,"ax",@progbits
	.globl	_rnd_below
	.type	_rnd_below,@function
_rnd_below:
	ld	hl, 3
	add	hl, sp
	ld	a, (hl)
	cp	a, 2
	jr	c, .Lrb_zero
	call	_rnd			; HL = r (16 bits)
	ld	iy, 0
	add	iy, sp
	ld	a, (iy + 3)		; n
	ld	d, l
	ld	e, a
	mlt	de			; r_lo * n
	ld	b, h
	ld	c, a
	mlt	bc			; r_hi * n
	ld	a, d			; (r_hi * n * 256 + r_lo * n) >> 16
	add	a, c
	ld	a, b
	adc	a, 0
	ret
.Lrb_zero:
	xor	a, a
	ret
.Lrb_end:
	.size	_rnd_below, .Lrb_end-_rnd_below

	.section	.text._mul_q8,"ax",@progbits
	.globl	_mul_q8
	.type	_mul_q8,@function
_mul_q8:
	; x = x2:x1:x0, k = k1:k0. (x * k) >> 8 mod 2^24 =
	;   (x0k0 >> 8) + x0k1 + x1k0 + ((x1k1 + x2k0 + (x2k1 << 8)) << 8)
	; MLT operands are cleared to 24 bits first: the result keeps no
	; stale upper byte whatever MLT does with it.
	ld	iy, 0
	add	iy, sp
	or	a, a
	sbc	hl, hl
	ld	bc, 0
	ld	b, (iy + 3)
	ld	c, (iy + 6)
	mlt	bc			; x0k0
	ld	l, b
	ld	bc, 0
	ld	b, (iy + 3)
	ld	c, (iy + 7)
	mlt	bc			; x0k1
	add	hl, bc
	ld	bc, 0
	ld	b, (iy + 4)
	ld	c, (iy + 6)
	mlt	bc			; x1k0
	add	hl, bc
	ex	de, hl			; DE = low part
	ld	hl, 0
	ld	h, (iy + 4)
	ld	l, (iy + 7)
	mlt	hl			; x1k1
	ld	bc, 0
	ld	b, (iy + 5)
	ld	c, (iy + 6)
	mlt	bc			; x2k0
	add	hl, bc
	ld	b, (iy + 5)
	ld	c, (iy + 7)
	mlt	bc			; x2k1: its low byte counts
	ld	a, h
	add	a, c
	ld	h, a			; U = high part mod 2^16
	push	hl			; U << 8: bytes 0, L, H
	dec	sp
	pop	hl
	inc	sp
	ld	l, 0
	add	hl, de
	ret
.Lmq_end:
	.size	_mul_q8, .Lmq_end-_mul_q8

	.section	.text._free_cost,"ax",@progbits
	.globl	_free_cost
	.type	_free_cost,@function
_free_cost:
	ld	iy, 0
	add	iy, sp
	ld	hl, (iy + 6)		; d
	push	hl			; x < 6 d: w = 5 (the usual case)
	pop	de
	add	hl, hl
	add	hl, de
	add	hl, hl
	ex	de, hl
	ld	hl, (iy + 3)
	or	a, a
	sbc	hl, de
	ld	c, 5
	jr	c, .Lfc_w
	ld	hl, (iy + 6)
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	ex	de, hl			; DE = 64 d (< 2^24)
	ld	hl, (iy + 3)		; x
	ld	c, 100
	push	de
	ex	de, hl
	add	hl, hl			; 128 d
	ex	de, hl
	pop	de
	jr	c, .Lfc_div		; 128 d >= 2^24 > x
	; x >= 128 d: the quotient is above 100
	push	hl
	push	de
	ex	de, hl
	add	hl, hl
	ex	de, hl			; DE = 128 d
	or	a, a
	sbc	hl, de
	pop	de
	pop	hl
	jr	nc, .Lfc_w
.Lfc_div:				; 7-bit quotient: x < 128 d
	ld	bc, 0x700		; B = 7 steps, C = quotient
.Lfc_loop:
	sla	c
	or	a, a
	sbc	hl, de
	jr	c, .Lfc_rest
	inc	c
	jr	.Lfc_shift
.Lfc_rest:
	add	hl, de
.Lfc_shift:
	add	hl, hl
	djnz	.Lfc_loop
	ld	a, c
	cp	a, 6
	jr	nc, .Lfc_hi
	ld	c, 5
	jr	.Lfc_w
.Lfc_hi:
	cp	a, 101
	jr	c, .Lfc_w
	ld	c, 100
.Lfc_w:					; HL = w * 2 * g_L16
	or	a, a
	sbc	hl, hl
	ld	h, c
	ld	a, (_g_L16)
	ld	l, a
	mlt	hl
	add	hl, hl
	ret
.Lfc_end:
	.size	_free_cost, .Lfc_end-_free_cost

	.section	.text._udiv24,"ax",@progbits
	.globl	_udiv24
	.type	_udiv24,@function
_udiv24:
	; uint24_t udiv24(uint24_t x, uint24_t d)   x / d, d >= 1
	; (sp+3) x, (sp+6) d. Result in HL. Restoring division with the divisor
	; first doubled up to the dividend: one step per quotient bit (a troop
	; ratio needs about 10, a loss per tile 6) instead of 24.
	ld	iy, 0
	add	iy, sp
	ld	hl, (iy + 3)		; R = x
	ld	de, (iy + 6)		; D = d
	or	a, a
	sbc	hl, de
	jr	c, .Ldv_zero		; x < d
	add	hl, de
	ld	b, 1			; quotient bits
.Ldv_norm:				; while 2 D <= R: D *= 2
	push	hl
	or	a, a
	sbc	hl, de
	sbc	hl, de
	pop	hl
	jr	c, .Ldv_go
	ex	de, hl
	add	hl, hl
	ex	de, hl
	inc	b
	jr	.Ldv_norm
.Ldv_go:
	ld	iy, 0			; quotient
.Ldv_step:
	add	iy, iy
	or	a, a
	sbc	hl, de
	jr	nc, .Ldv_one
	add	hl, de
	jr	.Ldv_next
.Ldv_one:
	inc	iy
.Ldv_next:
	dec	b
	jr	z, .Ldv_done
	add	hl, hl			; R *= 2 instead of D /= 2
	jr	nc, .Ldv_step
	add	iy, iy			; R >= 2^24 > D: the bit is 1, and R - D
	or	a, a			; is right modulo 2^24
	sbc	hl, de
	inc	iy
	jr	.Ldv_next
.Ldv_done:
	lea	hl, iy + 0
	ret
.Ldv_zero:
	or	a, a
	sbc	hl, hl
	ret
.Ldv_end:
	.size	_udiv24, .Ldv_end-_udiv24

	.section	.text._pop_tile,"ax",@progbits
	.globl	_pop_tile
	.type	_pop_tile,@function
_pop_tile:
	; uint8_t pop_tile(void)
	; Pops tiles of the ring of atk_ctx.a (while atk_ctx.pops < 48, counting
	; them there) until one is worth attacking: land of the defender touching
	; the attacker, not put back as exposed (a tile with one attacker
	; neighbour goes back to the end of the ring, when more than 3 are
	; queued, if rnd_n(terrain + 1) is not 0). Dead tiles leave the ring.
	; Returns its key (terrain | fallout << 2), the tile left at
	; ring[head], or 255 for none.
	; Registers: IX attack, IY tile pointer, BC row stride, D attacker,
	; E its land around the tile.
	push	ix
	ld	ix, (_atk_ctx + X_A)
	ld	bc, 0
	ld	a, (_g_W)
	ld	c, a
	ld	a, (_g_W+1)
	ld	b, a
.Lpt_loop:
	ld	a, (_atk_ctx + X_POPS)
	cp	a, 48
	jp	z, .Lpt_none
	ld	a, (ix + A_COUNT)
	or	a, a
	jp	z, .Lpt_none
	ld	hl, _atk_ctx + X_POPS
	inc	(hl)
	or	a, a			; IY = g_map + ring[head]
	sbc	hl, hl
	ld	l, (ix + A_HEAD)
	add	hl, hl
	lea	de, ix + A_RING
	add	hl, de
	ld	de, 0
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	ld	iy, (_g_map)
	add	iy, de
	ld	a, (iy)			; the defender's land?
	and	a, 31
	cp	a, (ix + A_DEF)
	jp	nz, .Lpt_dead
	ld	a, (iy)
	and	a, 96
	jp	z, .Lpt_dead
	ld	d, (ix + A_ATT)		; E = the attacker's land around it
	ld	e, 0
	ld	a, (iy + 1)
	and	a, 31
	cp	a, d
	jr	nz, .Lpt_n1
	ld	a, (iy + 1)
	and	a, 96
	jr	z, .Lpt_n1
	inc	e
.Lpt_n1:
	ld	a, (iy - 1)
	and	a, 31
	cp	a, d
	jr	nz, .Lpt_n2
	ld	a, (iy - 1)
	and	a, 96
	jr	z, .Lpt_n2
	inc	e
.Lpt_n2:
	lea	hl, iy + 0
	add	hl, bc
	ld	a, (hl)
	and	a, 31
	cp	a, d
	jr	nz, .Lpt_n3
	ld	a, (hl)
	and	a, 96
	jr	z, .Lpt_n3
	inc	e
.Lpt_n3:
	lea	hl, iy + 0
	or	a, a
	sbc	hl, bc
	ld	a, (hl)
	and	a, 31
	cp	a, d
	jr	nz, .Lpt_n4
	ld	a, (hl)
	and	a, 96
	jr	z, .Lpt_n4
	inc	e
.Lpt_n4:
	ld	a, e
	or	a, a
	jr	z, .Lpt_dead
	dec	a			; one neighbour, more than 3 queued:
	jr	nz, .Lpt_take		; exposed, try another first?
	ld	a, (ix + A_COUNT)
	cp	a, 4
	jr	c, .Lpt_take
	push	bc
	push	iy
	call	_rnd			; rnd_n(terrain + 1), terrain >= 1
	pop	iy
	pop	bc
	ld	a, (iy)
	rlca
	rlca
	rlca
	and	a, 3
	inc	a
	ld	d, l
	ld	e, a
	mlt	de			; r_lo * n
	ld	l, a
	mlt	hl			; r_hi * n
	ld	a, d
	add	a, l
	ld	a, h
	adc	a, 0
	jr	z, .Lpt_take
	ld	a, (ix + A_HEAD)	; put it back at the end
	inc	a
	and	a, 127
	ld	(ix + A_HEAD), a
	add	a, (ix + A_COUNT)
	dec	a
	and	a, 127
	lea	hl, iy + 0		; DE = tile index
	ld	de, (_g_map)
	or	a, a
	sbc	hl, de
	ex	de, hl
	or	a, a
	sbc	hl, hl
	ld	l, a
	add	hl, hl
	push	de
	lea	de, ix + A_RING
	add	hl, de
	pop	de
	ld	(hl), e
	inc	hl
	ld	(hl), d
	jp	.Lpt_loop
.Lpt_dead:
	ld	a, (ix + A_HEAD)
	inc	a
	and	a, 127
	ld	(ix + A_HEAD), a
	dec	(ix + A_COUNT)
	jp	.Lpt_loop
.Lpt_take:
	ld	a, (iy)			; key = b >> 5
	rlca
	rlca
	rlca
	and	a, 7
	pop	ix
	ret
.Lpt_none:
	ld	a, 255
	pop	ix
	ret
.Lpt_end:
	.size	_pop_tile, .Lpt_end-_pop_tile

	.section	.text._push_nbrs,"ax",@progbits
	.globl	_push_nbrs
	.type	_push_nbrs,@function
_push_nbrs:
	; void push_nbrs(attack_t *a, tile_t t)
	; Queues the defender's land around t (+1, -1, +W, -W, as g_dirofs)
	; at the end of the ring while it has room.
	; Registers: IX attack, BC t + offset, E defender, D count, IY ring.
	ld	iy, 0
	add	iy, sp
	push	ix
	ld	ix, (iy + 3)
	ld	bc, 0
	ld	c, (iy + 6)
	ld	b, (iy + 7)		; t
	ld	d, (ix + A_COUNT)
	ld	e, (ix + A_DEF)
	inc	bc			; t + 1
	call	.Lpn_one
	dec	bc
	dec	bc			; t - 1
	call	.Lpn_one
	inc	bc
	ld	hl, 0
	ld	a, (_g_W)
	ld	l, a
	ld	a, (_g_W+1)
	ld	h, a
	push	hl
	add	hl, bc
	push	hl
	pop	bc			; t + W
	call	.Lpn_one
	pop	hl
	push	bc			; t - W = (t + W) - 2 W
	ex	(sp), hl
	pop	bc
	or	a, a
	sbc	hl, bc
	sbc	hl, bc
	push	hl
	pop	bc
	call	.Lpn_one
	ld	(ix + A_COUNT), d
	pop	ix
	ret
.Lpn_one:				; BC = tile (16 bits): queue it if it is
	ld	a, d			; the defender's land and there is room
	cp	a, 128
	ret	nc
	ld	hl, (_g_map)
	push	bc
	push	de
	ld	de, 0
	ld	e, c
	ld	d, b
	add	hl, de
	pop	de
	pop	bc
	ld	a, (hl)
	and	a, 31
	cp	a, e
	ret	nz
	ld	a, (hl)
	and	a, 96
	ret	z
	ld	a, (ix + A_HEAD)
	add	a, d
	and	a, 127
	or	a, a
	sbc	hl, hl
	ld	l, a
	add	hl, hl
	push	de
	lea	de, ix + A_RING
	add	hl, de
	pop	de
	ld	(hl), c
	inc	hl
	ld	(hl), b
	inc	d
	ret
.Lpn_end:
	.size	_push_nbrs, .Lpn_end-_push_nbrs

	.section	.text._atk_loop,"ax",@progbits
	.globl	_atk_loop
	.type	_atk_loop,@function
_atk_loop:
	; uint8_t atk_loop(void): the tile loop of run_attack, as atk_loop_c.
	; Returns in A 0 (done), 1 (attack over), 2 (defender dead: end the
	; attack), 3 (defender down to 2 tiles: annex them). IX = the attack.
	push	ix
	ld	ix, (_atk_ctx + X_A)
	xor	a, a
	ld	(_atk_ctx + X_TAKEN), a
	ld	(_atk_ctx + X_POPS), a
.Lal_loop:
	ld	a, (_atk_ctx + X_TAKEN)
	cp	a, 24
	jp	z, .Lal_done
	call	_pop_tile
	cp	a, 255
	jp	z, .Lal_done
	ld	(_atk_ctx + X_KEY), a
	or	a, a			; t = ring[head]
	sbc	hl, hl
	ld	l, (ix + A_HEAD)
	add	hl, hl
	lea	de, ix + A_RING
	add	hl, de
	ld	de, 0
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	ld	(_atk_ctx + X_T), de
	ld	a, (_atk_ctx + X_DEF)
	or	a, a
	jr	z, .Lal_free
	ld	iy, (_atk_ctx + X_DP)	; a defense post near?
	ld	a, (iy + P_NPOST)
	or	a, a
	jr	z, .Lal_nopost
	push	de
	ld	a, (_atk_ctx + X_DEF)
	or	a, a
	sbc	hl, hl
	ld	l, a
	push	hl
	call	_post_near
	pop	de
	pop	de
	bit	0, a
	jr	z, .Lal_nopost
	ld	hl, _atk_ctx + X_KEY
	set	3, (hl)
.Lal_nopost:
	call	.Lal_keyhl		; have[key]?
	ld	de, _atk_have
	add	hl, de
	ld	a, (hl)
	or	a, a
	jr	nz, .Lal_known
	call	.Lal_keyhl
	push	hl
	call	_atk_tile_cost
	pop	hl
.Lal_known:
	call	.Lal_keyhl		; loss = cLoss[key] (24 bits)
	add	hl, hl
	add	hl, hl
	ld	de, _atk_cLoss
	add	hl, de
	ld	hl, (hl)
	ld	(_atk_ctx + X_LOSS), hl
	jr	.Lal_cost

.Lal_free:				; free land
	call	.Lal_keyhl
	ld	de, _atk_have
	add	hl, de
	ld	a, (hl)
	or	a, a
	jr	z, .Lal_fnew
	ld	a, (_atk_ctx + X_CACHE)
	or	a, a
	jr	nz, .Lal_fknown
.Lal_fnew:
	ld	(hl), 1
	call	.Lal_freecost
	ex	de, hl
	call	.Lal_keyhl		; cCost[key] = cost
	add	hl, hl
	ld	bc, _atk_cCost
	add	hl, bc
	ld	(hl), e
	inc	hl
	ld	(hl), d
.Lal_fknown:
	ld	a, (_atk_ctx + X_TRIBE)	; loss = freeLossT[tribe8 | key]
	ld	hl, _atk_ctx + X_KEY
	or	a, (hl)
	or	a, a
	sbc	hl, hl
	ld	l, a
	add	hl, hl
	ld	de, _atk_freeLossT
	add	hl, de
	ld	a, (hl)
	ld	(_atk_ctx + X_LOSS), a
	inc	hl
	ld	a, (hl)
	ld	(_atk_ctx + X_LOSS + 1), a
	xor	a, a
	ld	(_atk_ctx + X_LOSS + 2), a

.Lal_cost:				; BC = cost = cCost[key]
	call	.Lal_keyhl
	add	hl, hl
	ld	de, _atk_cCost
	add	hl, de
	ld	bc, 0
	ld	c, (hl)
	inc	hl
	ld	b, (hl)
	ld	hl, (ix + A_BUDGET)
	or	a, a
	sbc	hl, bc
	jr	nc, .Lal_buy
	ld	hl, (ix + A_BUDGET)	; a full budget buys any tile
	ld	de, BUDGET_MAX
	or	a, a
	sbc	hl, de
	jp	c, .Lal_done
	or	a, a
	sbc	hl, hl
.Lal_buy:				; HL = budget - cost
	ld	(ix + A_BUDGET), hl
	ld	a, (ix + A_HEAD)
	inc	a
	and	a, 127
	ld	(ix + A_HEAD), a
	dec	(ix + A_COUNT)
	ld	hl, _atk_ctx + X_TAKEN
	inc	(hl)
	ld	a, (_atk_ctx + X_DEF)
	or	a, a
	jr	z, .Lal_conq
	ld	iy, (_atk_ctx + X_DP)	; defender troops -= dloss, at least 0
	ld	hl, (iy + P_TROOPS)
	ld	de, (_atk_ctx + X_DLOSS)
	or	a, a
	sbc	hl, de
	ld	a, (_atk_ctx + X_DLOSS + 3)
	ld	b, a
	ld	a, (iy + P_TROOPS + 3)
	sbc	a, b
	jr	nc, .Lal_dt
	xor	a, a
	sbc	hl, hl
.Lal_dt:
	ld	(iy + P_TROOPS), hl
	ld	(iy + P_TROOPS + 3), a
.Lal_conq:
	ld	a, (_atk_ctx + X_ATT)	; conquer(t, att) through the hook
	or	a, a
	sbc	hl, hl
	ld	l, a
	push	hl
	ld	hl, (_atk_ctx + X_T)
	push	hl
	ld	hl, (_atk_ctx + X_CONQ)
	call	.Lal_jphl
	pop	hl
	pop	hl
	ld	a, (ix + A_ACTIVE)
	or	a, a
	jr	z, .Lal_over
	ld	hl, (_atk_ctx + X_T)	; push_nbrs(a, t)
	push	hl
	push	ix
	call	_push_nbrs
	pop	hl
	pop	hl
	ld	de, (_atk_ctx + X_LOSS)	; troops -= loss, or the attack dies
	ld	hl, (ix + A_TROOPS)
	or	a, a
	sbc	hl, de
	ld	a, (ix + A_TROOPS + 3)
	jr	c, .Lal_borrow
	jr	nz, .Lal_store
	or	a, a			; low bits equal: dies if troops == loss
	jr	z, .Lal_dies
	jr	.Lal_store
.Lal_borrow:				; borrow from the top byte
	or	a, a
	jr	z, .Lal_dies		; troops < loss
	dec	a
	ld	(ix + A_TROOPS + 3), a
.Lal_store:
	ld	(ix + A_TROOPS), hl
	ld	a, (_atk_ctx + X_DEF)
	or	a, a
	jp	z, .Lal_loop
	ld	iy, (_atk_ctx + X_DP)
	ld	a, (iy + P_ALIVE)
	or	a, a
	ld	a, 2
	jr	z, .Lal_ret
	ld	a, (iy + P_TILES + 1)
	or	a, a
	jp	nz, .Lal_loop
	ld	a, (iy + P_TILES)
	cp	a, 3
	jp	nc, .Lal_loop
	ld	a, 3
	jr	.Lal_ret
.Lal_dies:
	ld	(ix + A_ACTIVE), 0
.Lal_over:
	ld	a, 1
	jr	.Lal_ret
.Lal_done:
	xor	a, a
.Lal_ret:
	pop	ix
	ret
.Lal_jphl:
	jp	(hl)
.Lal_keyhl:				; HL = key
	ld	a, (_atk_ctx + X_KEY)
	or	a, a
	sbc	hl, hl
	ld	l, a
	ret
.Lal_freecost:				; HL = free_cost_of(a, key)
	ld	a, (ix + A_TROOPS + 3)
	or	a, a
	jr	nz, .Lal_fc5
	ld	hl, (ix + A_TROOPS)
	ld	de, 33333
	or	a, a
	sbc	hl, de
	jr	nc, .Lal_fc5
	add	hl, de
	inc	hl
	push	hl			; d = troops + 1
	call	.Lal_keyhl
	add	hl, hl
	add	hl, hl
	ld	de, _atk_freeX
	add	hl, de
	ld	hl, (hl)
	push	hl			; x = freeX[key]
	call	_free_cost
	pop	de
	pop	de
	ret
.Lal_fc5:				; from 33333 troops on, w = 5
	or	a, a
	sbc	hl, hl
	ld	a, (_atk_cost5)
	ld	l, a
	ld	a, (_atk_cost5 + 1)
	ld	h, a
	ret
.Lal_end:
	.size	_atk_loop, .Lal_end-_atk_loop

	.section	.text._atk_setup,"ax",@progbits
	.globl	_atk_setup
	.type	_atk_setup,@function
_atk_setup:
	; void atk_setup(attack_t *a), as atk_setup_c: the tick's budget, the
	; per-tick values of the tile cost (atk_K) and atk_ctx. IX = the attack.
	ld	iy, 0
	add	iy, sp
	push	ix
	ld	ix, (iy + 3)
	ld	(_atk_ctx + X_A), ix
	ld	hl, _atk_conquer
	ld	(_atk_ctx + X_CONQ), hl
	ld	de, 0
	ld	a, (ix + A_DEF)
	ld	(_atk_ctx + X_DEF), a
	ld	d, a
	ld	hl, _g_pl
	add	hl, de
	ld	(_atk_ctx + X_DP), hl
	ld	a, (ix + A_ATT)
	ld	(_atk_ctx + X_ATT), a
	call	.Lsu_ap
	ld	a, (iy + P_TYPE)	; tribe8
	cp	a, PT_TRIBE
	ld	a, 0
	jr	nz, .Lsu_t8
	ld	a, 8
.Lsu_t8:
	ld	(_atk_ctx + X_TRIBE), a
	or	a, a			; B = min(max(border, count), 600)
	sbc	hl, hl
	ld	l, (ix + A_BORDER)
	ld	h, (ix + A_BORDER + 1)
	ld	de, 0
	ld	e, (ix + A_COUNT)
	call	.Lsu_max
	ld	de, 600
	call	.Lsu_min
	push	hl
	call	_rnd			; + rnd_n(5) = (rnd() * 5) >> 16
	ld	a, 5
	ld	d, l
	ld	e, a
	mlt	de
	ld	l, a
	mlt	hl
	ld	a, d
	add	a, l
	ld	a, h
	adc	a, 0
	pop	hl
	ld	de, 0
	ld	e, a
	add	hl, de
	add	hl, hl			; budget = min(budget + 64 B, BUDGET_MAX)
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	ld	de, (ix + A_BUDGET)
	add	hl, de
	ld	de, BUDGET_MAX
	call	.Lsu_min
	ld	(ix + A_BUDGET), hl
	ld	a, (_atk_ctx + X_DEF)
	or	a, a
	jp	z, .Lsu_free

	ld	iy, (_atk_ctx + X_DP)	; an attack on a player
	ld	hl, (ix + A_TROOPS + 1)	; d = (troops >> 8) + 1
	inc	hl
	ld	a, (iy + P_TROOPS + 3)
	or	a, a
	jr	nz, .Lsu_big
	push	hl			; r8 = dt / d
	ld	hl, (iy + P_TROOPS)
	push	hl
	call	_udiv24
	pop	de			; DE = dt
	ex	(sp), hl		; (sp) = r8
	ld	iy, (_atk_ctx + X_DP)	; dloss = dt / (tiles, at least 1)
	or	a, a
	sbc	hl, hl
	ld	l, (iy + P_TILES)
	ld	h, (iy + P_TILES + 1)
	ld	a, h
	or	a, l
	jr	nz, .Lsu_tl
	inc	hl
.Lsu_tl:
	push	hl
	push	de
	call	_udiv24
	pop	de
	pop	de
	ld	(_atk_ctx + X_DLOSS), hl
	xor	a, a
	ld	(_atk_ctx + X_DLOSS + 3), a
	ex	de, hl			; DE = dl
	pop	hl			; HL = r8
	jr	.Lsu_k
.Lsu_big:				; troops beyond 24 bits: in C
	push	hl
	call	_atk_big_div		; HL = r8 (<= 60000), sets dloss, atk_dl
	pop	de
	ld	de, (_atk_dl)
.Lsu_k:					; HL = r8, DE = dl
	push	de
	ld	de, 60000
	call	.Lsu_min
	ld	(_atk_r8), hl
	ld	de, 512			; lossRatio = r8 clamped to 154..512
	call	.Lsu_min
	ld	de, 154
	call	.Lsu_max
	ld	de, _atk_K + K_LOSSR
	call	.Lsu_st16
	call	.Lsu_ap			; large-territory factors (256: none)
	ld	a, (iy + P_BIG8)
	ld	c, 179
	call	.Lsu_big8		; bA
	push	hl
	ld	c, 187
	call	.Lsu_big8		; bAs
	ld	de, _atk_K + K_BAS
	call	.Lsu_st16
	ld	iy, (_atk_ctx + X_DP)
	ld	a, (iy + P_BIG8)
	ld	c, 77
	call	.Lsu_big8		; bD
	ld	de, _atk_K + K_BD
	call	.Lsu_st16
	ex	de, hl			; DE = bD
	pop	hl			; HL = bA
	ld	a, 119			; f = (119 * bA >> 8) * bD >> 8
	inc	h
	dec	h
	jr	nz, .Lsu_f1		; bA = 256: 119
	ld	h, l
	ld	l, a
	mlt	hl
	ld	a, h
.Lsu_f1:
	inc	d
	dec	d
	jr	nz, .Lsu_f2		; bD = 256: unchanged
	ld	d, a
	mlt	de
	ld	a, d
.Lsu_f2:
	or	a, a			; term = f * g_S + min(dl, 20000)
	sbc	hl, hl
	ld	h, a
	ld	a, (_g_S)
	ld	l, a
	mlt	hl
	pop	de
	push	hl
	ex	de, hl
	ld	de, 20000
	call	.Lsu_min
	pop	de
	add	hl, de
	ld	de, _atk_K + K_TERM
	call	.Lsu_st16
	ld	hl, (_atk_r8)		; sc8: a8 = r8 clamped to 210..1920,
	ld	de, 5120		; times r8 / 20 in Q8 from 5120 on
	or	a, a
	sbc	hl, de
	jr	nc, .Lsu_b8
	add	hl, de
	ld	de, 1920
	call	.Lsu_min
	ld	de, 210
	call	.Lsu_max
	jr	.Lsu_sc8
.Lsu_b8:
	add	hl, de
	ld	de, 20
	push	de
	push	hl
	call	_udiv24
	pop	de
	pop	de
	push	hl
	ld	hl, 1920
	push	hl
	call	_mul_q8
	pop	de
	pop	de
.Lsu_sc8:
	ld	(_atk_K + K_SC8), hl
	ld	iy, (_atk_ctx + X_DP)	; traitor, L16, tribe malus
	ld	a, (iy + P_TRAIT)
	or	a, (iy + P_TRAIT + 1)
	jr	z, .Lsu_tr
	ld	a, 1
.Lsu_tr:
	ld	(_atk_K + K_TRAIT), a
	ld	a, (_g_L16)
	ld	(_atk_K + K_L16), a
	ld	b, 0
	ld	a, (iy + P_TYPE)
	cp	a, PT_TRIBE
	jr	nz, .Lsu_tm
	call	.Lsu_ap
	ld	a, (iy + P_TYPE)
	cp	a, PT_TRIBE
	jr	z, .Lsu_tm
	inc	b
.Lsu_tm:
	ld	a, b
	ld	(_atk_K + K_TRIBE), a
	ld	hl, _atk_K + K_BAS	; cost tag changed: forget the costs
	ld	de, _atk_costTag
	ld	b, K_TAGLEN
.Lsu_cmp:
	ld	a, (de)
	cp	a, (hl)
	jr	nz, .Lsu_tag
	inc	hl
	inc	de
	djnz	.Lsu_cmp
	jr	.Lsu_common
.Lsu_tag:
	ld	hl, _atk_K + K_BAS
	ld	de, _atk_costTag
	ld	bc, K_TAGLEN
	ldir
	ld	hl, _atk_tagHave
	call	.Lsu_clr16
	jr	.Lsu_common

.Lsu_free:				; free land: tables for this map
	ld	a, (_g_S)
	ld	hl, _atk_freeS
	cp	a, (hl)
	jr	nz, .Lsu_tab
	ld	a, (_g_L16)
	ld	hl, _atk_freeL16
	cp	a, (hl)
	jr	z, .Lsu_common
.Lsu_tab:
	call	_atk_free_tables
.Lsu_common:
	ld	hl, _atk_have
	call	.Lsu_clr16
	ld	a, (_atk_ctx + X_DEF)	; cacheOk = def || troops >= 40000
	or	a, a
	jr	nz, .Lsu_ok1
	ld	a, (ix + A_TROOPS + 3)
	or	a, a
	jr	nz, .Lsu_ok1
	ld	hl, (ix + A_TROOPS)
	ld	de, 40000
	or	a, a
	sbc	hl, de
	ld	a, 0
	jr	c, .Lsu_ok
.Lsu_ok1:
	ld	a, 1
.Lsu_ok:
	ld	(_atk_ctx + X_CACHE), a
	pop	ix
	ret

.Lsu_ap:				; IY = the attacker's player_t
	ld	de, 0
	ld	a, (_atk_ctx + X_ATT)
	ld	d, a
	ld	iy, _g_pl
	add	iy, de
	ret
.Lsu_big8:				; HL = 256 - (A * C >> 8)
	ld	b, a
	mlt	bc
	ld	hl, 256
	ld	de, 0
	ld	e, b
	or	a, a
	sbc	hl, de
	ret
.Lsu_min:				; HL = min(HL, DE)
	push	hl
	or	a, a
	sbc	hl, de
	pop	hl
	ret	c
	ex	de, hl
	ret
.Lsu_max:				; HL = max(HL, DE)
	push	hl
	or	a, a
	sbc	hl, de
	pop	hl
	ret	nc
	ex	de, hl
	ret
.Lsu_st16:				; (DE) = HL, 16 bits; keeps HL
	ex	de, hl
	ld	(hl), e
	inc	hl
	ld	(hl), d
	ex	de, hl
	ret
.Lsu_clr16:				; 16 zero bytes at HL
	ld	b, 16
	xor	a, a
.Lsu_clr:
	ld	(hl), a
	inc	hl
	djnz	.Lsu_clr
	ret
.Lsu_end:
	.size	_atk_setup, .Lsu_end-_atk_setup

	.extern	_atk_ctx
	.extern	_atk_have
	.extern	_atk_cCost
	.extern	_atk_cLoss
	.extern	_atk_freeLossT
	.extern	_atk_freeX
	.extern	_atk_cost5
	.extern	_atk_tile_cost
	.extern	_post_near
	.extern	_rnd
	.extern	_rnd_n
	.extern	_g_W
	.extern	_g_map
	.extern	_g_L16
	.extern	_atk_K
	.extern	_atk_costTag
	.extern	_atk_tagHave
	.extern	_atk_freeS
	.extern	_atk_freeL16
	.extern	_atk_r8
	.extern	_atk_dl
	.extern	_atk_big_div
	.extern	_atk_free_tables
	.extern	_atk_conquer
	.extern	_g_pl
	.extern	_g_S
