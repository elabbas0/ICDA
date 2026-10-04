bits 64

section .rodata
global font_asset_ui2x_start
global font_asset_ui2x_end
align 16
font_asset_ui2x_start:
    incbin "resources/fonts/ui-2x.icf"
font_asset_ui2x_end:

section .note.GNU-stack noalloc noexec nowrite progbits
