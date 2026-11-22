; CalcFront - hand-written eZ80 for the route search loop (src/sim/map.c)
;
; uint8_t search_loop(void)
;   Greedy best-first search over one water body, from the heap that
;   search() set up (map_srch.hn entries in g_mem->hq/hk): pops the tile of
;   lowest key until the goal (returns 1), an empty heap or the end of the
;   budget (returns 0). Marks each new tile visited with the direction back
;   (bits 3-4, bit 7), lists it in vlist (or map_note_far() once full) and
;   pushes it with the Manhattan distance to the goal or, guided, the
;   coarse guide's key. Leaves map_srch.hn, vn and pops as search_loop_c,
;   with the same heap, the same order of pushes: the same route.
;
; The heap holds 4-byte entries {uint16 key, tile} at hb = g_mem->hq; it is
; walked with pointers (children of the entry at hb + o: hb + 2o + 4, +8).
; Keys are compared as 16-bit numbers.
; Locals (ix-d), see L_* below; the 4 directions (as g_dirofs) are a table
; of 10-byte entries on the frame: offset to the neighbour, cell step, bits
; marking the way back, bit in the "toward the goal" mask, local x and y
; step inside the 8x8 cell.

	.assume	ADL = 1

; srch_t, the guide (checked by _Static_assert in map.c)
S_TO	= 0
S_GX	= 2
S_GY	= 4
S_COMP	= 6
S_GUIDED = 7
S_BUDGET = 8
S_POPS	= 10
S_HN	= 12
S_VN	= 14
M_HQ	= 8192			; offsetof(sim_mem_t, hq); vlist follows at +8192

L_HB	= -3			; heap base
L_HP	= -6			; heap end: hb + 4 * hn
L_VL	= -9			; vlist, end of the heap's room (hn = HEAPN)
L_VP	= -12			; vlist + 2 * vn
L_VLIM	= -15			; vlist + 2 * VMAX
L_BUD	= -18			; budget left
L_GX	= -21
L_GY	= -24
L_T	= -27			; tile being expanded
L_Q	= -30			; g_map + t
L_M	= -33			; its distance to the goal
L_PC	= -36			; its cell
L_NT	= -39			; neighbour pushed
L_NMV	= -42			; key pushed
L_LK	= -45			; heap's last entry: key
L_LT	= -48			;                    tile
L_CELL	= -51
L_TW	= -52			; directions toward the goal
L_PLX	= -53			; t's x and y in its cell
L_PLY	= -54
L_CNT	= -55
L_COMP	= -56
L_GUID	= -57
L_KM1	= -58
L_POOL	= -59
L_D	= -60
L_DIR	= -100			; 4 x 10 bytes
FRAME	= 100
D_OFS	= 0
D_CD	= 3
D_BACK	= 6
D_SIDE	= 7
D_SX	= 8
D_SY	= 9
DSIZE	= 10

	.section	.text._search_loop,"ax",@progbits
	.globl	_search_loop
	.type	_search_loop,@function
