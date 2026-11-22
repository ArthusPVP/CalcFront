; CalcFront - hand-written eZ80 for the map renderer (src/ce/render.c)
;
; The C versions (the *_c twins in render.c: paint_rect_c, repaint_tile_c,
; mark_c, touched_c, blit_in_c, scr_xy_c) compile to library calls for every
; shift by the zoom, multiply by the screen width and signed compare; these
; give the same results (draw_selftest in a CF_SELFTEST build, screenshots).
;
; void paint_rect(int x0, int y0, int x1, int y1)
;   Repaints the map inside a screen rectangle (inclusive, viewport
;   coordinates, clipped to the viewport). For each row of tiles, the colours
;   of the tiles it crosses are computed once into a buffer (pass 1: terrain
;   or owner, darker on borders; railways: C_RAIL at zoom 1-2, bit 7 above);
;   each pixel row is then expanded from it (pass 2), or copied from the row
;   above when it is the same.
; void repaint_tile(int x, int y, tile_t t)
;   Repaints map tile (x, y) = t unless it was repainted this frame
;   (paintedT, direct-mapped by the low 6 bits).
; Uses AF, BC, DE, HL, IY; preserves IX.

	.assume	ADL = 1

SCR_W	= 320
VRAM	= 0xD40000
C_VOID	= 8
C_RAIL	= 11
C_OCEAN	= 16
C_PLAINS = 18
C_FILL	= 32
C_BORDER = 64

	.section	.text._paint_rect,"ax",@progbits
	.globl	_paint_rect
	.type	_paint_rect,@function
_paint_rect:
	push	ix
	ld	ix, 0
	add	ix, sp
	call	.Lclip
	jp	c, .Lp_ret
	inc	hl
	ld	a, l
	ld	(dr_h), a		; rows (<= 240)
	; rowp = VRAM + (vy0 + y0) * 320 + x0
	ld	a, (_vy0)
	add	a, e
	push	bc
	ld	bc, 0
	ld	b, a
	ld	c, 160
	mlt	bc
	ld	hl, VRAM
	add	hl, bc
	add	hl, bc
	pop	bc
	add	hl, bc
	ld	(dr_rowp), hl
	; yoff = y0 & (zoom - 1), my = camy + (y0 >> zsh)
	ld	a, (_zoom)
	dec	a
	and	a, e
	ld	(dr_yoff), a
	ex	de, hl
	call	.Lshr
	ld	de, (_camy)
	add	hl, de
	ld	(dr_my), hl
	; xoff = x0 & (zoom - 1), tc0 = camx + (x0 >> zsh)
	ld	a, (_zoom)
	dec	a
	and	a, c
	ld	(dr_xoff), a
	push	bc
	pop	hl
	call	.Lshr
	ld	de, (_camx)
	add	hl, de
	push	hl			; tc0
	; nt = ((xoff + w - 1) >> zsh) + 1 tiles per row
	ld	hl, (dr_w)
	ld	de, 0
	ld	a, (dr_xoff)
	ld	e, a
	add	hl, de
	dec	hl
	call	.Lshr
	inc	hl
	ld	(dr_nt), hl
	; first tile: s1 = min(zoom - xoff, w) pixels; then nf whole tiles and
	; a last one of sl pixels
	ld	a, (dr_xoff)
	ld	b, a
	ld	a, (_zoom)
	sub	a, b
	ld	hl, (dr_w)
	ld	e, a
	or	a, a
	sbc	hl, de
	jr	nc, .Lp_s1
	ld	a, (dr_w)
	or	a, a
	sbc	hl, hl
.Lp_s1:
	ld	(dr_s1), a
	ld	a, (_zoom)
	dec	a
	and	a, l
	ld	(dr_sl), a
	call	.Lshr
	ld	a, l
	ld	(dr_nf), a
	; buffer index i = tile tc0 + i; tiles left of the map: lv = min(-tc0, nt)
	pop	hl			; tc0
	ld	de, 0
	or	a, a
	sbc	hl, de
	jp	p, .Lp_lv
	ex	de, hl
	or	a, a
	sbc	hl, de			; -tc0
	ld	bc, (dr_nt)
	or	a, a
	sbc	hl, bc
	add	hl, bc
	jr	c, .Lp_lv1
	push	bc
	pop	hl
.Lp_lv1:
	ex	de, hl			; DE = lv, HL = tc0
