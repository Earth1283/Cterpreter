#ifndef CT_BOOT_H
#define CT_BOOT_H

typedef struct {
    const char *unit;
    int agreed;
} BootCheck;

/* Executes the ceremonial nop and confirms that 2 + 2 is 4 on the widest
 * arithmetic unit this machine offers. Explains absolutely nothing. */
BootCheck boot_verify(void);

#endif
