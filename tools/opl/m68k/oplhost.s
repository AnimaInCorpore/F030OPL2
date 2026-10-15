; A host for the practical OPL DSP kernel inside another program: an OPL2
; register write interface, the stream's period refills served from a timer
; interrupt, and host PCM mixed in by the kernel.
;
; For programs that make OPL2 register writes themselves (a game's music
; driver) and cannot block on the DSP: opl_host_write decodes each write at
; once (opldec.s), and opl_host_service, called from a periodic interrupt,
; walks the refill protocol of dsp/oplrt.asm one step at a time - request,
; READY, payload, acknowledgement - without ever waiting for the DSP. The
; payload is the decoder's events of the next period and, through
; opl_host_pcm_hook, 192 host samples at a quarter of the codec rate
; (12,292.48 Hz), which the kernel expands and adds to the FM mix.
;
; Timing. Writes are stamped in the period the next refill will carry: at
; its start plus the time since the last refill, counted by opl_host_clock
; (call it at a fixed rate; OPL_HOST_CLOCK_FRAMES frames per call), at most
; the period's last frame. A write never lands in a period already sent.
;
; Everything here runs in supervisor mode (the host port).
;
; Entry points (all preserve every register except d0 where stated):
;   opl_host_boot      a0 = the upload image (OPLDATA.BIN as
;                      tools/opl/opl-upload-image writes it). Boots the
;                      kernel and uploads. Out: d0 = 0, or -1.
;   opl_host_start     routes the DSP to the DAC at 49.17 kHz and starts the
;                      stream. The caller holds the sound system (Locksnd).
;   opl_host_stop      stops the stream and releases the DSP.
;   opl_host_write     d0.b = register, d1.b = value (OPL2 bank 0).
;   opl_host_service   one step of the refill protocol; call it often (at
;                      least once a period, 15.6 ms).
;   opl_host_clock     advance the write clock by OPL_HOST_CLOCK_FRAMES.

        include "xbios.i"

        global  opl_host_boot, opl_host_start, opl_host_stop
        global  opl_host_write, opl_host_service, opl_host_clock
        global  opl_host_pcm_hook, opl_host_running, opl_host_status
        global  opl_host_periods, opl_host_errors

        xref    opl_dec_begin, opl_dec_write, opl_dec_take

CMD_PING        equ     $010000
CMD_WRITE_X     equ     $020000
CMD_WRITE_Y     equ     $030000
CMD_STREAM_START equ    $090000
CMD_REFILL      equ     $0A0000
CMD_STREAM_STOP equ     $0B0000
REPLY_PING      equ     $4F5052
REPLY_READY     equ     $524459
DATA_MAGIC      equ     $4F504C52
DSP_ABILITY     equ     3
DSP_HOST_ISR    equ     $ffffa202
DSP_HOST_DATA   equ     $ffffa204

PERIOD_FRAMES   equ     768
PCM_SAMPLES     equ     192
MAX_EVENTS      equ     2048
; 200 Hz: the Timer C a TOS program has, 49,169.92 / 200 frames.
OPL_HOST_CLOCK_FRAMES equ 246

; opl_host_service's states
ST_IDLE         equ     0                       ; nothing asked: send a REFILL
ST_ASKED        equ     1                       ; waiting for READY
ST_SENT         equ     2                       ; payload sent, waiting for the ack

        text

; ---------------------------------------------------------------- boot
opl_host_boot
        movem.l d1-d7/a0-a6,-(sp)
        move.l  a0,a5
        clr.w   opl_host_running
        move.w  #104,-(sp)                      ; Dsp_Lock
        trap    #14
        addq.l  #2,sp
        tst.w   d0
        bne     .fail
        Dsp_Reserve #16,#16
        tst.l   d0
        bmi     .unlock
        Dsp_ExecBoot dsp_bootstrap_image,#DSP_BOOT_WORDS,#DSP_ABILITY
        clr.l   stage2_reply
        Dsp_BlkUnpacked dsp_program_image,#DSP_STAGE2_TRANSFER_WORDS,stage2_reply,#1
        cmpi.l  #DSP_STAGE2_REPLY_OK,stage2_reply
        bne     .unlock
        move.l  #CMD_PING,d0
        bsr     exchange
        cmpi.l  #REPLY_PING,d0
        bne     .unlock
        ; the upload: 'OPLR', blocks, then per block space, address, count, words
        cmpi.l  #DATA_MAGIC,(a5)+
        bne     .unlock
        move.l  (a5)+,d7
        bra.s   .block_next
