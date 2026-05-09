#include "irq_controller.h"

#include "ioapic.h"
#include "lapic.h"
#include "pic.h"
#include "../firmware/acpi.h"

typedef struct {
    int (*init)(void *multiboot_info);
    void (*eoi)(int irq);
    void (*mask)(int irq);
    void (*unmask)(int irq);
    const char *name;
} irq_controller_ops_t;

static int apic_controller_init(void *multiboot_info) {
    const struct acpi_madt *madt;
    const uint8_t *ptr;
    const uint8_t *end;
    uint64_t lapic_phys = 0;

    if (acpi_init(multiboot_info) != 0) {
        return -1;
    }

    madt = acpi_madt();
    if (!madt) {
        return -1;
    }

    lapic_phys = madt->lapic_address;
    ptr = madt->entries;
    end = ((const uint8_t *)madt) + madt->header.length;
    while (ptr + sizeof(struct acpi_madt_entry_header) <= end) {
        const struct acpi_madt_entry_header *hdr = (const struct acpi_madt_entry_header *)ptr;
        if (hdr->length < sizeof(struct acpi_madt_entry_header) || ptr + hdr->length > end) {
            break;
        }
        if (hdr->type == 5 && hdr->length >= sizeof(struct acpi_madt_lapic_override)) {
            const struct acpi_madt_lapic_override *ovr =
                (const struct acpi_madt_lapic_override *)ptr;
            lapic_phys = ovr->lapic_address;
        }
        ptr += hdr->length;
    }

    if (lapic_init(lapic_phys) != 0) {
        return -1;
    }
    if (ioapic_init(madt) != 0) {
        return -1;
    }

    pic_disable();
    return 0;
}

static void apic_controller_eoi(int irq) {
    (void)irq;
    lapic_eoi();
}

static void apic_controller_mask(int irq) {
    if (irq == 0) {
        lapic_stop_timer();
        return;
    }
    ioapic_mask_irq((uint8_t)irq);
}

static void apic_controller_unmask(int irq) {
    if (irq == 0) {
        lapic_start_timer(32);
        return;
    }
    ioapic_unmask_irq((uint8_t)irq, (uint8_t)(32 + irq), (uint8_t)lapic_id());
}

static irq_controller_ops_t apic_ops = {
    .init = apic_controller_init,
    .eoi = apic_controller_eoi,
    .mask = apic_controller_mask,
    .unmask = apic_controller_unmask,
    .name = "Local APIC / IOAPIC"
};

static irq_controller_ops_t *active_controller = 0;

int irq_controller_init(void *multiboot_info) {
    if (apic_ops.init(multiboot_info) == 0) {
        active_controller = &apic_ops;
        return 0;
    }

    return -1;
}

void irq_controller_eoi(int irq) {
    if (!active_controller) {
        return;
    }
    active_controller->eoi(irq);
}

void irq_controller_mask(int irq) {
    if (!active_controller) {
        return;
    }
    active_controller->mask(irq);
}

void irq_controller_unmask(int irq) {
    if (!active_controller) {
        return;
    }
    active_controller->unmask(irq);
}

const char *irq_controller_name(void) {
    if (!active_controller) {
        return "uninitialized";
    }
    return active_controller->name;
}
