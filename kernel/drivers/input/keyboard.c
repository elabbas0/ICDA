#include "keyboard.h"

#include "../device.h"
#include "../../proc/sched.h"

#include <stdint.h>

#define KEYBOARD_DATA_PORT   0x60
#define KEYBOARD_STATUS_PORT 0x64
#define KEYBOARD_QUEUE_SIZE  128

static char keymap[128] = {
    0,
    27,
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',
    '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,
    '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,
    '*',
    0,
    ' ',
};

static char keymap_shift[128] = {
    0,
    27,
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',
    '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,
    '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,
    '*',
    0,
    ' ',
};

static volatile char queue[KEYBOARD_QUEUE_SIZE];
static volatile uint32_t queue_head = 0;
static volatile uint32_t queue_tail = 0;
static int shift_down = 0;
static int ctrl_down = 0;
static int caps_lock = 0;
static int extended_prefix = 0;
static kernel_device_t keyboard_device;

static int keyboard_device_has_char(void *context) {
    (void)context;
    return keyboard_has_char();
}

static int keyboard_device_read_char(void *context) {
    (void)context;
    return keyboard_read_char();
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void queue_push(char c) {
    uint32_t next = (queue_head + 1) % KEYBOARD_QUEUE_SIZE;
    if (next == queue_tail) {
        return;
    }

    queue[queue_head] = c;
    queue_head = next;
}

static void queue_push_seq(const char *seq) {
    while (*seq) {
        queue_push(*seq++);
    }
}

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static char apply_ctrl(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 'a' + 1);
    }
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 1);
    }
    return c;
}

static char translate_scancode(uint8_t scancode) {
    char c;

    if (scancode >= 128) {
        return 0;
    }

    c = shift_down ? keymap_shift[scancode] : keymap[scancode];
    if (!c) {
        return 0;
    }

    if (caps_lock && is_alpha(c)) {
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        else if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
    }

    if (ctrl_down && is_alpha(c)) {
        return apply_ctrl(c);
    }

    return c;
}

static void keyboard_handle_scancode(uint8_t scancode) {
    char c;

    if (scancode == 0xE0 || scancode == 0xE1) {
        extended_prefix = 1;
        return;
    }

    if (extended_prefix) {
        extended_prefix = 0;
        if (scancode & 0x80) {
            return;
        }
        switch (scancode) {
            case 0x48:
                queue_push_seq("\x1b[A");
                sched_wake_input_waiters();
                return;
            case 0x50:
                queue_push_seq("\x1b[B");
                sched_wake_input_waiters();
                return;
            case 0x4B:
                queue_push_seq("\x1b[D");
                sched_wake_input_waiters();
                return;
            case 0x4D:
                queue_push_seq("\x1b[C");
                sched_wake_input_waiters();
                return;
            case 0x53:
                queue_push_seq("\x1b[3~");
                sched_wake_input_waiters();
                return;
            default:
                return;
        }
    }

    if (scancode == 0x2A || scancode == 0x36) {
        shift_down = 1;
        return;
    }

    if (scancode == 0x1D) {
        ctrl_down = 1;
        return;
    }

    if (scancode == 0xAA || scancode == 0xB6) {
        shift_down = 0;
        return;
    }

    if (scancode == 0x9D) {
        ctrl_down = 0;
        return;
    }

    if (scancode == 0x3A) {
        caps_lock = !caps_lock;
        return;
    }

    if (scancode & 0x80) {
        return;
    }

    c = translate_scancode(scancode);
    if (c) {
        queue_push(c);
        sched_wake_input_waiters();
    }
}

static void keyboard_poll_hardware(void) {
    while (inb(KEYBOARD_STATUS_PORT) & 0x01) {
        uint8_t scancode = inb(KEYBOARD_DATA_PORT);
        keyboard_handle_scancode(scancode);
    }
}

void keyboard_pump(void) {
    keyboard_poll_hardware();
}

void keyboard_init(void) {
    queue_head = 0;
    queue_tail = 0;
    shift_down = 0;
    ctrl_down = 0;
    caps_lock = 0;
    extended_prefix = 0;

    while (inb(KEYBOARD_STATUS_PORT) & 0x01) {
        (void)inb(KEYBOARD_DATA_PORT);
    }

    keyboard_device.name = "keyboard";
    keyboard_device.class_id = DEVICE_CLASS_INPUT;
    static const input_device_ops_t ops = {
        .has_char = keyboard_device_has_char,
        .read_char = keyboard_device_read_char
    };
    keyboard_device.ops = &ops;
    keyboard_device.context = 0;
    keyboard_device.next = 0;
    device_register(&keyboard_device);

    outb(KEYBOARD_STATUS_PORT, 0xAE);
}

void keyboard_irq(struct registers *regs) {
    (void)regs;
    keyboard_poll_hardware();
}

int keyboard_has_char(void) {
    keyboard_poll_hardware();
    return queue_head != queue_tail;
}

int keyboard_read_char(void) {
    char c;

    keyboard_poll_hardware();

    if (queue_head == queue_tail) {
        return -1;
    }

    c = queue[queue_tail];
    queue_tail = (queue_tail + 1) % KEYBOARD_QUEUE_SIZE;
    return (unsigned char)c;
}
