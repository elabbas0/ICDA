


















#include "flip.h"
#include "../../memory/vmm.h"
#include "../../drivers/serial/serial.h"
#include <stdint.h>


#ifndef PHYSICAL_BASE
#define PHYSICAL_BASE 0xFFFF800000000000ULL
#endif


#define VBE_DISPI_IOPORT_INDEX      0x01CE
#define VBE_DISPI_IOPORT_DATA       0x01CF
#define VBE_DISPI_INDEX_ID          0x0
#define VBE_DISPI_INDEX_XRES        0x1
#define VBE_DISPI_INDEX_YRES        0x2
#define VBE_DISPI_INDEX_BPP         0x3
#define VBE_DISPI_INDEX_ENABLE      0x4
#define VBE_DISPI_INDEX_BANK        0x5
#define VBE_DISPI_INDEX_VIRT_WIDTH  0x6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 0x7
#define VBE_DISPI_INDEX_X_OFFSET    0x8
#define VBE_DISPI_INDEX_Y_OFFSET    0x9
#define VBE_DISPI_INDEX_VRAM_SIZE   0xA

#define VBE_DISPI_DISABLED 0x00
#define VBE_DISPI_ENABLED  0x01

static int flip_available = 0;
static uint32_t flip_frame_bytes = 0;   
static uint32_t flip_vram_size = 0;
static uint64_t flip_fb_phys = 0;       
static uint8_t *flip_page0_view = 0;    
static uint8_t *flip_page1_view = 0;    
static int flip_page = 0;               
static uint16_t flip_height = 0;        

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static uint16_t bochs_read(uint16_t reg) {
    outw(VBE_DISPI_IOPORT_INDEX, reg);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static void bochs_write(uint16_t reg, uint16_t val) {
    outw(VBE_DISPI_IOPORT_INDEX, reg);
    outw(VBE_DISPI_IOPORT_DATA, val);
}

int flip_probe(uint64_t fb_phys, uint32_t pitch, uint32_t height,
               uint32_t bpp, uint64_t vmm_addr_space) {
    uint16_t id;
    uint16_t vram16;
    (void)vmm_addr_space;

    flip_available = 0;
    flip_fb_phys = fb_phys;

    
    id = bochs_read(VBE_DISPI_INDEX_ID);
    if ((id & 0xFFF0) != 0xB0C0) {
        flip_available = 0;
        return -1;
    }

    


    vram16 = bochs_read(VBE_DISPI_INDEX_VRAM_SIZE);
    if (vram16 >= 256) {            
        flip_vram_size = (uint32_t)vram16 * 64U * 1024U;
    } else {
        flip_vram_size = 16U * 1024U * 1024U;
    }

    flip_frame_bytes = pitch * height;
    flip_height = (uint16_t)height;

    
    if (flip_vram_size < flip_frame_bytes * 2) {
        serial_write("flip: not enough VRAM for 2 frames\n");
        flip_available = 0;
        return -1;
    }

    
    flip_page0_view = (uint8_t *)((uint64_t)(fb_phys) + PHYSICAL_BASE);
    flip_page1_view = (uint8_t *)((uint64_t)(fb_phys + flip_frame_bytes) + PHYSICAL_BASE);



    

    bochs_write(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)(pitch / (bpp / 8)));
    bochs_write(VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16_t)(height * 2));



    



    if (bochs_read(VBE_DISPI_INDEX_VIRT_HEIGHT) != (uint16_t)(height * 2)) {
        serial_write("flip: VIRT_HEIGHT unsupported, using direct blit\n");
        bochs_write(VBE_DISPI_INDEX_VIRT_HEIGHT, height);
        bochs_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
        flip_available = 0;
        return -1;
    }

    
    bochs_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    flip_page = 0;

    flip_available = 1;
    serial_write("flip: Bochs VBE page flipping enabled\n");
    return 0;
}




uint8_t *flip_back_buffer(void) {
    if (!flip_available) return 0;
    
    return (flip_page == 0) ? flip_page1_view : flip_page0_view;
}





void flip_swap(void) {
    if (!flip_available) return;
    if (flip_page == 0) {
        
        bochs_write(VBE_DISPI_INDEX_Y_OFFSET, flip_height);
        flip_page = 1;
    } else {
        
        bochs_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
        flip_page = 0;
    }
}


int flip_active(void) {
    return flip_available;
}