_search_loop:
	push	ix
	ld	ix, 0
	add	ix, sp
	lea	hl, ix - FRAME
	ld	sp, hl
	ld	hl, (_g_mem)		; heap, vlist
	ld	de, M_HQ
	add	hl, de
	ld	(ix + L_HB), hl
	ex	de, hl
	or	a, a
	sbc	hl, hl
	ld	a, (_map_srch + S_HN)
	ld	l, a
	ld	a, (_map_srch + S_HN + 1)
	ld	h, a
	add	hl, hl
	add	hl, hl
	add	hl, de
	ld	(ix + L_HP), hl
	ld	hl, 8192
	add	hl, de
	ld	(ix + L_VL), hl
	ex	de, hl
	ld	hl, 4096
	add	hl, de
	ld	(ix + L_VLIM), hl
	or	a, a
	sbc	hl, hl
	ld	a, (_map_srch + S_VN)
	ld	l, a
	ld	a, (_map_srch + S_VN + 1)
	ld	h, a
	add	hl, hl
	add	hl, de
	ld	(ix + L_VP), hl
	ld	hl, _map_srch + S_GX	; goal, water body, guide, budget
	ld	de, 0
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	inc	hl
	ld	(ix + L_GX), de
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	inc	hl
	ld	(ix + L_GY), de
	ld	a, (hl)
	ld	(ix + L_COMP), a
	inc	hl
	ld	a, (hl)
	ld	(ix + L_GUID), a
	inc	hl
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	ld	(ix + L_BUD), de
	lea	iy, ix + L_DIR		; the directions
	ld	hl, 1			; right
	ld	(iy + D_OFS), hl
	ld	(iy + D_CD), hl
	ld	hl, 010108h		; back 1 << 3, side 1, x + 1
	ld	(iy + D_BACK), hl
	ld	(iy + D_SY), 0
	ld	hl, -1			; left
	ld	(iy + DSIZE + D_OFS), hl
	ld	(iy + DSIZE + D_CD), hl
	ld	hl, 0FF0200h		; back 0, side 2, x - 1
	ld	(iy + DSIZE + D_BACK), hl
	ld	(iy + DSIZE + D_SY), 0
	ld	hl, (_g_W)		; down
	ld	de, 0
	ld	e, l
	ld	d, h
	ld	(iy + 2 * DSIZE + D_OFS), de
	ld	hl, 000418h		; back 3 << 3, side 4, x + 0
	ld	(iy + 2 * DSIZE + D_BACK), hl
	ld	(iy + 2 * DSIZE + D_SY), 1
	or	a, a			; up
	sbc	hl, hl
	sbc	hl, de
	ld	(iy + 3 * DSIZE + D_OFS), hl
	ld	hl, 000810h		; back 2 << 3, side 8, x + 0
	ld	(iy + 3 * DSIZE + D_BACK), hl
	ld	(iy + 3 * DSIZE + D_SY), 0FFh
	or	a, a			; cell steps down and up
	sbc	hl, hl
	ld	a, (_map_cW)
	ld	l, a
	ld	(iy + 2 * DSIZE + D_CD), hl
	ex	de, hl
	or	a, a
	sbc	hl, hl
	sbc	hl, de
	ld	(iy + 3 * DSIZE + D_CD), hl

.Lloop:					; pop the heap
	ld	hl, (ix + L_HP)
	ld	de, (ix + L_HB)
	or	a, a
	sbc	hl, de
	jp	z, .Lfail		; empty
	ld	iy, (ix + L_HB)
	ld	de, 0
	ld	e, (iy + 2)
	ld	d, (iy + 3)
	ld	(ix + L_T), de		; t = top
	ld	hl, (ix + L_HP)
	ld	de, -4
	add	hl, de
	ld	(ix + L_HP), hl		; end
	ld	de, (hl)
	ld	(ix + L_LK), de		; last entry
	inc	hl
	inc	hl
	ld	de, (hl)
	ld	(ix + L_LT), de
.Ldn:					; IY = hole
	lea	hl, iy + 0
	ld	de, (ix + L_HB)
	or	a, a
	sbc	hl, de
	lea	de, iy + 4
	add	hl, de			; first child
	ld	de, (ix + L_HP)
	or	a, a
	sbc	hl, de
	jr	nc, .Ldn_end		; beyond the end
	add	hl, de
	push	hl
	ld	bc, 4
	add	hl, bc
	or	a, a
	sbc	hl, de			; second child before the end?
	pop	hl
	jr	nc, .Ldn_one
	ld	bc, (hl)		; the smaller child
	push	hl
	inc	hl
	inc	hl
	inc	hl
	inc	hl
	ld	hl, (hl)
	or	a, a
	sbc.sis	hl, bc
	pop	hl
	jr	nc, .Ldn_one
	inc	hl
	inc	hl
	inc	hl
	inc	hl
.Ldn_one:
	push	hl
	ld	hl, (hl)
	ld	de, (ix + L_LK)
	or	a, a
	sbc.sis	hl, de
	pop	hl
	jr	nc, .Ldn_end		; child key >= last key: the hole is its place
	ld	de, (hl)		; child up into the hole
	ld	(iy + 0), de
	inc	hl
	inc	hl
	inc	hl
	ld	a, (hl)
	ld	(iy + 3), a
	dec	hl
	dec	hl
	dec	hl
	push	hl
	pop	iy
	jr	.Ldn
