#include "nvme.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct parse_case {
    const char *input;
    int should_pass;
    uint16_t bus;
    uint16_t dev;
    uint16_t func;
    uint16_t nsid;
};

static int check_case(const struct parse_case *tc)
{
    struct controller_data *c;

    c = parse_target(tc->input);
    if (tc->should_pass) {
        if (c == NULL) {
            fprintf(stderr, "FAIL: %s returned NULL\n", tc->input);
            return -1;
        }
        if (c->bus != tc->bus || c->dev != tc->dev ||
            c->func != tc->func || c->nsid != tc->nsid) {
            fprintf(stderr,
                    "FAIL: %s parsed as %u:%u.%u,%u\n",
                    tc->input, c->bus, c->dev, c->func, c->nsid);
            free(c);
            return -1;
        }
        free(c);
        return 0;
    }

    if (c != NULL) {
        fprintf(stderr, "FAIL: %s should have failed\n", tc->input);
        free(c);
        return -1;
    }

    return 0;
}

int main(void)
{
    static const struct parse_case cases[] = {
        {"05:00.0,1", 1, 0x05u, 0x00u, 0u, 1u},
        {"5:0.7,1", 1, 0x05u, 0x00u, 7u, 1u},
        {"ff:1f.7,65535", 1, 0xffu, 0x1fu, 7u, 65535u},
        {"05:00.8,1", 0, 0u, 0u, 0u, 0u},
        {"05:20.0,1", 0, 0u, 0u, 0u, 0u},
        {"05-00.0,1", 0, 0u, 0u, 0u, 0u},
        {"05:00,0,1", 0, 0u, 0u, 0u, 0u},
        {"05:00.0", 0, 0u, 0u, 0u, 0u},
        {"05:00.0,0", 0, 0u, 0u, 0u, 0u},
        {"05:00.0,65536", 0, 0u, 0u, 0u, 0u},
        {"", 0, 0u, 0u, 0u, 0u},
    };
    size_t i;

    for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (check_case(&cases[i]) < 0) {
            return EXIT_FAILURE;
        }
    }

    printf("parse_target tests passed\n");
    return EXIT_SUCCESS;
}
