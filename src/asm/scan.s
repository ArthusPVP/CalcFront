; CalcFront - hand-written eZ80 for the hottest loop of the game
;
; const uint8_t *next_owned(const uint8_t *p, const uint8_t *end, uint8_t o)
;   First byte from p to end (inclusive) that is a land tile of player o:
;   (b & 0x1F) == o and (b & 0x60) != 0. Returns 0 if there is none.
;   The C version (src/sim/attack.c, used by the PC harness) spends 16
;   instructions per tile; this loop spends 9, all but one in registers.
;
; Arguments on the stack: (sp+3) p, (sp+6) end, (sp+9) o. Result in HL.
; Uses AF, BC, DE, HL, IY (all free for the callee in this ABI).

	.assume	ADL = 1
	.section	.text._next_owned,"ax",@progbits
	.globl	_next_owned
	.type	_next_owned,@function
_next_owned:
	ld	iy, 0
	add	iy, sp
	ld	hl, (iy + 6)		; end
	ld	de, (iy + 3)		; p
	ld	c, (iy + 9)		; o
	or	a, a
	sbc	hl, de			; end - p
	jr	c, .Lnone		; nothing left (p > end)
	inc	hl
	ex	de, hl			; hl = p, de = tiles to look at
.Lloop:
	ld	a, (hl)
	and	a, 31			; owner bits
	cp	a, c
	jr	z, .Lowner
.Lnext:
	inc	hl
	dec	de
	ld	a, d
	or	a, e
	jr	nz, .Lloop
.Lnone:
	or	a, a
	sbc	hl, hl
	ret
.Lowner:
	ld	a, (hl)
	and	a, 96			; terrain bits: zero means water
	jr	z, .Lnext
	ret
.Lfunc_end:
	.size	_next_owned, .Lfunc_end-_next_owned
