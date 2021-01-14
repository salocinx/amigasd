*
* disk-int.asm. Interrupt sub routine to handle disk change state.
*

_AbsExecBase EQU 4							; System pointer to Exec's library base.

    INCLUDE "exec/types.i"
    INCLUDE "hardware/custom.i"
    INCLUDE "hardware/intbits.i"

		XREF	_LVOCause					; Offset from Exec base for Cause()'ing software interrupts.
		XDEF    _DiskInt		    		; Function definition which is called by hardware interrupt.


* Entered with:       A0 == scratch
*  D0 == scratch      A1 == is_Data
*  D1 == scratch      A5 == vector to interrupt code (scratch)
*                     A6 == scratch
*
    section code

_DiskInt:
											; Passed pointer to sw_int is already in a1.
		move.l  _AbsExecBase,a6 			; Move exec.library base to a6.
		jsr     _LVOCause(a6)				; Call Exec's software interrupt Cause(a1) function.
        moveq.l #0,d0             			; Set Z flag to continue to process other disk change interrupts.
        rts                       			; Return to exec.
        END