.Ldn_end:
	ld	hl, (ix + L_LK)
	ld	(iy + 0), l
	ld	(iy + 1), h
	ld	hl, (ix + L_LT)
	ld	(iy + 2), l
	ld	(iy + 3), h

	ld	hl, (ix + L_T)		; the goal?
	ld	de, (_map_srch + S_TO)
	or	a, a
	sbc.sis	hl, de
	jp	z, .Lfound
	ld	hl, (ix + L_BUD)	; budget
	add	hl, de
	or	a, a
	sbc	hl, de
	jp	z, .Lfail
	dec	hl
	ld	(ix + L_BUD), hl

	ld	de, 0			; y = ty_in(t), x
	ld	e, (ix + L_T + 1)
	ld	hl, _g_rowOfHi
	add	hl, de
	add	hl, de
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	ld	hl, _g_rowStart + 2
	add	hl, de
	add	hl, de
	ld	bc, (ix + L_T)
.Lty:
	ld	a, c
	sub	a, (hl)
	inc	hl
	ld	a, b
	sbc	a, (hl)
	inc	hl
	jr	c, .Lty1
	inc	de
	jr	.Lty
.Lty1:
	dec	hl
	dec	hl
	dec	hl
	dec	hl
	ld	a, c
	sub	a, (hl)
	ld	c, a
	inc	hl
	ld	a, b
	sbc	a, (hl)
	ld	b, a			; BC = x, DE = y
	ld	a, (ix + L_GUID)
	or	a, a
	jr	z, .Lng
	ld	a, c			; t's cell, and x and y in it
	and	a, 7
	ld	(ix + L_PLX), a
	ld	a, e
	and	a, 7
	ld	(ix + L_PLY), a
	push	de
	ex	de, hl
	srl	h
	rr	l
	srl	h
	rr	l
	srl	h
	rr	l
	add	hl, hl
	ld	de, _map_cRow
	add	hl, de
	ld	de, 0
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	push	bc
	pop	hl
	srl	h
	rr	l
	srl	h
	rr	l
	srl	h
	rr	l
	add	hl, de
	ld	(ix + L_PC), hl
	pop	de
.Lng:
	ld	hl, (ix + L_GX)		; directions toward the goal, distance
	or	a, a
	sbc	hl, bc
	ld	a, 1
	jr	z, .Lx0
	jr	nc, .Lx1
	push	bc
	pop	hl
	ld	bc, (ix + L_GX)
	or	a, a
	sbc	hl, bc
	ld	a, 2
	jr	.Lx1
.Lx0:
	xor	a, a
.Lx1:
	ld	(ix + L_TW), a
	ld	(ix + L_M), hl
	ld	hl, (ix + L_GY)
	or	a, a
	sbc	hl, de
	ld	a, 4
	jr	z, .Ly0
	jr	nc, .Ly1
	ex	de, hl
	ld	de, (ix + L_GY)
	or	a, a
	sbc	hl, de
	ld	a, 8
	jr	.Ly1
.Ly0:
	xor	a, a
.Ly1:
	or	a, (ix + L_TW)
	ld	(ix + L_TW), a
	ld	de, (ix + L_M)
	add	hl, de
	ld	(ix + L_M), hl

	ld	hl, (ix + L_T)		; the neighbours
	ld	de, (_g_map)
	add	hl, de
	ld	(ix + L_Q), hl
	lea	iy, ix + L_DIR
	ld	(ix + L_CNT), 4
.Lnbr:
	ld	hl, (ix + L_Q)
	ld	de, (iy + D_OFS)
	add	hl, de
	ld	a, (hl)
	tst	a, 0E0h
	jr	nz, .Lnext		; land or visited
	and	a, 7
	cp	a, (ix + L_COMP)
	jr	nz, .Lnext		; another water body
	ex	de, hl
	ld	hl, (ix + L_HP)
	ld	bc, (ix + L_VL)
	or	a, a
	sbc	hl, bc
	jp	nc, .Lloop		; heap full: next tile
	ex	de, hl
	ld	a, (hl)			; visited, the way back
	or	a, (iy + D_BACK)
	or	a, 080h
	ld	(hl), a
	ld	de, (_g_map)
	or	a, a
	sbc	hl, de
	ld	(ix + L_NT), hl
	ex	de, hl
	ld	hl, (ix + L_VP)
	ld	bc, (ix + L_VLIM)
	or	a, a
	sbc	hl, bc
	jr	nc, .Lfar
	add	hl, bc
	ld	(hl), e
	inc	hl
	ld	(hl), d
	inc	hl
	ld	(ix + L_VP), hl
	jr	.Lkey
