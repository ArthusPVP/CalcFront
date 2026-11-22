; CalcFront - hand-written eZ80 for conquests and player scans (src/sim/map.c)
;
; void conquer(tile_t t, uint8_t p)
;   Tile t goes to player p: owner counts, fallout, coast counts (map_coast),
;   bounding box, neighbour masks, ui_dirty(), unit_on_conquer() on a
;   structure, kill_player() when the old owner is left with nothing.
;   Same effects, in the same order, as conquer_c.
; void scan_player(uint8_t p)
;   Rescans p's bounding box: neighbour mask and free-land flag, and the
;   box shrunk to p's land. Same result as scan_player_c.
;
; clang's versions spend their time on frame spills, shift loops (__ishl to
; index g_pl, __sand) and a call per helper; here the player is g_pl + p * 256
; by loading p into D, the coast counts are updated inline, and the scan
; tests a tile in 6 instructions.

	.assume	ADL = 1

; player_t offsets (checked by _Static_assert in map.c)
P_TILES	= 20
P_MINX	= 22
P_MAXX	= 24
P_MINY	= 26
P_MAXY	= 28
P_NBRS	= 38
P_NBRTN	= 46

; ------------------------------------------------------------- conquer ---
; Arguments after push ix: (ix+6) t, (ix+9) p. Locals: (ix-3) &g_map[t],
; (ix-6) old byte, (ix-5) old owner, (ix-9) map_coast[old], (ix-12)
; map_coast[p], (ix-15) g_W, (ix-18) byte of p's bit in a mask, (ix-21)
; that bit.
	.section	.text._conquer,"ax",@progbits
	.globl	_conquer
	.type	_conquer,@function
_conquer:
	ld	iy, 0
	add	iy, sp
	ld	de, 0
	ld	e, (iy + 3)
	ld	d, (iy + 4)
	ld	hl, (_g_map)
	add	hl, de
	ld	a, (hl)
	tst	a, 96
	ret	z			; water
	ld	c, a
	and	a, 31
	cp	a, (iy + 6)
	ret	z			; p's already
	ld	b, a			; B = old owner, C = old byte
	push	ix
	ld	ix, 0
	add	ix, sp
	push	hl			; (ix-3)
	push	bc			; (ix-6)
	xor	a, a
	ld	(_g_mapClean), a
	ld	de, _map_coast
	or	a, a
	sbc	hl, hl
	ld	l, b
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, de
	push	hl			; (ix-9) map_coast[old]
	or	a, a
	sbc	hl, hl
	ld	l, (ix + 9)
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, de
	push	hl			; (ix-12) map_coast[p]
	ld	hl, (_g_W)
	ld	de, 0
	ld	e, l
	ld	d, h
	push	de			; (ix-15) W
	ld	de, 0
	ld	e, (ix + 9)
	ld	hl, _g_bitMask
	add	hl, de
	ld	c, (hl)
	ld	hl, _g_bitLo
	add	hl, de
	ld	e, (hl)
	push	de			; (ix-18) byte of p
	push	bc			; (ix-21) bit of p
	ld	a, b			; old
	or	a, a
	jr	z, .Lc_fall
	ld	hl, _g_pl + P_TILES	; g_pl[old].tiles--
	ld	d, a
	ld	e, 0
	add	hl, de
	ld	a, (hl)
	sub	a, 1
	ld	(hl), a
	jr	nc, .Lc_fall
	inc	hl
	dec	(hl)
.Lc_fall:
	bit	7, (ix - 6)		; fallout: one more land tile left
	jr	z, .Lc_coast
	ld	hl, _g_landLeft
	inc	(hl)
	jr	nz, .Lc_coast
	inc	hl
	inc	(hl)
.Lc_coast:				; coast_move(t, old, p)
	ld	bc, (ix - 15)
	ld	hl, (ix - 3)
	inc	hl
	call	.Lc_cm
	ld	hl, (ix - 3)
	dec	hl
	call	.Lc_cm
	ld	hl, (ix - 3)
	add	hl, bc
	call	.Lc_cm
	ld	hl, (ix - 3)
	or	a, a
	sbc	hl, bc
	call	.Lc_cm
	ld	hl, (ix - 3)		; the new owner
	ld	a, (ix - 6)
	and	a, 96
	or	a, (ix + 9)
	ld	(hl), a
	ld	iy, _g_pl		; IY = g_pl[p]
	ld	de, 0
	ld	d, (ix + 9)
	add	iy, de
	inc	(iy + P_TILES)
	jr	nz, .Lc_row
	inc	(iy + P_TILES + 1)