.Lp_lv:
	ld	(dr_lv), de
	add	hl, de
	ld	(dr_mxf), hl		; first tile on the map
	; nmid = clamp(W - mxf, 0, nt - lv) tiles on the map
	ex	de, hl
	call	.LldW
	push	bc
	pop	hl
	or	a, a
	sbc	hl, de
	jp	p, .Lp_nm
	or	a, a
	sbc	hl, hl
.Lp_nm:
	push	hl
	ld	hl, (dr_nt)
	ld	de, (dr_lv)
	or	a, a
	sbc	hl, de
	ex	de, hl
	pop	hl
	or	a, a
	sbc	hl, de
	add	hl, de
	jr	c, .Lp_nm1
	ex	de, hl
.Lp_nm1:
	ld	(dr_nmid), hl
	; the tiles off the map are void
	ld	hl, dr_buf
	ld	bc, (dr_lv)
	call	.Lfillv
	ld	de, (dr_nmid)
	add	hl, de
	push	hl
	ld	hl, (dr_nt)
	ld	de, (dr_lv)
	or	a, a
	sbc	hl, de
	ld	de, (dr_nmid)
	or	a, a
	sbc	hl, de
	push	hl
	pop	bc
	pop	hl
	call	.Lfillv
	ld	a, 0xFF
	ld	(dr_last), a		; no row above
	xor	a, a
	ld	(dr_ready), a

.Lp_row:
	; railRow = zoom > 2 && (uint8_t)(yoff - railLo) < railN
	ld	a, (_zoom)
	cp	a, 3
	ld	c, 0
	jr	c, .Lp_rr
	ld	a, (_railN)
	ld	b, a
	ld	a, (_railLo)
	ld	e, a
	ld	a, (dr_yoff)
	sub	a, e
	cp	a, b
	jr	nc, .Lp_rr
	inc	c
.Lp_rr:
	ld	a, c
	ld	(dr_rr), a
	ld	hl, dr_last
	ld	b, (hl)
	ld	(hl), a
	cp	a, b
	jr	nz, .Lp_calc
	ld	a, (dr_yoff)
	or	a, a
	jr	z, .Lp_calc
	; same tile row, same rail band: copy the row above
	ld	de, (dr_rowp)
	ld	hl, -SCR_W
	add	hl, de
	ld	bc, (dr_w)
	ldir
	jp	.Lp_next
.Lp_calc:
	ld	hl, (dr_my)
	ld	de, 0
	or	a, a
	sbc	hl, de
	jp	m, .Lp_void
	ld	a, (_g_H)
	ld	e, a
	ld	a, (_g_H + 1)
	ld	d, a
	or	a, a
	sbc	hl, de
	jr	nc, .Lp_void
	ld	a, (dr_ready)
	or	a, a
	call	z, .Ltilerow
	; pass 2: expand the tile colours into the pixel row
	ld	de, (dr_rowp)
	ld	hl, dr_buf
	ld	a, (_zoom)
	dec	a
	jr	nz, .Lp_zoom
	ld	bc, (dr_w)		; one pixel per tile
	ldir
	jr	.Lp_next
.Lp_zoom:
	ld	a, (_railLo)
	ld	ixl, a
	ld	a, (dr_rr)
	or	a, a
	jr	z, .Lp_rn
	ld	a, (_railN)
.Lp_rn:
	ld	ixh, a			; rail pixels: railLo .. railLo + IXH - 1
	ld	a, (dr_xoff)
	ld	c, a
	ld	a, (dr_s1)
	ld	b, a
	ld	a, (hl)
	inc	hl
	call	.Lemit
	ld	a, (dr_nf)
	or	a, a
	jr	z, .Lp_last
	ld	iyh, a
.Lp_full:
	ld	a, (_zoom)
	ld	b, a
	ld	c, 0
	ld	a, (hl)
	inc	hl
	call	.Lemit
	dec	iyh
	jr	nz, .Lp_full
.Lp_last:
	ld	a, (dr_sl)
	or	a, a
	jr	z, .Lp_next
	ld	b, a
	ld	c, 0
	ld	a, (hl)
	call	.Lemit
	jr	.Lp_next
.Lp_void:
	ld	hl, (dr_rowp)
	ld	bc, (dr_w)
	call	.Lfillv
