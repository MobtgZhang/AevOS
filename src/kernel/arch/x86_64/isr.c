/*
 * x86_64 interrupt dispatcher.
 * Called from isr_stubs.S with the vector number.
 * Routes to the appropriate driver handler and sends PIC EOI.
 */
#include <aevos/types.h>
#include "../io.h"

extern void timer_handler(void);
extern void keyboard_handler(void);
extern void mouse_handler(void);

#define PIC1_CMD  0x20
#define PIC2_CMD  0xA0
#define PIC_EOI   0x20

void isr_dispatch(void *frame, uint64_t vector)
{
    (void)frame;

    switch (vector) {
    case 0x20:
        timer_handler();
        break;
    case 0x21:
        keyboard_handler();
        break;
    case 0x2C:
        mouse_handler();
        break;
    default:
        break;
    }

    if (vector >= 0x28)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}
