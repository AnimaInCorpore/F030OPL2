; The practical model's register decoder on the 68k: OPL2 register writes in,
; the DSP kernel's sample-stamped parameter events out.
;
; A translation of OplPractical::Decoder (tools/opl/opl-practical.h) for an
; OPL2: nine channels, eighteen operators, register bank 0. It keeps the
; chip's register image, marks what a write changes, and at the next frame
; (or an explicit flush) derives the operator and channel words and emits
; each one that differs from its shadow - the same words, in the same order,
; with the same values as the C++ decoder, which opldec-gate.py checks event
; for event. OplMidi::Pipeline::begin's fresh-machine reset is
; opl_dec_begin.
;
; Events go to opl_dec_events: 12 bytes each, frame.l, address.w, pad.w,
; value.l (the 24-bit word, sign-extended as the decoder holds it).
;
; Registers: every entry preserves all registers except the documented ones.
; No 64-bit multiply is used (the 68060 does not have one).

        global  opl_dec_begin, opl_dec_write, opl_dec_flush
        global  opl_dec_events, opl_dec_count, opl_dec_overflow
        global  opl_dec_take

; DSP layout (opl-practical.h).
OP_BASE         equ     $0400
OP_STRIDE       equ     32
CH_BASE         equ     $0900
CH_STRIDE       equ     8
WAVE_BASE       equ     $1000
SC_TREMOLO_SHIFT equ    $0010
SC_CHANNELS     equ     $0012
SC_RHYTHM       equ     $0030

OP_TRIG         equ     1
OP_TRIGSEEN     equ     2
OP_FLAGS        equ     3
OP_STATE        equ     4
OP_ENV          equ     5
OP_SL           equ     6
OP_RATE_A       equ     7
OP_RATE_D       equ     8
OP_RATE_S       equ     9
OP_RATE_R       equ     10
OP_TLKSL        equ     11
OP_INCBASE      equ     12
OP_WFBASE       equ     14
OP_VIBDELTA     equ     19
CH_CONN         equ     1
CH_FBMUL        equ     2
CH_ROUTE        equ     3

SLOTS           equ     18
CHANNELS        equ     9
ALL_BITS        equ     (1<<SLOTS)-1
INC_Q16         equ     1061258                 ; OPL_PRACTICAL_INC_Q16
INC_HI          equ     INC_Q16>>16             ; 16
INC_LO          equ     INC_Q16&$ffff           ; 12682
MAX_EVENTS      equ     3072                    ; Pipeline::kPendingCapacity
EV_SIZE         equ     12

; Decoder state, at a6 = dec.
S_SLOT          equ     0                       ; 18 x 12 bytes
SL_TREMOLO      equ     0
SL_VIBRATO      equ     1
SL_TYPE         equ     2
SL_KSR          equ     3
SL_MULT         equ     4
SL_KSL          equ     5
SL_TL           equ     6
SL_AR           equ     7
SL_DR           equ     8
SL_SL           equ     9
SL_RR           equ     10
SL_WF           equ     11
SL_SIZE         equ     12
S_CHAN          equ     S_SLOT+SLOTS*SL_SIZE    ; 9 x 6 bytes
CR_FNUM         equ     0                       ; .w
CR_BLOCK        equ     2
CR_FEEDBACK     equ     3
CR_CONNECTION   equ     4
CR_SIZE         equ     6
S_KEY           equ     S_CHAN+CHANNELS*CR_SIZE ; 18 bytes
S_NTS           equ     S_KEY+SLOTS
S_NEWM          equ     S_NTS+1
S_TREMSHIFT     equ     S_NTS+2
S_VIBSHIFT      equ     S_NTS+3
S_RHYTHM        equ     S_NTS+4
S_WFGATE        equ     S_NTS+5
S_WFENABLE      equ     S_NTS+6
S_TRIGGER       equ     (S_NTS+8+3)&~3          ; 18 longs
S_SHOP          equ     S_TRIGGER+SLOTS*4       ; 18 x 32 longs
S_SHCH          equ     S_SHOP+SLOTS*OP_STRIDE*4 ; 9 x 8 longs
S_SHSC          equ     S_SHCH+CHANNELS*CH_STRIDE*4 ; 3 longs
S_SHRHY         equ     S_SHSC+12
S_BLOCK         equ     S_SHRHY+4
S_DIRTYINC      equ     S_BLOCK+4
S_DIRTYENV      equ     S_DIRTYINC+4
S_DIRTYSLOT     equ     S_DIRTYENV+4
S_FRESH         equ     S_DIRTYSLOT+4           ; .w nonzero: the fresh-machine filter
S_SIZE          equ     S_FRESH+4

        text

