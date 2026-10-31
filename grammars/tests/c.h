#include <stdio.h>
#define BUFFER_SIZE 256

/* A tiny linked list. */
typedef struct node {
    int value;
    struct node *next;
} node_t;

static int sum(const node_t *head) {
    int total = 0;
    for (const node_t *n = head; n != NULL; n = n->next) {
        total += n->value; // accumulate
    }
    return total;
}

int main(void) {
    char buf[BUFFER_SIZE];
    snprintf(buf, sizeof buf, "sum=%d\n", sum(NULL));
    return 0;
}