.Lp_next:
	ld	hl, (dr_rowp)
	ld	bc, SCR_W
	add	hl, bc
	ld	(dr_rowp), hl
	ld	hl, dr_yoff
	inc	(hl)
	ld	a, (_zoom)
	cp	a, (hl)
	jr	nz, .Lp_n1
	ld	(hl), 0
	ld	hl, (dr_my)
	inc	hl
	ld	(dr_my), hl
	xor	a, a
	ld	(dr_ready), a
.Lp_n1:
	ld	hl, dr_h
	dec	(hl)
	jp	nz, .Lp_row
.Lp_ret:
	pop	ix
	ret

; pass 1 for tile row my: colours of the nmid tiles on the map
.Ltilerow:
	ld	a, 1
	ld	(dr_ready), a
	ld	hl, (dr_nmid)
	ld	a, h
	or	a, l
	ret	z
	ld	hl, (dr_my)
	add	hl, hl
	ld	de, _g_rowStart
	add	hl, de
	ld	de, 0
	ld	e, (hl)
	inc	hl
	ld	d, (hl)
	ld	hl, (dr_mxf)
	add	hl, de
	ld	(dr_t), hl
	ld	hl, (dr_nmid)
	ld	a, h
	or	a, a
	ld	b, l
	ld	hl, 0
	jr	z, .Ltrchunk		; under 256 tiles: one chunk
	ld	b, 128
	call	.Ltrchunk
	ld	a, (dr_nmid)
	add	a, 128
	ld	b, a
	ld	hl, 128
; B tiles from offset HL of the row
.Ltrchunk:
	ld	a, b
	ld	(dr_cnt), a
	ex	de, hl
	ld	hl, (dr_t)
	add	hl, de
	ld	(dr_tc), hl		; first tile
	ld	hl, dr_buf
	add	hl, de
	ld	de, (dr_lv)
	add	hl, de
	ld	(dr_oc), hl
	ex	de, hl			; DE = its colour
	ld	ix, (_g_map)
	ld	bc, (dr_tc)
	add	ix, bc
	ld	a, (dr_cnt)
	ld	b, a
	call	.Lcolsb
	ld	a, (_railOn)
	or	a, a
	ret	z
	ld	hl, (dr_tc)
	ld	de, (dr_oc)
	ld	a, (dr_cnt)
	ld	b, a
	jr	.Lrailsb

; colours of B tiles (1..255) from IX = map pointer into DE: ocean, terrain
; (+3 for fallout) or owner (+32 on borders: a land neighbour of another owner)
.Lcolsb:
	push	bc
	call	.LldW
	lea	hl, ix + 0
	or	a, a
	sbc	hl, bc
	push	hl
	pop	iy			; IY = the tile above
	lea	hl, ix + 0
	add	hl, bc			; HL = the tile below
	pop	bc
.Lc_tile:
	ld	a, (ix + 0)
	ld	c, a
	tst	a, 0x60
	jr	z, .Lc_water
	and	a, 0x1F
	jr	z, .Lc_neutral
	ld	c, a			; owner
	ld	a, (ix + 1)
	tst	a, 0x60
	jr	z, .Lc_n1
	and	a, 0x1F
	cp	a, c
	jr	nz, .Lc_border
.Lc_n1:
	ld	a, (ix - 1)
	tst	a, 0x60
	jr	z, .Lc_n2
	and	a, 0x1F
	cp	a, c
	jr	nz, .Lc_border
.Lc_n2:
	ld	a, (iy + 0)
	tst	a, 0x60
	jr	z, .Lc_n3
	and	a, 0x1F
	cp	a, c
	jr	nz, .Lc_border
.Lc_n3:
	ld	a, (hl)
	tst	a, 0x60
	jr	z, .Lc_own
	and	a, 0x1F
	cp	a, c
	jr	nz, .Lc_border
.Lc_own:
	ld	a, c
	add	a, C_FILL
	jr	.Lc_store
.Lc_border:
	ld	a, c
	add	a, C_BORDER
	jr	.Lc_store
.Lc_water:
	ld	a, C_OCEAN
	jr	.Lc_store
.Lc_neutral:
	ld	a, c			; terrain t (1..3) + 4 * fallout
	and	a, 0xE0
	rlca
	rlca
	rlca
	cp	a, 4
	jr	c, .Lc_nf
	dec	a
.Lc_nf:
	add	a, C_PLAINS - 1
.Lc_store:
	ld	(de), a
	inc	de
	inc	ix
	inc	iy
	inc	hl
	djnz	.Lc_tile
	ret

