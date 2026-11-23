#include <linux/string.h>
/* Vendor ARM kernel exports memset rather than upstream ARM __memzero. */
void __memzero(void *ptr,__kernel_size_t n)
{
    (memset)(ptr,0,n);
}