.Lfar:
	push	iy
	push	de
	call	_map_note_far
	pop	de
	pop	iy
.Lkey:
	ld	hl, (ix + L_M)		; one tile closer or further
	inc	hl
	ld	a, (ix + L_TW)
	and	a, (iy + D_SIDE)
	jr	z, .Lk1
	dec	hl
	dec	hl
.Lk1:
	ld	a, (ix + L_GUID)
	or	a, a
	call	nz, .Lguide
	call	.Lpush
.Lnext:
	lea	iy, iy + DSIZE
	dec	(ix + L_CNT)
	jr	nz, .Lnbr
	jp	.Lloop

.Lfound:
	ld	a, 1
	jr	.Lexit
.Lfail:
	xor	a, a
.Lexit:					; hn, vn, pops back to map_srch
	ld	c, a
	ld	hl, (ix + L_HP)
	ld	de, (ix + L_HB)
	or	a, a
	sbc	hl, de
	srl	h
	rr	l
	srl	h
	rr	l
	ld	a, l
	ld	(_map_srch + S_HN), a
	ld	a, h
	ld	(_map_srch + S_HN + 1), a
	ld	hl, (ix + L_VP)
	ld	de, (ix + L_VL)
	or	a, a
	sbc	hl, de
	srl	h
	rr	l
	ld	a, l
	ld	(_map_srch + S_VN), a
	ld	a, h
	ld	(_map_srch + S_VN + 1), a
	ld	hl, (_map_srch + S_BUDGET)
	ld	de, (ix + L_BUD)
	or	a, a
	sbc	hl, de
	ld	a, l
	ld	(_map_srch + S_POPS), a
	ld	a, h
	ld	(_map_srch + S_POPS + 1), a
	ld	a, c
	ld	sp, ix
	pop	ix
	ret

; heap push of tile (ix+L_NT) with key HL: sift the hole up from the end
.Lpush:
	push	iy
	ld	(ix + L_NMV), hl
	ld	a, l			; BC = 0xFFFF - key: parent key + BC
	cpl				; carries iff parent key > key
	ld	c, a
	ld	a, h
	cpl
	ld	b, a
	ld	iy, (ix + L_HP)
	lea	hl, iy + 4
	ld	(ix + L_HP), hl
	ld	de, (ix + L_HB)
.Lup:
	lea	hl, iy + 0
	or	a, a
	sbc	hl, de
	jr	z, .Lup_end		; the root
	dec	hl			; parent: hb + ((o - 4) / 2 & ~3)
	dec	hl
	dec	hl
	dec	hl
	srl	h
	rr	l
	ld	a, l
	and	a, 0FCh
	ld	l, a
	add	hl, de
	push	hl
	ld	hl, (hl)
	add.sis	hl, bc
	pop	hl
	jr	nc, .Lup_end
	push	de			; parent down into the hole
	ld	de, (hl)
	ld	(iy + 0), de
	inc	hl
	inc	hl
	inc	hl
	ld	a, (hl)
	ld	(iy + 3), a
	dec	hl
	dec	hl
	dec	hl
	pop	de
	push	hl
	pop	iy
	jr	.Lup
.Lup_end:
	ld	hl, (ix + L_NMV)
	ld	(iy + 0), l
	ld	(iy + 1), h
	ld	hl, (ix + L_NT)
	ld	(iy + 2), l
	ld	(iy + 3), h
	pop	iy
	ret

; guide_key(): the key of the neighbour from distance HL (IY: direction)
.Lguide:
	ld	(ix + L_NMV), hl
	ld	a, (ix + L_PLX)		; x, y in the cell, the cell
	add	a, (iy + D_SX)
	ld	c, a
	ld	a, (ix + L_PLY)
	add	a, (iy + D_SY)
	ld	b, a
	ld	hl, (ix + L_PC)
	or	a, c
	and	a, 0F8h
	jr	z, .Lg1
	ld	de, (iy + D_CD)		; into the next cell
	add	hl, de
	ld	a, c
	and	a, 7
	ld	c, a
	ld	a, b
	and	a, 7
	ld	b, a