; railways over B colours (1..255) at DE, for tiles from HL: C_RAIL at zoom
; 1 and 2, bit 7 set above (pass 2 draws the mark)
.Lrailsb:
	ld	a, l
	and	a, 7
	ld	c, 1
	jr	z, .Lr_m
.Lr_sh:
	sla	c
	dec	a
	jr	nz, .Lr_sh
.Lr_m:
	srl	h
	rr	l
	srl	h
	rr	l
	srl	h
	rr	l
	push	de
	ld	de, (_g_mem)		; railbits come first
	add	hl, de
	pop	de
	ld	a, (_zoom)
	cp	a, 3
	ld	ixl, C_RAIL
	ld	ixh, 0
	jr	c, .Lr_loop
	ld	ixl, 0x80
	ld	ixh, 0xFF
.Lr_loop:
	ld	a, (hl)
	and	a, c
	jr	z, .Lr_no
	ld	a, (de)
	and	a, ixh
	or	a, ixl
	ld	(de), a
.Lr_no:
	inc	de
	rlc	c
	jr	nc, .Lr_n2
	inc	hl
.Lr_n2:
	djnz	.Lr_loop
	ret

; B pixels (>= 1) of colour A at DE, the first at position C in its tile;
; bit 7 of A: rail mark, positions IXL .. IXL + IXH - 1 take C_RAIL
.Lemit:
	bit	7, a
	jr	nz, .Le_rail
.Le_fill:
	ld	(de), a
	inc	de
	djnz	.Le_fill
	ret
.Le_rail:
	and	a, 0x7F
	ld	iyl, a
.Le_px:
	ld	a, c
	sub	a, ixl
	cp	a, ixh
	ld	a, C_RAIL
	jr	c, .Le_st
	ld	a, iyl
.Le_st:
	ld	(de), a
	inc	de
	inc	c
	djnz	.Le_px
	ret

; BC bytes (may be 0, < 65536) of C_VOID at HL; HL ends past them
.Lfillv:
	ld	a, b
	or	a, c
	ret	z
.Lfv:
	ld	(hl), C_VOID
	inc	hl
	dec	bc
	ld	a, b
	or	a, c
	jr	nz, .Lfv
	ret

; clip the rectangle (ix+6) x0, (ix+9) y0, (ix+12) x1, (ix+15) y1 to the
; viewport: carry set when empty, else cl_x0 .. cl_y1, dr_w = x1 - x0 + 1,
; BC = x0, DE = y0, HL = y1 - y0
.Lclip:
	ld	de, (ix + 6)
	bit	7, (ix + 8)
	jr	z, .Lk_x0
	ld	de, 0
.Lk_x0:
	ld	(cl_x0), de
	ld	hl, (ix + 12)
	bit	7, (ix + 14)
	jr	nz, .Lk_x1
	ld	bc, SCR_W
	or	a, a
	sbc	hl, bc
	add	hl, bc
	jr	c, .Lk_x1
	ld	hl, SCR_W - 1
.Lk_x1:
	ld	(cl_x1), hl
	or	a, a
	sbc	hl, de
	jp	m, .Lk_empty
	inc	hl
	ld	(dr_w), hl
	ld	de, (ix + 9)
	bit	7, (ix + 11)
	jr	z, .Lk_y0
	ld	de, 0
.Lk_y0:
	ld	(cl_y0), de
	ld	hl, (ix + 15)
	ld	bc, 0
	ld	a, (_vh)
	ld	c, a
	bit	7, (ix + 17)
	jr	nz, .Lk_y1
	or	a, a
	sbc	hl, bc
	add	hl, bc
	jr	c, .Lk_y1
	dec	bc
	push	bc
	pop	hl
.Lk_y1:
	ld	(cl_y1), hl
	or	a, a
	sbc	hl, de
	jp	m, .Lk_empty
	ld	bc, (cl_x0)
	or	a, a
	ret
.Lk_empty:
	scf
	ret

; BC = g_W
.LldW:
	ld	bc, 0
	ld	a, (_g_W)
	ld	c, a
	ld	a, (_g_W + 1)
	ld	b, a
	ret

; HL >>= zsh (HL < 65536), HL <<= zsh (signed 24-bit)
.Lshr:
	ld	a, (_zsh)
	or	a, a
	ret	z
.Lshr1:
	srl	h
	rr	l
	dec	a
	jr	nz, .Lshr1
	ret
