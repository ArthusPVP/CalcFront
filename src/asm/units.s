; CalcFront - hand-written eZ80 for the unit loop (C versions: src/sim/units.c)
;
; uint8_t units_next(uint8_t i)
;   The slot, from i on, of the next unit units_tick() must run C code for
;   (g_unHi or more at the end); does on its way what units_tick() would do
;   for the others: counts trade ships and trains into ut_cnt[0..1] and
;   counts down the timers of boats, trade ships, trains, explosions and
;   structures, stopping instead at a timer of 1 (it runs out: C's turn).
;   Stops at warships, nukes and shells, and at a port or factory with no
;   timer whose turn it is ((g_tick + slot) & 7 == 0).
;
; int16_t st_find(tile_t t)
;   The slot of the structure standing on t in the structure list (low bytes
;   of their tiles in un_st[0], high bytes in un_st[1], slots in un_st[2], in
;   list order; un_nSt entries), or -1. An entry counts only if the unit in
;   that slot is a structure on t now (the list is refreshed by recount()).
;
; unit_t: 32 bytes, type at 0, pos at 2, timer at 10 (checked in units.c)

	.assume	ADL = 1

U_PORT	= 3
U_FACTORY	= 4
U_SAM	= 6
U_TRADE	= 8
U_WARSHIP	= 9
U_SHELL	= 13
U_TRAIN	= 14
U_BOOM	= 15
U_TYPES	= 16
MAX_UNITS	= 200

	.section	.text._units_next,"ax",@progbits
	.globl	_units_next
	.type	_units_next,@function
; IY unit, C slot, B units left, E low byte of g_tick
_units_next:
	ld	iy, 0
	add	iy, sp
	ld	c, (iy + 3)
	ld	a, (_g_unHi)
	sub	a, c
	jr	c, .Lend
	jr	z, .Lend
	ld	b, a
	or	a, a
	sbc	hl, hl
	ld	l, c
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	ld	de, _g_un
	add	hl, de
	push	hl
	pop	iy
	ld	a, (_g_tick)
	ld	e, a
.Lloop:
	ld	a, (iy)			; type
	or	a, a
	jr	z, .Lnext
	cp	a, U_SAM + 1
	jr	c, .Lstruct
	cp	a, U_WARSHIP
	jr	z, .Lend		; warship
	jr	c, .Lship		; boat or trade ship
	cp	a, U_TRAIN
	jr	z, .Ltrain
	jr	nc, .Lboom
	jr	.Lend			; nukes, shells
.Lboom:
	cp	a, U_TYPES
	jr	c, .Ltimer		; explosion
	jr	.Lnext
.Ltrain:
	ld	hl, _ut_cnt + 1
	inc	(hl)
	jr	.Ltimer
.Lship:
	cp	a, U_TRADE
	jr	nz, .Ltimer
	ld	hl, _ut_cnt
	inc	(hl)
	jr	.Ltimer
.Lstruct:
	ld	d, a
	ld	a, (iy + 10)
	or	a, (iy + 11)
	jr	nz, .Ltimer
	ld	a, d
	cp	a, U_PORT
	jr	z, .Lturn
	cp	a, U_FACTORY
	jr	nz, .Lnext
.Lturn:
	ld	a, e
	add	a, c
	and	a, 7
	jr	z, .Lend
.Lnext:
	lea	iy, iy + 32
	inc	c
	djnz	.Lloop
.Lend:
	ld	a, c
	ret
.Ltimer:				; --timer, stop at 1
	ld	a, (iy + 10)
	dec	a
	ld	(iy + 10), a
	jr	nz, .Lt1
	ld	a, (iy + 11)
	or	a, a
	jr	nz, .Lnext		; was hi:01
	inc	(iy + 10)		; 1: put it back, C's turn
	jr	.Lend
.Lt1:
	inc	a
	jr	nz, .Lnext
	dec	(iy + 11)		; low byte was 0: borrow
	jr	.Lnext

	.section	.text._st_find,"ax",@progbits
	.globl	_st_find
	.type	_st_find,@function
_st_find:
	ld	iy, 0
	add	iy, sp
	ld	a, (_un_nSt)
	or	a, a
	jr	z, .Lnone
	ld	bc, 0
	ld	c, a
	ld	hl, _un_st
.Lscan:
	ld	a, (iy + 3)
	cpir				; low byte of the tile
	jr	nz, .Lnone
	push	bc
	push	hl
	ld	de, MAX_UNITS - 1
	add	hl, de
	ld	a, (iy + 4)
	cp	a, (hl)			; high byte
	jr	nz, .Lcont
	ld	de, MAX_UNITS
	add	hl, de
	ld	c, (hl)			; slot
	or	a, a
	sbc	hl, hl
	ld	l, c
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	ld	de, _g_un
	add	hl, de
	ld	a, (hl)
	dec	a
	cp	a, U_SAM
	jr	nc, .Lcont		; not a structure (any more)
	inc	hl
	inc	hl
	ld	a, (iy + 3)
	cp	a, (hl)
	jr	nz, .Lcont
	inc	hl
	ld	a, (iy + 4)
	cp	a, (hl)
	jr	nz, .Lcont
	ld	a, c
	pop	hl
	pop	bc
	or	a, a
	sbc	hl, hl
	ld	l, a
	ret
.Lcont:
	pop	hl
	pop	bc
	ld	a, c
	or	a, a
	jr	nz, .Lscan
.Lnone:
	scf
	sbc	hl, hl
	ret

	.extern	_g_unHi
	.extern	_g_un
	.extern	_g_tick
	.extern	_ut_cnt
	.extern	_un_st
	.extern	_un_nSt