.Lg1:
	ex	de, hl			; DE = cell, C = x, B = y
	ld	hl, _map_cSplit
	add	hl, de
	ld	a, (hl)
	or	a, a
	jr	nz, .Lg_split
	ld	hl, (_map_cGoal)
	or	a, a
	sbc.sis	hl, de
	jr	z, .Lg_m
	ld	hl, _map_cDist
	add	hl, de
	ld	a, (hl)
	ld	hl, _map_cPar
	add	hl, de
	ld	e, (hl)
.Lg_key:				; cell_key(): A = d, E = par
	inc	a
	jr	z, .Lg_far		; d = 255: unknown
	dec	a
	ld	d, a
	ld	a, e
	or	a, a
	jr	z, .Lg_p0
	dec	a
	jr	z, .Lg_p1
	dec	a
	jr	z, .Lg_p2
	ld	a, b
	jr	.Lg_b
.Lg_p0:
	ld	a, 7
	sub	a, c
	jr	.Lg_b
.Lg_p1:
	ld	a, c
	jr	.Lg_b
.Lg_p2:
	ld	a, 7
	sub	a, b
.Lg_b:					; d * 16 + b
	ld	e, a
	or	a, a
	sbc	hl, hl
	ld	l, d
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	ld	a, l
	or	a, e
	ld	l, a
	ret
.Lg_far:				; 0xF000 | min(m, 0xFFF)
	ld	hl, (ix + L_NMV)
	ld	de, 0FFFh
	or	a, a
	sbc	hl, de
	jr	c, .Lg_f1
	or	a, a
	sbc	hl, hl
.Lg_f1:
	add	hl, de
	ld	a, h
	or	a, 0F0h
	ld	h, a
	ret
.Lg_m:
	ld	hl, (ix + L_NMV)
	ret
.Lg_split:				; split cell: the pool of (x, y) first
	ld	(ix + L_CELL), de
	dec	a
	ld	(ix + L_KM1), a
	or	a, a
	sbc	hl, hl
	ld	l, a
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	ld	de, _map_splitLab
	add	hl, de
	ld	a, b
	add	a, a
	bit	2, c
	jr	z, .Lg_s1
	inc	a
.Lg_s1:
	ld	de, 0
	ld	e, a
	add	hl, de
	ld	e, (hl)
	ld	a, c
	call	.Lpair
	ld	(ix + L_POOL), a
	ld	hl, (_map_cGoal)	; the goal's pool: m
	ld	de, (ix + L_CELL)
	or	a, a
	sbc.sis	hl, de
	jr	nz, .Lg_s2
	ld	a, (_map_gPool)
	cp	a, (ix + L_POOL)
	jr	z, .Lg_m
.Lg_s2:
	or	a, a
	sbc	hl, hl
	ld	l, (ix + L_KM1)
	push	hl
	add	hl, hl
	add	hl, hl
	ld	de, _map_splitDist
	add	hl, de
	ld	de, 0
	ld	e, (ix + L_POOL)
	add	hl, de
	ld	a, (hl)
	ld	(ix + L_D), a
	pop	hl
	ld	de, _map_splitPar
	add	hl, de
	ld	e, (hl)
	ld	a, (ix + L_POOL)
	call	.Lpair
	ld	e, a
	ld	a, (ix + L_D)
	jp	.Lg_key
.Lpair:					; A = (E >> 2 * (A & 3)) & 3
	and	a, 3
	jr	z, .Lpair2
.Lpair1:
	srl	e
	srl	e
	dec	a
	jr	nz, .Lpair1
.Lpair2:
	ld	a, e
	and	a, 3
	ret
.Lfunc_end:
	.size	_search_loop, .Lfunc_end-_search_loop

	.extern	_map_srch
	.extern	_map_note_far
	.extern	_map_cW
	.extern	_map_cRow
	.extern	_map_cSplit
	.extern	_map_cDist
	.extern	_map_cPar
	.extern	_map_cGoal
	.extern	_map_gPool
	.extern	_map_splitLab
	.extern	_map_splitDist
	.extern	_map_splitPar
	.extern	_g_mem
	.extern	_g_map
	.extern	_g_W
	.extern	_g_rowOfHi
	.extern	_g_rowStart
