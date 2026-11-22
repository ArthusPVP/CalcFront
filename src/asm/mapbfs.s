; CalcFront - hand-written eZ80 for the coarse guide's search (src/sim/map.c)
;
; uint16_t bfs_loop(uint16_t sc, uint8_t sPool, uint8_t comp)
;   Breadth-first search over (8x8 cell, pool) from the goal, which
;   coarse_bfs() has queued (cells in g_mem->hq, pools in g_mem->hk): numbers
;   the cells (map_cDist, map_cPar, map_splitDist, map_splitPar) until a few
;   layers past the start's pool (sc, sPool) on water body comp. Returns the
;   states dequeued. Same numbering, same queue order as bfs_loop_c.
;
; Arguments after push ix: (ix+6) sc, (ix+9) sPool, (ix+12) comp.

	.assume	ADL = 1

M_HQ	= 8192			; offsetof(sim_mem_t, hq), checked in map.c
M_HK	= 12288			; offsetof(sim_mem_t, hk)

L_QHC	= -3			; queue head: cell
L_QTC	= -6			; queue tail: cell
L_QHP	= -9			; queue head: pool
L_QTP	= -12			; queue tail: pool
L_ST	= -15			; states
L_CW	= -18			; cW
L_NCW	= -21			; -cW
L_C	= -24			; cell dequeued
L_NC	= -27			; its neighbour
L_P	= -28			; pool dequeued
L_K	= -29			; map_cSplit[c]
L_D	= -30			; its distance
L_ND	= -31			; the neighbours'
L_SIDES	= -32			; sides still to look at
L_A	= -33			; side
L_KN	= -34			; map_cSplit[nc] - 1
L_POOLS	= -35			; pools of nc reached
L_STOP	= -36
L_COMP4	= -37
FRAME	= 37

	.section	.text._bfs_loop,"ax",@progbits
	.globl	_bfs_loop
	.type	_bfs_loop,@function
_bfs_loop:
	push	ix
	ld	ix, 0
	add	ix, sp
	lea	hl, ix - FRAME
	ld	sp, hl
	ld	hl, (_g_mem)
	ld	de, M_HQ
	add	hl, de
	ld	(ix + L_QHC), hl
	inc	hl
	inc	hl
	ld	(ix + L_QTC), hl	; the goal is queued
	ld	hl, (_g_mem)
	ld	de, M_HK
	add	hl, de
	ld	(ix + L_QHP), hl
	inc	hl
	ld	(ix + L_QTP), hl
	ld	(ix + L_STOP), 255
	ld	a, (ix + 12)
	add	a, a
	add	a, a
	add	a, a
	add	a, a
	ld	(ix + L_COMP4), a
	or	a, a
	sbc	hl, hl
	ld	(ix + L_ST), hl
	ld	a, (_map_cW)
	ld	l, a
	ld	(ix + L_CW), hl
	ex	de, hl
	or	a, a
	sbc	hl, hl
	sbc	hl, de
	ld	(ix + L_NCW), hl

.Lb_loop:				; dequeue (c, p)
	ld	hl, (ix + L_QHP)
	ld	de, (ix + L_QTP)
	or	a, a
	sbc	hl, de
	jp	nc, .Lb_end
	add	hl, de
	ld	a, (hl)
	inc	hl
	ld	(ix + L_QHP), hl
	ld	(ix + L_P), a
	ld	hl, (ix + L_QHC)
	ld	de, 0
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	inc	hl
	ld	(ix + L_QHC), hl
	ld	(ix + L_C), de
	ld	hl, (ix + L_ST)
	inc	hl
	ld	(ix + L_ST), hl
	ld	hl, _map_cSplit
	add	hl, de
	ld	a, (hl)
	ld	(ix + L_K), a
	or	a, a
	jr	z, .Lb_one
	dec	a			; split: its distance and sides
	or	a, a
	sbc	hl, hl
	ld	l, a
	add	hl, hl
	add	hl, hl
	ld	e, (ix + L_P)
	ld	d, 0
	add	hl, de
	push	hl
	ld	de, _map_splitDist
	add	hl, de
	ld	a, (hl)
	ld	(ix + L_D), a
	pop	hl
	ld	de, _map_splitSides
	add	hl, de
	ld	a, (hl)
	ld	(ix + L_SIDES), a
	jr	.Lb_d