; Pipeline::begin(frame, freshMachine = true): the decoder's reset, of which
; a freshly uploaded machine needs only the nine channels' output routing.
; In: d0.l = frame. The event list is emptied.
opl_dec_begin
        movem.l d0-d7/a0-a6,-(sp)
        lea     dec,a6
        move.l  d0,d7
        clr.l   opl_dec_count
        clr.w   opl_dec_overflow
        move.l  a6,a0
        move.w  #S_SIZE/2-1,d1
.clear
        clr.w   (a0)+
        dbra    d1,.clear
        move.l  d7,S_BLOCK(a6)
        move.b  #4,S_TREMSHIFT(a6)
        move.b  #1,S_VIBSHIFT(a6)
        move.b  #1,S_WFGATE(a6)
        move.w  #1,S_FRESH(a6)
        ; every operator: release, envelope 0x1ff << 12, the first waveform;
        ; the reset's writes of them go nowhere through the filter
        lea     S_SHOP(a6),a0
        moveq   #SLOTS-1,d1
.ops
        move.l  #3,OP_STATE*4(a0)
        move.l  #$1ff<<12,OP_ENV*4(a0)
        move.l  #WAVE_BASE,OP_WFBASE*4(a0)
        lea     OP_STRIDE*4(a0),a0
        dbra    d1,.ops
        ; the channels: routing 3, written; connection and feedback 0
        moveq   #0,d6
.channels
        move.w  d6,d0
        lsl.w   #5,d0                           ; * CH_STRIDE * 4
        move.l  #3,S_SHCH+CH_ROUTE*4(a6,d0.w)
        move.w  d6,d0
        lsl.w   #3,d0
        addi.w  #CH_BASE+CH_ROUTE,d0
        moveq   #3,d1
        bsr     emit_raw
        addq.w  #1,d6
        cmpi.w  #CHANNELS,d6
        blo.s   .channels
        move.l  #4,S_SHSC(a6)                   ; tremolo shift
        move.l  #CHANNELS,S_SHSC+8(a6)          ; channel count
        clr.w   S_FRESH(a6)
        movem.l (sp)+,d0-d7/a0-a6
        rts

; Decoder::write(frame, reg, value). In: d0.l = frame, d1.w = register,
; d2.b = value.
opl_dec_write
        movem.l d0-d7/a0-a6,-(sp)
        lea     dec,a6
        cmp.l   S_BLOCK(a6),d0
        beq.s   .same
        bsr     flush
.same
        move.l  d0,S_BLOCK(a6)
        btst    #8,d1
        bne     .out                            ; bank 1: not an OPL2's
        moveq   #0,d3
        move.b  d1,d3                           ; low
        moveq   #0,d4
        move.b  d2,d4                           ; value
        ; the operator this register addresses, or -1
        moveq   #$1f,d0
        and.w   d3,d0
        lea     address_slot(pc),a0
        move.b  0(a0,d0.w),d0
        ext.w   d0
        bmi.s   .no_slot
        lea     slot_index(pc),a0
        move.b  0(a0,d0.w),d0
        ext.w   d0
.no_slot
        move.w  d0,d5                           ; index (-1: none)
        move.w  d3,d0
        andi.w  #$f0,d0
        lsr.w   #3,d0
        move.w  .cases(pc,d0.w),d0
        jsr     .cases(pc,d0.w)
.out
        movem.l (sp)+,d0-d7/a0-a6
        rts
