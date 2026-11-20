#include <ctype.h>
#include <stdio.h>
int main(void) {
    for (int c=-1;c<256;c++) {
        if (!!isspace(c)!=!!(isspace)(c) || !!isdigit(c)!=!!(isdigit)(c) ||
            !!isalpha(c)!=!!(isalpha)(c) || !!isalnum(c)!=!!(isalnum)(c)) return 1;
    }
    puts("PASS ZR10 ctype accessor matches installed libc for EOF and all byte values");
    return 0;
}