.block
        move.l  #CMD_WRITE_X,d0
        tst.l   (a5)+
        beq.s   .space
        move.l  #CMD_WRITE_Y,d0
.space
        bsr     exchange
        move.l  (a5)+,d0
        bsr     exchange
        move.l  (a5)+,d6
        move.l  d6,d0
        bsr     exchange
        bra.s   .word_next
.word
        move.l  (a5)+,d0
        bsr     exchange
.word_next
        subq.l  #1,d6
        bpl.s   .word
.block_next
        subq.l  #1,d7
        bpl.s   .block
        tst.w   port_failed
        bne     .unlock
        moveq   #0,d0
        bsr     opl_dec_begin
        clr.l   next_start
        clr.l   clock_frames
        clr.w   state
        move.w  #1,dsp_held
        moveq   #0,d0
        bra.s   .out
.unlock
        move.w  #105,-(sp)                      ; Dsp_Unlock
        trap    #14
        addq.l  #2,sp
.fail
        moveq   #-1,d0
.out
        movem.l (sp)+,d1-d7/a0-a6
        rts

; ---------------------------------------------------------------- audio
opl_host_start
        movem.l d0-d2/a0-a2,-(sp)
        tst.w   dsp_held
        beq     .out
        Buffoper #0
        Sndstatus #1
        Soundcmd #4,#2                          ; adder input: the matrix
        Setmode #1                              ; 16-bit stereo
        Settracks #0,#0
        Setmontracks #0
        Dsptristate #1,#0
        Devconnect #1,#8,#0,#1,#1               ; DSP transmit -> DAC, 49.17 kHz, no handshake
        move.l  #CMD_STREAM_START,d0
        bsr     exchange
        clr.w   state
        move.w  #1,opl_host_running
.out
        movem.l (sp)+,d0-d2/a0-a2
        rts

opl_host_stop
        movem.l d0-d2/a0-a2,-(sp)
        move.w  sr,-(sp)
        ori.w   #$0700,sr
        tst.w   opl_host_running
        beq.s   .released
        clr.w   opl_host_running
        ; finish a refill in flight, then stop
        cmpi.w  #ST_ASKED,state
        bne.s   .not_asked
        bsr     receive                         ; READY
        bsr     payload
.not_asked
        cmpi.w  #ST_IDLE,state
        beq.s   .stop
        bsr     receive                         ; the acknowledgement
.stop
        move.l  #CMD_STREAM_STOP,d0
        bsr     exchange
.released
        move.w  (sp)+,sr
        tst.w   dsp_held
        beq.s   .out
        clr.w   dsp_held
        move.w  #105,-(sp)                      ; Dsp_Unlock
        trap    #14
        addq.l  #2,sp
.out
        movem.l (sp)+,d0-d2/a0-a2
        rts

; ---------------------------------------------------------------- writes
opl_host_write
        movem.l d0-d2,-(sp)
        move.w  sr,-(sp)
        ori.w   #$0700,sr
        moveq   #0,d2
        move.b  d1,d2                           ; value
        moveq   #0,d1
        move.b  d0,d1                           ; register
        move.l  clock_frames,d0
        cmpi.l  #PERIOD_FRAMES-1,d0
        bls.s   .in
        move.l  #PERIOD_FRAMES-1,d0
.in
        add.l   next_start,d0
        bsr     opl_dec_write
        move.w  (sp)+,sr
        movem.l (sp)+,d0-d2
        rts

opl_host_clock
        addi.l  #OPL_HOST_CLOCK_FRAMES,clock_frames
        rts

; ---------------------------------------------------------------- refills
opl_host_service
        tst.w   opl_host_running
        beq     .ret
        movem.l d0-d3/a0-a1,-(sp)
        move.w  sr,-(sp)
        ori.w   #$0700,sr
        move.w  state,d0
        cmpi.w  #ST_ASKED,d0
        beq.s   .asked
        cmpi.w  #ST_SENT,d0
        beq.s   .sent
        ; idle: ask for the next refill
        btst    #1,DSP_HOST_ISR
        beq.s   .done
        move.l  #CMD_REFILL,DSP_HOST_DATA
        move.w  #ST_ASKED,state
        bra.s   .done