.cases
        dc.w    w_00-.cases,w_none-.cases,w_20-.cases,w_20-.cases
        dc.w    w_40-.cases,w_40-.cases,w_60-.cases,w_60-.cases
        dc.w    w_80-.cases,w_80-.cases,w_a0-.cases,w_b0-.cases
        dc.w    w_c0-.cases,w_none-.cases,w_e0-.cases,w_e0-.cases

w_none
        rts

; 0x00-0x0f: register 8 (note select), register 1 (waveform enable).
w_00
        move.b  d3,d0
        andi.b  #$0f,d0
        cmpi.b  #8,d0
        bne.s   .not_8
        move.b  d4,d0
        lsr.b   #6,d0
        andi.b  #1,d0
        move.b  d0,S_NTS(a6)
        ori.l   #ALL_BITS,S_DIRTYENV(a6)
        rts
.not_8
        cmpi.b  #1,d0
        bne.s   .out
        move.b  d4,d0
        lsr.b   #5,d0
        andi.b  #1,d0
        cmp.b   S_WFENABLE(a6),d0
        beq.s   .same
        tst.b   S_WFGATE(a6)
        beq.s   .same
        ori.l   #ALL_BITS,S_DIRTYSLOT(a6)
.same
        move.b  d0,S_WFENABLE(a6)
.out
        rts

; a0 = the slot's registers, or return from the case when there is none.
slot_regs
        move.w  d5,d0
        bmi.s   .none
        mulu.w  #SL_SIZE,d0
        lea     S_SLOT(a6,d0.w),a0
        rts
.none
        addq.l  #4,sp                           ; out of the case
        rts

; d0.l = the slot's bit.
slot_bit
        moveq   #0,d0
        bset    d5,d0
        rts

w_20
        bsr     slot_regs
        move.b  d4,d0
        lsr.b   #7,d0
        move.b  d0,SL_TREMOLO(a0)
        move.b  d4,d0
        lsr.b   #6,d0
        andi.b  #1,d0
        move.b  d0,SL_VIBRATO(a0)
        move.b  d4,d0
        lsr.b   #5,d0
        andi.b  #1,d0
        move.b  d0,SL_TYPE(a0)
        move.b  d4,d0
        lsr.b   #4,d0
        andi.b  #1,d0
        move.b  d0,SL_KSR(a0)
        move.b  d4,d0
        andi.b  #$0f,d0
        move.b  d0,SL_MULT(a0)
        bsr     slot_bit
        or.l    d0,S_DIRTYINC(a6)
        or.l    d0,S_DIRTYENV(a6)
        or.l    d0,S_DIRTYSLOT(a6)
        rts

w_40
        bsr     slot_regs
        move.b  d4,d0
        lsr.b   #6,d0
        move.b  d0,SL_KSL(a0)
        move.b  d4,d0
        andi.b  #$3f,d0
        move.b  d0,SL_TL(a0)
        bsr     slot_bit
        or.l    d0,S_DIRTYENV(a6)
        rts

w_60
        bsr     slot_regs
        move.b  d4,d0
        lsr.b   #4,d0
        move.b  d0,SL_AR(a0)
        move.b  d4,d0
        andi.b  #$0f,d0
        move.b  d0,SL_DR(a0)
        bsr     slot_bit
        or.l    d0,S_DIRTYENV(a6)
        rts

w_80
        bsr     slot_regs
        move.b  d4,d0
        lsr.b   #4,d0
        move.b  d0,SL_SL(a0)
        move.b  d4,d0
        andi.b  #$0f,d0
        move.b  d0,SL_RR(a0)
        bsr     slot_bit
        or.l    d0,S_DIRTYENV(a6)
        or.l    d0,S_DIRTYSLOT(a6)
        rts

w_e0
        bsr     slot_regs
        move.b  d4,d0
        moveq   #3,d1
        tst.b   S_NEWM(a6)
        beq.s   .mask
        moveq   #7,d1
.mask
        and.b   d1,d0
        move.b  d0,SL_WF(a0)
        bsr     slot_bit
        or.l    d0,S_DIRTYSLOT(a6)
        rts

