bits 64

; Intel Wireless 8260 firmware for the iwm(4) port (kernel/drivers/net/iwm).
; Redistributable; see resources/firmware/LICENCE.iwlwifi_firmware.
section .rodata
global iwm_fw_8000c_start
global iwm_fw_8000c_end
align 16
iwm_fw_8000c_start:
    incbin "resources/firmware/iwlwifi-8000C-36.ucode"
iwm_fw_8000c_end:

section .note.GNU-stack noalloc noexec nowrite progbits
