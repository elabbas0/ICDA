# Moved code comments

This file preserves comments that were removed from the source tree by the comment-stripping task.

## /Makefile

# (userspace/*.wav gitignored, e.g. ilove.wav) stays out: use Releases/LFS.

## /kernel/boot.asm

; multiboot2 header
    ; request the memory map tag from the bootloader
    ; framebuffer request tag
    ; width=0/height=0 asks the bootloader to keep its current mode: a
    ; fixed 1024x768 request makes GRUB switch the GOP to a mode some
    ; monitors/GPUs reject over HDMI (e.g. an RTX 3060 Ti), leaving the
    ; screen black.  GRUB (gfxmode=auto + gfxpayload=keep) hands us the
    ; mode it already established, which the display demonstrably shows.
    ; end tag
; bss - page tables, multiboot pointer, stack
; map 0-64GB using one PML4 entry -> one PDP -> 64 page directories
; each page directory covers 1GB using 512 x 2MB huge pages
; 64 page directories = 64GB total coverage
; stack must be last
; gdt for 64-bit long mode
; macro: fill one page directory with 512 x 2MB pages
; %1 = page directory address
; %2 = base physical address in GB (e.g. 0 = 0GB, 1 = 1GB)
; entry point - 32-bit protected mode
    ; check long mode support
    ; wire PML4[0] -> PDP
    ; wire PDP[0..3] -> first 4 page directories (0-4GB, done in 32-bit)
    ; map first 4GB (32-bit can handle these addresses)
    ; enable PAE
    ; point cr3 to pml4
    ; enable long mode via EFER
    ; enable paging. WP (bit 16) is set alongside PG so supervisor
    ; writes honor read-only PTEs too: with WP clear, the kernel could
    ; silently write user pages mapped read-only (e.g. mprotect'd
    ; regions), defeating W^X. All copy_to_user paths probe writability
    ; first (uaccess.h), so legitimate flows never trip this.
    ; load GDT and far jump to 64-bit
; 64-bit long mode - now we can use full 64-bit addresses
    ; map 4GB-64GB using 64-bit addresses
    ; each PDP entry covers 1GB, we fill entries 4-63
    ; pd_table_0 is contiguous in memory so we can walk through them
    ; pd_table_N = pd_table_0 + N * 4096
    ; calculate pd_table address: pd_table_0 + rcx * 4096
    ; fill this page directory: 512 x 2MB pages starting at rcx * 1GB
    ; reload cr3 to flush TLB with new mappings
    ; pass multiboot info pointer as first argument (rdi)
; error - no long mode support

## /kernel/cpu/fpu.c

/*
 */

## /kernel/cpu/fpu.h

/* x87/SSE state for user threads.
 * math and wide pixel loops. */
/* Enable x87 + SSE (CR0.EM=0, CR0.MP=1, CR4.OSFXSR, CR4.OSXMMEXCPT),
 * first thread is created. */
/* Copy the clean default state into a new thread's save area
 * (512 bytes, 16-byte aligned). */
/* Save the live FPU/SSE registers into prev and load next. */

## /kernel/cpu/gdt.c

// null, kernel code, kernel data, user code, user data, TSS low, TSS high
static struct tss       tss;   // one global TSS
// reloads segment registers with new GDT
    // null descriptor - required as first entry
    // kernel code segment - ring 0, executable, 64-bit
    // kernel data segment - ring 0, writable
    // user code segment - ring 3, executable, 64-bit
    // user data segment - ring 3, writable
    // TSS descriptor ΓÇö 64-bit TSS takes two 8-byte GDT slots (16 bytes total)
    // Format: base[31:0] split across the standard fields, type=0x89 (available TSS)
    // The high slot holds base[63:32] in its low 32 bits, rest zero.
    // clear TSS and set iomap_base past the end (disables I/O permission bitmap)
    // low 8 bytes of TSS descriptor (slot 5)
    gdt[5].access      =  GDT_PRESENT | 0x09;   // 0x09 = available 64-bit TSS
    // high 8 bytes of TSS descriptor (slot 6) ΓÇö upper 32 bits of base, rest 0
    // load the TSS into TR (task register) ΓÇö selector 0x28, RPL=0

## /kernel/cpu/gdt.h

// each GDT entry is 8 bytes
    uint16_t limit_low;     // lower 16 bits of limit
    uint16_t base_low;      // lower 16 bits of base
    uint8_t  base_mid;      // next 8 bits of base
    uint8_t  access;        // access flags
    uint8_t  granularity;   // granularity + upper 4 bits of limit
    uint8_t  base_high;     // upper 8 bits of base
// the GDT pointer loaded via lgdt
    uint16_t limit;         // size of GDT - 1
    uint64_t base;          // address of GDT
// access byte flags
#define GDT_PRESENT     (1 << 7)   // segment present
#define GDT_RING0       (0 << 5)   // privilege level 0 (kernel)
#define GDT_RING3       (3 << 5)   // privilege level 3 (user)
#define GDT_SYSTEM      (0 << 4)   // system segment
#define GDT_CODE_DATA   (1 << 4)   // code or data segment
#define GDT_EXEC        (1 << 3)   // executable (code segment)
#define GDT_DC          (1 << 2)   // direction/conforming
#define GDT_RW          (1 << 1)   // readable/writable
#define GDT_ACCESSED    (1 << 0)   // accessed (set by CPU)
// granularity byte flags
#define GDT_GRAN_4K     (1 << 7)   // page granularity
#define GDT_LONG_MODE   (1 << 5)   // 64-bit segment
#define GDT_32BIT       (1 << 6)   // 32-bit segment
// segment selectors (index * 8)
#define GDT_TSS          0x28   // TSS occupies slots 5+6 (16 bytes)
// Task State Segment ΓÇö 64-bit layout (Intel SDM Vol.3 ┬º8.7)
// We only care about RSP0: the kernel stack the CPU switches to on ring3->ring0
    uint64_t rsp0;          // kernel stack pointer for ring 0
    uint64_t ist[7];        // interrupt stack table (unused for now)
    uint16_t iomap_base;    // offset to I/O permission bitmap (past end = disabled)
// update RSP0 in the TSS ΓÇö call on every context switch so the CPU
// knows which kernel stack to use if a ring-3 process is interrupted

## /kernel/cpu/gdt_flush.asm

; rdi = pointer to gdt_ptr struct
    ; reload code segment via far return
    ; reload all data segment registers

## /kernel/cpu/idt.c

    idt[index].selector    = 0x08;      // kernel code segment
    // cpu exceptions 0-31
    // hardware irqs 32-47

## /kernel/cpu/idt.h

// each IDT entry is 16 bytes in 64-bit mode
    uint16_t offset_low;    // lower 16 bits of handler address
    uint16_t selector;      // code segment selector
    uint8_t  ist;           // interrupt stack table (0 = none)
    uint8_t  flags;         // type and attributes
    uint16_t offset_mid;    // middle 16 bits of handler address
    uint32_t offset_high;   // upper 32 bits of handler address
    uint32_t zero;          // reserved
// IDT pointer loaded via lidt
// flags
#define IDT_INTERRUPT   0x0E    // interrupt gate - disables interrupts on entry
#define IDT_TRAP        0x0F    // trap gate - keeps interrupts enabled

## /kernel/cpu/idt_flush.asm


## /kernel/cpu/irq_controller.c

    /* The 8259 PIC path is only usable with the local APIC off.  UEFI
     * UEFI hardware. */

## /kernel/cpu/isr.asm

; macro for exceptions WITHOUT error code
; macro for exceptions WITH error code
; macro for hardware irqs
; cpu exceptions
; hardware irqs (irq number, interrupt number)
; common exception handler
    ; Reload the data segments for the interrupted context BEFORE the
    ; register frame is restored.  Doing it after the pops (through CX)
    ; zeroed RCX of whatever code the interrupt landed in - any user
    ; instruction could see RCX change under it on a timer tick.  RAX is
    ; used as scratch here and restored by the pops below.
    ; Frame: 15 GPRs, int_no, err_code, then RIP, CS (at rsp + 18*8).
; common irq handler
    ; Reload the data segments for the interrupted context BEFORE the
    ; register frame is restored.  Doing it after the pops (through CX)
    ; zeroed RCX of whatever code the interrupt landed in - any user
    ; instruction could see RCX change under it on a timer tick.  RAX is
    ; used as scratch here and restored by the pops below.
    ; Frame: 15 GPRs, int_no, err_code, then RIP, CS (at rsp + 18*8).
    ; Exit-pending check must happen BEFORE restoring the user register
    ; frame: the old code loaded current_thread_ptr into RDX after the
    ; pops, so every int 0x80 returned to user mode with RDX holding a
    ; kernel direct-map pointer (the thread struct).  User code that kept
    ; a pointer live in RDX across a syscall and dereferenced it after
    ; then wrote/read through a kernel address and page-faulted - the
    ; root cause of the WM's intermittent crashes.  r11 is in the syscall
    ; ABI's clobber set, so using it here leaks nothing.
; idt flush

## /kernel/cpu/isr.c

// called from isr.asm when a cpu exception fires
    // if a C handler is registered for this vector, call it and return
// called from isr.asm when a hardware irq fires
    /* The timer handler may context-switch away and not return here
     * nest. */

## /kernel/cpu/isr.h

// must match push order in isr.asm exactly (last pushed = first field)
    // pushed automatically by cpu on interrupt:
// exception names (defined in isr.c)
// cpu exception stubs (defined in isr.asm)
// hardware irq stubs (defined in isr.asm)
// c handlers called from assembly stubs
// register a custom irq handler
// register a custom exception handler (0-31)
// if a handler is registered it is called instead of the default panic

## /kernel/cpu/lapic.h

/* Turn the local APIC off entirely.  The legacy 8259 PIC path needs this:
 * INTR pin again. */

## /kernel/cpu/multiboot2.h

// GRUB fills and passes the address via ebx 
// every tag starts with these two fields
// tag type 8 = framebuffer info
    uint32_t type;              // always 8
    uint64_t framebuffer_addr;  // physical address of framebuffer
    uint32_t framebuffer_pitch; // bytes per row
    uint32_t framebuffer_width; // pixels wide
    uint32_t framebuffer_height;// pixels tall
    uint8_t  framebuffer_bpp;   // bits per pixel (32 = ARGB)
    uint8_t  framebuffer_type;  // 1 = RGB color
// the info struct header
// tag types
// alignment between tags
// memory map entry types
#define MULTIBOOT_MEMORY_AVAILABLE      1   // free RAM ΓÇö safe to use
#define MULTIBOOT_MEMORY_RESERVED       2   // firmware/hardware reserved
#define MULTIBOOT_MEMORY_ACPI           3   // ACPI reclaimable
#define MULTIBOOT_MEMORY_NVS            4   // ACPI non-volatile (do not touch)
#define MULTIBOOT_MEMORY_BADRAM         5   // defective memory
// a single entry in the memory map
    uint64_t addr;   // physical base address of this region
    uint64_t len;    // length in bytes
    uint32_t type;   // one of MULTIBOOT_MEMORY_* above
    uint32_t zero;   // reserved, always 0
// tag type 6 = memory map
    uint32_t type;          // always 6
    uint32_t size;          // total size of this tag including entries
    uint32_t entry_size;    // size of each entry (usually 24 bytes)
    uint32_t entry_version; // currently 0
    struct multiboot_mmap_entry entries[0]; // variable-length array of entries
// ACPI RSDP handed off directly by the bootloader.
// The payload is revision-specific, so we treat it as raw bytes for now.

## /kernel/cpu/pat.c

/*
 */
/* ---- MSRs and encodings (Intel SDM Vol.3A, 11.12.1) ---- */
/* Memory type encodings (SDM Table 4-1) */
#define PAT_WB  0x00  /* Write-Back      */
#define PAT_WT  0x01  /* Write-Through   */
#define PAT_UCM 0x02  /* Uncacheable Minus (UC-) */
#define PAT_UC  0x03  /* Uncacheable     */
#define PAT_WC  0x01  /* Write-Combining (WC = 01h, SDM Table 11-8) */
#define PAT_WP  0x07  /* Write-Protected */
/*
 */
#define PAT_WC_BYTE_IDX PAT_WC_SLOT  /* byte index in the 64-bit MSR value */
/* ---- CPUID / rdmsr / wrmsr (no libc, -ffreestanding) ---- */
/* Serialize execution: CPUID serializes all prior loads/stores (SDM 2A.4).
 * any subsequent PTE walk uses the new value. */
/* ---- serial hex printer (no libc) ---- */
/* ---- public API ---- */
/* Non-zero when PAT slot 4 has been programmed to WC and is safe
 * WC (PAT-based) vs. UC (PCD+PWT) caching for device pages. */
    /* CPUID.1:EDX bit 16 = PAT feature flag (SDM Vol.2A, CPUID) */
    /* Read current IA32_PAT */
    /* Program the chosen slot to WC (01h).  All other slots preserved.
     * Slot 4 default = 00h (WB); overwrite to 01h (WC). */
    /* Serialize, WRMSR, serialize (SDM Vol.3A, 8.3.2) */
    /* Verify read-back */
    /* --- serial proof (visible on HW serial port) --- */
    /* --- console one-liner (also visible on screen) --- */

## /kernel/cpu/pat.h

/* Program PAT MSR entry for write-combining (WC) framebuffer access.
 * Returns 1 if PAT WC was enabled, 0 if skipped. */
/* Returns non-zero after pat_init_wc() successfully programmed PAT slot 4
 * UC (PCD+PWT, safe fallback) for framebuffer/device mappings. */

## /kernel/cpu/pic.c

// write a byte to an I/O port
    /* Mask everything except the slave cascade line: the slave PIC is
     * the slave (8-15, e.g. PS/2 mouse IRQ12) can ever reach the CPU. */

## /kernel/cpu/pic.h

// PIC ports
// end of interrupt command

## /kernel/crypto/gcm.c

/* GF(2^128) multiplication per NIST SP 800-38D (bit-serial, MSB first). */

## /kernel/crypto/gcm.h

/* AES-128-GCM. rk is the expanded key from aes128_expand_key.
 * Encrypt: ct_out and tag written. Decrypt: returns 0 if tag matches. */

## /kernel/crypto/x25519.c

/* Curve25519 (RFC 7748) field arithmetic mod 2^255-19 using five 51-bit
 * in freestanding mode). */
    h[4] = (ld8(s + 24) >> 12) & FE_MASK; /* & FE_MASK also clears input bit 255 */
/* h = f - g; adds 2p first so limbs stay positive (results < 2^52). */
/* h = z^(2^255 - 21) = z^(p-2) = 1/z */
    fe_mul(t1, z, t1);          /* z^9 */
    fe_mul(t0, t0, t1);         /* z^11 */
    fe_mul(t1, t1, t2);         /* z^31 = 2^5 - 1 */
    fe_mul(t1, t2, t1);         /* 2^10 - 1 */
    fe_mul(t2, t2, t1);         /* 2^20 - 1 */
    fe_mul(t2, t3, t2);         /* 2^40 - 1 */
    fe_mul(t1, t2, t1);         /* 2^50 - 1 */
    fe_mul(t2, t2, t1);         /* 2^100 - 1 */
    fe_mul(t2, t3, t2);         /* 2^200 - 1 */
    fe_mul(t1, t2, t1);         /* 2^250 - 1 */
    fe_sqr(t1, t1);             /* 2^252 - 4 */
    fe_sqr(t1, t1);             /* 2^253 - 8 */
    fe_sqr(t1, t1);             /* 2^254 - 16 */
    fe_sqr(t1, t1);             /* 2^255 - 32 */
    fe_mul(out, t1, t0);        /* 2^255 - 21 */

## /kernel/crypto/x25519.h

/* Returns -1 if the peer public key yields an all-zero shared secret. */

## /kernel/dev/devnodes.c

/* Verbose serial tracing (fb-claim identity log). Default off;
 * enable with SERIAL_VERBOSE=1. Error paths always log. */
/* ---- local helpers ---- */
/* Minimal serial u64 printer (no printf in kernel). */
/* ---- /dev/console surface (wraps drivers/console) ---- */
/* ---- /dev/input surface (keyboard only; mouse stays direct) ---- */
/* ---- /dev/fb0 surface (claim state moved here from syscall.c) ---- */
        /* WM is gone: unmute fb text so the text VT / recovery
         * console is visible again. GUI VT re-mutes on next claim. */
    /* If the previous claimant is gone (killed, crashed, or exited
     * via a VT switch), let the new process take over the screen. */
    /* Identity gate, log-only (P0 step 2): record who claims; no denial.
     * Verbose-only: enable with SERIAL_VERBOSE=1. */
        /* Device framebuffer: WC (PAT-based) when available for fast
         * is a reserved-bit #PF (ERR=RSVD) on any userspace touch. */
            /* Unwind already-mapped pages on mid-loop failure. */
        /* Report the real pixel format.  The window manager blits
         * (typical on real GPUs) or 24bpp (QEMU/GRUB fallbacks). */
    /* Keep the PS/2 cursor position clamped to the real screen size */
    /* WM owns the screen now: console text goes serial-only so it can
     * never scribble over the composited desktop. */
/* ---- /dev/rtc (read-only text node) ---- */
/* ---- population ---- */
    /* Idempotent: a persistfs replay may already have recreated /dev. */
    /* Discoverability nodes (plain files; dispatch uses the registry). */

## /kernel/dev/devops.h

/*
 */
    /* /dev/console */
    /* /dev/input */
    /* /dev/fb0 (info/out are syscall_fb_info_t / syscall_gpu_info_t) */
    /* Readable text nodes (/dev/rtc): SYS_VFS_READ on the node's path
     * Writes at most cap-1 bytes and returns the count. */
/* Register a table under an absolute path ("/dev/console"). The table
 * must point at static storage. Returns 0, or -1 when full. */
/* Look up a table by path. Returns NULL when absent. */
/* Typed accessors for the three known nodes (NULL when missing). */
/* Create /dev + nodes and register the three tables. Idempotent:
 * Returns 0 normally; nonzero is log-only for the caller. */

## /kernel/diag/bootstage.c

            /* Boot splash: advance the progress bar instead of painting
             * the raw stamp over the clean screen. */
            /* Draw the stamp in place without touching the console cursor:
             * on the same row and the real log would be invisible. */

## /kernel/diag/splash.c

static uint32_t splash_last_fill = 0;                /* bar fill permille */
static uint32_t splash_frame = 0;                    /* tick-driven anim frame */
/* Draw one ASCII glyph scaled by `scale` (integer pixels per font pixel),
 * centered horizontally around the caller-provided pixel origin. */
/* Small centered 1x line (normal font size). */
        /* network / hda / audio / nvme / ahci / ata live in the S12 gap */
    /* Wordmark + tagline + version, vertically balanced around the upper third. */
    /* Show "v" + ICDA_VERSION_STRING centered below the tagline.
     * Bounds-safe: strlen("v" ICDA_VERSION_STRING) <= 16. */
    /* Progress bar track. */
/* Tick-driven spinner: 8 dots on a small ring under the stage label.
 * (one clear + 8 dots) ΓÇö microseconds, no measurable boot slowdown. */
    /* Clear the spinner cell to the splash background first so the
     * clips, and the cell is 36x36 centered at (cx,cy). */
        /* Eased leading edge: paint the newest chunk with a brighter
         * rather than stepped. */
    /* Stage label under the bar - the one piece of boot telemetry kept
     * on screen, so a hang still names the failing subsystem. */
    /* Spinner below the label. Gated on screen height so small modes
     * never draw off-screen. */

## /kernel/diag/splash.h

/* Boot splash: replaces the raw "[boot] probing ..." console dump with a
 * (and VT switching) works normally afterwards. */

## /kernel/drivers/audio/hda.c