.Lc_row:				; y = ty_in(t), x = t - g_rowStart[y]
	ld	e, (ix + 7)
	ld	d, 0
	ld	hl, _g_rowOfHi
	add	hl, de
	add	hl, de
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	ld	hl, _g_rowStart + 2
	add	hl, de
	add	hl, de
	ld	c, (ix + 6)
	ld	b, (ix + 7)
.Lc_ty:
	ld	a, c
	sub	a, (hl)
	inc	hl
	ld	a, b
	sbc	a, (hl)
	inc	hl
	jr	c, .Lc_ty1
	inc	de
	jr	.Lc_ty
.Lc_ty1:
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
	ld	a, c			; bounding box
	sub	a, (iy + P_MINX)
	ld	a, b
	sbc	a, (iy + P_MINX + 1)
	jr	nc, .Lc_b1
	ld	(iy + P_MINX), c
	ld	(iy + P_MINX + 1), b
.Lc_b1:
	ld	a, (iy + P_MAXX)
	sub	a, c
	ld	a, (iy + P_MAXX + 1)
	sbc	a, b
	jr	nc, .Lc_b2
	ld	(iy + P_MAXX), c
	ld	(iy + P_MAXX + 1), b
.Lc_b2:
	ld	a, e
	sub	a, (iy + P_MINY)
	ld	a, d
	sbc	a, (iy + P_MINY + 1)
	jr	nc, .Lc_b3
	ld	(iy + P_MINY), e
	ld	(iy + P_MINY + 1), d
.Lc_b3:
	ld	a, (iy + P_MAXY)
	sub	a, e
	ld	a, (iy + P_MAXY + 1)
	sbc	a, d
	jr	nc, .Lc_b4
	ld	(iy + P_MAXY), e
	ld	(iy + P_MAXY + 1), d
.Lc_b4:					; neighbours
	ld	hl, (ix - 3)
	inc	hl
	call	.Lc_nb
	ld	hl, (ix - 3)
	dec	hl
	call	.Lc_nb
	ld	hl, (ix - 3)
	ld	de, (ix - 15)
	add	hl, de
	call	.Lc_nb
	ld	hl, (ix - 3)
	ld	de, (ix - 15)
	or	a, a
	sbc	hl, de
	call	.Lc_nb
	ld	hl, (ix + 6)
	push	hl
	call	_ui_dirty
	pop	hl
	ld	de, 0			; has_struct(t): bit t of g_stbits
	ld	e, (ix + 6)
	ld	hl, _g_bitMask
	add	hl, de
	ld	c, (hl)
	or	a, a
	sbc	hl, hl
	ld	l, e
	ld	h, (ix + 7)
	srl	h
	rr	l
	srl	h
	rr	l
	srl	h
	rr	l
	ld	de, (_g_stbits)
	add	hl, de
	ld	a, (hl)
	and	a, c
	jr	z, .Lc_kill
	ld	l, (ix + 9)
	push	hl
	ld	hl, (ix + 6)
	push	hl
	call	_unit_on_conquer
	pop	hl
	pop	hl
.Lc_kill:				; the old owner has no tile left?
	ld	a, (ix - 5)
	or	a, a
	jr	z, .Lc_end
	ld	hl, _g_pl + P_TILES
	ld	de, 0
	ld	d, a
	add	hl, de
	ld	a, (hl)
	inc	hl
	or	a, (hl)
	jr	nz, .Lc_end
	ld	l, (ix + 9)
	push	hl
	ld	l, (ix - 5)
	push	hl
	call	_kill_player
	pop	hl
	pop	hl
.Lc_end:
	ld	sp, ix
	pop	ix
	ret

.Lc_cm:					; coast counts across the water at HL
	ld	a, (hl)
	tst	a, 96
	ret	nz
	and	a, 7
	ret	z
	add	a, a
	ld	de, 0
	ld	e, a
	ld	a, (ix - 5)
	or	a, a
	jr	z, .Lc_cm1
	ld	hl, (ix - 9)		; map_coast[old][c]--
	add	hl, de
	ld	a, (hl)
	sub	a, 1
	ld	(hl), a
	jr	nc, .Lc_cm1
	inc	hl
	dec	(hl)
.Lc_cm1:
	ld	a, (ix + 9)
	or	a, a
	ret	z
	ld	hl, (ix - 12)		; map_coast[p][c]++
	add	hl, de
	inc	(hl)
	ret	nz
	inc	hl
	inc	(hl)
	ret