; a1 = channel c's registers, d6.w = c; return from the case unless
; (low & 0x0f) < 9.
chan_regs
        moveq   #$0f,d6
        and.w   d3,d6
        cmpi.w  #CHANNELS,d6
        bhs.s   .none
        move.w  d6,d0
        mulu.w  #CR_SIZE,d0
        lea     S_CHAN(a6,d0.w),a1
        rts
.none
        addq.l  #4,sp
        rts

; d0.l = channel d6's two operator bits.
chan_bits
        moveq   #3,d0
        move.w  d6,d1
        add.w   d1,d1
        lsl.l   d1,d0
        rts

w_a0
        bsr     chan_regs
        move.w  CR_FNUM(a1),d0
        andi.w  #$300,d0
        or.b    d4,d0
        move.w  d0,CR_FNUM(a1)
        bsr     chan_bits
        or.l    d0,S_DIRTYINC(a6)
        or.l    d0,S_DIRTYENV(a6)
        rts

w_b0
        cmpi.b  #$bd,d3
        bne.s   .channel
        ; 0xbd: the depths and the rhythm section
        move.b  d4,d0
        lsr.b   #7,d0
        eori.b  #1,d0
        andi.b  #1,d0
        add.b   d0,d0
        addq.b  #2,d0
        move.b  d0,S_TREMSHIFT(a6)
        move.b  d4,d0
        lsr.b   #6,d0
        andi.b  #1,d0
        eori.b  #1,d0
        move.b  d0,S_VIBSHIFT(a6)
        moveq   #0,d1
        move.b  S_TREMSHIFT(a6),d1
        move.w  #SC_TREMOLO_SHIFT,d0
        bsr     emit_scalar
        ori.l   #ALL_BITS,S_DIRTYINC(a6)
        move.b  d4,d0
        andi.b  #$3f,d0
        bra     set_rhythm
.channel
        bsr     chan_regs
        move.w  CR_FNUM(a1),d0
        andi.w  #$ff,d0
        move.b  d4,d1
        andi.w  #3,d1
        lsl.w   #8,d1
        or.w    d1,d0
        move.w  d0,CR_FNUM(a1)
        move.b  d4,d0
        lsr.b   #2,d0
        andi.b  #7,d0
        move.b  d0,CR_BLOCK(a1)
        move.b  d4,d2
        lsr.b   #5,d2
        andi.b  #1,d2                           ; on
        move.w  d6,d5
        add.w   d5,d5                           ; the modulator
        moveq   #1,d1                           ; kKeyChannel
        bsr     set_key
        addq.w  #1,d5                           ; the carrier
        bsr     set_key
        bsr     chan_bits
        or.l    d0,S_DIRTYINC(a6)
        or.l    d0,S_DIRTYENV(a6)
        rts

w_c0
        bsr     chan_regs
        move.b  d4,d0
        andi.b  #$0e,d0
        lsr.b   #1,d0
        move.b  d0,CR_FEEDBACK(a1)
        move.b  d4,d0
        andi.b  #1,d0
        move.b  d0,CR_CONNECTION(a1)
        ; updateChannel: connection, then the feedback multiplier
        moveq   #0,d1
        move.b  CR_CONNECTION(a1),d1
        moveq   #CH_CONN,d0
        bsr     emit_channel
        moveq   #0,d1
        move.b  CR_FEEDBACK(a1),d0
        beq.s   .fb
        addq.b  #7,d0
        bset    d0,d1
.fb
        moveq   #CH_FBMUL,d0
        bra     emit_channel                    ; newm is 0 on an OPL2: no routing

; setKey: d5.w = index, d1.b = source, d2.b = on.
set_key
        movem.l d0/d3,-(sp)
        move.b  S_KEY(a6,d5.w),d0
        move.b  d0,d3
        tst.b   d2
        beq.s   .off
        or.b    d1,d3
        bra.s   .keyed
.off
        move.b  d1,d3
        not.b   d3
        and.b   d0,d3
.keyed
        tst.b   d3
        beq.s   .no_edge
        tst.b   d0
        bne.s   .no_edge
        move.w  d5,d0
        lsl.w   #2,d0
        addq.l  #1,S_TRIGGER(a6,d0.w)