/* Slice C no-hang guarantee: every terminal audio failure logs exactly
 * per-chunk write errors propagate to the playback layer, which logs.) */
    /* Bounded hard: a codec that is present but unresponsive must not
     * first poll, so the cap only bites on wedged hardware. */
        /* Real codecs (e.g. Realtek ALC-series mixers) can report far
         * list cannot overflow the stack and corrupt the graph walk. */
        case 0x1: return 4; /* speaker */
        case 0x2: return 3; /* headphone */
        case 0x0: return 2; /* line out */
        case 0x4: return 1; /* SPDIF out */
        case 0x5: return 1; /* digital other out */
        /*
         */
    /* Liveness check first.  Each failed verb costs up to 200k MMIO
     * is wedged - skip the expensive walk entirely. */
    /* A live codec answers the graph walk quickly; a codec that misses
     * retries are plenty. */
        /* No HDA hardware (headless/VM without audio): skip init cleanly. */

## /kernel/drivers/audio/playback.c

/* WAV decode guards (Slice C audio hardening): reject anything outside
 * file up-front (no streaming path); longer files are refused. */
    /* Strict header validation: PCM only, 1-2 channels, 8/16-bit,
     * sane sample rate, internally consistent byte/block rates. */
    /* Data chunk must sit inside the file, be non-empty, frame-aligned,
     * and bounded in absolute size and total duration. */

## /kernel/drivers/audio/speaker.c

/* Slice C audio hardening: the tone duration comes straight from a
 * needlessly blast the speaker). 500 ticks = 5 s at 100 Hz. */
    /* Clamp out-of-range requests instead of programming garbage
     * divisors into the PIT. */
    /* Bounded busy-wait only: cap the duration so a bad/huge argument
     * can never spin the CPU (or the speaker) indefinitely. */

## /kernel/drivers/console/console.c

/* When nonzero, fb text output is suppressed (serial still flows).
 * Set while the GUI VT is waiting for / owned by the WM. */

## /kernel/drivers/console/console.h

/* Slice B: mute framebuffer text while the GUI owns the screen (or
 * leave this unmuted. */

## /kernel/drivers/display/flip.c

/*
 */
/* Physical-to-virtual offset (from vmm.h) */
/* Bochs VBE I/O ports and register indices */
static uint32_t flip_frame_bytes = 0;   /* one frame: pitch * height */
static uint64_t flip_fb_phys = 0;       /* from Multiboot2 */
static uint8_t *flip_page0_view = 0;    /* kernel vaddr of page 0 */
static uint8_t *flip_page1_view = 0;    /* kernel vaddr of page 1 */
static int flip_page = 0;               /* 0 = CRTC shows page 0 */
static uint16_t flip_height = 0;        /* single-frame height for Y_OFFSET */
    /* Bochs VBE ID should be 0xB0C0 or higher */
    /* Query VRAM size from the Bochs register (returns 64KB units).
     * versions do not implement it). */
    if (vram16 >= 256) {            /* >= 256 * 64KB = 16 MB */
    /* Need at least 2 frames of video memory for page flipping */
    /* Map both pages into kernel space via HHDM */
    /* Configure virtual framebuffer: width >= physical (needed for
     * Y-offset) and height = 2 * yres so the CRTC can scan either page. */
    /* Hardening: read VIRT_HEIGHT back. Cores that ignore the write
     * the legacy direct-blit path instead of freezing on page 0. */
    /* Start with Y_OFFSET = 0 (page 0 is the front). */
/* Get a pointer the compositor can draw the NEXT frame into.
 * here, then calls flip_swap() to present it. */
    /* Return the page that the CRTC is NOT currently scanning. */
/* Atomic display swap: the CRTC starts scanning out from the back
 * between the two pages. */
        /* Currently showing page 0; flip to page 1. */
        /* Currently showing page 1; flip back to page 0. */
/* Check if page flipping is active. */

## /kernel/drivers/display/flip.h

/* Probe Bochs VBE for page flipping support.  Call after the Multiboot2
 * for mapping the back page. */
/* Pointer to the off-screen (back) page in video memory.  The compositor
 * page flipping is not available. */
/* Atomic display swap: the CRTC starts scanning from the back page. */
/* 1 if page flipping is active, 0 if falling back to direct-to-FB. */
#endif /* DISPLAY_FLIP_H */

## /kernel/drivers/display/font.h

#define FONT_FIRST  32   // first ASCII char in table (space)
#define FONT_LAST   126  // last ASCII char in table (~)
    // 32 - space
    // 33 - !
    // 34 - "
    // 35 - #
    // 36 - $
    // 37 - %
    // 38 - &
    // 39 - '
    // 40 - (
    // 41 - )
    // 42 - *
    // 43 - +
    // 44 - ,
    // 45 - -
    // 46 - .
    // 47 - /
    // 48 - 0
    // 49 - 1
    // 50 - 2
    // 51 - 3
    // 52 - 4
    // 53 - 5
    // 54 - 6
    // 55 - 7
    // 56 - 8
    // 57 - 9
    // 58 - :
    // 59 - ;
    // 60 - <
    // 61 - =
    // 62 - >
    // 63 - ?
    // 64 - @
    // 65 - A
    // 66 - B
    // 67 - C
    // 68 - D
    // 69 - E
    // 70 - F
    // 71 - G
    // 72 - H
    // 73 - I
    // 74 - J
    // 75 - K
    // 76 - L
    // 77 - M
    // 78 - N
    // 79 - O
    // 80 - P
    // 81 - Q
    // 82 - R
    // 83 - S
    // 84 - T
    // 85 - U
    // 86 - V
    // 87 - W
    // 88 - X
    // 89 - Y
    // 90 - Z
    // 91 - [
    // 92 - backslash
    // 93 - ]
    // 94 - ^
    // 95 - _
    // 96 - `
    // 97 - a
    // 98 - b
    // 99 - c
    // 100 - d
    // 101 - e
    // 102 - f
    // 103 - g
    // 104 - h
    // 105 - i
    // 106 - j
    // 107 - k
    // 108 - l
    // 109 - m
    // 110 - n
    // 111 - o
    // 112 - p
    // 113 - q
    // 114 - r
    // 115 - s
    // 116 - t
    // 117 - u
    // 118 - v
    // 119 - w
    // 120 - x
    // 121 - y
    // 122 - z
    // 123 - {
    // 124 - |
    // 125 - }
    // 126 - ~

## /kernel/drivers/display/framebuffer.c

// internal state
/* 1 when 24-bit pixels are BGR-ordered on the wire (byte 0 = blue),
 * honor this; 32-bit native writes are unaffected. */
/* When flip page-flipping is active the compositor needs a 2-frame
 * the first frame. */
// cursor position in characters
// public screen dimensions
// ============================================================
// parse multiboot2 info to find framebuffer tag
            /* 24-bit color order comes from the VBE masks, not from the
             * is exactly what happened before this flag existed. */
                /* Byte 0 carries the channel at bit position 0. */
                /* No mask info: assume the common BGR 24-bit layout. */
                /* 32-bit native uint32 writes are order-correct. */
/* Adopt a new physical framebuffer (e.g. virtio-gpu backing).
 * sees the new buffer without reinitialising the console. */
    fb_bgr_order  = 0;      /* 32-bit native XRGB8888 */
    fb_hhdm       = 1;      /* already mapped through HHDM */
// return the raw physical address of the framebuffer (0 if not ready)
// Enable/disable double-frame mode for page-flipping compositor.
// return the size of the framebuffer in bytes
// adjust fb_addr to point through the HHDM; call once after vmm_init
// ============================================================
// pixel operations
    /* Byte-wise store: a 32-bit store overlaps into the next pixel on
     * 24-bit layouts, and must honor the BGR wire order there. */
    /* 24-bit: byte stores honoring the BGR wire order. Previously this
     * function silently did nothing below 32bpp. */
// ============================================================
// render font
            // MSB is leftmost pixel
// ============================================================
// scroll
    // move all rows up by one character height
    // clear last row
// ============================================================
// char and string output

## /kernel/drivers/display/framebuffer.h

// 32 bit ARGB format
// ============================================================
// functions
// return the raw physical address of the framebuffer (0 if not ready)
// return the size of the framebuffer in bytes
// bits per pixel and bytes-per-row of the native framebuffer format
// remap fb_addr through the HHDM after vmm_init; must be called once after paging is live
// Enable double-frame mode: fb_phys_size() returns 2x when enabled.
// The kernel console (fb_addr, fb_width, fb_height) is unaffected.
// Adopt a new physical framebuffer (e.g. from virtio-gpu).  Updates all
// fb_* globals so fb_print / fb_phys_addr / devnodes work immediately.
// screen dimensions (set after fb_init)

## /kernel/drivers/display/gpu.c

/* Registered display devices, newest first. */
/* ---- fbdev driver: wraps the firmware framebuffer -------------------- */
    /* Single scanout buffer: the compositor already blitted into the
     * without changing userspace. */
    /* No hardware cursor plane on the firmware framebuffer: the WM
     * draws a software cursor, so report unsupported. */
/* ---- registry -------------------------------------------------------- */
/* ---- init ------------------------------------------------------------ */
    /* fb_init parses the multiboot framebuffer tag; it must run first
     * (the console already does this before we get here). */
    /* Expose the native mode the firmware set up (gfxpayload=keep). */
    fbdev.needs_present = 0;  /* fbdev present is a no-op */
    /* Probe for Bochs VBE page flipping.  On success, double the
     * false (flip_available = 0) and the legacy blit path is used. */

## /kernel/drivers/display/gpu.h

/*
 */
    uint32_t pitch;    /* bytes per row */
    /* Scanout memory (physical).  The compositor maps this and blits
     * into it; present() tells the device the frame is ready. */
    /* Capabilities */
    int         hw_cursor;      /* device has a hardware cursor plane */
    int         needs_present;  /* 1 when present() does real DMA work (e.g. virtio-gpu) */
    /* Driver ops */
    int (*present)(struct gpu_device *dev);              /* commit frame */
                      const uint32_t *image, int w, int h); /* hw cursor or -1 */
/* Register a display driver (called by the fbdev driver at init). */
/* The primary (boot) display. */
/* Find a device by name; NULL if absent. */
/* Initialize the built-in fbdev driver from the multiboot framebuffer.
 * Returns 0 on success, -1 if no usable framebuffer exists. */

## /kernel/drivers/display/vga.c

// internal state
// ============================================================
// helpers
// write a character + color directly to VGA memory at a position
// scroll screen up by one line when we hit the bottom
    // move every row one row up
    // clear the last row
    // move cursor to start of last row
// ============================================================
// public API
// initialize VGA 
// fill entire screen with blank characters
// set default color for future prints
// handle newline and scroll if we hit the bottom
// print a single character at current cursor position
    // wrap to next line if we hit the edge
// print a null-terminated string
// print a signed integer
    // build digits in reverse
// print an unsigned int as hexadecimal e.g. 0x1F3A

## /kernel/drivers/display/vga.h

// ============================================================
// combine foreground + background into one attribute byte
// common combos
// ============================================================
// Functions

## /kernel/drivers/display/virtio_gpu.c

/*
 */
/* Verbose serial tracing for probe/setup steps. Default off; the
 * always log. */
/* Legacy virtio PCI register offsets (I/O-port based) */
#define VIRTIO_REG_DEVICE_FEATURES  0x00  /* 32-bit RO */
#define VIRTIO_REG_DRIVER_FEATURES  0x04  /* 32-bit WO */
#define VIRTIO_REG_QUEUE_ADDRESS    0x08  /* 32-bit WO: phys >> 12 */
#define VIRTIO_REG_QUEUE_SIZE       0x0C  /* 16-bit RO */
#define VIRTIO_REG_QUEUE_SELECT     0x0E  /* 16-bit WO */
#define VIRTIO_REG_QUEUE_NOTIFY     0x10  /* 16-bit WO */
#define VIRTIO_REG_DEVICE_STATUS    0x12  /* 8-bit  WO */
/* Status bits */
/* Descriptor flags */
/* ---- Driver state ---- */
    uint16_t port;            /* I/O port base from BAR0 */
    /* Control queue (queue 0) ΓÇö single contiguous legacy layout */
    /* Command buffer (one 4-KiB page) */
    /* Display state from GET_DISPLAY_INFO */
/* ---- x86 port I/O helpers ---- */
/* ---- Serial helper: print uint32_t as decimal (verbose only) ---- */
/*
 */
/* ---- Control queue setup (legacy packed layout) ---- */
    /* Legacy layout: desc table + avail ring contiguously, used ring at 4K boundary */
    /* Allocate command buffer */
/* ---- Controlq send: two chained descriptors ---- */
    /* desc[0]: request ΓÇö OUT to device, chain to desc[1] */
    /* desc[1]: response area ΓÇö IN from device (WRITE), no chain */
    /* Post head index 0 to available ring */
    /* Notify device */
    /* Poll used ring ΓÇö bounded with vg_pause() inside each iteration */
                    /* Response is at cmd_buf + request_len */
                    return -1;  /* device error */
    return -1;  /* timeout */
/* ---- High-level control commands ---- */
    return -1;  /* no enabled display mode */
    /* Build the full command (header + 1 mem_entry) in local buf */
/* ---- gpu_device_t callbacks ---- */
    return -1;  /* no hardware cursor on virtio-gpu */
/* ---- Public init ---- */
    /* Step 0: early-out when multiboot framebuffer already present */
    /* Step 1: PCI scan for vendor 0x1AF4, device 0x1050 */
    /* Step 2: Read BAR0 ΓÇö must be I/O (PIO, bit 0 == 1) */
    /* Step 3: lifecycle reset -> ACK -> Driver */
    /* Features: don't negotiate virgl/edid for minimal driver */
    /* Step 4: select queue 0, read negotiated size */
    /* Allocate legacy packed queue (single contiguous region) */
    /* Tell device about control queue: Queue Address at 0x08 (phys >> 12) */
    /* NOTE: No 0x0C high-dword write ΓÇö legacy has no such register;
     * it would clobber Queue Size (0x0C is read-only Queue Size in legacy). */
    /* DRIVER_OK */
    /* Step 5: GET_DISPLAY_INFO */
    /* Step 6: RESOURCE_CREATE_2D (resource_id=1, XRGB8888) */
    /* Step 7: allocate backing pages */
    /* Zero the backing buffer */
    /* Step 8: ATTACH_BACKING (single contiguous entry ΓÇö valid since backing IS contiguous) */
    /* Step 9: SET_SCANOUT (scanout 0, resource 1) */
    /* Step 10: adopt as the system framebuffer ΓÇö update framebuffer.c globals
     * so fb_print / fb_phys_addr / devnodes claim map all work. */
    /* Register as gpu_device_t (only when no primary exists) */

## /kernel/drivers/display/virtio_gpu.h

/* Virtio GPU PCI Device ID (vendor 0x1AF4) */
/* Controlq index */
/* Control command types */
/* Response types */
/* Pixel format */
/* ---- Virtio-gpu controlq structs (packed, spec-matching) ---- */
    /* followed by virtio_gpu_mem_entry_t[nr_entries] */
/* ---- Virtqueue ring structs (same layout as virtio_net) ---- */
/* ---- Public API ---- */
/* Initialize virtio-gpu.  Returns 0 on success, -1 if no device or failure.
 * Only called when fb_available() == 0 (no multiboot framebuffer). */
/* Transfer + flush full framebuffer to host.  Returns 0 on success, -1 on error. */
/* 1 when the virtio-gpu device is initialized and the scanout is live. */

## /kernel/drivers/input/mouse.c

/* Discard any bytes still queued after init (e.g. a late ACK on real
 * every packet by one byte. */
    /* Enable auxiliary PS/2 port */
    /* Enable IRQ12 in PS/2 controller config byte */
    config |= 0x02;    /* enable IRQ12 */
    config &= ~0x20;   /* clear mouse clock disable bit */
    /* Reset mouse */
    (void)ps2_read();   /* ACK 0xFA */
    (void)ps2_read();   /* self-test 0xAA */
    (void)ps2_read();   /* device id 0x00 */
    /* Set defaults */
    /* Enable data reporting (stream mode) */
    /* Discard any bytes that arrived late (see mouse_drain_output). */
        /* Resolution change: recenter so the pointer lands where the
         * first event. */
    /* Rate-limited diagnostic: at most once per ~500ms (5M TSC ticks
     * the serial port during heavy compositing. */
            /* Minimal u64ΓåÆdecimal (no printf in kernel). */
    /* Drain every byte currently in the output buffer. One IRQ12 can
     * QEMU, so always check the status port first. */
        /* Real HW: some PS/2 controllers (AUX) don't reliably set
         * We are in IRQ12 so the data port is mouse. */
        /* First byte must have bit 3 set; resync if not */
            /* Y axis is inverted for screen coords */
            /* Ignore if overflow bits set */
                // Real HW: ring overflow under heavy composite ΓÇö keep newest event
            /* Wake any process blocked on input (e.g. the window manager
             * instead of on the next scheduler tick. */

## /kernel/drivers/input/mouse.h

    uint8_t buttons;   /* bit 0=left, bit 1=right, bit 2=middle */
int     mouse_read_event(mouse_event_t *out);   /* 0=got event, -1=empty */

## /kernel/drivers/net/net_drv.c

/*
 */
    /* Try e1000 first (works in QEMU and VirtualBox) */

## /kernel/drivers/net/net_drv.h

/*
 */

## /kernel/drivers/net/virtio_net.c

/*
 */
/* Verbose serial tracing for probe/negotiation steps. Default off;
 * the single "[virtio] ready" line and all error lines always log. */
/* Virtio PCI capability offsets */
/* Virtio register offsets (modern, capability-based) */
/* Status bits */
/* Descriptor flags */
/* Feature bits */
    /* TX queue (queue 0) */
    /* RX queue (queue 1) */
    volatile uint8_t *cfg;  /* MMIO base for device-specific config */
    /* Zero the page */
/*
 */
/*
 */
    if (bar0 & 0x01) return 0; /* I/O BAR */
    /* TX queue (queue 0) */
        vn.tx_desc[i].flags = VIRTIO_DESC_F_WRITE;  /* host reads */
    /* RX queue (queue 1) */
        /* Pre-fill the available ring */
    /* Read BAR0 value */
    /* Setup queues */
    vn_select_queue(0); /* TX */
    vn_select_queue(1); /* RX */
    /* Tell device about TX queue */
    vn_write32(vn.mmio, 0x08, (uint32_t)vn.tx_desc_phys);  /* Queue PFN (legacy) */
    /* Tell device about RX queue */
    /* Read MAC address from device config */
        /* Fallback: use a fixed MAC for testing */
    /* Set driver OK */
    /* Prepend virtio-net header */
    /* Copy frame data after header */
    /* Setup descriptor */
    vn.tx_desc[desc_idx].flags = 0;  /* host reads */
    /* Add to available ring */
    /* Notify device */
    /* Advance to next descriptor */
    /* Check if there are new used buffers */
    if (vn.rx_last_used == used_idx) return 0; /* No new data */
    /* Process the oldest used buffer */
    /* Skip the virtio-net header */
    /* Copy data from receive buffer */
    /* Re-arm the descriptor: reset it and put it back on the available ring */
    /* Notify device that buffers are available */

## /kernel/drivers/net/virtio_net.h

/* Virtio PCI Device IDs */
/* Virtio Feature Bits (network device) */
/* Virtio Queue Sizes */
/* Virtio Net Header */
    // uint16_t num_buffers; // Only if VIRTIO_NET_F_MRG_RXBUF
/* Virtio Descriptor */
/* Virtio Available Ring */
/* Virtio Used Ring Element */
/* Virtio Used Ring */

## /kernel/drivers/pci/pci.c

        /* Every probe must carry the bus/device/function it intends to
         * and mis-read the multi-function flag of every device. */

## /kernel/drivers/rtc/rtc.c

/*
 */
#define RTC_CENTURY 0x32   /* ACPI FADT default; 0 on many boards */
    /* Bit 7 of the address port keeps NMIs disabled while selecting. */
        /* 12-hour mode: 12 AM is 0, 12 PM is 12. */

## /kernel/drivers/rtc/rtc.h

/* CMOS real-time clock (MC146818-compatible, ports 0x70/0x71).
 * default and most Linux-installed machines. */
    uint8_t  month;   /* 1..12 */
    uint8_t  day;     /* 1..31 */
    uint8_t  hour;    /* 0..23 */
