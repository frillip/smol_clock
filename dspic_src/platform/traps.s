; The trap handlers (platform/fault.h). app.gld puts each in the
; application's forwarding table; the IPL's trap vectors go there while the
; application runs (MAILBOX_RUNNING_VALUE).
;
; A trap stacks the return address as two words: PC<15:1> with SFA in
; bit 0, then SR<7:0>, IPL3 and PC<22:16>. Each handler passes its trap number and both
; words to fault_trap(), which records them and resets. The stack is set
; back to its start first, as a stack error leaves it past its limit; the
; return address is read before that.

        .text

        .global __OscillatorFail
__OscillatorFail:
        mov     #1, w0
        bra     trap_common

        .global __AddressError
__AddressError:
        mov     #2, w0
        bra     trap_common

        .global __StackError
__StackError:
        mov     #4, w0
        bra     trap_common

        .global __MathError
__MathError:
        mov     #5, w0
        bra     trap_common

        .global __DMACError
__DMACError:
        mov     #6, w0
        bra     trap_common

        .global __HardTrapError
__HardTrapError:
        mov     #3, w0
        bra     trap_common

        .global __SoftTrapError
__SoftTrapError:
        mov     #7, w0

trap_common:
        mov     [w15-4], w1             ; PC<15:0>
        mov     [w15-2], w2             ; SR<7:0>, IPL3, PC<22:16>
        mov     #__SP_init, w15
        mov     #__SPLIM_init, w3
        mov     w3, SPLIM
        nop                             ; no stack use straight after SPLIM is written
        call    _fault_trap             ; does not return
        reset

        .end