.no_edge
        move.b  d3,S_KEY(a6,d5.w)
        moveq   #0,d0
        bset    d5,d0
        or.l    d0,S_DIRTYSLOT(a6)
        movem.l (sp)+,d0/d3
        rts

; setRhythm: d0.b = register 0xbd's low six bits.
set_rhythm
        move.b  d0,S_RHYTHM(a6)
        move.b  d0,d7
        moveq   #2,d1                           ; kKeyDrum
        lea     .drums(pc),a0
        moveq   #5,d6
.drum
        move.b  (a0)+,d5
        ext.w   d5
        move.b  (a0)+,d0
        moveq   #0,d2
        btst    #5,d7
        beq.s   .key
        and.b   d7,d0
        sne     d2
.key
        bsr     set_key
        dbra    d6,.drum
        moveq   #0,d1
        btst    #5,d7
        beq.s   .scalar
        moveq   #1,d1
.scalar
        move.w  #SC_RHYTHM,d0
        bra     emit_scalar
; hi-hat, cymbal, tom-tom, snare, bass drum modulator and carrier
.drums
        dc.b    14,$01, 17,$02, 16,$04, 15,$08, 12,$10, 13,$10

; ---------------------------------------------------------------- flush

opl_dec_flush
        movem.l d0-d7/a0-a6,-(sp)
        lea     dec,a6
        bsr     flush
        movem.l (sp)+,d0-d7/a0-a6
        rts

; Every marked word, operator by operator in index order.
flush
        movem.l d0-d7/a0-a1,-(sp)
        moveq   #0,d5
.next
        move.l  S_DIRTYINC(a6),d0
        or.l    S_DIRTYENV(a6),d0
        or.l    S_DIRTYSLOT(a6),d0
        lsr.l   d5,d0
        beq.s   .done
        move.l  S_DIRTYINC(a6),d0
        btst    d5,d0
        beq.s   .env
        bsr     update_increment
.env
        move.l  S_DIRTYENV(a6),d0
        btst    d5,d0
        beq.s   .slot
        bsr     update_envelope
.slot
        move.l  S_DIRTYSLOT(a6),d0
        btst    d5,d0
        beq.s   .step
        bsr     update_slot
.step
        addq.w  #1,d5
        cmpi.w  #32,d5
        blo.s   .next
.done
        clr.l   S_DIRTYINC(a6)
        clr.l   S_DIRTYENV(a6)
        clr.l   S_DIRTYSLOT(a6)
        movem.l (sp)+,d0-d7/a0-a1
        rts

; a0 = slot d5's registers, a1 = its channel's.
regs_of
        move.w  d5,d0
        mulu.w  #SL_SIZE,d0
        lea     S_SLOT(a6,d0.w),a0
        move.w  d5,d0
        lsr.w   #1,d0
        mulu.w  #CR_SIZE,d0
        lea     S_CHAN(a6,d0.w),a1
        rts

; updateIncrement(d5)
update_increment
        movem.l d0-d7,-(sp)
        bsr     regs_of
        moveq   #0,d6
        move.w  CR_FNUM(a1),d6                  ; fnum
        move.l  d6,d0
        lsr.l   #7,d0
        andi.l  #7,d0                           ; range
        moveq   #0,d1
        move.b  S_VIBSHIFT(a6),d1
        move.l  d0,d2
        lsr.l   #1,d2
        lsr.l   d1,d2                           ; half
        move.l  d0,d3
        lsr.l   d1,d3                           ; full
        move.l  d6,d0
        bsr     increment
        move.l  d0,d7                           ; base
        move.l  d0,d1
        moveq   #OP_INCBASE,d0
        bsr     emit_op
        move.l  d6,d0
        add.l   d2,d0
        moveq   #OP_VIBDELTA+1,d4
        bsr     .delta
        move.l  d6,d0
        add.l   d3,d0
        moveq   #OP_VIBDELTA+2,d4
        bsr     .delta
        move.l  d6,d0
        sub.l   d2,d0
        moveq   #OP_VIBDELTA+3,d4
        bsr     .delta
        move.l  d6,d0
        sub.l   d3,d0
        moveq   #OP_VIBDELTA+4,d4
        bsr     .delta
        movem.l (sp)+,d0-d7
        rts
