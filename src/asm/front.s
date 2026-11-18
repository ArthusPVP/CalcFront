; CalcFront - hand-written eZ80 for the frontier scan of land attacks
;
; uint16_t front_scan(const uint8_t *p, uint24_t len, uint24_t rows,
;                     uint8_t def, uint8_t att, tile_t *ring)
;   Scans `rows` rows of `len` tiles (row stride g_W) from p for frontier
;   tiles: land of def with a land neighbour of att. Records them in row
;   order into ring[0..RING-1], reservoir-sampling (rnd_n) once it is full,
;   and returns how many there are. ring = 0: returns 1 at the first one.
;   Same result and rnd_n calls as front_scan_c (src/sim/attack.c).
;
; The scan spends most of its time skipping tiles, so the skip loop is 5
; instructions with no counter: the tile just right of the row (e1, inside
; the map: the frame column at worst) is replaced for the row by a sentinel,
; a water byte with def's owner bits, which stops it. That tile is read as a
; neighbour only by the last tile of the row (`end`): when it was the
; attacker's land, `end` is a frontier tile whenever it is def's land, and is
; added at the end of the row if the other neighbours did not add it.
;
; Arguments after push ix: (ix+6) p, (ix+9) len, (ix+12) rows, (ix+15) def,
; (ix+18) att, (ix+21) ring. Locals: (ix-3) n, (ix-6) last frontier tile,
; (ix-9) e1, (ix-12) byte at e1, (ix-11) e1 was the attacker's land.
; Registers: HL tile, BC row stride, D 31, E def, IYL att.

	.assume	ADL = 1
	.section	.text._front_scan,"ax",@progbits
	.globl	_front_scan
	.type	_front_scan,@function
_front_scan:
	push	ix
	ld	ix, 0
	add	ix, sp
	or	a, a
	sbc	hl, hl
	push	hl			; (ix-3) n = 0
	push	hl			; (ix-6) last = 0
	push	hl			; (ix-9) e1
	push	hl			; (ix-12) byte at e1, (ix-11) flag
	ld	bc, 0
	ld	a, (_g_W)
	ld	c, a
	ld	a, (_g_W+1)
	ld	b, a
	ld	a, (ix + 18)
	ld	iyl, a
.Lrow:
	ld	hl, (ix + 6)
	ld	de, (ix + 9)
	add	hl, de			; e1 = p + len
	ld	(ix - 9), hl
	ld	a, (hl)
	ld	(ix - 12), a
	ld	e, a
	and	a, 31
	cp	a, iyl
	ld	a, 0
	jr	nz, .Lflag
	ld	a, e
	and	a, 96
.Lflag:
	ld	(ix - 11), a		; nonzero: e1 is the attacker's land
	ld	a, (ix + 15)
	ld	(hl), a			; the sentinel
	ld	e, a
	ld	d, 31
	ld	hl, (ix + 6)
.Lskip:					; find def's owner bits
	ld	a, (hl)
	inc	hl
	and	a, d
	cp	a, e
	jr	nz, .Lskip
	dec	hl
	ld	a, (hl)
	and	a, 96
	jr	z, .Lwater
	dec	hl			; first tile of a run: left neighbour
	ld	a, (hl)
	and	a, d
	cp	a, iyl
	jr	nz, .Lc1
	ld	a, (hl)
	and	a, 96
	jr	z, .Lc1
	inc	hl
	jr	.Lfront
.Lc1:
	inc	hl
.Lrun:					; right, below, above
	inc	hl
	ld	a, (hl)
	dec	hl
	and	a, d
	cp	a, iyl
	jr	nz, .Lr1
	inc	hl
	ld	a, (hl)
	dec	hl
	and	a, 96
	jr	nz, .Lfront
.Lr1:
	add	hl, bc
	ld	a, (hl)
	and	a, d
	cp	a, iyl
	jr	nz, .Lr2
	ld	a, (hl)
	and	a, 96
	jr	nz, .Lfront_dn