/* Read a consistent snapshot (retries across update cycles).
 * Returns 0 on success, -1 if the clock looks absent. */
/* Format "YYYY-MM-DD HH:MM:SS\n" into buf; returns bytes written
 * (excluding the NUL) or 0 on failure. */

## /kernel/drivers/serial/serial.c

    outb(COM1 + 1, 0x00);    // Disable interrupts
    outb(COM1 + 3, 0x80);    // Enable DLAB
    outb(COM1 + 0, 0x03);    // Divisor low: 38400 baud
    outb(COM1 + 1, 0x00);    // Divisor high
    outb(COM1 + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(COM1 + 2, 0xC7);    // Enable FIFO, clear it, 14-byte threshold
    outb(COM1 + 4, 0x0B);    // IRQs enabled, RTS/DSR set

## /kernel/drivers/storage/ahci.c

        /* Controller not responding (BAR reads all-ones): skip instead of
         * poking 32 phantom ports through the bounded waits. */

## /kernel/drivers/storage/nvme.c

        /* BAR mapped but controller not responding (e.g. device asleep
         * bounded wait. */

## /kernel/fs/fd.h

/*
 */
/* open(2) flag subset we honor (Linux values). */
/* Lazily initializes the table (idempotent, safe on zeroed procs). */
/* Resolve `kpath` (already validated kernel-side string) under `cwd`
 * and allocate an fd >= 3. Returns the fd, or a negative -U_Exxx. */
/* Look up an fd. Stdio (0..2) returns 0 with *is_stdio = 1 and
 * anything else returns -U_EBADF. */
/* Update the offset of a live non-stdio fd. Returns 0 or -U_EBADF. */
/* Stored open flags of a live non-stdio fd (FD_O_* above). */
/* Close one fd. Stdio or unused fds return -U_EBADF. */
/* Drop every entry. Called on every process-exit path; VFS nodes
 * themselves are owned by the VFS and need no release. */

## /kernel/fs/initramfs.c

/* CI test-image extras (nptest/nptestlx/gui_demo). Default off:
 * `make` builds the production image; CI builds with CI_IMAGE=1. */
/* Seed one blob entry and advance the cursor. Keeps initramfs_init
 * correct when CI_IMAGE entries are compiled out. */
    /* Cursor walks the table in order, so gated-out entries cannot
     * then the CI-only test apps in table order. */

## /kernel/fs/vfs.c

        /* In-place overwrite inside the existing buffer. */

## /kernel/fs/vfs.h

/* Offset write directly against a node (fd layer). Grows the file when
 * off+size exceeds it, zero-filling the gap. Returns 0 or -1. */

## /kernel/ipc/msgq.c

            /* Sleep a tick between checks so a blocking reader does not
             * busy-spin the whole scheduler at 100Hz. */
                /* hard limit to avoid infinite hang */

## /kernel/ipc/msgq.h

/* Returns 1-based handle, 0 on failure */
/* Enqueue a MSGQ_MSG_SIZE-byte message. Returns 0 ok, -1 full/bad */
/* Dequeue a message. block=0 non-blocking (returns -1 if empty).
   block=1 spins until a message arrives. Returns 0 ok. */
/* Returns 1 if message waiting, 0 if empty, -1 bad handle */
/* Future ref-counting stub */

## /kernel/ipc/shm.c

#define SHM_MAX_PAGES 4096   /* 4096*4096 = 16 MiB max per region */
        /* zero page */
    return idx + 1; /* 1-based handle */
            /* unmap already-mapped pages on failure */

## /kernel/ipc/shm.h

/* Virtual address base for SHM mappings in userspace (each slot 16 MiB).
 * to each address space, far from both user text (512 GiB) and the stack. */
/* Returns 1-based handle, 0 on failure */
/* Map handle into current process VA; returns virtual addr or 0 */
/* Unmap handle from current process */
/* Release handle; frees physical pages when ref_count reaches 0 */
/* Get byte size of region */

## /kernel/kernel.c

/* CI-only boot self-test facility (default off). Build with
 * icda.test=* command-line hook entirely. */
    /* Apply any Ctrl+Alt+F1..F6 virtual terminal switch requested by the
     * keyboard driver (force-exits the foreground user app if needed). */
    /* GPU device layer: registers the firmware framebuffer as the fbdev
     * display driver (the same role efifb/simpledrm play on Linux). */
    /* Boot splash: hides the raw probe log behind a clean loading screen
     * before the shell starts so the text console works afterwards. */
    /* Program PAT MSR slot 4 to Write-Combining for framebuffer.
     * CPUID-gated: silently skips if PAT is not supported. */
    /* Threads copy the default FPU/SSE image at creation, so the unit
     * must be enabled before the scheduler builds its first thread. */
    /* virtio-gpu: only when no multiboot framebuffer is available.
     * Must run AFTER pci_init so PCI devices are enumerated. */
    /* /dev nodes + ops registry for the syscall gate. Log-only on
     * failed populate degrades syscalls instead of halting boot. */
            /* Any stray console writes between here and the WM's first
             * Text VTs stay unmuted so the shell stays visible. */
        /* Boot self-test facility (CI + bring-up debugging). With
         * already implies full control, so this adds no privilege. */
            /* Mirror the console to serial for the duration so every
             * app's exit code ΓÇö that comes from user_last_exit_code. */
            /* PID1 supervision (P0 step 2): the kernel runs init, init
             * out-param (B1) ΓÇö nonzero init exit means failure. */
                /* Reap init's orphans (B2): only init itself is waited
                 * on, so its force-exited children would leak. */

## /kernel/memory/pf.c

// scheduler hook 
// the page fault handler needs the address space of the currently running process so it can map the new page into the right PML4.
// before the scheduler exists this is NULL, meaning "use the kernel AS".
// helpers 
// read CR2 ΓÇö the cpu stores the faulting virtual address there
//  panic helper 
/* Fault details always go straight to the serial port too: the console
 * a headless box is otherwise invisible. */
// Conditions that must ALL be true:
//   1. Not-present fault (P=0) ΓÇö the page hasn't been mapped yet, not a
//      protection violation.
//   2. Write access (W=1) ΓÇö pushing onto the stack is always a write.
//   3. The faulting address is inside the user stack region
//      [USER_STACK_LIMIT, USER_STACK_TOP).
//   4. The faulting page is at most one page below the current RSP page ΓÇö
//      a correct program should only extend the stack one page at a time
//      (the ABI guarantees this for the standard call sequence).
//      We allow a small slack (4 pages) to handle red zones and alloca.
// NOTE: once we have a PCB we can also check against per-process stack limits.
    if (e & PF_PRESENT)  return 0;  // protection fault, not missing page
    if (!(e & PF_WRITE)) return 0;  // read fault can't be stack growth
    // must be in the user stack region
    /* Kernel-mode fault (PF_USER clear): the kernel legitimately writes
     * not apply - bound by process kind and the stack region instead. */
    // must be within SLACK pages below rsp's current page
    if (fault_page > rsp_page) return 0;               // above rsp ΓÇö weird
    if (rsp_page - fault_page > slack) return 0;        // too far below
//  main page fault handler 
        // allocate a fresh physical frame
        // map the page into the current address space as user read-write
        // zero the new page so the process sees clean memory
        // return from the handler ΓÇö the cpu will re-execute the faulting instruction
    /* A user-mode fault kills the offending process instead of halting
     * panic - they indicate a kernel bug and are not recoverable. */
            /* SIGSEGV analog (-11).  Deliberately distinct from the
             * U_EFAULT (-14) syscall error in syscall.h. */
            /* Switch away; the zombie thread is never rescheduled, so the
             * faulting instruction is never re-executed. */
    // not a recoverable fault ΓÇö panic with full details
// init 

## /kernel/memory/pf.h

//  page fault error code bits (pushed by cpu as err_code) 
#define PF_PRESENT   (1ULL << 0)  // 0 = not-present fault, 1 = protection fault
#define PF_WRITE     (1ULL << 1)  // 0 = read,  1 = write
#define PF_USER      (1ULL << 2)  // 0 = kernel, 1 = user mode
#define PF_RESERVED  (1ULL << 3)  // reserved PTE bit was set
#define PF_IFETCH    (1ULL << 4)  // fault during instruction fetch
//  user stack layout 
// Each user process gets a stack that grows downward from USER_STACK_TOP.
// The VMM will map new pages on demand inside [USER_STACK_LIMIT, USER_STACK_TOP).
#define USER_STACK_TOP    0x00007FFFFFFFE000ULL  // first byte ABOVE user stack
#define USER_STACK_LIMIT  0x00007FFFFFF00000ULL  // max stack size ~8 MiB
// How many pages are mapped up front when a user process is created.
// One page is far too small: apps keep multi-KB stack buffers (e.g. a
// 4KiB directory listing in ic_icon_load_folder), and at different
// optimization levels the compiler can lay those frames out so they
// cross the single mapped page, faulting in the middle of a kernel
// copy.  16 KiB of headroom covers the startup path of every app.
//  scheduler hook 
// The scheduler must call this whenever it switches to a new process so the
// page fault handler knows which addr_space_t to map new pages into.
// Before the scheduler exists, the kernel address space is used.
//  init 
// Registers the page fault handler with the ISR system (vec 14).
// Call after pmm_init() and vmm_init().

## /kernel/memory/pmm.c

// kernel_end is exported by the linker script
    // pass 1: find max usable memory
    // mark everything used initially
    // pass 2: free usable regions
    // protect reserved regions
    // find first free frame

## /kernel/memory/vmm.c

// tracks whether the HHDM is live; 0 = use phys addr directly, 1 = use PHYS_TO_VIRT
// return a writable pointer to a physical page table page,
// using identity mapping before HHDM is up, PHYS_TO_VIRT after
// print helpers (no libc available)
// allocate and zero a physical page for use as a page table
// map a single 2 MiB huge page virt -> phys (virt and phys must be 2 MiB aligned)
    /* Translate software VMM_WC flag to hardware PAT bit for 2 MiB PDE.
     * Clear PCD/PWT so index = {PAT=1,PCD=0,PWT=0} = 4 -> PAT[4] = WC. */
        flags |= (1ULL << 12);  /* PAT bit for 2M PDE */
    // set the huge bit directly in the PD ΓÇö no PT needed
// walk the 4-level page table for virt; allocate intermediate tables if alloc=1
// returns a pointer to the final PTE, or NULL if a level is missing and alloc=0
    if (pdpt[i3] & PTE_HUGE) return NULL; // 1 GiB page, can't descend
    if (pd[i2] & PTE_HUGE) return NULL; // 2 MiB page, can't descend
    /* Translate software VMM_WC flag to hardware PAT bit for 4 KiB PTE.
     * Clear PCD/PWT so index = {PAT=1,PCD=0,PWT=0} = 4 -> PAT[4] = WC. */
        flags |= (1ULL << 7);  /* PAT bit for 4K PTE */
/* True when virt is present and software-writable in this address
 * failing the syscall). No allocation, no side effects. */
    if (end < phys) return 0; /* reject wrap-around near 2^64 */
    // descriptor page and PML4 are separate so the PML4 stays page-aligned
    // The kernel still executes from the low bootstrap mapping while it also
    // maintains higher-half aliases. Keep PML4[0] as a supervisor-only shared
    // mapping so interrupt/syscall entry can fetch kernel code after a ring
    // transition, then inherit the higher-half kernel mappings too.
// free all PT pages under a PD (user half only)
    // walk user half only (entries 0-255)
    /* Identity map (low half, PML4[0]): capped at 512 MiB.  User address
     * image + low reserved region and stays below USER_TEXT_BASE. */
    /* HHDM (high half, PML4[256]): cover ALL physical RAM so that
     * never triggered it). */
        /* Select WC (fast, PAT-based) or UC (safe fallback) for the
         * would occur if slot 4 were left at its reset value (WB). */
            /* Guard: do not flip in-RAM HHDM PDEs from WB to WC.
             * skip is a no-op safety net. */
    // enable PSE (bit 4) and PGE (bit 7) in CR4 before loading CR3
    // PSE is required for 2 MiB huge pages; PGE enables the global page flag
    cr4 |= (1ULL << 4) | (1ULL << 7);  // PSE | PGE
    // The framebuffer pointer still points at a physical address here.
    // Move it onto the HHDM before any further printing in the new CR3.

## /kernel/memory/vmm.h

// page sizes
// x86-64 page table entry flags
#define PTE_PRESENT    (1ULL << 0)   // page is present
#define PTE_WRITE      (1ULL << 1)   // read/write (if 0, read-only)
#define PTE_USER       (1ULL << 2)   // user-accessible (ring 3)
#define PTE_WRITE_THRU (1ULL << 3)   // write-through caching
#define PTE_NO_CACHE   (1ULL << 4)   // disable caching
#define PTE_ACCESSED   (1ULL << 5)   // set by cpu on access
#define PTE_DIRTY      (1ULL << 6)   // set by cpu on write
#define PTE_HUGE       (1ULL << 7)   // 2 MiB (in PD) or 1 GiB (in PDPT)
#define PTE_GLOBAL     (1ULL << 8)   // not flushed on CR3 switch
#define PTE_NX         (1ULL << 63)  // no-execute (requires EFER.NXE)
// extract the physical address from an entry
// virtual address index decomposition (9-9-9-9-12)
// bits [63:48] must equal bit 47 on x86-64
// virtual memory layout
//   0x0000000000000000 - 0x00007FFFFFFFFFFF : user space  (128 TiB)
//   0xFFFF800000000000 - 0xFFFFFFFFFFFFFFFF : kernel space (128 TiB)
#define PHYSICAL_BASE   0xFFFF800000000000ULL   // higher-half direct map
#define KERNEL_VMA      0xFFFFFFFF80000000ULL   // kernel image (-2 GiB)
// convert physical <-> virtual via the direct map
// 64-bit page table entry; 512 entries per table = 4 KiB
// address space identified by its PML4 physical address
    /* Number of 4 KiB pages mapped (tracked for task-manager memory
     * accounting; only increments for newly allocated mappings). */
// public flag aliases
// Software-only flag: write-combining (WC) via PAT MSR.
// Translated to the correct hardware bit in map_huge_page (2M: bit 12)
// and vmm_map_page (4K: bit 7) ΓÇö the PAT bit position differs per level
// per SDM Vol.3A, 4.9.  Requires pat_init_wc() to have set PAT slot 4 = WC.
// common flag combinations
// must be called after pmm_init(); fb_phys/fb_size map the framebuffer into the HHDM
// map a single 4 KiB page virt -> phys; returns 0 on success, -1 on failure
// unmap a single page; free_phys=1 releases the physical frame to the PMM
// map a contiguous range; size is rounded up to PAGE_SIZE_4K
// unmap a contiguous range
// walk page tables and return the physical address for virt, or 0 if unmapped
// 1 when virt is present and writable (PTE R/W) in user half, else 0
// allocate a new user address space with kernel mappings inherited; NULL on fail
// free all user-space page table pages; kernel mappings are left intact
// load an address space into CR3
// return the kernel address space
// make a physical range accessible through the higher-half direct map.
// returns the virtual base address for the requested physical address.
// invalidate a single TLB entry
// flush the entire TLB by reloading CR3
// print mapped page count and PMM free frames

## /kernel/net/net.c

    /* Use DHCP-provided DNS first, then fallbacks */
/* ---- DHCP client ---- */
    /* Build DHCP DISCOVER */
    packet[4] = 0x00; /* xid - use ticks as transaction id */
    /* flags = broadcast */
    /* ciaddr = 0 (discover) */
    /* chaddr = our MAC */
    /* magic cookie */
    /* options: message type = DISCOVER */
    /* end option */
    /* Broadcast MAC */
    /* Send DISCOVER on UDP port 68 -> 67 */
    /* Wait for OFFER (up to 2 seconds) */
        /* Parse DHCP manually: expect UDP from port 67 to broadcast:68 */
        /* Parse options */
        } /* end parse block */
    } /* end while */
    /* Build DHCP REQUEST */
        /* message type = REQUEST */
        /* requested IP */
        /* server id */
    /* Wait for ACK (up to 2 seconds) */
        /* Parse DHCP manually: UDP from port 67 to broadcast:68 */
    /* Try DHCP first */
        /* Fallback to QEMU static config */
            /* Oversized pages (modern sites easily exceed the cap) are
             * the browser's tolerant parser renders the first part. */
                /* Truncate oversized pages instead of failing (see the
                 * plain-HTTP path above). */

## /kernel/net/net.h

// network error codes
// ethernet types
// ARP constants
// IP protocol numbers
// TCP flags
#define NET_LOCAL_IP               0x0F02000AU // 10.0.2.15 little-endian host order
#define NET_GATEWAY_IP             0x0202000AU // 10.0.2.2
#define NET_DNS_IP                 0x0302000AU // 10.0.2.3
#define NET_DNS_IP_ALT1            0x08080808U // 8.8.8.8
#define NET_DNS_IP_ALT2            0x01010101U // 1.1.1.1
#define NET_NETMASK                0x00FFFFFFU // 255.255.255.0
// packet structures
// global net state
// byte order helpers
// memory helpers
// string helpers
// checksum
// IP helpers
// low-level helpers
// TCP/UDP send
// TCP/UDP parse
// TCP connect
// ARP
// HTTP helpers
// public API

## /kernel/net/tls.c

#define TLS_VERSION_MINOR 3  /* TLS 1.2 = 3.3 */
    uint32_t length;  // 3 bytes
    /* TLS 1.3 state */
/* ---- TLS 1.3 (RFC 8446) ------------------------------------------------ */
/* HKDF-Expand-Label; out_len <= 32 so a single HMAC block suffices. */
    /* HKDF-Expand with one iteration: T(1) = HMAC(prk, info || 0x01) */
/* Encrypt one TLS 1.3 record: inner = data || type, wire type 23. */
    /* Reuse the raw TCP sender of tls_send_record by inlining it here is not
     * possible; build the ethernet frame the same way it does. */
/* Read and decrypt the next TLS 1.3 record. Plaintext CCS records are
 * skipped. On success returns 1 with the inner content type and plaintext. */
/* Finish the TLS 1.3 handshake after the ServerHello selected TLS 1.3.
 * conn->x25519_priv. */
    /* Middlebox compatibility: dummy change_cipher_spec. */
    /* Read the encrypted server flight: EncryptedExtensions, Certificate,
     * accumulated in a heap buffer. */
                /* Stateless extensions need no verification. */
                /* P0 fail-closed: there is no CA trust store and no
                 * entirely for now ΓÇö by design, loudly. */
    /* Application traffic secrets over transcript CH..server Finished. */
    /* Client Finished (transcript hash is the same CH..server Finished). */
    /* Heap-allocate large buffers to avoid stack overflow (kernel stack = 16KB) */
    /* Compact the reassembly buffer so long transfers don't fill it. */
    /* x25519 keypair for a possible TLS 1.3 handshake (generated up front so
     * the key share can go into the ClientHello). */
    /* 32-byte legacy session id (TLS 1.3 middlebox compatibility mode) */
    /* cipher suites: TLS 1.3 GCM first, then the legacy TLS 1.2 RSA suites */
    ch[ch_len++] = 0xFF; /* TLS_EMPTY_RENEGOTIATION_INFO_SCSV */
        /* signature_algorithms */
        ch[ch_len++] = 0x04; ch[ch_len++] = 0x03; // ecdsa_secp256r1_sha256
        ch[ch_len++] = 0x08; ch[ch_len++] = 0x04; // rsa_pss_rsae_sha256
        ch[ch_len++] = 0x04; ch[ch_len++] = 0x01; // rsa_pkcs1_sha256
        ch[ch_len++] = 0x05; ch[ch_len++] = 0x01; // rsa_pkcs1_sha384
        ch[ch_len++] = 0x06; ch[ch_len++] = 0x01; // rsa_pkcs1_sha512
        ch[ch_len++] = 0x02; ch[ch_len++] = 0x01; // rsa_pkcs1_sha1
        /* supported_versions: TLS 1.3 and TLS 1.2 */
        /* supported_groups: x25519, secp256r1 */
        /* psk_key_exchange_modes: psk_dhe_ke */
        /* key_share: x25519 public key */
            /* Once TLS 1.3 is selected, the following records are encrypted;
             * leave them in rx_buf for the 1.3 handshake continuation. */
                        /* version(2) + random(32) + sid_len(1) + sid + cipher(2) + comp(1) [+ exts] */
                            /* Scan extensions for TLS 1.3 selection and the
                             * server key share. */
                        /* P0 fail-closed (TLS 1.2 path): the certificate
                         * MITM. Refuse until CA verification lands. */
                        /* P0: cert parsing preserved for the future CA
                         * fail-closed above. */
    /* TLS 1.2 key_block layout: client_mac, server_mac, client_key, server_key, client_iv, server_iv */
    /* Heap-allocate payload to avoid stack overflow (TLS_CAP=16K, kernel stack=16K) */