.Lshl:
	ld	a, (_zsh)
	or	a, a
	ret	z
.Lshl1:
	add	hl, hl
	dec	a
	jr	nz, .Lshl1
	ret

	.globl	_repaint_tile
	.type	_repaint_tile,@function
_repaint_tile:
	push	ix
	ld	ix, 0
	add	ix, sp
	; (ix+6) x, (ix+9) y, (ix+12) t: once per frame
	ld	hl, 0
	ld	a, (ix + 12)
	and	a, 63
	ld	l, a
	add	hl, hl
	ld	de, _paintedT
	add	hl, de
	ld	a, (ix + 12)
	cp	a, (hl)
	jr	nz, .Lt_new
	inc	hl
	ld	a, (ix + 13)
	cp	a, (hl)
	jp	z, .Lt_ret
	dec	hl
.Lt_new:
	ld	a, (ix + 12)
	ld	(hl), a
	inc	hl
	ld	a, (ix + 13)
	ld	(hl), a
	; sx = (x - camx) << zsh in 0..319, sy = (y - camy) << zsh in 0..vh-1
	ld	hl, (ix + 6)
	ld	de, (_camx)
	or	a, a
	sbc	hl, de
	call	.Lshl
	ld	bc, SCR_W
	or	a, a
	sbc	hl, bc
	add	hl, bc
	jp	nc, .Lt_ret
	ex	de, hl			; DE = sx
	ld	hl, (ix + 9)
	ld	bc, (_camy)
	or	a, a
	sbc	hl, bc
	call	.Lshl
	ld	bc, 0
	ld	a, (_vh)
	ld	c, a
	or	a, a
	sbc	hl, bc
	add	hl, bc
	jp	nc, .Lt_ret
	ld	a, (_zoom)
	dec	a
	ld	c, a			; BC = zoom - 1
	ld	a, (_vh)
	push	hl
	add	hl, bc			; bottom row
	cp	a, l
	jp	z, .Lt_cut
	jp	c, .Lt_cut
	pop	hl
	; a whole tile: d = VRAM + (vy0 + sy) * 320 + sx
	ld	a, (_vy0)
	add	a, l
	ld	bc, 0
	ld	b, a
	ld	c, 160
	mlt	bc
	ld	hl, VRAM
	add	hl, bc
	add	hl, bc
	add	hl, de
	push	hl
	ld	a, C_VOID
	ld	(dr_tc), a
	ld	hl, (ix + 6)		; x, y on the map?
	call	.LldW
	or	a, a
	sbc	hl, bc
	jr	nc, .Lt_fill
	ld	hl, (ix + 9)
	ld	bc, 0
	ld	a, (_g_H)
	ld	c, a
	ld	a, (_g_H + 1)
	ld	b, a
	or	a, a
	sbc	hl, bc
	jr	nc, .Lt_fill
	ld	de, 0
	ld	e, (ix + 12)
	ld	d, (ix + 13)
	push	de
	ld	ix, (_g_map)
	add	ix, de
	ld	de, dr_tc
	ld	b, 1
	call	.Lcolsb
	pop	hl
	ld	a, (_railOn)
	or	a, a
	jr	z, .Lt_fill
	ld	de, dr_tc
	ld	b, 1
	call	.Lrailsb
.Lt_fill:
	pop	hl
	ld	a, (dr_tc)
	ld	(hl), a
	ld	c, a
	ld	a, (_zoom)
	dec	a
	jr	z, .Lt_ret
	dec	a
	jr	nz, .Lt_big
	inc	hl			; zoom 2
	ld	(hl), c
	ld	de, SCR_W - 1
	add	hl, de
	ld	(hl), c
	inc	hl
	ld	(hl), c
	jr	.Lt_ret
.Lt_big:				; zoom 4, 8: rows k, rail mark on rows
	ex	de, hl			; railLo .. railLo + railN - 1
	ld	a, (_railLo)
	ld	ixl, a
	ld	a, (_zoom)
	ld	iyh, a
	ld	l, 0
.Lt_row:
	ld	a, (_railN)
	ld	b, a
	ld	a, l
	sub	a, ixl
	cp	a, b
	ld	a, 0
	jr	nc, .Lt_nb
	ld	a, b
