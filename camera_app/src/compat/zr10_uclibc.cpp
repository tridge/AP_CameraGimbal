/* Bootlin's uClibc headers expect a thread-locale ctype accessor, absent from
 * the ZR10 libc. Both export the C-locale classification table. These apps
 * retain the C character-classification locale (translations are UTF-8 text). */
#include <ctype.h>

const __ctype_mask_t **__ctype_b_loc(void)
{
    return &__C_ctype_b;
}
