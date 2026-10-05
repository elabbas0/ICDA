bits 64

section .rodata
global font_asset_ui2x_start
global font_asset_ui2x_end
global font_asset_ttf_sans_start
global font_asset_ttf_sans_end
global font_asset_ttf_bold_start
global font_asset_ttf_bold_end
global font_asset_ttf_mono_start
global font_asset_ttf_mono_end
align 16
font_asset_ui2x_start:
    incbin "resources/fonts/ui-2x.icf"
font_asset_ui2x_end:

; TrueType faces for Surfer (OFL, see resources/fonts/*-OFL.txt)
align 16
font_asset_ttf_sans_start:
    incbin "resources/fonts/Inter-Regular.ttf"
font_asset_ttf_sans_end:
align 16
font_asset_ttf_bold_start:
    incbin "resources/fonts/Inter-SemiBold.ttf"
font_asset_ttf_bold_end:
align 16
font_asset_ttf_mono_start:
    incbin "resources/fonts/JetBrainsMono-Regular.ttf"
font_asset_ttf_mono_end:

section .note.GNU-stack noalloc noexec nowrite progbits