.delta
        bsr     increment
        sub.l   d7,d0
        bsr     wrap24
        move.l  d0,d1
        move.l  d4,d0
        bra     emit_op

; increment(fnum = d0.l, block, mult of a0/a1) -> d0.l
increment
        movem.l d1-d3,-(sp)
        moveq   #0,d1
        move.b  CR_BLOCK(a1),d1
        lsl.l   d1,d0
        lsr.l   #1,d0
        moveq   #0,d1
        move.b  SL_MULT(a0),d1
        move.b  freq_multiply(pc,d1.w),d1
        mulu.l  d1,d0                           ; 32-bit product, as uint32 wraps
        lsr.l   #1,d0
        andi.l  #$7ffff,d0                      ; native
        cmpi.l  #$40000,d0
        blo.s   .positive
        subi.l  #$80000,d0                      ; aliased, negative
        neg.l   d0                              ; |aliased|
        move.l  d0,d2
        mulu.l  #INC_LO,d2
        addi.l  #$ffff,d2
        clr.w   d2
        swap    d2                              ; ceil(a * lo / 65536)
        mulu.l  #INC_HI,d0
        add.l   d2,d0
        neg.l   d0                              ; floor of the negative product
        bra.s   .out
.positive
        move.l  d0,d2
        mulu.l  #INC_LO,d2
        clr.w   d2
        swap    d2
        mulu.l  #INC_HI,d0
        add.l   d2,d0
.out
        movem.l (sp)+,d1-d3
        rts
freq_multiply
        dc.b    1,2,4,6,8,10,12,14,16,18,20,20,24,24,30,30

; d0.l -> d0.l: the low 24 bits, sign-extended.
wrap24
        lsl.l   #8,d0
        asr.l   #8,d0
        rts

; updateEnvelope(d5)
update_envelope
        movem.l d0-d7,-(sp)
        bsr     regs_of
        moveq   #0,d6
        move.w  CR_FNUM(a1),d6
        moveq   #0,d7
        move.b  CR_BLOCK(a1),d7
        ; ksv = (block << 1) | ((fnum >> (9 - nts)) & 1)
        moveq   #9,d1
        sub.b   S_NTS(a6),d1
        move.l  d6,d2
        lsr.l   d1,d2
        andi.l  #1,d2
        move.l  d7,d3
        add.l   d3,d3
        or.l    d2,d3                           ; ksv
        ; ksl = (kKslRom[fnum >> 6] << 2) - ((8 - block) << 5), at least 0
        move.l  d6,d0
        lsr.l   #6,d0
        moveq   #0,d1
        move.b  ksl_rom(pc,d0.w),d1
        lsl.l   #2,d1
        moveq   #8,d0
        sub.l   d7,d0
        lsl.l   #5,d0
        sub.l   d0,d1
        bpl.s   .ksl
        moveq   #0,d1
.ksl
        moveq   #0,d0
        move.b  SL_KSL(a0),d0
        move.b  ksl_shift(pc,d0.w),d0
        asr.l   d0,d1
        moveq   #0,d0
        move.b  SL_TL(a0),d0
        lsl.l   #2,d0
        add.l   d0,d1
        moveq   #OP_TLKSL,d0
        bsr     emit_op
        ; ks = ksv >> ((ksr ^ 1) << 1)
        moveq   #0,d0
        move.b  SL_KSR(a0),d0
        eori.b  #1,d0
        add.b   d0,d0
        lsr.l   d0,d3                           ; ks
        move.b  SL_AR(a0),d1
        bsr     effective_rate
        moveq   #OP_RATE_A,d0
        bsr     emit_op
        move.b  SL_DR(a0),d1
        bsr     effective_rate
        moveq   #OP_RATE_D,d0
        bsr     emit_op
        moveq   #0,d1
        tst.b   SL_TYPE(a0)
        bne.s   .sustain
        move.b  SL_RR(a0),d1
        bsr     effective_rate
