# Any_Ps3 — PS3-style module fixture: OPD entry descriptor, a real NID import
# from sysPrxForUser (_sys_strlen, NID 0x2d36462b), one unknown-NID import
# (0xdeadbeef), and the PowerOpen call-stub pattern (verified against the
# PSL1GHT stub sequence) whose slot the module linker patches with an HLE
# function descriptor {addr, rtoc}.
#
# Data layout (assembled at 0x11000, metadata hand-written):
#   0x11000 "sysPrxForUser\0"
#   0x11010 "testmod\0"
#   0x11020 sceModuleInfo (0x34 bytes; libstub window 0x11060..0x1108C)
#   0x11060 libstub entry  (0x2C bytes; nfunc=2)
#   0x11090 NID table      (0x2d36462b _sys_strlen, 0xdeadbeef unknown)
#   0x110A0 slot array     (u32 slots — patched by the linker)
#   0x110B0 entry table    (u32 slot ADDRESSES — the libstub contract)
#   0x110C0 entry OPD     {u32 main, u32 rtoc=0x11000}
#
# main() calls _sys_strlen("hi") -> expects 2, then the unknown import ->
# expects CELL_ENOSYS (0x80010002), then exits 42 via _sys_process_exit.
.machine "cell"
.text
.globl main
main:
        lis %r3, str_hi@ha
        addi %r3, %r3, str_hi@l
        bl stub_strlen
        cmpwi %r3, 2
        bne fail1
        bl stub_missing
        lis %r4, 0x8001
        ori %r4, %r4, 0x0002        # CELL_ENOSYS
        cmpw %r3, %r4
        bne fail2
        li %r3, 42
        b done
fail1:
        li %r3, 0x11                # distinct failure codes
        b done
fail2:
        li %r3, 0x22
        b done
done:
        li %r11, 22                 # _sys_process_exit(status)
        sc
        b .

# PowerOpen call stub (PS3 static-linking contract; the slot holds a pointer
# to an 8-byte descriptor {addr, rtoc}, patched by the module manager).
stub_strlen:
        mflr %r0
        std %r0, 16(%r1)
        std %r2, 40(%r1)
        stdu %r1, -128(%r1)
        lis %r12, 0x110A0@ha
        lwz %r12, 0x110A0@l(%r12)   # r12 = slot -> descriptor ptr
        lwz %r0, 0(%r12)            # r0 = function address
        lwz %r2, 4(%r12)            # r2 = callee rtoc
        mtctr %r0
        bctrl
        addi %r1, %r1, 128
        ld %r2, 40(%r1)
        ld %r0, 16(%r1)
        mtlr %r0
        blr

stub_missing:
        mflr %r0
        std %r0, 16(%r1)
        std %r2, 40(%r1)
        stdu %r1, -128(%r1)
        lis %r12, 0x110A0@ha
        lwz %r12, 0x110A4@l(%r12)   # second slot
        lwz %r0, 0(%r12)
        lwz %r2, 4(%r12)
        mtctr %r0
        bctrl
        addi %r1, %r1, 128
        ld %r2, 40(%r1)
        ld %r0, 16(%r1)
        mtlr %r0
        blr

str_hi:
        .ascii "hi\0"

.section .data
# NOTE: .org is relative to the SECTION start; ld places .data at 0x11000,
# so .org 0x20 places the module_info at vaddr 0x11020 etc. Pointers below
# use the absolute (final) addresses.
.org 0
mod_libname:
        .ascii "sysPrxForUser\0"
        .byte 0
.org 0x10
mod_name:
        .ascii "testmod\0"
        .byte 0
.org 0x20
mod_info:
        .short 0                    # modattr
        .byte 1, 0                  # modver
        .ascii "testmod\0"
        .rept 19
        .byte 0
        .endr                       # pad modname field to 27 bytes
        .byte 0                     # infover
        .long 0                     # gp
        .long 0                     # libent_top
        .long 0                     # libent_btm
        .long 0x11060               # libstub_top
        .long 0x1108C               # libstub_btm
.org 0x60
mod_libstub:
        .short 0x2C                 # size
        .short 1                    # version
        .short 0                    # attribute
        .short 2                    # nfunc
        .short 0                    # nvar
        .short 0                    # ntls
        .long 0                     # reserved
        .long 0x11000               # libname
        .long 0x11090               # func_nid_table
        .long 0x110B0               # func_entry_table (array of slot ADDRESSES)
        .long 0                     # var_nid_table
        .long 0                     # var_entry_table
        .long 0                     # tls_nid_table
        .long 0                     # tls_entry_table
.org 0x90
mod_nids:
        .long 0x2d36462b            # _sys_strlen (sysPrxForUser, verified NID)
        .long 0xdeadbeef            # unknown import (honest ENOSYS expected)
.org 0xA0
mod_slots:
        .long 0                     # slot[0] — patched with descriptor address
        .long 0                     # slot[1]
.org 0xB0
mod_fent:
        .long 0x110A0               # entry[0] = &slot[0]
        .long 0x110A4               # entry[1] = &slot[1]
.org 0xC0
mod_entry_opd:
        .long 0x10000               # main
        .long 0x11000               # rtoc (TOC base for the test module)