## /kernel/power/power.c

/* FADT (FACP) field offsets used for S5: PM1a_CNT_BLK is at byte 64,
 * behavior, now with the mandatory SLP_EN bit). */
    /* Port 0x80 is the conventional CMOS-delay sink: harmless write. */
    /* 1. 8042 keyboard-controller reset pulse (port 0x64, cmd 0xFE).
     * Bound the busy-wait so a missing controller cannot hang us. */
        /* Give the controller a chance; if we are still alive, move on. */
    /* 2. Reset Control Register (port 0xCF9): 0x06 = reset+init,
     * then 0x0E = reset+full. Small delays between writes. */
    /* 3. Triple fault as a last resort: load an empty IDT and fault. */
        /* Bounds-checked FADT reads: both CNT blocks are 4-byte
         * little-endian port numbers. */
    /* 1. ACPI S5 first (correct on real HW): SLP_TYP=5 in bits 12:10
     * Write PM1b too when present. */
        /* Give the power state a moment to take effect. */
        /* Retry once in case the first write raced firmware. */
    /* 2. QEMU fallbacks (harmless on real HW: unassigned ports).
     * 0x604/0xB004 with 0x2000 is the QEMU poweroff sequence. */
    /* 3. QEMU isa-debug-exit: writing 0x31 to port 0x501 powers off the
     * hardware (port usually unassigned). */

## /kernel/power/power.h

/* Reboot the machine (8042 keyboard-controller reset). Never returns. */
/* Power off via ACPI S5 when a FADT is available; falls back to the
 * QEMU isa-debug-exit port (0x501) and finally a halt. Never returns. */

## /kernel/proc/elf.h

// Dynamic section tags
// x86-64 relocation types
/* ELF64 Rel (implicit addend) entry: 16 bytes */
/* ELF64 Rela (explicit addend) entry: 24 bytes */

## /kernel/proc/process.h

    /* External identity (P0 OS-ification, step 2). INERT for now: filled
     * child goes READY, so no gate can observe a half-set identity). */
    /* Real file-descriptor table for the Linux personality (B2).
     * helpers below lazily reserve stdio on first use. */
    /* Task-manager accounting: scheduler ticks consumed and user memory */
    /* x87/SSE register image (fxsave layout), swapped by fpu_switch()
     * so the 16-byte alignment fxsave needs holds by construction. */

## /kernel/proc/sched.asm

; void switch_context(thread_t *prev, thread_t *next)
;                      rdi              rsi
;
; thread_t layout:
;   offset 0 : kernel_rsp

## /kernel/proc/sched.c

    /* Per-process CPU accounting: the timer fires at 100 Hz, so each
     * tick a thread runs counts as one cpu_tick for its owner. */
/* Reap EXITED processes nobody will ever wait on: children whose
 * only orphans. */
    /* The timer IRQ that triggers a VT switch can fire while the
     * the current thread's owner. */

## /kernel/proc/sched.h

/* IRQ-context safe: marks the current user process (and every other user
 * the app for the newly selected virtual terminal. */

## /kernel/proc/user.c

        // ICX blobs need writable pages for .data/.bss sections
        /* For ET_DYN, always apply the load bias so the binary maps at
         * USER_TEXT_BASE regardless of its link-time p_vaddr. */
    /* Apply relocations for ET_DYN (PIE) binaries. The user-space .app files
     * added. Handle both DT_REL (implicit addend) and DT_RELA (explicit). */
        /* Helper: kernel virtual pointer for a relocated user virtual address. */
    /* External identity (B3: before READY, so no gate ever observes a
     * to uid 0 / session leader with a fresh monotonic token. */
    /* Task-manager label: the basename of the spawned image. */

## /kernel/proc/user_programs.asm

; CI_IMAGE selects the test-image extras (gui_demo, nptest, nptestlx).
; The Makefile passes -DCI_IMAGE=0 for production, =1 for CI.
; Production builds embed product apps only.

## /kernel/syscall/native_abi.h

/*
 */
/* Count of native calls: numbers 0..69 inclusive. */

## /kernel/syscall/syscall.c

/* Verbose serial tracing (per-mount / audio-claim identity logs).
 * Default off; enable with SERIAL_VERBOSE=1. Error paths always log. */
/* ABI freeze (native_abi.h v1): the native numbers below are a stable
 * enforces kernel/userspace sync. */
/* Framebuffer claim state lives in kernel/dev/devnodes.c alongside the
 * /dev/fb0 ops (moved out of the syscall gate in P0 OS-ification). */
    /* P0 gate: probe the NUL-terminated string before the console
     * layer scans it (unbounded read otherwise). */
/* Minimal serial u64 printer for identity-gate logging (no printf). */
/* P0 gate helpers: validated path (512B cap) and buffer range. */
    /* Readable device nodes produce their contents on demand. */
/* Shared directory formatter: writes one-per-line child entries of `dir`
 * Returns bytes written; optionally reports emitted entry count. */
    /* Count the pages this process actually has mapped in its own
     * and excluded by the accounting in vmm_map_page). */
    /* WAIT_VBLANK (bit 0): single sched_yield, never spin.
     * to let the scheduler run and give the CRTC time to scan. */
    /* Overflow-guarded image range probe (w*h*4 bytes). */
    /* 0 = shutdown, 1 = reboot.  Neither returns. */
    /* Identity gate, log-only (P0 step 2): record who mounts; no denial.
     * Verbose-only: enable with SERIAL_VERBOSE=1. */
    /* Bounded at the gate as well as in the driver: a huge ticks value
     * must never reach the speaker busy-wait (DoS via long spin). */
    /* Identity gate, log-only (P0 step 2): record who claims; no denial.
     * Verbose-only: enable with SERIAL_VERBOSE=1. */