.sustain
        moveq   #OP_RATE_S,d0
        bsr     emit_op
        move.b  SL_RR(a0),d1
        bsr     effective_rate
        moveq   #OP_RATE_R,d0
        bsr     emit_op
        movem.l (sp)+,d0-d7
        rts
ksl_rom
        dc.b    0,32,40,45,48,51,53,55,56,58,59,60,61,62,63,64
ksl_shift
        dc.b    8,1,2,0

; effectiveRate(ks = d3, nibble = d1.b) -> d1.l
effective_rate
        andi.l  #$0f,d1
        beq.s   .out
        lsl.l   #2,d1
        add.l   d3,d1
        cmpi.l  #64,d1
        blo.s   .out
        andi.l  #3,d1
        addi.l  #60,d1
.out
        rts

; updateSlot(d5)
update_slot
        movem.l d0-d7,-(sp)
        bsr     regs_of
        moveq   #0,d1
        move.b  SL_SL(a0),d1
        cmpi.b  #15,d1
        bne.s   .sl
        moveq   #31,d1
.sl
        lsl.l   #4,d1
        moveq   #12,d0
        lsl.l   d0,d1
        moveq   #OP_SL,d0
        bsr     emit_op
        moveq   #0,d1
        tst.b   S_KEY(a6,d5.w)
        beq.s   .unkeyed
        moveq   #1,d1
.unkeyed
        tst.b   SL_TREMOLO(a0)
        beq.s   .no_trem
        addq.l  #2,d1
.no_trem
        tst.b   SL_VIBRATO(a0)
        beq.s   .no_vib
        addq.l  #4,d1
.no_vib
        moveq   #OP_FLAGS,d0
        bsr     emit_op
        moveq   #0,d1
        tst.b   S_WFGATE(a6)
        beq.s   .wf
        tst.b   S_WFENABLE(a6)
        beq.s   .base
.wf
        move.b  SL_WF(a0),d1
.base
        moveq   #10,d0
        lsl.l   d0,d1                           ; * 1024 (waveforms 0-3 on an OPL2)
        addi.l  #WAVE_BASE,d1
        moveq   #OP_WFBASE,d0
        bsr     emit_op
        move.w  d5,d0
        lsl.w   #2,d0
        move.l  S_TRIGGER(a6,d0.w),d1
        moveq   #OP_TRIG,d0
        bsr     emit_op
        movem.l (sp)+,d0-d7
        rts

; ---------------------------------------------------------------- emission

; emitOp(index d5, offset d0, value d1.l)
emit_op
        movem.l d0-d2/a0,-(sp)
        move.w  d5,d2
        lsl.w   #7,d2                           ; * OP_STRIDE * 4
        move.w  d0,a0
        add.w   d0,d0
        add.w   d0,d0
        add.w   d0,d2
        cmp.l   S_SHOP(a6,d2.w),d1
        beq.s   .out
        move.l  d1,S_SHOP(a6,d2.w)
        move.w  d5,d0
        lsl.w   #5,d0
        add.w   a0,d0
        addi.w  #OP_BASE,d0
        bsr     emit_raw
.out
        movem.l (sp)+,d0-d2/a0
        rts

; emitChannel(index d6, offset d0, value d1.l)
emit_channel
        movem.l d0-d2/a0,-(sp)
        move.w  d6,d2
        lsl.w   #5,d2
        move.w  d0,a0
        add.w   d0,d0
        add.w   d0,d0
        add.w   d0,d2
        cmp.l   S_SHCH(a6,d2.w),d1
        beq.s   .out
        move.l  d1,S_SHCH(a6,d2.w)
        move.w  d6,d0
        lsl.w   #3,d0
        add.w   a0,d0
        addi.w  #CH_BASE,d0
        bsr     emit_raw
.out
        movem.l (sp)+,d0-d2/a0
        rts

; emitScalar(address d0.w, value d1.l)
emit_scalar
        movem.l d0-d2,-(sp)
        cmpi.w  #SC_RHYTHM,d0
        bne.s   .table
        cmp.l   S_SHRHY(a6),d1
        beq.s   .out
        move.l  d1,S_SHRHY(a6)
        bra.s   .send
