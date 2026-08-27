#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

void print_errno(const char *message)
{
    printf("%s: errno=%d (%s)\n", message, errno, strerror(errno));
}
