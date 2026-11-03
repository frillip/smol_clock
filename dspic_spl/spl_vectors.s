; The SPL's side of the boot layout (common/boot_layout.h), with spl.gld,
; which places the entry GOTO at 0x000800.
;
; The hardware IVT belongs to the IPL. Since XC16 1.26 the linker builds an
; IVT itself unless an input section named .ivt exists: this one replaces
; it, and is noload, so the SPL's hex holds nothing at 0x0004-0x01FF. The SPL
; never enables an interrupt. tools/make_image.py refuses an SPL hex with
; anything outside 0x000800-0x001F7F.

        .section .ivt, code, address(0x4), noload, keep
        .space  0x1FC

        .end