.Lc_nb:					; neighbour at HL: masks, free land
	ld	a, (hl)
	tst	a, 96
	ret	z			; water
	and	a, 31
	cp	a, (ix + 9)
	ret	z			; p's
	or	a, a
	jr	z, .Lc_tn
	ld	b, a			; B = o
	ld	de, 0
	ld	e, a
	ld	hl, _g_bitMask
	add	hl, de
	ld	c, (hl)
	ld	hl, _g_bitLo
	add	hl, de
	ld	e, (hl)
	lea	hl, iy + P_NBRS		; p's mask gets o
	add	hl, de
	ld	a, (hl)
	or	a, c
	ld	(hl), a
	ld	hl, _g_pl + P_NBRS	; o's mask gets p
	ld	d, b
	ld	e, 0
	add	hl, de
	ld	de, (ix - 18)
	add	hl, de
	ld	a, (hl)
	or	a, (ix - 21)
	ld	(hl), a
	ret
.Lc_tn:
	ld	(iy + P_NBRTN), 1
	ret
.Lc_end_fn:
	.size	_conquer, .Lc_end_fn-_conquer

; --------------------------------------------------------- scan_player ---
; Argument after push ix: (ix+6) p. IY = g_pl[p] throughout. Locals:
; (ix-3) row (g_map + g_rowStart[y]), (ix-6) y, (ix-9) first and (ix-12)
; last of p's land tiles in the row (0: none yet), (ix-15) g_W, (ix-19..-16)
; neighbour mask, (ix-20) free-land flag, (ix-24) new minx, (ix-27) maxx,
; (ix-30) miny (0xFFFFFF: no land found yet), (ix-33) maxy, (ix-36) tiles
; per row. Registers in the scan: HL tile, B and C count, D 31, E p.
; The box's first and last tile of p in each row give the new box; the
; neighbours of every tile of p give the mask.
	.section	.text._scan_player,"ax",@progbits
	.globl	_scan_player
	.type	_scan_player,@function
_scan_player:
	ld	iy, 0
	add	iy, sp
	ld	a, (iy + 3)
	ld	iy, _g_pl
	ld	de, 0
	ld	d, a
	add	iy, de
	ld	a, (iy + P_TILES)
	or	a, (iy + P_TILES + 1)
	ret	z
	push	ix
	ld	ix, 0
	add	ix, sp
	or	a, a
	sbc	hl, hl
	push	hl			; (ix-3)
	push	hl			; (ix-6)
	push	hl			; (ix-9)
	push	hl			; (ix-12)
	ld	hl, (_g_W)
	ld	de, 0
	ld	e, l
	ld	d, h
	push	de			; (ix-15) W
	ld	de, 0
	push	de			; (ix-18) mask bytes 1-3
	push	de			; (ix-21) mask byte 0 at (ix-19), tn at (ix-20)
	ld	hl, 0FFFFh
	push	hl			; (ix-24) minx = 0xFFFF
	push	de			; (ix-27) maxx = 0
	dec	de
	push	de			; (ix-30) miny = 0xFFFFFF: none yet
	push	de			; (ix-33) maxy
	push	de			; (ix-36) n
	ld	l, (iy + P_MAXX)	; n = maxx - minx + 1
	ld	h, (iy + P_MAXX + 1)
	ld	e, (iy + P_MINX)
	ld	d, (iy + P_MINX + 1)
	or	a, a
	sbc.sis	hl, de
	inc	hl
	ld	(ix - 36), hl
	ld	de, 0
	ld	e, (iy + P_MINY)
	ld	d, (iy + P_MINY + 1)
.Ls_row:
	ld	(ix - 6), de
	ld	hl, _g_rowStart
	add	hl, de
	add	hl, de
	ld	de, 0
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	ld	hl, (_g_map)
	add	hl, de
	ld	(ix - 3), hl
	ld	e, (iy + P_MINX)
	ld	d, (iy + P_MINX + 1)
	add	hl, de			; first tile of the box's row
	ld	de, 0
	ld	(ix - 9), de		; first = 0
	ld	b, (ix - 36)		; n tiles: B low byte, C high (djnz pairs)
	ld	c, (ix - 35)
	ld	a, b
	or	a, a
	jr	z, .Ls_n1
	inc	c
.Ls_n1:
	ld	d, 31
	ld	e, (ix + 6)
.Ls_tile:
	ld	a, (hl)
	and	a, d
	cp	a, e
	jr	z, .Ls_hit
