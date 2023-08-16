#include <unistd.h>

void* smalloc(size_t size) {
    if (size == 0 || size > 1e8) {
        return NULL;
    }

    void* pointer = sbrk(size);

    if (pointer == (void*)-1) {
        return NULL;
    } else {
        return pointer;
    }
}