.Lb_one:
	ld	hl, _map_cDist
	add	hl, de
	ld	a, (hl)
	ld	(ix + L_D), a
.Lb_d:
	ld	a, (ix + L_STOP)	; the start: stop a few layers further
	inc	a
	jr	nz, .Lb_s
	ld	hl, (ix + L_C)
	ld	de, (ix + 6)
	or	a, a
	sbc.sis	hl, de
	jr	nz, .Lb_s
	ld	a, (ix + L_P)
	cp	a, (ix + 9)
	jr	nz, .Lb_s
	ld	a, (ix + L_D)
	cp	a, 251
	ld	b, 253
	jr	nc, .Lb_s1
	add	a, 3
	ld	b, a
.Lb_s1:
	ld	(ix + L_STOP), b
.Lb_s:
	ld	a, (ix + L_D)
	cp	a, (ix + L_STOP)
	jp	nc, .Lb_end
	cp	a, 254
	jr	c, .Lb_nd
	ld	a, 253
.Lb_nd:
	inc	a
	ld	(ix + L_ND), a
	ld	a, (ix + L_K)
	or	a, a
	jr	nz, .Lb_sides
	ld	de, (ix + L_C)		; one pool: where our sea crosses
	ld	hl, _map_cCross
	add	hl, de
	ld	b, (hl)
	ld	c, 0
	ld	a, b
	and	a, 7
	cp	a, (ix + 12)
	jr	nz, .Lb_c1
	set	0, c
.Lb_c1:
	ld	a, b
	and	a, 0F0h
	cp	a, (ix + L_COMP4)
	jr	nz, .Lb_c2
	set	2, c
.Lb_c2:
	ld	a, e
	or	a, d
	jr	z, .Lb_c3
	dec	hl
	ld	a, (hl)
	inc	hl
	and	a, 7
	cp	a, (ix + 12)
	jr	nz, .Lb_c3
	set	1, c
.Lb_c3:
	push	hl
	ex	de, hl
	ld	de, (ix + L_CW)
	or	a, a
	sbc	hl, de
	pop	hl
	jr	c, .Lb_c4
	or	a, a
	sbc	hl, de
	ld	a, (hl)
	and	a, 0F0h
	cp	a, (ix + L_COMP4)
	jr	nz, .Lb_c4
	set	3, c
.Lb_c4:
	ld	(ix + L_SIDES), c
.Lb_sides:
	ld	(ix + L_A), 0
.Lb_side:
	ld	a, (ix + L_SIDES)
	or	a, a
	jp	z, .Lb_loop
	srl	a
	ld	(ix + L_SIDES), a
	jr	c, .Lb_do
.Lb_next:
	inc	(ix + L_A)
	jr	.Lb_side

.Lb_do:					; nc = c + step[a], 16 bits
	ld	a, (ix + L_A)
	ld	de, 1
	or	a, a
	jr	z, .Lb_st
	ld	de, -1
	dec	a
	jr	z, .Lb_st
	ld	de, (ix + L_CW)
	dec	a
	jr	z, .Lb_st
	ld	de, (ix + L_NCW)
.Lb_st:
	ld	hl, (ix + L_C)
	add	hl, de
	ld	de, 0
	ld	e, l
	ld	d, h
	ld	(ix + L_NC), de
	ld	hl, _map_cSplit
	add	hl, de
	ld	a, (hl)
	or	a, a
	jr	nz, .Lb_split
	ld	hl, _map_cDist		; one pool there: they touch
	add	hl, de
	ld	a, (hl)
	inc	a
	jr	nz, .Lb_next
	ld	a, (ix + L_ND)
	ld	(hl), a
	ld	hl, _map_cPar
	add	hl, de
	ld	a, (ix + L_A)
	xor	a, 1
	ld	(hl), a
	xor	a, a
	call	.Lb_enq
	jr	.Lb_next