.Lr2:
	or	a, a
	sbc	hl, bc
	sbc	hl, bc
	ld	a, (hl)
	and	a, d
	cp	a, iyl
	jr	nz, .Lr3
	ld	a, (hl)
	and	a, 96
	jr	nz, .Lfront_up
.Lr3:
	add	hl, bc
.Lafter:				; follow the run of def's land
	inc	hl
	ld	a, (hl)
	and	a, d
	cp	a, e
	jr	nz, .Lskip
	ld	a, (hl)
	and	a, 96
	jr	z, .Lskip		; water (the sentinel too): the skip loop
	jr	.Lrun			; stops on it again and sorts it out
.Lfront_dn:
	or	a, a
	sbc	hl, bc
	jr	.Lfront
.Lfront_up:
	add	hl, bc
.Lfront:
	call	.Lrecord
	jr	.Lafter

.Lwater:				; water with def's owner bits: e1?
	push	hl
	ld	de, (ix - 9)
	or	a, a
	sbc	hl, de
	pop	hl
	ld	e, (ix + 15)
	ld	d, 31
	jr	z, .Lrow_end
	inc	hl
	jr	.Lskip
.Lrow_end:
	ld	a, (ix - 12)
	ld	(hl), a			; restore e1
	ld	a, (ix - 11)
	or	a, a
	jr	z, .Lrow_next
	dec	hl			; end: def's land, not recorded yet?
	ld	a, (hl)
	and	a, d
	cp	a, e
	jr	nz, .Lrow_next
	ld	a, (hl)
	and	a, 96
	jr	z, .Lrow_next
	push	hl
	ld	de, (ix - 6)
	or	a, a
	sbc	hl, de
	pop	hl
	jr	z, .Lrow_next
	call	.Lrecord
.Lrow_next:
	ld	hl, (ix + 6)
	add	hl, bc
	ld	(ix + 6), hl
	ld	hl, (ix + 12)
	dec	hl
	ld	(ix + 12), hl
	ld	a, h
	or	a, l
	jp	nz, .Lrow
	ld	hl, (ix - 3)
	ld	sp, ix
	pop	ix
	ret

.Lrecord:				; record frontier tile HL (kept), n++
	ld	a, (ix + 23)		; ring = 0: border test
	or	a, a
	jr	z, .Lret1
	ld	(ix - 6), hl
	push	hl
	push	bc
	ld	de, (_g_map)
	or	a, a
	sbc	hl, de
	ex	de, hl			; DE = tile index
	ld	hl, (ix - 3)
	ld	bc, 128			; RING
	or	a, a
	sbc	hl, bc
	jr	nc, .Lres
	add	hl, bc
.Lstore:				; ring[HL] = DE
	add	hl, hl
	ld	bc, (ix + 21)
	add	hl, bc
	ld	(hl), e
	inc	hl
	ld	(hl), d
.Lrec_done:
	ld	hl, (ix - 3)
	inc	hl
	ld	(ix - 3), hl
	pop	bc
	pop	hl
	ld	e, (ix + 15)
	ld	d, 31
	ret
.Lres:					; ring full: j = rnd_n(n + 1), kept if j < RING
	push	de
	ld	hl, (ix - 3)
	inc	hl
	push	hl
	call	_rnd_n
	pop	de
	pop	de
	ld	a, (ix + 18)
	ld	iyl, a
	ld	a, h
	or	a, a
	jr	nz, .Lrec_done
	ld	a, l
	cp	a, 128
	jr	nc, .Lrec_done
	or	a, a
	sbc	hl, hl
	ld	l, a
	jr	.Lstore
.Lret1:					; restore e1, return 1
	ld	hl, (ix - 9)
	ld	a, (ix - 12)
	ld	(hl), a
	ld	hl, 1
	ld	sp, ix
	pop	ix
	ret
.Lfunc_end:
	.size	_front_scan, .Lfunc_end-_front_scan
