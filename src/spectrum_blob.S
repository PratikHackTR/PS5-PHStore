.section .rodata.spectrum_helper,"a",@progbits
.balign 16
.global spectrum_helper_elf
.global spectrum_helper_elf_end
spectrum_helper_elf:
.incbin "third_party/spectrum/exfat-helper.elf"
spectrum_helper_elf_end:
.section .note.GNU-stack,"",@progbits
