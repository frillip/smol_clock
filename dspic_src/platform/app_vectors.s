; The application's side of the boot layout (common/boot_layout.h), with
; app.gld, which builds the entry GOTO and the forwarding table.
;
; The hardware IVT belongs to the IPL. Since XC16 1.26 the linker builds an
; IVT itself unless an input section named .ivt exists, as MCC's bootloader
; relies on: this one replaces it, and is noload, so the application's hex
; holds nothing at 0x0004-0x01FF. The application's version-record page is
; reserved the same way, so nothing is linked into it. Checked by
; tools/make_image.py, which refuses an image with anything below 0x002000.

        .section .ivt, code, address(0x4), noload, keep
        .space  0x1FC

        .section .app_record, code, address(0x29000), noload, keep
        .space  0x800

        .end