.Lb_split:
	dec	a
	ld	(ix + L_KN), a
	ld	a, (ix + L_K)
	or	a, a
	jr	nz, .Lb_bp
	ld	a, (ix + L_A)		; we are one pool: nc's pools crossing back
	xor	a, 1
	ld	b, a
	inc	b
	xor	a, a
	scf
.Lb_bit:
	rla
	djnz	.Lb_bit
	ld	c, a			; C = side bit of a ^ 1
	or	a, a
	sbc	hl, hl
	ld	l, (ix + L_KN)
	add	hl, hl
	add	hl, hl
	ld	de, _map_splitSides
	add	hl, de
	ld	b, 0
	ld	a, (hl)
	and	a, c
	jr	z, .Lb_q1
	set	0, b
.Lb_q1:
	inc	hl
	ld	a, (hl)
	and	a, c
	jr	z, .Lb_q2
	set	1, b
.Lb_q2:
	inc	hl
	ld	a, (hl)
	and	a, c
	jr	z, .Lb_q3
	set	2, b
.Lb_q3:
	inc	hl
	ld	a, (hl)
	and	a, c
	jr	z, .Lb_q4
	set	3, b
.Lb_q4:
	ld	a, b
	jr	.Lb_pools
.Lb_bp:					; split on both sides: border_pools()
	ld	hl, (ix + L_NC)
	push	hl
	ld	l, (ix + L_A)
	push	hl
	ld	l, (ix + L_P)
	push	hl
	ld	hl, (ix + L_C)
	push	hl
	call	_map_border_pools
	pop	hl
	pop	hl
	pop	hl
	pop	hl
.Lb_pools:				; queue the pools reached for the first time
	ld	(ix + L_POOLS), a
	or	a, a
	sbc	hl, hl
	ld	l, (ix + L_KN)
	push	hl
	ld	de, _map_splitPar
	add	hl, de
	ld	c, (hl)			; C = par
	pop	hl
	add	hl, hl
	add	hl, hl
	ld	de, _map_splitDist
	add	hl, de			; HL = splitDist[kn - 1] + np
	ld	a, (ix + L_A)
	xor	a, 1
	ld	d, a			; D = (a ^ 1) << 2 np
	ld	e, 0FCh			; E = ~(3 << 2 np)
	ld	b, 0			; B = np
.Lb_np:
	ld	a, (ix + L_POOLS)
	or	a, a
	jr	z, .Lb_np_end
	srl	a
	ld	(ix + L_POOLS), a
	jr	nc, .Lb_np_next
	ld	a, (hl)
	inc	a
	jr	nz, .Lb_np_next
	ld	a, (ix + L_ND)
	ld	(hl), a
	ld	a, c
	and	a, e
	or	a, d
	ld	c, a
	push	hl
	push	de
	ld	de, (ix + L_NC)
	ld	a, b
	call	.Lb_enq
	pop	de
	pop	hl
.Lb_np_next:
	inc	hl
	inc	b
	sla	d
	sla	d
	rlc	e
	rlc	e
	jr	.Lb_np
.Lb_np_end:
	or	a, a
	sbc	hl, hl
	ld	l, (ix + L_KN)
	ld	de, _map_splitPar
	add	hl, de
	ld	(hl), c
	jp	.Lb_next

.Lb_enq:				; queue (DE, A); uses HL
	ld	hl, (ix + L_QTC)
	ld	(hl), e
	inc	hl
	ld	(hl), d
	inc	hl
	ld	(ix + L_QTC), hl
	ld	hl, (ix + L_QTP)
	ld	(hl), a
	inc	hl
	ld	(ix + L_QTP), hl
	ret

.Lb_end:
	ld	hl, (ix + L_ST)
	ld	sp, ix
	pop	ix
	ret
.Lfunc_end:
	.size	_bfs_loop, .Lfunc_end-_bfs_loop

	.extern	_g_mem
	.extern	_map_cW
	.extern	_map_cSplit
	.extern	_map_cDist
	.extern	_map_cPar
	.extern	_map_cCross
	.extern	_map_splitDist
	.extern	_map_splitSides
	.extern	_map_splitPar
	.extern	_map_border_pools