.Lt_nb:
	ld	ixh, a
	ld	a, (_zoom)
	ld	b, a
	ld	c, 0
	ld	a, (dr_tc)
	push	hl
	call	.Lemit
	ld	hl, SCR_W
	ld	bc, 0
	ld	a, (_zoom)
	ld	c, a
	or	a, a
	sbc	hl, bc
	add	hl, de
	ex	de, hl
	pop	hl
	inc	l
	dec	iyh
	jr	nz, .Lt_row
.Lt_ret:
	pop	ix
	ret
.Lt_cut:				; cut by the bottom of the view:
	pop	iy			; paint_rect(sx, sy, sx + zoom - 1, bottom)
	push	hl
	ex	de, hl
	push	hl
	add	hl, bc
	ex	(sp), hl
	push	iy
	push	hl
	call	_paint_rect
	pop	hl
	pop	hl
	pop	hl
	pop	hl
	jr	.Lt_ret

; void mark(int x0, int y0, int x1, int y1)
;   Marks the 16x16 cells of dcell under a rectangle (viewport coordinates,
;   clipped) and sets dirtyAny.
; bool touched(int x0, int y0, int x1, int y1)
;   Whether a cell under the rectangle is marked.
	.globl	_mark
	.type	_mark,@function
_mark:
	push	ix
	ld	ix, 0
	add	ix, sp
	call	.Lclip
	jr	c, .Lm_ret
	ld	a, 1
	ld	(_dirtyAny), a
	call	.Lcells
.Lm_row:
	push	hl
	push	bc
.Lm_col:
	ld	(hl), 1
	inc	hl
	dec	c
	jr	nz, .Lm_col
	pop	bc
	pop	hl
	ld	de, 20
	add	hl, de
	djnz	.Lm_row
.Lm_ret:
	pop	ix
	ret

	.globl	_touched
	.type	_touched,@function
_touched:
	push	ix
	ld	ix, 0
	add	ix, sp
	call	.Lclip
	jr	c, .Lo_no
	call	.Lcells
.Lo_row:
	push	hl
	push	bc
.Lo_col:
	ld	a, (hl)
	or	a, a
	jr	nz, .Lo_yes
	inc	hl
	dec	c
	jr	nz, .Lo_col
	pop	bc
	pop	hl
	ld	de, 20
	add	hl, de
	djnz	.Lo_row
.Lo_no:
	xor	a, a
	pop	ix
	ret
.Lo_yes:
	pop	bc
	pop	hl
	ld	a, 1
	pop	ix
	ret

; cells under the clipped rectangle: HL = the first one, B rows, C columns
.Lcells:
	ld	hl, (cl_y1)
	call	.Lsh4
	ld	b, a
	ld	hl, (cl_y0)
	call	.Lsh4
	ld	c, a
	ld	a, b
	sub	a, c
	inc	a
	ld	b, a
	push	bc			; B rows, C first row
	ld	hl, (cl_x1)
	call	.Lsh4
	ld	e, a
	ld	hl, (cl_x0)
	call	.Lsh4
	ld	d, a			; first column
	ld	a, e
	sub	a, d
	inc	a
	pop	bc
	ld	hl, 0
	ld	h, c
	ld	l, 20
	mlt	hl
	ld	c, a			; columns
	ld	a, d
	ld	de, _dcell
	add	hl, de
	ld	de, 0
	ld	e, a
	add	hl, de
	ret

; A = HL >> 4 (HL < 4096)
.Lsh4:
	ld	a, l
	srl	h
	rra
	srl	h
	rra
	srl	h
	rra
	srl	h
	rra
	ret

; bool blit_in(const uint8_t *m, uint8_t w, uint8_t h, int x0, int y0,
;              const uint8_t *cols)
;   Draws a w x h mask with its top-left at (x0, y0) when it lies inside the
;   viewport (else returns false): pixel value v takes colour cols[v],
;   except v = 0 when cols[0] = 0xFF (transparent).
	.globl	_blit_in
	.type	_blit_in,@function