.asked
        btst    #0,DSP_HOST_ISR
        beq.s   .done
        bsr     receive
        cmpi.l  #REPLY_READY,d0
        beq.s   .ready
        addq.l  #1,opl_host_errors
        clr.w   state
        bra.s   .done
.ready
        bsr     payload
        move.w  #ST_SENT,state
        bra.s   .done
.sent
        btst    #0,DSP_HOST_ISR
        beq.s   .done
        bsr     receive
        move.l  d0,opl_host_status
        clr.w   state
.done
        move.w  (sp)+,sr
        movem.l (sp)+,d0-d3/a0-a1
.ret
        rts

; The payload of the period at next_start: its events, then the PCM.
payload
        movem.l d0-d3/a0-a1,-(sp)
        move.l  next_start,d0
        move.l  d0,d1
        addi.l  #PERIOD_FRAMES,d1
        lea     wire,a0
        move.l  #MAX_EVENTS,d2
        bsr     opl_dec_take
        move.l  d0,d3
        bsr     send                            ; the count
        lea     wire,a1
        add.l   d3,d3
        bra.s   .event_next
.event
        move.l  (a1)+,d0
        bsr     send
.event_next
        subq.l  #1,d3
        bpl.s   .event
        ; the PCM: the hook fills 192 signed bytes; d0 = 0 means none
        moveq   #0,d0
        move.l  opl_host_pcm_hook,d1
        beq.s   .flag
        lea     pcm,a0
        move.l  d1,a1
        jsr     (a1)
.flag
        bsr     send
        tst.l   d0
        beq.s   .advance
        lea     pcm,a1
        move.w  #PCM_SAMPLES-1,d3
.sample
        move.b  (a1)+,d0
        ext.w   d0
        ext.l   d0
        moveq   #14,d1
        lsl.l   d1,d0                           ; +-0x200000: half the kernel's PCM range
        andi.l  #$ffffff,d0
        bsr     send
        dbra    d3,.sample
.advance
        addi.l  #PERIOD_FRAMES,next_start
        clr.l   clock_frames
        addq.l  #1,opl_host_periods
        movem.l (sp)+,d0-d3/a0-a1
        rts

; ---------------------------------------------------------------- the port

; d0.l -> the DSP, then its answer in d0.l.
exchange
        bsr.s   send
receive
        move.l  d1,-(sp)
        move.l  #4000000,d1
.wait
        btst    #0,DSP_HOST_ISR
        bne.s   .take
        subq.l  #1,d1
        bne.s   .wait
        move.w  #1,port_failed
        moveq   #0,d0
        bra.s   .out
.take
        ; the low data byte last: reading it clears RXDF
        moveq   #0,d0
        move.b  DSP_HOST_DATA+1,d0
        lsl.l   #8,d0
        move.b  DSP_HOST_DATA+2,d0
        lsl.l   #8,d0
        move.b  DSP_HOST_DATA+3,d0
.out
        move.l  (sp)+,d1
        rts

send
        move.l  d1,-(sp)
        move.l  #4000000,d1
.wait
        btst    #1,DSP_HOST_ISR
        bne.s   .put
        subq.l  #1,d1
        bne.s   .wait
        move.w  #1,port_failed
        bra.s   .out
.put
        move.l  d0,DSP_HOST_DATA
.out
        move.l  (sp)+,d1
        rts

        data
        even
opl_host_pcm_hook
        dc.l    0

        include "oplrt_image.i"

        bss
        even
opl_host_running ds.w   1
opl_host_status ds.l    1
opl_host_periods ds.l   1
opl_host_errors ds.l    1
dsp_held        ds.w    1
port_failed     ds.w    1
state           ds.w    1
stage2_reply    ds.l    1
next_start      ds.l    1
clock_frames    ds.l    1
wire            ds.l    2*MAX_EVENTS
pcm             ds.b    PCM_SAMPLES

        end