.Ls_next:
	inc	hl
	djnz	.Ls_tile
	dec	c
	jr	nz, .Ls_tile
	ld	a, (ix - 7)		; end of row: any tile of p?
	or	a, a
	jr	z, .Ls_row_next
	ld	hl, (ix - 9)		; first: x below the new minx?
	ld	de, (ix - 3)
	or	a, a
	sbc	hl, de
	ld	de, (ix - 24)
	or	a, a
	sbc	hl, de
	jr	nc, .Ls_r1
	add	hl, de
	ld	(ix - 24), hl
.Ls_r1:
	ld	hl, (ix - 12)		; last: x above the new maxx?
	ld	de, (ix - 3)
	or	a, a
	sbc	hl, de
	ld	de, (ix - 27)
	or	a, a
	sbc	hl, de
	jr	c, .Ls_r2
	add	hl, de
	ld	(ix - 27), hl
.Ls_r2:
	ld	de, (ix - 6)
	ld	a, (ix - 28)
	or	a, a
	jr	z, .Ls_r3
	ld	(ix - 30), de		; miny: the first row with land
.Ls_r3:
	ld	(ix - 33), de		; maxy: the last one
.Ls_row_next:
	ld	de, (ix - 6)
	ld	a, e
	cp	a, (iy + P_MAXY)
	jr	nz, .Ls_row_inc
	ld	a, d
	cp	a, (iy + P_MAXY + 1)
	jr	z, .Ls_done
.Ls_row_inc:
	inc	de
	jp	.Ls_row

.Ls_hit:				; p's owner bits: land?
	ld	a, (hl)
	tst	a, 96
	jr	z, .Ls_next
	ld	a, (ix - 7)
	or	a, a
	jr	nz, .Ls_h1
	ld	(ix - 9), hl
.Ls_h1:
	ld	(ix - 12), hl
	push	bc
	inc	hl
	call	.Ls_nb
	dec	hl
	dec	hl
	call	.Ls_nb
	inc	hl
	ld	bc, (ix - 15)
	add	hl, bc
	call	.Ls_nb
	or	a, a
	sbc	hl, bc
	sbc	hl, bc
	call	.Ls_nb
	add	hl, bc
	pop	bc
	jp	.Ls_next

.Ls_nb:					; neighbour at HL (kept, and BC, DE)
	ld	a, (hl)
	and	a, d
	cp	a, e
	ret	z			; p's (or water)
	ld	a, (hl)
	tst	a, 96
	ret	z			; water
	and	a, d
	jr	z, .Ls_tn
	push	hl
	push	de
	ld	de, 0
	ld	e, a
	ld	hl, _g_bitMask
	add	hl, de
	ld	a, (hl)
	ld	hl, _g_bitLo
	add	hl, de
	ld	e, (hl)
	lea	hl, ix - 19
	add	hl, de
	or	a, (hl)
	ld	(hl), a
	pop	de
	pop	hl
	ret
.Ls_tn:
	ld	(ix - 20), 1
	ret

.Ls_done:
	ld	hl, (ix - 19)		; the mask, free land
	ld	(iy + P_NBRS), hl
	ld	a, (ix - 16)
	ld	(iy + P_NBRS + 3), a
	ld	a, (ix - 20)
	ld	(iy + P_NBRTN), a
	ld	a, (ix - 28)		; land found: the new box
	or	a, a
	jr	nz, .Ls_end
	ld	a, (ix - 24)
	ld	(iy + P_MINX), a
	ld	a, (ix - 23)
	ld	(iy + P_MINX + 1), a
	ld	a, (ix - 27)
	ld	(iy + P_MAXX), a
	ld	a, (ix - 26)
	ld	(iy + P_MAXX + 1), a
	ld	a, (ix - 30)
	ld	(iy + P_MINY), a
	ld	a, (ix - 29)
	ld	(iy + P_MINY + 1), a
	ld	a, (ix - 33)
	ld	(iy + P_MAXY), a
	ld	a, (ix - 32)
	ld	(iy + P_MAXY + 1), a
.Ls_end:
	ld	sp, ix
	pop	ix
	ret
.Ls_end_fn:
	.size	_scan_player, .Ls_end_fn-_scan_player

	.extern	_g_map
	.extern	_g_W
	.extern	_g_pl
	.extern	_g_rowOfHi
	.extern	_g_rowStart
	.extern	_g_bitLo
	.extern	_g_bitMask
	.extern	_g_stbits
	.extern	_g_landLeft
	.extern	_g_mapClean
	.extern	_map_coast
	.extern	_ui_dirty
	.extern	_unit_on_conquer
	.extern	_kill_player
