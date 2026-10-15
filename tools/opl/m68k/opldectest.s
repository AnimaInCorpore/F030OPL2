; Test program for opldec.s: the same register stream through the 68k
; decoder, the periods taken as opldec-ref.cpp takes them.
;
; Reads REGS.BIN ('OPLR', count, records of frame.l reg.w value.w), writes
; EVENTS.BIN (per period: count.l and two words an event; then the overflow
; flag) and DONE.TXT. opldec-gate.py compares EVENTS.BIN with the C++
; reference's.

        global  _main
        xref    opl_dec_begin, opl_dec_write, opl_dec_take, opl_dec_overflow

PERIOD_FRAMES   equ     24*32
MAX_WIRE        equ     2048

        text
_main
        move.l  4(sp),a0
        move.l  #256,d0
        add.l   12(a0),d0
        add.l   20(a0),d0
        add.l   28(a0),d0
        move.l  d0,-(sp)
        move.l  a0,-(sp)
        clr.w   -(sp)
        move.w  #$4a,-(sp)              ; Mshrink
        trap    #1
        lea     12(sp),sp

        clr.w   -(sp)
        pea     name_in
        move.w  #$3d,-(sp)              ; Fopen
        trap    #1
        addq.l  #8,sp
        tst.l   d0
        bmi     .quit
        move.w  d0,d7
        pea     regs
        move.l  #REGS_BYTES,-(sp)
        move.w  d7,-(sp)
        move.w  #$3f,-(sp)              ; Fread
        trap    #1
        lea     12(sp),sp
        move.w  d7,-(sp)
        move.w  #$3e,-(sp)
        trap    #1
        addq.l  #4,sp

        lea     regs,a5
        cmpi.l  #$4f504c52,(a5)+
        bne     .quit
        move.l  (a5)+,d6                ; records
        lea     out,a4
        moveq   #0,d5                   ; period
        moveq   #0,d0
        bsr     opl_dec_begin
        bra.s   .next
.record
        move.l  (a5),d4                 ; frame
.periods
        move.l  d5,d0
        addq.l  #1,d0
        mulu.l  #PERIOD_FRAMES,d0
        cmp.l   d0,d4
        blo.s   .write
        bsr     take
        bra.s   .periods
.write
        move.l  (a5)+,d0
        move.w  (a5)+,d1
        move.w  (a5)+,d2
        bsr     opl_dec_write
.next
        subq.l  #1,d6
        bpl.s   .record
        bsr     take
        moveq   #0,d0
        tst.w   opl_dec_overflow
        beq.s   .flag
        moveq   #-1,d0
.flag
        move.l  d0,(a4)+

        clr.w   -(sp)
        pea     name_out
        move.w  #$3c,-(sp)              ; Fcreate
        trap    #1
        addq.l  #8,sp
        move.w  d0,d7
        move.l  a4,d0
        sub.l   #out,d0
        pea     out
        move.l  d0,-(sp)
        move.w  d7,-(sp)
        move.w  #$40,-(sp)              ; Fwrite
        trap    #1
        lea     12(sp),sp
        move.w  d7,-(sp)
        move.w  #$3e,-(sp)
        trap    #1
        addq.l  #4,sp
        clr.w   -(sp)
        pea     name_done
        move.w  #$3c,-(sp)
        trap    #1
        addq.l  #8,sp
        move.w  d0,-(sp)
        move.w  #$3e,-(sp)
        trap    #1
        addq.l  #4,sp
.quit
        clr.w   -(sp)
        trap    #1

; The period d5: its events to the output after their count.
take
        move.l  d5,d0
        mulu.l  #PERIOD_FRAMES,d0
        move.l  d0,d1
        addi.l  #PERIOD_FRAMES,d1
        lea     4(a4),a0
        move.l  #MAX_WIRE,d2
        bsr     opl_dec_take
        move.l  d0,(a4)
        lsl.l   #3,d0
        lea     4(a4,d0.l),a4
        addq.l  #1,d5
        rts

        data
name_in         dc.b    "REGS.BIN",0
name_out        dc.b    "EVENTS.BIN",0
name_done       dc.b    "DONE.TXT",0
        even

        bss
        even
REGS_BYTES      equ     1024*1024
regs            ds.b    REGS_BYTES
out             ds.b    6*1024*1024

        end
