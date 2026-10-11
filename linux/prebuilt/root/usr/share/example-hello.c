/*
 * The smallest program: what it prints is shown under the text when you Run it (F6).
 * Try the debugger on it: click before a line's number to mark it, press F5, then F10.
 */
#include <stdio.h>

static int square(int n)
{
    int result = n * n;

    return result;
}

int main(void)
{
    int i, total = 0;

    printf("hello from the machine's own compiler\n");
    for (i = 1; i <= 5; i++) {
        total += square(i);
        printf("%d squared is %d, and the sum so far %d\n", i, square(i), total);
    }
    return 0;
}