.table
        move.w  d0,d2
        subi.w  #SC_TREMOLO_SHIFT,d2
        add.w   d2,d2
        add.w   d2,d2
        cmp.l   S_SHSC(a6,d2.w),d1
        beq.s   .out
        move.l  d1,S_SHSC(a6,d2.w)
.send
        bsr     emit_raw
.out
        movem.l (sp)+,d0-d2
        rts

; One event at the current frame: address d0.w, value d1.l.
emit_raw
        tst.w   S_FRESH(a6)
        beq.s   .take
        ; the fresh-machine filter passes the channels' routing only
        move.w  d0,d2
        subi.w  #CH_BASE,d2
        bmi.s   .out
        cmpi.w  #CHANNELS*CH_STRIDE,d2
        bhs.s   .out
        andi.w  #CH_STRIDE-1,d2
        cmpi.w  #CH_ROUTE,d2
        bne.s   .out
.take
        movem.l d2/a0,-(sp)
        move.l  opl_dec_count,d2
        cmpi.l  #MAX_EVENTS,d2
        bhs.s   .full
        mulu.w  #EV_SIZE,d2
        lea     opl_dec_events,a0
        adda.l  d2,a0
        move.l  S_BLOCK(a6),(a0)+
        move.w  d0,(a0)+
        clr.w   (a0)+
        move.l  d1,(a0)
        addq.l  #1,opl_dec_count
        bra.s   .done
.full
        move.w  #1,opl_dec_overflow
.done
        movem.l (sp)+,d2/a0
.out
        rts

; Pipeline::takePeriod's selection: d0.l = the period's start frame, d1.l =
; its end, a0 = the wire buffer (two longs an event: frame - start << 12 |
; address, value & 0xffffff), d2.l = its capacity. Flushes first. Out: d0.l =
; events written; events of later frames stay, in order.
opl_dec_take
        movem.l d1-d7/a1-a6,-(sp)
        lea     dec,a6
        bsr     flush
        move.l  d0,d6                           ; start
        move.l  d1,d7                           ; end
        lea     opl_dec_events,a1
        move.l  a1,a2                           ; kept
        move.l  opl_dec_count,d3
        moveq   #0,d4                           ; taken
        moveq   #0,d5                           ; kept count
        bra.s   .next
.event
        move.l  (a1),d0
        sub.l   d7,d0
        bpl.s   .keep                           ; not due
        cmp.l   d2,d4
        blo.s   .send
        move.w  #1,opl_dec_overflow             ; due, but the table is full
        bra.s   .keep
.send
        move.l  (a1),d0
        sub.l   d6,d0
        moveq   #12,d1
        lsl.l   d1,d0
        or.w    4(a1),d0
        move.l  d0,(a0)+
        move.l  8(a1),d0
        andi.l  #$ffffff,d0
        move.l  d0,(a0)+
        addq.l  #1,d4
        lea     EV_SIZE(a1),a1
        bra.s   .next
.keep
        move.l  (a1)+,(a2)+
        move.l  (a1)+,(a2)+
        move.l  (a1)+,(a2)+
        addq.l  #1,d5
.next
        subq.l  #1,d3
        bpl.s   .event
        move.l  d5,opl_dec_count
        move.l  d4,d0
        movem.l (sp)+,d1-d7/a1-a6
        rts

; Register offset 0x00..0x1f to slot, -1 where nothing is decoded
; (OplKernel::kAddressSlot), and slot to record index (Decoder::slotIndex).
address_slot
        dc.b    0,1,2,3,4,5,-1,-1,6,7,8,9,10,11,-1,-1
        dc.b    12,13,14,15,16,17,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
slot_index
        dc.b    0,2,4,1,3,5,6,8,10,7,9,11,12,14,16,13,15,17
        even

        bss
        even
dec
        ds.b    S_SIZE
opl_dec_count
        ds.l    1
opl_dec_overflow
        ds.w    1
        even
opl_dec_events
        ds.b    MAX_EVENTS*EV_SIZE

        end