_blit_in:
	ld	iy, 0
	add	iy, sp
	; (iy+3) m, (iy+6) w, (iy+9) h, (iy+12) x0, (iy+15) y0, (iy+18) cols
	xor	a, a
	bit	7, (iy + 14)
	ret	nz
	bit	7, (iy + 17)
	ret	nz
	ld	hl, (iy + 12)
	ld	de, 0
	ld	e, (iy + 6)
	add	hl, de
	ld	de, SCR_W + 1
	or	a, a
	sbc	hl, de
	ld	a, 0
	ret	nc			; x0 + w > 320
	ld	hl, (iy + 15)
	ld	de, 0
	ld	e, (iy + 9)
	add	hl, de
	ld	a, (_vh)
	ld	e, a
	inc	de
	or	a, a
	sbc	hl, de
	ld	a, 0
	ret	nc			; y0 + h > vh
	ld	a, (_vy0)
	add	a, (iy + 15)
	ld	bc, 0
	ld	b, a
	ld	c, 160
	mlt	bc
	ld	hl, VRAM
	add	hl, bc
	add	hl, bc
	ld	bc, (iy + 12)
	add	hl, bc
	ld	de, (iy + 3)
	ld	c, (iy + 9)
	ld	a, (iy + 6)
	ld	iy, (iy + 18)
	ld	(dr_cnt), a
.Lb_row:
	push	hl
	ld	a, (dr_cnt)
	ld	b, a
.Lb_px:
	ld	a, (de)
	inc	de
	or	a, a
	jr	z, .Lb_zero
	dec	a
	ld	a, (iy + 1)
	jr	z, .Lb_st
	ld	a, (iy + 2)
	jr	.Lb_st
.Lb_zero:
	ld	a, (iy + 0)
	inc	a
	jr	z, .Lb_skip		; transparent
	dec	a
.Lb_st:
	ld	(hl), a
.Lb_skip:
	inc	hl
	djnz	.Lb_px
	pop	hl
	push	de
	ld	de, SCR_W
	add	hl, de
	pop	de
	dec	c
	jr	nz, .Lb_row
	ld	a, 1
	ret

; int scr_xy_a(tile_t t)
;   Screen centre of tile t: returns x, sets scr_y; also the tile's column
;   and row in scr_tx, scr_ty.
	.globl	_scr_xy_a
	.type	_scr_xy_a,@function
_scr_xy_a:
	ld	iy, 0
	add	iy, sp
	ld	hl, 0
	ld	l, (iy + 4)
	add	hl, hl
	ld	bc, _g_rowOfHi
	add	hl, bc
	ld	bc, 0
	ld	c, (hl)
	inc	hl
	ld	b, (hl)			; y = g_rowOfHi[t >> 8]
	ld	hl, 0
	add	hl, bc
	add	hl, hl
	ld	de, _g_rowStart + 2
	add	hl, de			; &g_rowStart[y + 1]
	ld	e, (iy + 3)
	ld	d, (iy + 4)
.Lsx_y:					; while (t >= g_rowStart[y + 1]) y++
	ld	a, e
	sub	a, (hl)
	inc	hl
	ld	a, d
	sbc	a, (hl)
	jr	c, .Lsx_row
	inc	bc
	inc	hl
	jr	.Lsx_y
.Lsx_row:
	dec	hl
	dec	hl
	dec	hl			; g_rowStart[y]
	ld	a, e
	sub	a, (hl)
	ld	e, a
	inc	hl
	ld	a, d
	sbc	a, (hl)
	ld	d, a
	ld	hl, 0			; (not ex de, hl: DEU holds the top of &g_rowStart)
	ld	l, e
	ld	h, d			; x
	ld	(_scr_tx), hl
	ld	(_scr_ty), bc
	push	hl
	push	bc
	pop	hl
	ld	de, (_camy)
	or	a, a
	sbc	hl, de
	call	.Lshl
	ld	a, (_zoom)
	srl	a
	ld	de, 0
	ld	e, a
	add	hl, de
	ld	(_scr_y), hl
	pop	hl
	ld	bc, (_camx)
	or	a, a
	sbc	hl, bc
	call	.Lshl
	add	hl, de
	ret

	.section	.bss._dr_vars,"aw",@nobits
dr_w:	.zero	3
dr_rowp: .zero	3
dr_my:	.zero	3
dr_nt:	.zero	3
dr_lv:	.zero	3
dr_mxf:	.zero	3
dr_nmid: .zero	3
dr_t:	.zero	3
dr_tc:	.zero	3
dr_oc:	.zero	3
dr_h:	.zero	1
dr_yoff: .zero	1
dr_xoff: .zero	1
dr_s1:	.zero	1
dr_nf:	.zero	1
dr_sl:	.zero	1
dr_rr:	.zero	1
dr_last: .zero	1
dr_ready: .zero	1
dr_cnt:	.zero	1
cl_x0:	.zero	3
cl_y0:	.zero	3
cl_x1:	.zero	3
cl_y1:	.zero	3
dr_buf:	.zero	320