/* P0 gate for the (host, path, out_path) string triple shared by the
 * classic remote-input vector, so bound them tightly. */
        case 0: { // read
        case 1: { // write
            /* Source buffer: read probe (a read-only source mapping is
             * legitimate here; chunks are re-probed per copy). */
                /* NUL-safe chunked console output: con_write scans
                 * for NUL, so never hand it raw user memory. */
        case 2: { // open
        case 3: // close
        case 5: { // fstat
        case 9: { // mmap
        case 10: { // mprotect ΓÇö real implementation with W^X
            /* Linux PROT_* bits. PROT_EXEC without PROT_READ is mapped
             * rejected: this kernel is W^X. */
                /* Re-map the same frame with new permissions. */
        case 11: { // munmap ΓÇö real implementation (P0/B3)
            /* Shared framebuffer window: device memory, not PMM-owned.
             * Never free it here; use SYS_MAP_FRAMEBUFFER/SYS_SHM_UNMAP. */
            /* SHM window: ref-counted shared frames owned by shm.c.
             * Detaching must go through SYS_SHM_UNMAP/SYS_SHM_CLOSE. */
                /* Present user pages here are always PMM-owned: text,
                 * excluded above. Non-present pages are skipped. */
        case 12: { // brk
        case 60: // exit
        case 231: // exit_group
        case 78: { // getdents ΓÇö fd-based (P0/B2)
            /* List the fd's own directory (not cwd), honoring the fd
             * offset so repeated calls page through entries. */
                    dirent[18] = 0; // DT_UNKNOWN
        case 158: // arch_prctl
/* Thin trap gate (P0 OS-ification): personality routing lives here and
 * never to new SYS numbers (see native_abi.h). */
        /* ---- IPC / GUI syscalls ---- */
            /* Claim + mapping policy lives in /dev/fb0 (devnodes.c);
             * the gate only validates the caller's info struct. */
            /* 1 once the window manager has claimed the framebuffer, so
             * GUI-capable apps know the desktop is on screen. */
            /* Mask to valid flag bits: sys_call0 does not set rdi,
             * so garbage must be zeroed.  flags=0 = legacy no-op. */

## /kernel/syscall/syscall.h

/*
 */
#define U_ENOENT  2   /* no such file or directory */
#define U_EBADF   9   /* bad file descriptor */
#define U_ENOMEM  12  /* out of memory / unmapped range */
#define U_EACCES  13  /* permission denied (incl. TLS refusing to connect) */
#define U_EFAULT  14  /* bad user-space address */
#define U_EINVAL  22  /* invalid argument */
    /* IPC / GUI ΓÇö added for desktop environment */
    /* GPU device layer (DRM/KMS-shaped general GPU driver) */
    /* Power management */
    /* Process stats for the task manager */
    uint64_t virt_addr;  /* userspace VA of mapped framebuffer */
    uint32_t pitch;      /* bytes per row */
    char     name[32];   /* driver name, e.g. "fbdev" */
    int32_t  width;      /* current mode */
    uint32_t hw_cursor;  /* device has a hardware cursor plane */
    uint32_t flip_active; /* 1 when tear-free page flipping is active */
    uint32_t needs_present; /* append-only ABI: 1 when present() does real DMA work */
    uint64_t cpu_ticks;  /* scheduler ticks consumed by this process */
    uint64_t mem_bytes;  /* user-space pages mapped, in bytes */
    char     name[64];   /* executable basename */
    uint8_t  buttons;    /* bit0=left, bit1=right, bit2=middle */

## /kernel/syscall/uaccess.h

/*
 */
/* Top of the user half (vmm.h layout: user is [0, 0x0000800000000000)). */
/* Upper bound for any single validated range (1 GiB). Rejects nonsense
 * lengths before the page walk. */
/* Upper bound for bounded string scans (1 MiB). */
/* Ensure one user page is safe for kernel access: present (and, when
 * page may be touched, 0 to fail closed. */
/* Validate + fault-in [addr, addr+len) for the given address space.
 * len == 0 is valid. Returns 1 (safe) or 0 (fail closed). */
/* Same, for the calling process. Trusted (non-user) callers bypass. */
/* Back-compat alias: unqualified uses are reads. */
/* Copy kernel -> user. Returns 0 on success, -1 with nothing touched
 * small bounded lengths). */
/* Copy user -> kernel. Returns 0 on success, -1 on failure. */
/* Bounded strlen on a user string. Every touched page is ensured first,
 * (uint64_t)-1 when the string runs past `max` or outside user memory. */

## /kernel/tty/tty.c

    /* Drop the F12 sentinel (0x80): the WM owns it; console/VT
     * never sees it. */

## /kernel/vt/vt.c

/*
 */
    /* Wipe the screen so the next app starts clean.  The GUI repaints
     * console is visible. */
    /* Force-exit every user process (the foreground app and whatever it
     * app for the newly active VT. */

## /kernel/vt/vt.h

/*
 */
/* Called from keyboard IRQ context to request a switch (1..VT_TEXT_MAX). */
/* Called from the timer IRQ every tick; applies a pending switch. */
/* Path of the app that should run on the active VT. */
#endif /* KERNEL_VT_H */

## /scripts/Enable-ICDA-Virt.ps1

# Enable-ICDA-Virt.ps1 - enable virtualization stack for Docker Desktop WSL backend
# Works without winget (IoT LTSC has no Store). Must run elevated.
# Usage (elevated PowerShell): powershell -ExecutionPolicy Bypass -File Enable-ICDA-Virt.ps1
# Optional Hyper-V fallback:  powershell -ExecutionPolicy Bypass -File Enable-ICDA-Virt.ps1 -HyperV

## /scripts/check-abi.sh

#!/usr/bin/env sh
# scripts/check-abi.sh ΓÇö fail when the native syscall numbers drift
# between kernel/syscall/syscall.h and userspace/icda_sys.h.
# The ABI is frozen (see kernel/syscall/native_abi.h); run this in CI
# and before pushing any syscall-layer change.
# Strict enum parse: lines like "    SYS_FOO       = 12," (trailing comma
# optional, so the last entry matches too).
# Userspace mirrors via `SYS_FOO` tokens in the sys_call wrappers
# (excluding the USERSPACE_ICDA_SYS_H include guard).
# Every kernel number must appear in userspace...
# ...and userspace must not reference unknown numbers.

## /scripts/gen-compile-commands.ps1

#>
    # Sources with no Makefile rule: clone the userspace flags from a
    # parsed entry, swapping only the input/output paths.
            # Sanity: template substitution must have replaced the source.
    # UTF-8 without BOM: clangd and most editors prefer it.

## /scripts/gen_fonts.py

#!/usr/bin/env python3
# Codepoints beyond printable ASCII that UI strings may use (UTF-8).
# (enum suffix, file key, pixel size) - order defines ic_font_style_t.
    # Kerning per font file, in font units.

## /scripts/gen_icons.py

#!/usr/bin/env python3
    # 1 px inner highlight along the top edge, fading down the sides.
    # Hairline edge so light tiles hold their shape on light wallpapers.
# --------------------------------------------------------------- app icons
        # Pencil at 45 degrees: orange body, dark tip.
# ----------------------------------------------------------- object icons
    body += bytes(SIZE * SIZE // 8)  # AND mask: alpha channel decides

## /scripts/gen_sounds.py

#!/usr/bin/env python3
    # Warm power-on: E5 (659.25) then B5 (987.77) overlapping, soft 3rd.
    # Soft notification ping: A5 + octave shimmer, fast bloom, ~1 s tail.
    # Original music-box arpeggio (own melody, 8 eighth-notes at 100 bpm):
    #   C5 E5 G5 B5 A5 G5 E5 D5, gentle overlap + sparkle octave.

## /scripts/gui-check.py

#!/usr/bin/env python3
        # The emulated PS/2 mouse takes relative motion only, and each
        # packet carries a signed 8-bit delta, so walk there in steps.
        # Send each axis as its own input-send-event: QEMU 7.2's PS/2
        # mouse silently drops the y event when x and y share a call.
        # QEMU can also lose the very first rel event after a fresh
        # boot, so the driver issues a throwaway warm-up move once.
        # QEMU 7.2 wants an object: {"type": "qcode", "data": name}

## /scripts/gui-cursor-click.py

#!/usr/bin/env python3
            # black outline corner above-left of the tip
    # the true tip is the uppermost-leftmost candidate; drop clusters that
    # are just the diagonal body of another candidate
    # click: press + release at current spot

## /scripts/hmp-tool.pl

#!/usr/bin/perl
# Generic HMP driver for interactive testing of ICDA.
# Args: sequence of ops:
#   k:<keys>        -> sendkey <keys>
#   m:<dx>,<dy>     -> mouse_move dx dy
#   r:<n>x<dx>,<dy> -> n mouse_move steps of dx,dy each
#   b:<mask>        -> mouse_button <mask>
#   s:<name>        -> screendump /workspace/<name>.ppm
#   w:<secs>        -> sleep

## /scripts/icda-arch.sh

#!/usr/bin/env sh
# scripts/icda-arch.sh ΓÇö native Arch Linux workflow (no Docker needed).
#
# Mirrors the scripts/icda.cmd surface for Arch:
#   ./scripts/icda-arch.sh ready [--headless] [--uefi]   check, build, smoke, run qemu
#   ./scripts/icda-arch.sh check                          verify host dependencies
#   ./scripts/icda-arch.sh install                        sudo pacman -S the missing deps
#   ./scripts/icda-arch.sh build                          make kernel.iso kernel-usb.img
#   ./scripts/icda-arch.sh smoke                          build + QEMU smoke test
#   ./scripts/icda-arch.sh qemu [--headless] [--uefi] [--sdl|--gtk|--vnc]
#                                                         interactive QEMU run
#   ./scripts/icda-arch.sh clean                          make clean
#
# Run GUI targets (qemu/ready) as YOUR USER, never under sudo: root has
# no access to your Wayland/X11 session, so QEMU silently falls back to
# a VNC server ("VNC server running on ::1:5900") instead of a window.
# Only `install` needs root (it re-execs sudo itself).
#
# Speed: KVM is used automatically when /dev/kvm is accessible,
# otherwise QEMU falls back to TCG (slow but works).
# GUI actions must not run as root (no display access -> silent VNC).
# Display backend selection. QEMU's default (GTK) is flaky on Wayland
# compositors such as Hyprland and, worse, QEMU silently falls back to
# a VNC server when no display opens. So: SDL on Wayland (solid
# Wayland backend), GTK elsewhere, explicit --sdl/--gtk/--vnc override.
# Prints e.g. `-display sdl` (empty for GTK default).
# Fail fast when a local window is requested but no display session is
# reachable (instead of booting into an invisible desktop). Skipped for
# headless runs and explicit --vnc.
# Extra make overrides: KVM accel + display backend when available,
# Arch OVMF path for UEFI.
# NOTE: the QEMU assignment contains spaces, so call sites must expand
# it quoted:  make target "$(qemu_assign)" $(ovmf_assign)
        # shellcheck disable=SC2086
            # shellcheck disable=SC2086

## /scripts/icda.ps1

        # The requirement check below will report the missing Docker CLI and
        # trigger the installer path.

## /scripts/icda.sh

#!/usr/bin/env sh

## /scripts/install-requirements.sh

#!/usr/bin/env sh

## /scripts/ppm2png.py

#!/usr/bin/env python3

## /scripts/qemu-smoke.sh

#!/usr/bin/env sh
# 120 s: the tree takes ~60 s to reach the shell even on KVM (audio and
# storage probes), and TCG hosts need far longer. Override per-run with
# QEMU_TIMEOUT= (CI uses 600).

## /scripts/qmp-input.ps1

#>
    # Pacing between deltas.  gui-check.py sleeps 1.2 s per step because
    # the guest drops rel events that arrive faster than it can service
    # them; under TCG a much shorter delay silently loses motion and the
    # cursor lands somewhere else than requested.
    # PS/2 motion is lossy: a long move can land short, so a blind click
    # after -Move may miss its target.  -Sync re-reads the cursor out of
    # the framebuffer and corrects until it agrees, instead of trusting
    # gui-cursor.txt to still be accurate.
#>
    # Bucket changed pixels into 16x16 cells, then merge adjacent cells
    # into clusters by flood fill over the occupied grid.
    # Flood fill the occupied cells.
#>
    # The ICDA cursor is a solid white arrow over a dark rim, so scan a
    # coarse grid for near-white clusters.  The arrow's tip is its
    # topmost-leftmost white pixel, so scanning top-down and keeping the
    # first strong block finds the tip rather than the wider tail.
    # NOTE: the parameter must not be called $args - that is a PowerShell
    # automatic variable, and shadowing it makes the value arrive as an
    # object[] that serializes to a JSON array, which QMP rejects with
    # "input member 'arguments' must be an object".
        # Warm-up: QEMU sometimes drops the first rel event after boot.
            # Measure where the guest really put the pointer instead of
            # trusting our arithmetic: jiggle by a known delta and diff
            # two frames.  The changed pixels are the old and new cursor
            # spots, so their centroid is our position plus half the
            # jiggle.  Immune to the light text all over this UI.
            # A small jiggle is itself liable to be dropped by the PS/2
            # mouse, which reads as "no motion".  Use a delta large enough
            # to survive; the centroid correction scales with it.
        # wm.c uses DBLCLICK_TICKS 40 at a 100 Hz tick, i.e. 400 ms
        # between the two button-down events.  Keep the whole pair well
        # inside that or the WM reads it as two single clicks.

## /scripts/qmp-screendump.ps1

#>
# Normalise to an absolute Windows path with forward slashes: QEMU's
# screendump parses it as a POSIX-ish path and chokes on backslashes.
        # Read until we get the reply matching our request id.

## /scripts/qmp-unix.py

#!/usr/bin/env python3

## /scripts/qmpc.c

/* Tiny QMP client over a unix socket.  Usage:
 */
    /* QMP sends a greeting immediately on connect; consume it, then
     * negotiate capabilities and consume that reply too. */
        /* PS/2 mouse: relative motion, sent as separate events */

## /scripts/run-selftest-gates.sh

#!/usr/bin/env sh
# scripts/run-selftest-gates.sh - run the CI self-test gates locally.
#
# The CI gates in .github/workflows/qemu.yml assume a fresh checkout:
# CI_SELFTEST=1 / CI_IMAGE=1 are make *variables*, not file dependencies,
# so on an already-built tree `make kernel.iso CI_SELFTEST=1` reports
# "up to date" and silently reuses a kernel.bin built with the self-test
# compiled out.  This script therefore cleans first, which is what makes
# the NPTEST output appear at all.
    # The patch is undone by a trap, not at the end of the body: if this
    # script is killed mid-gate (timeout, OOM, Ctrl-C) the container's
    # /tmp dies with it and the bind-mounted grub.cfg would be left
    # patched.  Saving the pristine text in a variable keeps the undo
    # independent of any file that lives only inside the container.

## /scripts/visual-key.sh

#!/bin/sh
# Diagnostic: does QMP input reach the guest at all?
# Ctrl+Alt+F2 should switch to the text-shell VT (very visible).
  # hold ctrl+alt, tap f2

## /userspace/argc_elf.asm


## /userspace/audiod.c

/*
 */
static uint32_t win_start;      /* source frame of window start */
static uint32_t win_frames;     /* frames actually read into window */
    /* no file logging in production - keeps the root directory clean
     * and avoids a VFS write on every state transition. */
/* Minimal RIFF/WAVE reader over the header buffer. */
/* Make sure source frames [frame, frame + needed) are in the window. */

## /userspace/audioplay.c

/*
 */
    /* view */
    /* pointer */
    int  playing;              /* the kernel reports an active stream */
/* ------------------------------------------------------------- layout */
/* ------------------------------------------------------------ helpers */
    /* Keep pointing at the same file across a rescan. */
/* ------------------------------------------------------------- actions */
/* Progress of the active stream, 0..1, or -1 when nothing is playing. */
/* ------------------------------------------------------------ drawing */
    /* The progress bar moves while a track plays. */
/* -------------------------------------------------------------- events */
            /* A plain click plays, like every other music player. */
    /* `user` carries argv[1] when the shell opened a specific file. */

## /userspace/browser.c

/*
 */
/* Links are byte ranges in the render text; the clickable rect is
 * computed at draw time from the proportional layout. */
    int  scroll;               /* pixels scrolled down the page */
    int  content_h;            /* full page height in pixels */
    int  wrap_cols;            /* characters per wrapped line */
    /* pointer */
    /* The address field's text is const in ic_textfield_t, so the
     * editable buffer lives here and the field points at it. */
/* --------------------------------------------------------------- HTML */
    /* Entity strings built char-by-char so the source survives any
     * HTML-escaping in the editor/toolchain. */
/* ------------------------------------------------- HTML -> render text */
static uint64_t rt_i;            /* write index into br.text */
static uint64_t rt_line_start;   /* where the current visual line began */
static uint64_t rt_last_space;   /* offset of the last emitted space */
static int      rt_cols;         /* wrap width in characters */
/* Tags whose whole subtree carries no displayable text. */
/* Tags that introduce a line break in the text view. */
    if (br.text[rt_i - 1] == '\n') return;      /* collapse blank lines */
/* Map a Unicode codepoint to something the atlas can show. */
        case 0x2013: case 0x2014: c = '-'; break;   /* en/em dash */
/* Turn a raw href into an absolute URL against the current page. */
        /* Relative to the current path's directory. */
/* Build the word-wrapped plain-text view of the fetched page. */
    if (!ic_strprefix(url, "http://") && !ic_strprefix(url, "https://")) {
/* ------------------------------------------------------------- layout */
/* One place for the reading column, the wrap width and the document
 * height: draw(), the resize reflow and the hit-test all use it. */
    if (col_w > 720) col_w = 720;              /* comfortable measure */
/* ------------------------------------------------------------ drawing */
/* Byte offset of the start of absolute line `row` in the render text. */
        /* Body copy, then the accent-coloured link spans on top. */
/* -------------------------------------------------------------- events */
            /* Clicking elsewhere drops focus from the address field. */
        /* Links. */
                /* Walk the line to find the byte under the pointer. */
        /* The wrap width changed, so reflow the page and re-derive the
         * document height. */
    /* Page buffers come from shared-memory regions: process-owned memory
     * that is properly mapped. */

## /userspace/crt0.asm

; Shared userland entry point.
; The kernel places argc at [rsp] and argv at [rsp+8] on the initial
; stack (see user_build_initial_stack in kernel/proc/user.c). We pass
; both to main() and exit with main's return value.

## /userspace/desktop.c

/*
 */
/* Icon grid vs detail list. */
/* The small modal that asks for one line of text. */
/* Context-menu actions.  Delete is deliberately absent: the VFS has no
 * (scripts/check-abi.sh), so there is no honest way to offer it yet. */
    int       selected;          /* index into items, -1 none */
    int       hover;              /* index under the pointer, -1 none */
    int       history_pos;       /* where we are in the history list */
    int       scroll;             /* first visible row (grid) or item (list) */
    int       rows;               /* visible grid rows / list rows */
    int       cols;               /* grid columns */
    int       first_item;         /* first drawn index (list view) */
    /* pointer */
    /* modal text prompt */
    /* context menu */
    int  menu_item;               /* -1 = the folder itself */
    /* Get Info */
/* ------------------------------------------------------------- layout */
/* The action buttons are right-aligned and the path field takes what is
 * boots at 800x600) they shorten rather than truncating to "New Fol...". */
/* Places in the sidebar. */
/* The sidebar caption sits above the first item, so items start below it
 * rather than at the same offset. */
        /* Page the grid so the selection stays on screen. */
/* ------------------------------------------------------------ helpers */
/* ---------------------------------------------------------- browsing */
/* ------------------------------------------------------------ actions */
/* Rename is copy-then-truncate: the VFS has no rename primitive and the
 * The status line says so rather than pretending the file is gone. */
/* ------------------------------------------------------------- dialog */
/* ------------------------------------------------------------ drawing */
        /* Trim the leading slash for display: "/" stays as "Disk". */
/* A folder, an app, a track or a document: pick the glyph by type. */
/* -------------------------------------------------------------- events */
        /* Reject anything outside the content area *before* dividing.
         * selects a grid cell instead. */
    /* The leading item count depends on whether an item was targeted. */
        i = 3; /* past the separator */
            /* No unlink in the VFS, so say so instead of faking it. */

## /userspace/diskman.c

/*
 */
/* Role codes for icda_set_partition_role. */
/* Actions that need a confirmation. */
    int        focus_parts;      /* the partition table has the selection */
    int        runtime_device;   /* the disk the system is running from, or -1 */
    /* view */
    /* pointer */
    /* confirmation */
    int  action;                 /* DA_* */
    int  action_arg;             /* fs_type or role code */
    int  alert_hover;            /* 0 cancel, 1 confirm */
/* ------------------------------------------------------------- layout */
/* The five action buttons share the rest of the toolbar. */
/* The selected device is protected when it holds the running system. */
/* Height of the pane title face, so the detail header and the table
 * below it share one measured origin. */
/* ------------------------------------------------------------ helpers */
/* "931.5 GB" from a sector count. */
/* Read a `key=value` token out of a kernel storage line. */
/* The kernel prints one line per device and one per partition, each
 * starting with two spaces and "index: ". */
    /* Prefer a real disk over the live image, the way a human would. */
/* ------------------------------------------------------------- actions */
/* The alert's title, message and target, from the pending action. */
/* ------------------------------------------------------------ drawing */
    /* The two formats act on the focused list: device or partition. */
    /* Roles only make sense on a partition. */
/* -------------------------------------------------------------- events */
            if (d >= 0) dm.hover_device = -100 - d;    /* negative: a device */
        /* The formats follow whichever list has the selection. */

## /userspace/editor.c

/*
 */
    uint64_t cursor;          /* byte offset */
    uint64_t want_col;        /* sticky column for vertical moves */
    /* view */
    /* pointer */
/* ------------------------------------------------------------- layout */
/* Everything geometric, from one place. */
/* ----------------------------------------------------- buffer helpers */
/* Keep the cursor inside the visible area, both ways. */
/* ------------------------------------------------------------ editing */
    /* The write returns a byte count, or (uint64_t)-1 on failure. */
/* ------------------------------------------------------------ drawing */
    /* Gutter background, then the line numbers of the visible rows. */
    /* Text rows, clipped horizontally to the viewport.  Rows are drawn
     * per visible run keeps a full 1920x1080 document cheap. */
            /* Extend the run while the bytes are printable and on screen. */
                /* The run ends at screen column `end_col` (exclusive). */
/* -------------------------------------------------------------- events */
/* Map a click inside the text area to a byte offset. */
        /* Explorer "Open With Editor" passes the file on the command line. */

## /userspace/font.h

/* Bitmap font ΓÇö identical data to kernel/drivers/display/font.h */
#define FONT_FIRST  32   /* space */
#define FONT_LAST  126   /* tilde */
    /* 32 space */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 33 !     */ {0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 34 "     */ {0x00,0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 35 #     */ {0x00,0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00},
    /* 36 $     */ {0x00,0x00,0x18,0x7E,0xDB,0xD8,0x7E,0x1B,0xDB,0x7E,0x18,0x00,0x00,0x00,0x00,0x00},
    /* 37 %     */ {0x00,0x00,0x00,0xC6,0xC6,0x0C,0x18,0x30,0x60,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00},
    /* 38 &     */ {0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00,0x00},
    /* 39 '     */ {0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 40 (     */ {0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00},
    /* 41 )     */ {0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
    /* 42 *     */ {0x00,0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 43 +     */ {0x00,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 44 ,     */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00},
    /* 45 -     */ {0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 46 .     */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    /* 47 /     */ {0x00,0x00,0x06,0x06,0x0C,0x0C,0x18,0x30,0x60,0x60,0xC0,0x00,0x00,0x00,0x00,0x00},
    /* 48 0     */ {0x00,0x00,0x3C,0x66,0xC3,0xC3,0xDB,0xDB,0xC3,0x66,0x3C,0x00,0x00,0x00,0x00,0x00},
    /* 49 1     */ {0x00,0x00,0x18,0x38,0x58,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00,0x00},
    /* 50 2     */ {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC6,0xFE,0x00,0x00,0x00,0x00,0x00},
    /* 51 3     */ {0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 52 4     */ {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00,0x00},
    /* 53 5     */ {0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 54 6     */ {0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 55 7     */ {0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x00,0x00,0x00,0x00,0x00},
    /* 56 8     */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 57 9     */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00,0x00},
    /* 58 :     */ {0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 59 ;     */ {0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
    /* 60 <     */ {0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00,0x00},
    /* 61 =     */ {0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 62 >     */ {0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00,0x00},
    /* 63 ?     */ {0x00,0x00,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    /* 64 @     */ {0x00,0x00,0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 65 A     */ {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00},
    /* 66 B     */ {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00,0x00},
    /* 67 C     */ {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00,0x00},
    /* 68 D     */ {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00,0x00},
    /* 69 E     */ {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x62,0x66,0xFE,0x00,0x00,0x00,0x00,0x00},
    /* 70 F     */ {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0xF0,0x00,0x00,0x00,0x00,0x00},
    /* 71 G     */ {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00,0x00,0x00},
    /* 72 H     */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00},
    /* 73 I     */ {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00},
    /* 74 J     */ {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00,0x00},
    /* 75 K     */ {0x00,0x00,0xE6,0x66,0x6C,0x6C,0x78,0x6C,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00,0x00},
    /* 76 L     */ {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x62,0x66,0x66,0xFE,0x00,0x00,0x00,0x00,0x00},
    /* 77 M     */ {0x00,0x00,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00},
    /* 78 N     */ {0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00},
    /* 79 O     */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 80 P     */ {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00,0x00},
    /* 81 Q     */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0xCC,0x76,0x00,0x00,0x00,0x00,0x00},
    /* 82 R     */ {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00,0x00,0x00},
    /* 83 S     */ {0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 84 T     */ {0x00,0x00,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00},
    /* 85 U     */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 86 V     */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00,0x00,0x00,0x00,0x00},
    /* 87 W     */ {0x00,0x00,0xC6,0xC6,0xC6,0xD6,0xD6,0xFE,0xEE,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00},
    /* 88 X     */ {0x00,0x00,0xC6,0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00},
    /* 89 Y     */ {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x78,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00,0x00},
    /* 90 Z     */ {0x00,0x00,0xFE,0xC6,0x86,0x0C,0x18,0x30,0x62,0xC6,0xFE,0x00,0x00,0x00,0x00,0x00},
    /* 91 [     */ {0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00,0x00,0x00},
    /* 92 bslsh */ {0x00,0x00,0xC0,0xC0,0x60,0x60,0x30,0x18,0x0C,0x06,0x06,0x00,0x00,0x00,0x00,0x00},
    /* 93 ]     */ {0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00,0x00,0x00},
    /* 94 ^     */ {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 95 _     */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00,0x00},
    /* 96 `     */ {0x00,0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 97 a     */ {0x00,0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00,0x00},
    /* 98 b     */ {0x00,0x00,0xE0,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0xDC,0x00,0x00,0x00,0x00,0x00},
    /* 99 c     */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 100 d    */ {0x00,0x00,0x1C,0x0C,0x0C,0x7C,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00,0x00},
    /* 101 e    */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 102 f    */ {0x00,0x00,0x1C,0x36,0x30,0x78,0x30,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00,0x00},
    /* 103 g    */ {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0x00,0x00,0x00},
    /* 104 h    */ {0x00,0x00,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00,0x00},
    /* 105 i    */ {0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00},
    /* 106 j    */ {0x00,0x00,0x06,0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x66,0x3C,0x00,0x00,0x00,0x00},
    /* 107 k    */ {0x00,0x00,0xE0,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00,0x00},
    /* 108 l    */ {0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00},
    /* 109 m    */ {0x00,0x00,0x00,0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0xC6,0x00,0x00,0x00,0x00,0x00},
    /* 110 n    */ {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00},
    /* 111 o    */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 112 p    */ {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00,0x00,0x00},
    /* 113 q    */ {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0x00,0x00,0x00},
    /* 114 r    */ {0x00,0x00,0x00,0x00,0x00,0xDC,0x76,0x66,0x60,0x60,0xF0,0x00,0x00,0x00,0x00,0x00},
    /* 115 s    */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0x70,0x1C,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 116 t    */ {0x00,0x00,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00,0x00},
    /* 117 u    */ {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00,0x00},
    /* 118 v    */ {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00,0x00,0x00,0x00,0x00},
    /* 119 w    */ {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00,0x00,0x00,0x00,0x00},
    /* 120 x    */ {0x00,0x00,0x00,0x00,0x00,0xC6,0x6C,0x38,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00},
    /* 121 y    */ {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8,0x00,0x00,0x00},
    /* 122 z    */ {0x00,0x00,0x00,0x00,0x00,0xFE,0xCC,0x18,0x30,0x66,0xFE,0x00,0x00,0x00,0x00,0x00},
    /* 123 {    */ {0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00,0x00},
    /* 124 |    */ {0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    /* 125 }    */ {0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00,0x00},
    /* 126 ~    */ {0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
/* Draw a single character into a 32bpp pixel buffer.
 * pitch is the number of BYTES per row of the buffer. */
#endif /* USERSPACE_FONT_H */

## /userspace/font_atlas.h

/* Auto-generated Proportional Anti-Aliased Font Atlas for ICDA */
    const ic_glyph_t *glyphs; /* [32..126] */
#endif /* USERSPACE_FONT_ATLAS_H */

## /userspace/gui.c

/* Staging buffer.  Apps draw into this private copy instead of the shared
 * screen; larger requests fall back to drawing straight into the SHM. */
    /* Draw into the staging copy when it fits (the normal case); the
     * WM sees only committed frames via gui_flush(). */
        /* Commit the finished frame: one tight copy beats the WM
         * catching us between draw steps. */
        /* Unmap our side only.  The WM also has this region mapped (it
         * mapping and closes the region when it processes CLOSE_WINDOW. */

## /userspace/gui.h

/*
 */
/* Open a window. Returns 0 on success, -1 on failure. */
/* Get direct pointer to the 32bpp ARGB pixel buffer. */
/* Tell the WM this window's buffer has changed and needs compositing. */
/* Poll for an event. Returns 1 if event was written to *out, 0 if none. */
/* Block until an event arrives, then fill *out. */
/* Close the window and release resources. */
/* -- Convenience drawing helpers (draw into gui_pixel_buffer()) -- */
#endif /* GUI_H */

## /userspace/gui_demo.c

/*
 */
/* A gallery row: a left-hand label and a right-hand control. */
    ROW_BUTTONS = 0,   /* a row of buttons of every style */
    ROW_TOGGLE,         /* one switch */
    ROW_SEGMENTED,      /* a two-option segmented control */
    ROW_SLIDER,         /* a slider with its value */
    ROW_PROGRESS,       /* a determinate progress bar */
    ROW_TEXTFIELD,      /* an editable single-line field */
    ROW_LIST,           /* a short list with a selection */
    ROW_TABLE,          /* a table header plus two rows */
    ROW_SIDEBAR,        /* a source list */
    ROW_SYMBOLS,        /* the symbol set */
    ROW_MENU            /* a rendered menu, for review */
    int         selected_row;        /* ROW_LIST selection */
/* Menu rendering needs a scratch buffer for the backdrop blur. */
/* ------------------------------------------------------------- layout */
/* The y of group i, from the row heights above it.  Every group is one
 * label row plus a control row, so the table is regular. */
/* Control rects inside a group, all derived from the group's top. */
/* ------------------------------------------------------------ drawing */
    if (ctrl + h < 0 || top > gd.view_h) return;      /* off screen */
    /* The gallery scrolls under a small title strip. */
    /* The indeterminate progress stripe and the caret both keep moving. */
/* -------------------------------------------------------------- events */
        /* The gallery is a live preview: nothing to reload. */

## /userspace/gui_proto.h

/* Well-known queue name the WM listens on */
/* GUI message types */
#define GUI_MSG_OPEN_WINDOW    1   /* app -> WM: request a new window */
#define GUI_MSG_OPEN_OK        2   /* WM -> app: window created */
#define GUI_MSG_OPEN_FAIL      3   /* WM -> app: window creation failed */
#define GUI_MSG_CLOSE_WINDOW   4   /* app -> WM or WM -> app */
#define GUI_MSG_FLUSH          5   /* app -> WM: mark window dirty, repaint */
#define GUI_MSG_KEY_EVENT      6   /* WM -> app: key press/release */
#define GUI_MSG_MOUSE_EVENT    7   /* WM -> app: mouse in window coords */
#define GUI_MSG_RESIZE         8   /* WM -> app: window was resized */
#define GUI_MSG_FOCUS          9   /* WM -> app: gained/lost focus */
/* Mouse button bits */
/*
 */
    uint32_t type;        /* GUI_MSG_* */
    uint32_t window_id;   /* identifies which window */
        /* GUI_MSG_OPEN_WINDOW: app -> WM */
        } open_req;       /* 40 bytes */
        /* GUI_MSG_OPEN_OK: WM -> app */
            uint64_t shm_handle;  /* kernel SHM handle for pixel buffer */
            uint64_t reply_queue; /* WM-assigned queue handle for this window */
        } open_ok;        /* 40 bytes */
        /* GUI_MSG_KEY_EVENT */
            uint32_t keycode;     /* ASCII or special key code */
            uint8_t  pressed;     /* 1=press, 0=release */
        } key;            /* 40 bytes */
        /* GUI_MSG_MOUSE_EVENT */
            int32_t  x, y;        /* position in window-local coords */
            uint8_t  buttons;     /* GUI_BTN_* bitmask */
        } mouse;          /* 40 bytes */
        /* GUI_MSG_FOCUS: WM -> app */
            uint8_t  focused;     /* 1 = gained focus, 0 = lost */
        } focus;          /* 40 bytes */
        /* GUI_MSG_RESIZE: WM -> app */
            uint64_t shm_handle;  /* new window pixel buffer */
        } resize;         /* 40 bytes */
        /* GUI_MSG_FLUSH: app -> WM (no extra payload) */
        /* GUI_MSG_CLOSE_WINDOW: no extra payload */
    uint8_t _tail[16]; /* padding to 64 bytes total */
/* Compile-time size check: sizeof(gui_msg_t) must equal 64 */
#endif /* GUI_PROTO_H */

## /userspace/ic_anim.c

/*
 */
/* ------------------------------------------------------------ easing */
    /* B(u) for a curve anchored at 0 and 1: 3(1-u)^2 u p1 + 3(1-u) u^2 p2 + u^3 */
    /* Solve x(u) = t: Newton first, bisection if the slope flattens. */
/* ------------------------------------------------------------- tween */
/* ------------------------------------------------------------ spring */
#define IC_SPRING_STEP_S  0.004f   /* integrate in <=4 ms slices */
#define IC_SPRING_MAX_DT  0.100f   /* a stalled frame must not explode */
#define IC_SPRING_REST_X  0.0015f  /* in units of the animated value */
        /* Semi-implicit Euler: stable for the stiffness range we use. */

## /userspace/ic_anim.h

/*
 */
/* ------------------------------------------------------------ easing */
    IC_EASE_STANDARD,     /* cubic-bezier(0.25, 0.10, 0.25, 1.00) */
    IC_EASE_DECELERATE,   /* cubic-bezier(0.00, 0.00, 0.20, 1.00) - things arriving */
    IC_EASE_ACCELERATE,   /* cubic-bezier(0.40, 0.00, 1.00, 1.00) - things leaving */
    IC_EASE_EMPHASIZED    /* cubic-bezier(0.20, 0.00, 0.00, 1.00) - large moves */
/* Evaluate a CSS-style cubic-bezier timing curve at progress t (0..1). */
/* ------------------------------------------------------------- tween */
/* Jump to a value with no animation. */
/* Animate from the tween's current value (so interrupting a running
 * tween never jumps) to `to`. */
/* Linear progress 0..1 (1 when idle). */
/* ------------------------------------------------------------ spring */
/* Damped harmonic oscillator, parameterised like modern UI toolkits:
 * overshoot; 0.7-0.85 = a gentle settle). */
/* Advance to now and return the current value. */
#endif /* USERSPACE_IC_ANIM_H */

## /userspace/ic_app.c

/*
 */
/* Escape-sequence decoder for keys the terminal-style keyboard driver
 * delivers as "ESC [ x". */
    int     state;     /* 0 idle, 1 got ESC, 2 got ESC[, 3 got ESC[n */
                /* gui_poll_event already switched the buffer. */
        /* A lone ESC with nothing after it in this batch is the Esc key. */

## /userspace/ic_app.h

/*
 */
/* Decoded keys: printable ASCII and control characters keep their byte
 * value; navigation keys get codes above 0xFF. */
    IC_EV_MOUSE_MOVE = 1,   /* x, y (also while a button is held) */
    IC_EV_MOUSE_DOWN,       /* x, y, button */
    IC_EV_MOUSE_UP,         /* x, y, button */
    IC_EV_MOUSE_LEAVE,      /* pointer left the window */
    IC_EV_KEY,              /* key */
    IC_EV_FOCUS,            /* window became key */
    IC_EV_BLUR,             /* window lost key status */
    IC_EV_RESIZE,           /* width/height changed (already applied) */
    IC_EV_APPEARANCE        /* palette changed (dark/light/accent) */
    uint8_t         button;   /* GUI_BTN_* for DOWN/UP */
    /* Called once after the window exists. */
    /* Paint the whole window into c. */
    /* Handle one event. */
    /* Optional: called every loop iteration (~10 ms) for background
     * work such as polling a child process. */
    int      mouse_x, mouse_y;  /* last position inside the window */
    uint8_t  buttons;           /* currently held GUI_BTN_* */
    /* private */
/* Schedule a redraw. */
/* Call from draw() while something is still moving: another frame will
 * follow at display rate. */
/* Caret blink phase; calling it from draw() also keeps the blink going. */
/* Restart the blink with the caret visible (after typing / moving). */
/* Close the window and return from ic_app_run. */
#endif /* USERSPACE_IC_APP_H */

## /userspace/ic_font.c

/*
 */
#define IC_GLYPH_ELLIPSIS (95)          /* first extra: U+2026 */
/* Decode one UTF-8 sequence at s (at most n bytes); returns glyph index
 * and stores the byte length consumed. */
/* Pen advance of the first n bytes in 1/64 px. */
/* Light text on dark backgrounds reads thin with plain sRGB blending;
 * a gentle coverage lift compensates (macOS-style stem darkening). */
            /* x^0.8 via x * x^-0.2 approximation is overkill here; a
             * blend towards sqrt reproduces the curve closely enough. */
    /* Centre the cap height; round so odd leftovers go below. */
        /* Truncate at the widest prefix that leaves room for "ΓÇª". */
        /* Hard line breaks end the line early. */

## /userspace/ic_font.h

/*
 */
    IC_FONT_CAPTION = 0,   /* 11 regular  - secondary metadata */
    IC_FONT_CAPTION_EMPH,  /* 11 medium   - badges, section labels */
    IC_FONT_FOOTNOTE,      /* 12 regular  - hints, status text */
    IC_FONT_BODY,          /* 13 regular  - default UI text */
    IC_FONT_BODY_EMPH,     /* 13 medium   - menu items, emphasised body */
    IC_FONT_HEADLINE,      /* 13 semibold - window titles, row titles */
    IC_FONT_SUBHEAD,       /* 15 regular  - reading text */
    IC_FONT_TITLE3,        /* 15 semibold - group headers */
    IC_FONT_TITLE2,        /* 17 semibold - panel headers */
    IC_FONT_TITLE1,        /* 22 semibold - page titles */
    IC_FONT_LARGE_TITLE,   /* 28 bold     - hero numbers, splash */
    IC_FONT_MONO,          /* 13 mono     - terminal, editor */
    IC_FONT_MONO_SMALL,    /* 12 mono     - dense code */
    uint8_t  w, h;      /* coverage mask size */
    int8_t   ox, oy;    /* mask top-left relative to pen on baseline */
    uint32_t off;       /* offset into the face's alpha array */
    uint8_t  l, r;      /* glyph indices */
    int16_t  units;     /* x adjustment in font units */
    uint8_t            px;        /* nominal size */
    const uint16_t    *adv;       /* advance per glyph, 1/64 px */
    const ic_fglyph_t *glyphs;    /* glyph-major, IC_FONT_SUBPIXEL phases each */
    const ic_fkern_t  *kern;      /* sorted by (l, r) */
/* Advance width of s in pixels (rounded up). */
/* Measure only the first n bytes. */
/* Bytes of s that fit in max_w pixels (whole characters). */
/* Draw s with its baseline at y; returns the advance in pixels. */
/* Baseline that optically centres a line of f inside [y, y + h). */
/* Single line inside r: vertically centred, aligned horizontally, and
 * truncated with an ellipsis when it does not fit. */
/* Word-wrapped paragraph starting with its first line box at r.y.
 * returns the height used.  Pass c == NULL to only measure. */
#endif /* USERSPACE_IC_FONT_H */

## /userspace/ic_fonts_gen.h

/* Generated by scripts/gen_fonts.py - do not edit. */
#endif /* USERSPACE_IC_FONTS_GEN_H */

## /userspace/ic_gfx.c

/*
 */
/* ------------------------------------------------------------ colour */
/* ------------------------------------------------------------ canvas */
    /* An empty clip must stay empty, not fall back to "everything". */
/* Clip a rect against the canvas; returns 0 if nothing is left. */
/* ------------------------------------------------------------ shapes */
/* Rounded-rect geometry with radii already clamped to fit. */
/* Coverage (0..1) of the pixel whose centre is (fx, fy). */
/* Column extent of the left/right corner zones for row py. */
/* ------------------------------------------------------------- blits */
    /* fx/fy are 16.16 texel coordinates of the sample. */
            /* Premultiply while filtering so transparent texels do not
             * bleed their (meaningless) colour into the edge. */
/* ------------------------------------------------------------ effects */
/* One box pass over n pixels with stride, radius r, edge-clamped. */
    /* 16.16 reciprocal avoids a divide per channel per pixel. */
/* --- shadow tiles --- */
    int     radius_q;  /* radius * 4, cache key */
    int     extent;    /* how far the shadow reaches past the shape edge */
    int     size;      /* tile edge = extent + radius + extent */
    /* The shape's top-left corner sits at (ext, ext) and the shape runs
     * treats it as an infinitely large rounded rect. */
        /* Pixels fully covered by the (opaque) shape itself are skipped;
         * corner squares still get shadow under the arc cut-outs. */
    /* Blur the whole shape (not just the clipped part) so the result is
     * stable no matter which damage rect is being repainted. */

## /userspace/ic_gfx.h

/*
 */
/* ------------------------------------------------------------ types */
/* A 32bpp surface.  pitch == w.  The clip rect is active when
 * canvases built with { px, w, h } keep working unchanged. */
/* Scale a colour's alpha by f (0..1). */
/* Mix two colours in RGB and alpha: t=0 -> a, t=1 -> b. */
/* Composite `top` over opaque `base`; result is opaque. */
/* ------------------------------------------------------------ canvas */
/* Intersect the current clip with a rect (for nested drawing). */
/* Effective drawable bounds (clip & canvas), as x0,y0 inclusive / x1,y1
 * exclusive.  Returns 0 when nothing is drawable. */
/* ------------------------------------------------------------ shapes */
/* Anti-aliased rounded rectangle.  Straight edges sit on pixel
 * boundaries (integer geometry); only the corner arcs are antialiased. */
/* Per-corner radii: top-left, top-right, bottom-right, bottom-left. */
/* Stroke `width` px wide, drawn inside the rect bounds. */
/* Round-capped line (capsule) - the building block for vector glyphs. */
/* Vertical gradient clipped to a rounded rect. */
/* ------------------------------------------------------------- blits */
/* Copy an opaque source (pitch in pixels) at `opacity` (0..255). */
/* As ic_gfx_blit, but the source is masked to a rounded rect with the
 * given per-corner radii (window content under rounded chrome). */
/* Bilinear scale of an opaque source into dst rect, with opacity and a
 * rounded mask radius (in destination pixels, all four corners). */
/* Per-corner mask radii: top-left, top-right, bottom-right, bottom-left. */
/* Blit a straight-alpha RGBA byte image (icons), scaled bilinearly. */
/* Blend an 8-bit coverage mask tinted with `color`. */
/* ------------------------------------------------------------ effects */
/* Approximate Gaussian blur (three box passes) of a region in place.
 * `scratch` must hold max(w, h) pixels. */
/* Soft drop shadow for a rounded rect.  `blur` is the Gaussian-like
 * the rest).  Shadow tiles are cached per (radius, blur). */
/* Translucent backdrop: blur the pixels under a rounded rect, then lay a
 * too small a scratch the blur is skipped and only the tint is drawn. */
#endif /* USERSPACE_IC_GFX_H */

## /userspace/ic_symbols.c

/*
 */
    int     n;        /* mask edge in px */
    float   scale;    /* px per unit */
    float   ox, oy;   /* mask-space origin of the unit box centre */
    float   stroke;   /* stroke width in px */
/* Unit-space -> mask-space. */
/* Filled disc (unit coords). */
/* Ring (stroke of a circle). */
/* Arc from angle a0 to a1 (radians, clockwise from +x in screen space),
 * approximated by short segments; max-accumulation hides the joints. */
    /* Rotate a unit vector incrementally: no libm needed. */
    /* cos/sin of a0 by the same series after range reduction */
        /* Taylor to 10th order is plenty on [-pi, pi] after halving. */
/* Filled convex polygon via 4x4 supersampling (small shapes only). */
/* Rounded-rect outline in unit coords (radius in units). */
/* The globe's meridian: a narrow ellipse traced as a polyline. */
        float t = (float)i / (float)steps;       /* 0..1 around half turn */
        /* cos/sin via short series on a in [-pi/2, pi/2] */
        /* 8 teeth as stubby radial strokes around a ring. */

## /userspace/ic_theme.c

/*
 */
    { 0x3A8BFF, 0x2F7BF0 },   /* blue */
    { 0xA46BF5, 0x8E52E3 },   /* purple */
    { 0xF0609E, 0xDC437C },   /* pink */
    { 0xF2555A, 0xE0434A },   /* red */
    { 0xF5923A, 0xE27A1F },   /* orange */
    { 0x3CC46A, 0x2DA25A },   /* green */
    { 0x8E8E93, 0x77777D },   /* graphite */
    /* contact,              ambient */
    { { 1, 1, 0x50 },       { 3, 1, 0x28 } },   /* control */
    { { 2, 1, 0x60 },       { 16, 8, 0x68 } },  /* menu */
    { { 3, 1, 0x70 },       { 28, 16, 0x78 } }, /* window */
    { { 3, 1, 0x50 },       { 18, 10, 0x4C } }, /* window idle */

## /userspace/ic_theme.h

/*
 */
/* ------------------------------------------------------------ spacing */
/* 4 px grid.  Use the named steps; odd values are a smell. */
/* ------------------------------------------------------------ radii */
#define IC_R_WINDOW     10.0f   /* window frame */
#define IC_R_PANEL      12.0f   /* launcher, popovers, dialogs */
#define IC_R_MENU        8.0f   /* context menus */
#define IC_R_MENU_ITEM   5.0f   /* highlighted row inside a menu */
#define IC_R_GROUP       9.0f   /* grouped settings cards */
#define IC_R_CONTROL     6.0f   /* buttons, fields, segmented controls */
#define IC_R_ROW         6.0f   /* selected list/sidebar row */
#define IC_R_TILE       12.0f   /* desktop/launcher icon highlight */
/* ------------------------------------------------------------ sizes */
#define IC_H_TITLEBAR   34      /* window title bar */
#define IC_H_TASKBAR    48      /* shell bar */
#define IC_H_CONTROL    26      /* push button, field, popup */
#define IC_H_CONTROL_SM 22      /* compact controls in toolbars */
#define IC_H_ROW        28      /* list / sidebar row */
#define IC_H_ROW_TALL   44      /* grouped row with a subtitle */
#define IC_H_TOOLBAR    44      /* in-window toolbar strip */
/* ------------------------------------------------------------ motion */
#define IC_DUR_INSTANT   80     /* hover / press feedback */
#define IC_DUR_FAST     140     /* menus, small fades */
#define IC_DUR_BASE     220     /* window open, toggles */
#define IC_DUR_SLOW     340     /* minimize, large moves */
/* Springs: response (s), damping ratio. */
/* ------------------------------------------------------------ elevation */
    int      blur;   /* spread */
    int      dy;     /* downward offset */
    uint32_t alpha;  /* peak opacity 0..255 */
    IC_ELEV_CONTROL = 0,   /* raised controls, toggle knob */
    IC_ELEV_MENU,          /* menus, popovers, launcher */
    IC_ELEV_WINDOW,        /* focused window */
    IC_ELEV_WINDOW_IDLE,   /* unfocused window */
/* Paint the two-layer shadow (tight contact + soft ambient) for a
 * rounded rect at the given elevation. */
/* Same, with the shadow's opacity scaled (0..1) for fades. */
/* ------------------------------------------------------------ palette */
    /* surfaces */
    ic_color_t desktop;          /* behind everything (no wallpaper) */
    ic_color_t window;           /* window content background */
    ic_color_t content;          /* inset content: lists, text areas */
    ic_color_t sidebar;          /* sidebars and source lists */
    ic_color_t group;            /* grouped rows (cards) */
    ic_color_t titlebar;         /* title bar and unified toolbars */
    ic_color_t material_menu;    /* tint over blur: menus, launcher */
    ic_color_t material_bar;     /* tint over blur: taskbar */
    /* controls */
    ic_color_t control;          /* push button face */
    ic_color_t control_stroke;   /* 1 px edge of controls and fields */
    ic_color_t field;            /* text field face */
    ic_color_t toggle_off;       /* switch track when off */
    ic_color_t knob;             /* switch knob, slider thumb */
    ic_color_t fill_hover;       /* translucent row/icon hover */
    ic_color_t fill_selected_idle; /* selection in an unfocused list */
    ic_color_t segment_track;    /* segmented control track */
    ic_color_t segment_selected; /* segmented control selected pill */
    ic_color_t scroller;         /* overlay scrollbar thumb */
    /* text */
    /* lines */
    ic_color_t separator;        /* hairlines between rows / sections */
    ic_color_t frame;            /* outer hairline around windows / menus */
    ic_color_t highlight;        /* inner top highlight on raised surfaces */
    ic_color_t group_stroke;     /* edge of grouped cards */
    ic_color_t bar_edge;         /* bottom edge of title bars / toolbars */
    /* accent and status */
    ic_color_t accent_soft;      /* tinted fills: selection behind text */
    ic_color_t close_hover;      /* title-bar close button hover */
/* Current palette (loads appearance/accent from the settings store the
 * first time).  The pointer stays valid; its contents change on reload. */
/* Re-read /cfg/icda-settings; returns 1 if the palette changed. */
/* Build a palette explicitly (Settings previews, tests). */
/* Swatch colour for an accent choice (Settings UI). */
#endif /* USERSPACE_IC_THEME_H */

## /userspace/ic_time.c

/*
 */
#define IC_TICK_NS       10000000ULL  /* 100 Hz scheduler tick */
/* Nanoseconds per TSC cycle as 32.32 fixed point. */
/* Block until the next tick boundary.  Sleeping (not yield-spinning)
 * taken after each wake sits at the same phase of the tick. */
    /* 64x64->128 multiply: a slow emulated TSC makes the rate large
     * enough that a 64-bit product would overflow within seconds. */
/* ---- wall clock ---- */
/* Monday-based weekday (Sakamoto's method). */
    dow = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;   /* 0 = Sunday */

## /userspace/ic_time.h

/*
 */
/* Calibrate the TSC against the scheduler tick.  Blocks for about two
 * only useful to move the cost to a convenient moment (app start). */
/* Monotonic time since the first ic_time_* call. */
/* Seconds as float, for animation math. */
/* ---- wall clock (read from /dev/rtc) ---- */
    int year, month, day;       /* month 1..12 */
    int weekday;                /* 0 = Monday .. 6 = Sunday */
/* Returns 0 on success, -1 when no clock is available. */
/* "14:05" */
/* "Mon 28 Sep" */
#endif /* USERSPACE_IC_TIME_H */

## /userspace/ic_ui.c

/*
 */
/* ------------------------------------------------------------ buttons */
/* ------------------------------------------------------------ toggle */
    /* knob: soft contact shadow, then face */
/* ------------------------------------------------------------ segmented */
/* ------------------------------------------------------------ slider */
/* ------------------------------------------------------------ text field */
/* ------------------------------------------------------------ containers */
/* ------------------------------------------------------------ panels */
/* ------------------------------------------------------------ menus */
/* ------------------------------------------------------------ alert */

## /userspace/ic_ui.h

/*
 */
/* ------------------------------------------------------------ symbols */
/* A small vector symbol set drawn with antialiased strokes, sized by
 * size so they sit with Inter at matching sizes. */
/* Draw a symbol centred on (cx, cy) inside a size x size box. */
/* ------------------------------------------------------------ controls */
/* Derive a state from pointer facts. */
    IC_BUTTON_DEFAULT = 0,  /* neutral push button */
    IC_BUTTON_PRIMARY,      /* the one default action: accent fill */
    IC_BUTTON_DESTRUCTIVE,  /* irreversible action: danger text */
    IC_BUTTON_PLAIN         /* borderless: toolbars, inline actions */
/* Width a button needs for its label (+ optional symbol). */
/* Square borderless icon button (toolbar). */
/* Switch.  `on` is the animated position 0..1 (drive with a tween of
 * IC_DUR_BASE); the knob slides and the track cross-fades. */
/* Segmented control; `slide` is the animated selection position
 * (float index) so the selection pill glides between segments. */
/* Horizontal slider, value 0..1. */
/* Progress bar, value 0..1 (negative: indeterminate stripe at phase). */
/* Single-line text field.  `scroll_px` is the horizontal text scroll
 * ic_ui_textfield_scroll to update it). */
    int         cursor;        /* byte offset */
    int         sel_start;     /* == sel_end: no selection */
    int         caret_on;      /* blink phase */
    ic_symbol_t leading;       /* IC_SYM_SEARCH for search fields */
/* Byte offset nearest to a click at x. */
/* Focus ring around a control shape. */
/* ------------------------------------------------------------ containers */
/* Window content background. */
/* Grouped card (System-Settings style).  Rows inside are ic_ui_group_row
 * rects of equal height stacked from the card's top. */
/* Row chrome: separator above rows other than the first, hover fill
 * that respects the card's rounded corners. */
/* Title (+ optional subtitle) laid out at the row's leading edge. */
/* Section header above a group. */
/* Source-list / sidebar item. */
/* Table/list row: selection pill (accent when the list is focused),
 * hover fill, zebra-free.  Returns the colour to use for row text. */
/* Column header strip for tables. */
/* Toolbar strip at the top of a window's content (with bottom hairline). */
/* Status bar strip at the bottom of a window's content. */
/* Overlay scrollbar: a thin pill inside the right edge of `view`,
 * visible with `alpha` (0..1) so it can fade out when idle. */
/* Thumb rect for hit-testing / dragging. */
/* Empty state: symbol, title and a short message centred in r. */
/* ------------------------------------------------------------ menus */
    const char *labels[IC_MENU_ITEMS_MAX];   /* IC_MENU_SEPARATOR for a divider */
    const char *shortcuts[IC_MENU_ITEMS_MAX];/* optional right-aligned hints */
    int         hover;                        /* item under the pointer, -1 none */
/* Item index at (x, y) for a menu placed at (mx, my); -1 if none or a
 * separator/disabled item. */
/* Draw the menu (material background, shadow, rows); scratch backs
 * caller compositing the menu layer (the WM does this). */
/* Floating material panel (launcher, popovers, alerts). */
/* Modal alert card: title, message, up to 2 buttons (right aligned,
 * last = primary).  Returns the rects of the drawn buttons. */
#endif /* USERSPACE_IC_UI_H */

## /userspace/ic_version.h

/*
 */
#include "version.h"   /* ICDA_VERSION_* definitions (root, -I. covers it) */
/* Alias IC_VERSION_* -> ICDA_VERSION_* (keep existing names/signatures). */
#endif /* USERSPACE_IC_VERSION_H */

## /userspace/icda_sys.h

/* Returns 1 once the window manager has claimed the framebuffer. */
    uint32_t flip_active;  /* 1 when tear-free page flipping is active */
    uint32_t needs_present; /* append-only ABI: 1 when present() does real DMA work */
/* GPU device layer (DRM/KMS-shaped general GPU driver) */
/* Legacy present (flags=0, no vblank wait): equivalent to the old no-arg call. */
/* Present with flags: bit0 = WAIT_VBLANK (single sched_yield, never spin). */
/* Power: 0 = shutdown, 1 = reboot.  Does not return. */
/* Task-manager stats for one process */

## /userspace/icon_data.h

/* Generated by scripts/gen_icons.py - do not edit by hand. */
#endif /* USERSPACE_ICON_DATA_H */

## /userspace/init.c

/* PID1 supervisor (P0 OS-ification, step 2).
 */
    /* Product boot: keep the splash/black screen clean until the WM's
     * to the console (visible in text mode / on failure). */
    /* Propagate the child's exit code: a clean VT-switch exit (0)
     * recovery path (B1). SYS_WAITPID returns the code itself. */

## /userspace/init_start.asm

    ; Standard argc/argv stack layout (see user_build_initial_stack):
    ; [rsp] = argc, [rsp+8] = argv[0].

## /userspace/libicda.c

/* ================================ memory ============================== */
/* ============================== strings ============================== */
/* =========================== extended strings ========================== */
        /* Still need to compute how many chars we would write. */
    /* len is the number of significant digits; write them in forward order. */
    char tmp[16];  /* 64-bit hex fits in 16 chars */
        /* Check for overflow: v > MAX/10, or v == MAX/10 and digit > MAX%10 (5). */
            /* consume remaining digits */
/* ========================= character classification ==================== */
/* ================================ UTF-8 ================================ */
        /* A codepoint leader is any byte that does NOT start with 10xxxxxx. */
    if (!s) return 1; /* NULL is "empty" */
            /* ASCII ΓÇö always valid. */
            return 0; /* invalid leader byte */
        /* Collect continuation bytes. */
            if (s[i + j] == 0) return 0; /* truncated */
            if ((c2 & 0xC0) != 0x80) return 0; /* not a continuation */
        /* Reject overlong encodings. */
        /* Reject surrogate halves (U+D800..U+DFFF). */
        /* Reject codepoints above U+10FFFF. */
/* ============================ arena allocator ========================== */
    /* align must be a power of two and within IC_ARENA_ALIGN_MAX */
    /* Sanity: bump pointer must not have overrun capacity. */
    /* Align the bump pointer up to the requested alignment. */
    /* Stepwise overflow check: pad must fit, then size must fit in remaining. */
/* ============================= ring buffer ============================= */
    if (next == r->head) return -1; /* full */
    if (r->head == r->tail) return -1; /* empty */
/* ============================== canvas =============================== */
/* A canvas dimension can never legitimately exceed the 2560x1600 back
 * and panics the kernel.  Reject anything implausible outright. */
/* Is (dx,dy) within a rounded rect of size w x h and corner radius r?
 * (dx,dy) are offsets from the rect origin; used for both fill and outline. */
/* Soft drop shadow: a real blurred-feel falloff (quadratic alpha ramp)
 * grey outline - modern desktops draw under windows. */
    sh_r = m < 10 ? m : 10;      /* corner radius of the shadow body */
    /* Row loop over the expanded rect; the band is the outer m pixels. */
            /* Side bands only */
            /* Top/bottom strips span the full width */
            /* Distance to the rounded-rect body (Euclidean at corners). */
            /* In the corner regions subtract the body radius for a
             * rounded-corner shadow silhouette. */
                /* Quadratic falloff: sharp near the window, soft far out. */
                    /* 1px in from each edge */
/* Proportional atlas sibling of ic_text_clip: fills the background
 * to the bitmap font when the atlas face is NULL. */
        /* Draw the fitting prefix through a bounded copy. Titles and
         * labels are short; the stack buffer covers them all. */
/* =============================== icons =============================== */
/* ========================= .ico files =============================== */
/* Icons dropped into a folder (e.g. /usr/share/icons) as .ico files are
 * skipped so the largest decodable size is always chosen. */
/* Little-endian 16/32-bit readers for the binary formats. */
    if (ico_u16(blob) != 0 || ico_u16(blob + 2) != 1) return -1;   /* reserved + type */
    /* Pick the largest decodable BMP entry that fits max_decode, so
     * multi-size .ico files keep their best quality. */
            continue;   /* PNG-compressed entry: not supported */
            if (ico_u32(blob + off + 16) != 0) continue;   /* BI_RGB only */
            /* Guard against malformed files walking off the entry. */
                int src_row = dib_h > 0 ? (h - 1 - y) : y;   /* ICO DIBs are bottom-up */
                        dst[x * 4 + 0] = p[2];   /* B G R A -> R G B A */
        if (stem_len <= 4) continue;                /* just ".ico" */
        stem[stem_len - 4] = 0;                     /* strip extension */
/* =============================== theme =============================== */
    /* Dark slate + electric-blue accent ΓÇö charcoal surfaces, one vivid
     * highlight, rounded corners everywhere. */
/* =========================== window chrome =========================== */
/* ---- single-pixel helper (private to chrome glyphs) ---- */
/* ---- vector caption-button glyphs (1.3.1) ---- */
/* All glyphs target the 18├ù16 button rect (IC_BTN_W ├ù IC_BTN_H) with
 * ΓëÑ3 px padding on every side.  (bx,by) is the button top-left. */
/* Minimize: 2 px-thick horizontal bar, centered in the button. */
    /* 8 px wide, 2 px tall, centred: bx+5..bx+12, by+7..by+8 */
/* Maximize: 2 px-thick outline rect, 8├ù8, centred in the button.
 * Outer rect: bx+5..bx+12, by+4..by+11  (8├ù8)                      */
    /* top edge  (2 px thick) */
    /* bottom edge */
    /* left edge (between top/bottom, already drawn by corners) */
    /* right edge */
/* Close: 2 px-thick X, 8├ù8, centred in the button.
 * consistent thickness without floating-point math.                */
    /* 8├ù8 area: bx+5..bx+12, by+4..by+11  */
        /* \ diagonal (top-left ΓåÆ bottom-right) */
        /* / diagonal (top-right ΓåÆ bottom-left) */
    /* 2nd pixel of each diagonal for thickness: shift right on \,
     * shift left on /, skip the very last row (would overflow).      */
        ic_px(c, bx + 6 + i, by + 4 + i, color);   /* \ +1 col */
        ic_px(c, bx + 11 - i, by + 4 + i, color);   /* / ΓêÆ1 col */
    /* Fake glass: vertical gradient + top highlight line over the base. */
    /* ---- caption-button glyphs (vector, 1.3.1) ----
     * icon parameters are kept for ABI compatibility and are unused. */
/* ============================== widgets ============================== */
/* ============================ menu / dialog / slider ================= */
/* ============================ app skeleton =========================== */
    on_draw(ud); /* initial paint */
        /* Repaint after input, and on the 8-tick cadence so nothing
         * animates the WM at a fixed 20fps while idle. */
/* ================================ ic_io ================================ */
/* ================================ ic_app ================================ */
/* ================================ ic_http ================================ */
    if (ic_strprefix(url, "https://")) {
    } else if (ic_strprefix(url, "http://")) {
/* ============================ gui2 layout ============================== */
/* ============================ gui2 scroll ============================= */
    /* Track background */
    /* Thumb */
/* ============================ gui2 widgets ============================ */
    /* Background + border */
    /* Inner highlight when focused */
    /* Text or placeholder */
    /* Block cursor when focused */
    /* Shift bytes right */
    /* Shift bytes left */
    /* Background */
    /* Visible row range */
/* =========================== gui2 text wrap =========================== */
                /* For words wider than a row the chunking loop below
                   wrap here for words that fit on the next line.        */
        /* Chunk long words across rows exactly like ic_text_draw_wrap. */
        /* Measure word */
        /* Does word (+ optional preceding space) fit? */
        /* Draw space before word (if not first on line) */
        /* Draw word, chunking across rows when wider than remaining space */
/* ============================ font atlas ============================== */
        if (c < 32 || c > 126) c = 63; /* '?' */
    /* Total alpha buffer size: last glyph (char 126) end offset */
        if (c2 < 32 || c2 > 126) c2 = 63; /* '?' */

## /userspace/libicda.h

/*
 */
#include "gui.h"      /* gui_open_window / gui_pixel_buffer / gui_flush ... */
#include "font_atlas.h" /* ic_atlas_font_t, ic_glyph_t ΓÇö promoted in 1.3 */
/* Design system and rendering stack (each module documents itself). */
#include "ic_time.h"    /* monotonic TSC clock */
#include "ic_anim.h"    /* easing, tweens, springs */
#include "ic_gfx.h"     /* rasteriser: AA shapes, blits, blur, shadows */
#include "ic_font.h"    /* type system (Inter / JetBrains Mono) */
#include "ic_theme.h"   /* palette + spacing/radius/motion tokens */
#include "ic_ui.h"      /* controls, containers, menus, symbols */
#include "ic_app.h"     /* application runtime: events, redraw pacing */
/* ================================ memory ============================== */
/* Safe memory primitives.  All functions are NULL-safe: if both pointers
 * function returns without writing.  All sizes are in bytes. */
/* Copy n bytes from src to dst.  Overlapping regions are NOT handled
 * if n is 0. */
/* Copy n bytes from src to dst, safe for overlapping regions. */
/* Set n bytes at dst to value (only the low byte is used). */
/* Compare n bytes.  Returns <0, 0, or >0 like memcmp. */
/* Zero n bytes at dst. */
/* ================================ strings ============================== */
/* =========================== extended strings ========================== */
/* Return the length of s, but never scan past cap bytes. */
/* Copy at most n characters from src to dst, NUL-terminate if cap allows.
 * Returns pointer to dst.  If src is NULL, dst is zero-filled (up to cap). */
/* Append src to dst (finding the NUL in dst first).  NUL-terminates if
 * (excluding NUL) ΓÇö like strlcat.  If cap is 0, returns src length. */
/* Format val as a decimal string into buf, NUL-terminate (if cap > 0).
 * Returns number of characters written (excluding the NUL). */
/* Format val as a lowercase hex string into buf, NUL-terminate.
 * Returns number of characters written (excluding the NUL). */
/* Parse a decimal string to uint64_t.  Returns 1 on success, 0 on
 * UINT64_MAX). */
/* ========================= character classification ==================== */
/* ================================ UTF-8 ================================ */
/* Count the number of Unicode codepoints in s (NUL-terminated). */
/* Validate s as well-formed UTF-8.  Returns 1 if valid, 0 if not.
 * surrogate halves, and codepoints above U+10FFFF. */
/* ============================ arena allocator ========================== */
/* A bump/arena allocator over a caller-provided buffer.  No kernel
 * use ic_arena_reset to reclaim the entire buffer. */
    uint8_t  *buf;    /* base of the caller-owned buffer (may NOT be NULL) */
    uint64_t  cap;    /* total capacity in bytes */
    uint64_t  offset; /* next free byte (bump pointer) */
/* Initialise the arena over the given buffer.  buf may be NULL only if
 * cap is 0 (creating a permanently-full arena).  Returns 0 on success. */
/* Allocate `size` bytes aligned to `align` (must be power of 2, >= 1).
 * enough room.  Never fails for size==0 (returns NULL by convention). */
/* Reset the bump pointer ΓÇö logically frees everything. */
/* Bytes used so far. */
/* Bytes remaining. */
/* ============================= ring buffer ============================= */
/* A single-byte ring buffer (FIFO) over a caller-provided buffer.
 * at most `cap - 1` bytes. */
    uint8_t  *buf;   /* base of the caller-owned buffer (may NOT be NULL) */
    uint64_t  cap;   /* usable capacity (ring stores cap-1 bytes max) */
    uint64_t  head;  /* read index */
    uint64_t  tail;  /* write index */
/* Initialise the ring over the given buffer.  cap must be >= 2.
 * Returns 0 on success, -1 on invalid parameters. */
/* Push one byte.  Returns 0 on success, -1 if full. */
/* Pop one byte.  Returns 0 on success, -1 if empty. */
/* Number of bytes currently stored. */
/* Free slots available for pushing. */
/* Empty the ring (reset head/tail/count to initial state). */
/* ============================== canvas =============================== */
/* A 32bpp (0xAARRGGBB) pixel surface. The WM and GUI apps both draw
 * into their window buffer (gui_pixel_buffer). pitch == w always. */
/* ic_canvas_t is defined in ic_gfx.h (it gained a clip rect). */
/* Soft drop shadow (quadratic alpha falloff band around a rounded rect). */
/* Proportional-atlas sibling of ic_text_clip (clipped whole glyphs).
 * glyphs (for gradients). */
/* =============================== icons =============================== */
/* .icn format (straight-alpha RGBA, top-left origin):
 */
/* Draw scaled to dw x dh with nearest-neighbor sampling + alpha blend. */
/* Load every *.ico in a folder into the runtime icon registry, keyed by the
 * replaces the stock icon.  Returns 0 if at least one icon loaded. */
/* Parse a classic BMP-compressed .ico blob and decode one image into
 * quality.  PNG-compressed entries are skipped. */
/* Folder registry first, then the builtin set (userspace/icon_data.h).
 * Returns 0 for unknown names. */
/* =============================== theme =============================== */
/* Centralized design tokens ΓÇö radius, spacing, shadow. */
/* =========================== window chrome =========================== */
/* A window's x/y is its client origin; the title bar sits above it.
 * answer hit-tests in screen coordinates. */
#define IC_BTN_MAX_OFF 72   /* x offsets of title-bar buttons from the right edge */
    int      x, y;        /* client origin */
    int      w, h;        /* client size */
    int      anim;        /* 0..IC_ANIM_MAX window open/restore animation */
    int      hover_close; /* pointer over the close button (chrome hover) */
    int      hover_min;   /* pointer over the minimize button */
    int      hover_max;   /* pointer over the maximize/restore button */
/* ============================== widgets ============================== */
/* ic_rect_t is defined in ic_gfx.h. */
    IC_BTN_ACTIVE,     /* pressed */
/* ============================ menu / dialog / slider ================= */
/* Stateless primitives (hit-testing kept separate, like buttons): the
 * caller owns open/close/value state and redraws on change. */
    int selected;   /* highlighted index, -1 for none */
/* ============================ app skeleton =========================== */
/* Runs the standard GUI app loop: opens a window, polls the event queue,
 * after events and periodically so animations keep running. */
/* ================================ ic_io ================================ */
/* Errno codes ΓÇö mirror kernel/syscall/syscall.h so userspace code does
 * success or negative errno (e.g. -U_ENOENT). */
#define U_ENOENT  2   /* no such file or directory */
#define U_EBADF   9   /* bad file descriptor */
#define U_ENOMEM  12  /* out of memory / buffer too small */
#define U_EACCES  13  /* permission denied */
#define U_EFAULT  14  /* bad address */
#define U_EINVAL  22  /* invalid argument */
/* File, directory, and path helpers ΓÇö thin wrappers over icda_sys.h
 * All functions return 0 on success or a negative errno on failure. */
/* Read an entire file into buf (up to cap bytes).  Returns 0 on success.
 * errno on failure: -U_ENOENT (not found), -U_EFAULT (bad path/buf). */
/* Write len bytes from buf to path.  Returns 0 on success, negative errno
 * on failure.  buf may be NULL only when len is 0. */
/* Stat a path.  Returns 0 on success, negative errno on failure.
 * out must be non-NULL. */
/* Get current working directory into buf (up to cap bytes).
 * Returns 0 on success, negative errno on failure. */
/* Create directory at path.  Returns 0 on success, negative errno. */
/* Create (touch) a file at path.  Returns 0 on success, negative errno. */
/* Join two path components with a single '/' separator.
 * (result NUL-terminated in dst), -U_ENOMEM if result exceeds cap. */
/* Normalize src into dst: resolve '.'  '..'  '//'  trailing '/'.
 * -U_ENOMEM if the result would exceed cap (dst left empty). */
/* List directory entries into buf.  Returns 0 on success.
 * len_out (optional) receives bytes written. */
/* Directory iterator ΓÇö walks NUL/newline-separated entries produced by
 *   while (ic_dir_next(&cur, &name, &nlen, &is_dir)) { ... } */
/* ================================ ic_app ================================ */
/* Process, timer, and IPC helpers ΓÇö thin wrappers over icda_sys.h
 * syscalls with NULL checks and RAII-style resource management. */
/* Spawn a process.  Returns pid on success, (uint64_t)-errno on failure. */
/* Spawn with argument string.  args may be NULL (treated as ""). */
/* Wait for a child process.  Returns exit code on success, negative errno. */
/* Sleep for the given number of scheduler ticks. */
/* Return current tick count. */
/* Yield the CPU to the scheduler. */
/* Exit the current process.  Does not return. */
/* Shared memory RAII wrapper ΓÇö create + map on acquire, unmap + close on
 * release.  Double-release is safe (checks .valid). */
/* Acquire shared memory of the given size.  Returns 0 on success. */
/* Release shared memory (unmap + close).  Safe to call twice. */
/* Message queue helpers ΓÇö all handle-checked.  Messages are 64 bytes
 * gui_msg_t (kernel asserts the message is exactly 64 bytes). */
/* ================================ ic_http ================================ */
/* Shared HTTP fetch logic ΓÇö deduplicates browser/curl URL parsing and
 * kernel HTTP(S), and reads the result into a caller buffer or file. */
/* Parse a URL into its components.  Supports http:// and https://.
 * -1 on format error.  All output params required. */
/* Resolve a hostname to IPv4.  Tries IPv4 literal first, then DNS.
 * Returns 0 on success, negative errno on failure. */
/* Fetch a URL to a VFS file.  dns + http/https_get_ipv4 wrapper.
 * will be truncated by the kernel. */
/* Fetch a URL into a caller-provided memory buffer.  Uses a scratch
 * len_out (optional) receives bytes read into buf (never exceeds cap). */
/* ============================ gui2 layout ============================== */
/* Stateless row/column layout helpers.  Pure compute ΓÇö no drawing.
 * Returns 0 on success, -U_ENOMEM if count exceeds out_cap. */
/* ============================ gui2 scroll ============================= */
/* Minimal vertical scroll state + helpers.  Caller owns the struct
 * and redraws after any mutation. */
    int offset;    /* current scroll position in pixels */
    int content_h; /* total content height in pixels */
    int view_h;    /* visible viewport height in pixels */
/* Clamp offset into [0, max(0, content_h ΓêÆ view_h)]. */
/* Draw a vertical scrollbar thumb inside track.  No-op when content
 * fits the viewport.  track.w is the scrollbar width (ΓëÑ 6 recommended). */
/* Hit-test the scrollbar track for a click/drag.  On hit, updates
 * s->offset proportionally and returns 1.  Returns 0 on miss. */
/* ============================ gui2 widgets ============================ */
/* Text field ΓÇö bordered box, clipped text, block cursor when focused.
 * buf is empty (may be NULL). */
/* Insert one byte at cursor.  Returns 0 on success, ΓêÆ1 if full or
 * cursor out of range.  Updates *len_io and *cursor_io. */
/* Delete one byte before cursor.  Returns 0 on success, ΓêÆ1 if
 * cursor is at 0.  Updates *len_io and *cursor_io. */
/* List view ΓÇö visible rows only, highlight selected row.
 * get_label(i, ud) returns the label for row i (or NULL to skip). */
/* Hit-test a click in a list view.  Returns row index or ΓêÆ1. */
/* =========================== gui2 text wrap =========================== */
/* Word-wrap helpers for the 8px monospace font.
 * Hard-break long words at max_px when no space is available. */
/* Count the number of visual lines the string needs when wrapped to
 * max_px pixels wide.  Returns total line count. */
/* Draw s with word-wrapping at max_px width, starting at (x, y).
 * only, never writes past the canvas. */
/* ============================ font atlas ============================== */
/* Proportional anti-aliased font from the generated atlas
 * (< 32 or > 126) render as '?' (glyph 63). */
/* Default (regular) face from the atlas.  Never returns NULL. */
/* Pixel width of s rendered in the given font.  0 if font is NULL
 * or s is NULL. */
/* Draw s at (x, y) with alpha-blended glyphs.  fg_rgb is the
 * canvas is NULL. */
/* Line height of the given font in pixels.  0 if font is NULL. */
#endif /* USERSPACE_LIBICDA_H */

## /userspace/nptest.c

/* Negative syscall-gate test (P0 hardening).
 */
    /* Kernel-half addresses must be rejected, never dereferenced. */
    /* Low unmapped page (nothing is mapped at 0x1000 in user space). */
    /* Sanity: a good call must still succeed (guards over-blocking). */
    /* /dev/fb0 claim path (moved to devnodes.c in P0 OS-ification):
     * starts after us is unaffected. */
    /* W^X kill-path: spawn the linux test in suicide mode (it mprotects
     * terminate it with -11; survival or any other code is a FAIL. */

## /userspace/nptestlx.c

/* Linux-personality syscall test (runs as /bin/nptestlx.elf).
 */
/* Kernel vfs_stat_t layout (see kernel/fs/vfs.h). */
    /* Suicide mode: used by native nptest to verify the W^X kill path.
     * 42 means protection did NOT fire. */
        /* Double close must fail, not silently succeed. */
    /* getdents pages through the fd offset. */
    /* brk ladder. */
    /* mmap / mprotect / munmap incl. W^X and guard windows. */
    /* arch_prctl stub documents itself: returns 0, programs nothing
     * (real FSBASE is a later compat-ladder rung). */

## /userspace/nptestlx_start.asm

    ; Linux x86-64 entry convention, matching user_build_initial_stack:
    ; [rsp] = argc, [rsp+8] = argv[0].
    ; In /bin/ this process has the Linux personality, so exit via
    ; Linux nr 60 (native SYS_EXIT would hit the default -1 path).

## /userspace/settings.c

/*
 */
/* Content column. */
/* Toggle rows: which setting, its copy, its badge colour. */
    int             hover_row;        /* row index within the current pane */
/* ------------------------------------------------------------ layout */
/* Appearance pane: row 0 = mode (segmented), row 1 = accent swatches. */
/* Toggle panes: rows are the toggles belonging to the pane. */
/* ------------------------------------------------------------ drawing */
    /* Live preview of the controls in the chosen look. */
    /* Keep drawing while any control is mid-transition. */
/* ------------------------------------------------------------ events */
    /* The appearance rows hold their own controls; only toggles
     * highlight the whole row. */
        /* Another app (or the desktop menu) may have changed settings. */

## /userspace/settings_store.h

/* Slice C shared settings store (WM + Settings app + audio clients).
 */
#endif /* USERSPACE_SETTINGS_STORE_H */

## /userspace/shell.c

    /* Slice C master mute: skip audio paths when disabled. */

## /userspace/taskman.c

/*
 */
#define TM_SAMPLE_TICKS  100      /* ~1 s at the 100 Hz scheduler */
/* Table columns, in pixels from the table's left edge. */
    int      suspended;           /* we asked for it; the kernel has no flag */
    /* view */
    int first_row;                /* index of the first drawn process */
    int last_row;                 /* one past the last drawn process */
    /* pointer state */
    /* confirmation */
    int alert_action;             /* TM_* pending confirmation */
    int alert_hover;              /* 0 cancel, 1 confirm */
/* ------------------------------------------------------------- layout */
/* Rows, the visible slice and the scroll clamp: one function, called by
 * draw() and by every reflow. */
/* ------------------------------------------------------------ helpers */
/* ------------------------------------------------------------ sampling */
        /* Carry our own suspend flag across samples, matched by pid. */
/* ------------------------------------------------------------- actions */
/* ------------------------------------------------------------ drawing */
        /* One sampling window: 100 ticks of wall clock. */
        /* Consume the delta so the next frame measures the next window. */
    /* The table is the only focusable surface; keep the caret alive. */
/* -------------------------------------------------------------- events */

## /userspace/terminal.c

/*
 */
/* One logical line of output.  Roles map to palette colours at draw
 * time, so a scrollback recorded in dark mode stays correct in light. */
    TERM_ROLE_TEXT = 0,   /* label */
    TERM_ROLE_MUTED,      /* label_tertiary */
    TERM_ROLE_ACCENT,     /* accent */
    TERM_ROLE_ERROR,      /* danger */
    TERM_ROLE_PROMPT      /* success: the prompt */
    int         count;      /* logical lines used */
    int         head;       /* first valid index (oldest line) */
    /* command line */
    int   cmd_cursor;               /* byte offset in cmd */
    int   history_pos;              /* index for up/down, -1 = editing */
    char  draft[TERM_CMD_MAX];      /* in-progress line, restored on Esc */
    /* view, all recomputed by layout() */
    int rows;         /* visible text rows */
    int cols;         /* columns that fit the content box */
    int log_rows;     /* wrapped rows the log occupies */
    int cmd_rows;     /* wrapped rows the command line occupies */
    int total_rows;   /* log_rows + cmd_rows */
    int cursor_row;   /* absolute wrapped row of the command-line cursor */
    int cursor_col;   /* absolute column of the command-line cursor */
    int scroll_rows;  /* rows scrolled back from the bottom */
/* Right-click menu.  The blur in ic_ui_menu needs w*h + max(w,h) px. */
/* --------------------------------------------------------------- log */
    /* Full: drop the oldest line.  One 1 KB memmove per 2000 appended
     * lines beats a ring's index maths on every write. */
/* --------------------------------------------------------- geometry */
/* Wrapped rows a string of `len` bytes needs. */
/* The single source of truth for geometry: draw() and every reflow call
 * this, so nothing else computes rows or columns. */
/* -------------------------------------------------------------- input */
/* True when `line` is exactly `word` or starts with `word` + a space, so
 * "lsx" does not fire the "ls" builtin. */
    /* A bare name resolves against the installed app folder. */
/* Tab completes against the apps the shell knows about. */
/* Byte-offset stepping that respects UTF-8 continuation bytes. */
/* -------------------------------------------------------------- menu */
/* ------------------------------------------------------------ drawing */
/* The prompt and the buffer are one logical line, so it is composed into
 * a scratch string and wrapped exactly like log output. */
    /* Hairline around the inset console surface. */
    /* Log lines. */
        /* A line owns rows_for(len) rows: its wrapped chunks, plus one
         * row per line, so the log overwrites the command line. */
    /* The live command line, wrapped like any other line. */
    /* Caret, on top of the console surface only. */
    /* Overlay scrollbar: only while scrolled back or being dragged. */
    /* Scrolled back: say so, so the frozen view is not mistaken for a
     * hung terminal. */
    /* Keep the caret blinking while the terminal has focus. */
/* -------------------------------------------------------------- events */
/* Dragging the overlay scrollbar: the thumb tracks the pointer, so map
 * y back to a first visible row. */
        /* Rows and columns come from the new size; the log re-wraps and
         * the scroll position is re-clamped by layout(). */

## /userspace/wm.c

/*
 */
#define CURSOR_SAVE_DIM 48   /* max icon cursor dimension for save/restore */
/* Window animations.  Zoom kinds (open/close/minimize/restore) scale a
 * rects and scales only the content. */
    int       pix_w;          /* app buffer size (may lag x/w/h mid-resize) */
    int       x;              /* client rect */
    int       anim_maximized; /* maximized flag to apply when GEOMETRY ends */
    wm_hit_t  hover;          /* caption button under the pointer */
    int       pointer_in;     /* pointer inside the client last frame */
static int task_order[MAX_WINDOWS];   /* taskbar order = open order */
/* back_buffer holds the scene without the cursor; the cursor is blitted
 * snapshots, fading menus). */
/* Screen size, fixed after startup (clamped to the back buffer). */
/* The scene canvas; its clip is set to each damage region in turn. */
/* Page-flipping state: when gpu_info.flip_active the compositor blits
 * existing blit functions write to the correct page without edits. */
static int wm_flip_page = 0;   /* toggles 0/1 after each present */
/* Pointer state lives in file scope so every handler sees one truth. */
/* Pointer interactions in progress. */
static int drag_win = -1;           /* title-bar move */
static int resize_win = -1;         /* edge resize */
static int capture_win = -1;        /* client that owns the pressed button */
static int press_win = -1;          /* caption button press target */
static int title_click_win = -1;    /* double-click-to-zoom tracking */
/* System settings (persisted in /cfg/icda-settings).  Re-read about once
 * appearance and the accent. */
/* ---- damage tracking -------------------------------------------------
 * blits only those rectangles to the framebuffer. */
/* Frame diagnostics (F12 overlay). */
/* Outer rect (title bar + client) for a client rect. */
/* Every screen area a window can touch this frame, animation included:
 * zoom kinds animate outer rects, GEOMETRY animates client rects. */
/* ---- pointer sprite ---------------------------------------------------
 * rasterised once from a polygon with 4x4 supersampling. */
/* Tight 64-bit copy for full-row blits. */
/* ---- cursor-backbuffer path (eliminates flicker) --------------------
 * blitted once, then back_buffer is restored to its scene-only state. */
/* ---- desktop icons -----------------------------------------------------
 * drag/reorder only changes data. */
    int pinned;     /* shown on the desktop */
    int cell_x;     /* grid cell */
    /* Pinned by default: the everyday apps.  Everything else is one
     * right-click away ("Add to Desktop"). */
/* ---- drag + rubber-band state ---- */
static int desk_drag_icon = -1;   /* press-armed icon, -1 none */
static int desk_dragging = 0;     /* threshold passed, ghost follows */
static int desk_grab_dx = 0;      /* cursor offset inside the cell */
static int desk_press_desktop = 0;/* press began on desktop, not a window */
static int rubber_armed = 0;      /* press began on empty desktop */
/* Motion with the left button held: starts icon drags and rubber-bands
 * past the movement threshold. */
/* ---- context menu + info alert --------------------------------------
 * Menus carry the full option set; all state is file scope. */
static int ctx_icon = -1;   /* target icon, -1 = desktop background */
/* ---- persist (best-effort; RAM-only on live ISO) ----
 * registry defaults stand in. */
/* Left release on the desktop: drop a drag (snap + swap) or finish a
 * rubber-band (exclusive select). */
/* Repaint one grid cell of the wallpaper layer (and its icon). */
/* ---- taskbar / launcher state ---- */
/* Window slot for the n-th visible taskbar entry. */
/* Blurred surfaces sample whatever lies under their whole rect, so a
 * rebuilt in full before it is blurred, never mixed with last frame. */
/* ---- focus ---- */
    /* A busy app's queue may be full: drop instead of blocking the WM. */
/* ---- window geometry + animation ---- */
    /* Keep the title bar reachable; the body may hang off-screen. */
/* Current rect and opacity of an animating window.  For zoom kinds the
 * rect is the outer frame; for GEOMETRY it is the client rect. */
/* Replace the window's buffer with one of the new client size and tell
 * the app still maps it and releases it when it processes the resize. */
/* Advance finished animations (called once per loop iteration). */
    /* Cascade new windows from the upper left, centred-ish when there
     * is room. */
    /* Focus only after the open handshake: the app is blocked in recv()
     * for the reply. */
/* ---- compositing ---- */
/* Zoom animations scale a snapshot of the frame: render the frame at its
 * hairline) and scale that into the animated outer rect. */
        /* Live frame at the animated size; content scaled to fit. */
/* Draw a transient surface through layer_buffer so it can fade: copy the
 * scene under `area`, draw on the copy, blend the copy back. */
/* 24-bit wire order is B,G,R (VBE colour masks report red at bit 16). */
/* Blit one rectangle of the scene buffer to the real framebuffer. */
/* Rebuild one region of the scene, back to front, clipped to it. */
/* Repaint every damaged rectangle (or the whole screen), then put the
 * pointer on top with a single blit of its box. */
/* Pointer moved and nothing else changed: redraw just the pointer box. */
/* Present the frame: remap real_fb to the back page in flip mode. */
/* Shutdown/restart: fade the live scene to black with a status line,
 * present each step, then ask the kernel to power off / reboot. */
/* ---- input ---- */
/* Topmost window whose frame (or resize grip) is under the pointer. */
        /* The alert's only button spans its bottom edge. */
/* Right-button edges: clients get the event, the desktop opens the
 * context menu. */
/* Once per batch of pointer events: drags, hover feedback, motion and
 * enter/leave forwarding. */
                /* Pulling a maximized window off the top restores its
                 * size under the pointer. */
    /* Hover: caption buttons, taskbar, launcher, menus. */
    /* Client motion: the captured window gets every move (drags leave
     * client area plus one leave event (x = y = -1). */
        /* F12: diagnostics overlay. */
    /* Replace the stock icon set with whatever the user dropped into
     * /usr/share/icons (folder.ico, terminal.ico, ...). */
    /* The kernel starts scanning page 0, so the WM draws page 1 first. */
    /* virtio-gpu always needs TRANSFER+FLUSH; fbdev only in flip mode. */
            /* Drain every queued pointer event.  Button edges are handled
             * once on the final position. */
        /* Composite at most once per tick with vsync on; immediately
         * otherwise.  Idle frames cost nothing: no damage, no work. */

## /userspace/wm_frame.c

/*
 */
#define WM_GRIP_OUT     5    /* resize grip reach outside the frame */
#define WM_GRIP_IN      3    /* ... and inside it */
    /* Title bar. */
    /* Title: centred on the frame, clear of the caption buttons. */
    /* Client content: the app buffer, masked to the frame's bottom
     * corners; uncovered space (mid-resize) shows the window colour. */
    /* Hairline around the whole frame (outside), crisp against any
     * background. */

## /userspace/wm_frame.h

/*
 */
/* How far a frame's shadow can reach past its rect (damage margin). */
    int         x, y, w, h;   /* client rect */
    int         maximized;    /* square corners, no shadow */
    wm_hit_t    hover;        /* caption button under the pointer */
    wm_hit_t    pressed;      /* caption button held down */
/* Whole frame (title bar + client) in screen space. */
/* Screen area the frame may touch, shadow included. */
/* Two-layer elevation shadow; `opacity` scales it for fades. */
/* Title bar, caption buttons, hairlines and client content.  `px` is
 * filled with the window background. */
#endif /* USERSPACE_WM_FRAME_H */

## /userspace/wm_shell.c

/*
 */
/* ------------------------------------------------------------ apps */
/* ------------------------------------------------------------ wallpaper */
    float cx, cy;       /* centre, fraction of screen */
    float radius;       /* fraction of the screen diagonal */
            /* Deterministic dither kills 8-bit banding in the gradients. */
/* ------------------------------------------------------------ desktop icons */
            /* Labels float on the wallpaper; in dark appearance a soft
             * drop shadow keeps them legible over the bright glows. */
/* ------------------------------------------------------------ taskbar */
    /* Launcher button. */
    /* Running windows. */
    /* Status: now playing + clock. */
/* ------------------------------------------------------------ launcher */
/* ------------------------------------------------------------ overlays */

## /userspace/wm_shell.h

/*
 */
/* ------------------------------------------------------------ apps */
    const char *icon;    /* ic_icon_builtin() name */
/* Icon for a window title (the app registry's label is the key). */
/* ------------------------------------------------------------ wallpaper */
/* Paint the wallpaper for the current appearance over the canvas clip
 * (or the whole canvas).  Deterministic, so partial repaints match. */
/* ------------------------------------------------------------ desktop icons */
/* Cell origin for a grid position. */
/* Hit area (icon + label) inside a cell. */
/* ------------------------------------------------------------ taskbar */
    int              hover;        /* task index or WM_BAR_* */
    const char      *time_text;    /* "14:05" or NULL */
    const char      *date_text;    /* "Mon 28 Sep" or NULL */
    const char      *audio_text;   /* now playing, or NULL */
/* ------------------------------------------------------------ launcher */
/* App index, WM_LAUNCH_SHUTDOWN/RESTART, WM_LAUNCH_NONE (inside, on
 * nothing) or WM_LAUNCH_OUTSIDE. */
/* ------------------------------------------------------------ overlays */
/* Shutdown/restart curtain: t runs 0..1 as the scene fades out. */
/* Diagnostics panel (F12). */
#endif /* USERSPACE_WM_SHELL_H */

## /version.h

/* ---- current release --------------------------------------------------- */
/* Pack into a single uint32_t:  (major << 24) | (minor << 8) | patch       *
 * Range: major 0-255, minor 0-255, patch 0-255.                            */
/* ---- human-readable banner (for -v flags / logging) --------------------- */
#endif /* ICDA_VERSION_H */
