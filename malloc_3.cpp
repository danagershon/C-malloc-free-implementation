#include <unistd.h>
#include <string.h>
#include <sys/mman.h>

// malloc family of functions prototypes

void* smalloc(size_t size);

void* scalloc(size_t num, size_t size);

void sfree(void* p);

void* srealloc(void* oldp, size_t size);

// ----------------------------------------------------------------------------

// private functions for testing

size_t _num_free_blocks();

size_t _num_free_bytes();

size_t _num_allocated_blocks();

size_t _num_allocated_bytes();

size_t _num_meta_data_bytes();

size_t _size_meta_data();

// ----------------------------------------------------------------------------

size_t min(size_t a, size_t b) {
    return a < b ? a : b;
}

const int KB = 1024;

typedef enum {
    FREE = 0,
    TOTAL = 1
} BytesType;

typedef enum {
    TOTAL_PAYLOAD = 0,
    ACTIVE_PAYLOAD = 1
} SizeType;

class MallocMetadata {
public:
    size_t size[2];
    bool is_free;
    MallocMetadata *next, *prev; // no use for mmap blocks

    MallocMetadata() = default;

    void* getPayloadBlockAddr();
};

void *MallocMetadata::getPayloadBlockAddr() {
    return (void*)(this + 1);
}

class HeapBlocksList {
public:
    const size_t SPLITTING_THRESHOLD = 128;

    MallocMetadata *head, *tail;
    size_t blocks_count[2];
    size_t bytes_count[2];

    HeapBlocksList();

    void* allocateBlock(size_t payload_size);

    void* allocateZeroedBlock(size_t payload_size);

    MallocMetadata* findFreeBlock(size_t payload_size);

    void* createNewBlock(size_t payload_size);

    void* useFreeBlock(MallocMetadata* free_block_metadata,
            size_t new_active_payload_size);

    void useFreeBlockWithoutSplit(MallocMetadata* original_block_metadata,
            size_t new_active_payload_size);

    void useFreeBlockWithSplit(MallocMetadata* original_block_metadata,
            size_t new_active_payload_size, size_t remaining_payload_size);

    void updateMetaDataAfterFreeBlockSplit(MallocMetadata* original_block_metadata,
            MallocMetadata* remaining_block_metadata,
            size_t new_active_payload_size,
            size_t remaining_payload_size);

    void* useWildernessBlock(size_t payload_size);

    void setNewBlockMetaData(size_t payload_size,
            MallocMetadata* block_metadata);

    void releaseUsedBlock(void* payload_addr);

    void combineFreeBlockWithSucc(MallocMetadata* block_metadata);

    void combineFreeBlockWithPred(MallocMetadata* block_metadata);

    void combineFreeBlockWithSuccAndPred(MallocMetadata* block_metadata);

    void freeBlockWithoutCombining(MallocMetadata* block_metadata);

    void* reallocateActiveBlock(void* old_payload_addr, size_t new_payload_size);

    void* reallocateWithSameBlock(MallocMetadata* old_block_metadata,
            size_t new_payload_size);

    void splitUsedBlock(MallocMetadata* old_block_metadata,
            size_t new_payload_size, size_t remaining_payload_size);

    void* reallocateWildernessBlock(size_t new_payload_size);

    bool canReallocateUsingPredOrSucc(MallocMetadata* old_block_metadata,
            size_t new_payload_size);

    bool canReallocateUsingPredOnly(MallocMetadata* old_block_metadata,
            size_t new_payload_size);

    bool canReallocateUsingSuccOnly(MallocMetadata* old_block_metadata,
            size_t new_payload_size);

    bool canReallocateUsingPredAndSucc(MallocMetadata* old_block_metadata,
            size_t new_payload_size);

    void* reallocateUsingPredOrSucc(MallocMetadata* old_block_metadata,
            size_t new_payload_size);

    void* reallocateUsingPredOnly(MallocMetadata* old_block_metadata,
            size_t new_payload_size);

    void* reallocateUsingSuccOnly(MallocMetadata* old_block_metadata,
            size_t new_payload_size);

    void* reallocateUsingPredAndSucc(MallocMetadata* old_block_metadata,
            size_t new_payload_size);

    void* reallocateToOtherBlock(MallocMetadata* old_block_metadata,
            void * old_payload_addr, size_t new_payload_size);
};

// ----------------------------------------------------------------------------

HeapBlocksList::HeapBlocksList()
        : head(NULL), tail(NULL)
{
    blocks_count[FREE] = 0;
    blocks_count[TOTAL] = 0;

    bytes_count[FREE] = 0;
    bytes_count[TOTAL] = 0;
}

MallocMetadata* HeapBlocksList::findFreeBlock(size_t payload_size) {
    MallocMetadata* curr_block_metadata = head;

    while (curr_block_metadata != NULL) {
        if (curr_block_metadata->is_free
            && curr_block_metadata->size[TOTAL_PAYLOAD] >= payload_size) {
            break;
        }
        curr_block_metadata = curr_block_metadata->next;
    }

    return curr_block_metadata;
}

void *HeapBlocksList::allocateBlock(size_t payload_size) {
    MallocMetadata* free_block_metadata = findFreeBlock(payload_size);
    void* payload_block_addr = NULL;

    if (free_block_metadata == NULL && tail != NULL && tail->is_free) {
        // enlarge “Wilderness” block and use it
        payload_block_addr = useWildernessBlock(payload_size);
    }
    else if (free_block_metadata == NULL) {
        // must allocate a new block
        payload_block_addr = createNewBlock(payload_size);
    }
    else {
        // use free block and also do splitting if needed
        payload_block_addr = useFreeBlock(free_block_metadata, payload_size);
    }

    return payload_block_addr;
}

void *HeapBlocksList::createNewBlock(size_t payload_size) {
    void* old_prog_break = sbrk(0);
    size_t total_allocation_size = sizeof(MallocMetadata) + payload_size;

    if (sbrk(total_allocation_size) == (void*)-1) {
        // sbrk() failed
        return NULL;
    }

    auto* new_block_metadata = (MallocMetadata*)old_prog_break;
    setNewBlockMetaData(payload_size, new_block_metadata);

    if (head == NULL) { // list empty
        head = new_block_metadata;
        tail = head;
    } else {
        tail->next = new_block_metadata;
        tail = new_block_metadata;
    }

    blocks_count[TOTAL]++;
    // blocks_count[FREE] doesn't change
    bytes_count[TOTAL] += total_allocation_size;
    // bytes_count[FREE] doesn't change

    return new_block_metadata->getPayloadBlockAddr();
}

void HeapBlocksList::setNewBlockMetaData(size_t payload_size,
        MallocMetadata* block_metadata) {
    block_metadata->is_free = false;

    block_metadata->size[TOTAL_PAYLOAD] = payload_size;
    block_metadata->size[ACTIVE_PAYLOAD] = payload_size;

    block_metadata->next = NULL;
    block_metadata->prev = tail;
}

void *HeapBlocksList::useFreeBlock(MallocMetadata* free_block_metadata,
        size_t new_active_payload_size) {

    size_t remaining_payload_size = 0;
    if (free_block_metadata->size[TOTAL_PAYLOAD] - new_active_payload_size > sizeof(MallocMetadata)) {
        remaining_payload_size =
                free_block_metadata->size[TOTAL_PAYLOAD]
                - new_active_payload_size
                - sizeof(MallocMetadata); // for metadata of remaining block
    }

    if (remaining_payload_size < SPLITTING_THRESHOLD) {
        // remaining size to small, don't split
        useFreeBlockWithoutSplit(free_block_metadata,
                new_active_payload_size);
    } else {
        useFreeBlockWithSplit(free_block_metadata,
                new_active_payload_size, remaining_payload_size);
    }

    return free_block_metadata->getPayloadBlockAddr();
}

void HeapBlocksList::useFreeBlockWithoutSplit(MallocMetadata* original_block_metadata,
        size_t new_active_payload_size) {
    original_block_metadata->is_free = false;
    original_block_metadata->size[ACTIVE_PAYLOAD] = new_active_payload_size;

    // blocks_count[TOTAL] doesn't change
    blocks_count[FREE]--;
    // bytes_count[TOTAL] doesn't change
    bytes_count[FREE] -= original_block_metadata->size[TOTAL_PAYLOAD];
}

void HeapBlocksList::useFreeBlockWithSplit(MallocMetadata* original_block_metadata,
        size_t new_active_payload_size, size_t remaining_payload_size) {

    auto* remaining_block_metadata =
            (MallocMetadata*)((char*)original_block_metadata
                              + sizeof(MallocMetadata)
                              + new_active_payload_size);

    updateMetaDataAfterFreeBlockSplit(original_block_metadata,
            remaining_block_metadata,
            new_active_payload_size,
            remaining_payload_size);

    if (tail == original_block_metadata) {
        tail = remaining_block_metadata;
    }

    blocks_count[TOTAL]++;
    /* blocks_count[FREE] doesn't change because original block becomes used but
     * remaining block is now counted as a new free block */
    // bytes_count[TOTAL] doesn't change
    bytes_count[FREE] -= new_active_payload_size
                         + sizeof(MallocMetadata); // because of remaining block
}

void HeapBlocksList::updateMetaDataAfterFreeBlockSplit(MallocMetadata *original_block_metadata,
        MallocMetadata *remaining_block_metadata,
        size_t new_active_payload_size,
        size_t remaining_payload_size) {

    original_block_metadata->is_free = false;
    original_block_metadata->size[ACTIVE_PAYLOAD] = new_active_payload_size;
    original_block_metadata->size[TOTAL_PAYLOAD] = new_active_payload_size;

    remaining_block_metadata->next = original_block_metadata->next;
    if (original_block_metadata->next != NULL) {
        original_block_metadata->next->prev = remaining_block_metadata;
    }
    original_block_metadata->next = remaining_block_metadata;
    remaining_block_metadata->prev = original_block_metadata;

    remaining_block_metadata->is_free = true;
    remaining_block_metadata->size[TOTAL_PAYLOAD] = remaining_payload_size;
    remaining_block_metadata->size[ACTIVE_PAYLOAD] = 0;
}

void* HeapBlocksList::useWildernessBlock(size_t payload_size) {
    // here wilderness block is free

    MallocMetadata* wilderness_block_metadata = tail;

    size_t extra_needed_size = payload_size
                               - wilderness_block_metadata->size[TOTAL_PAYLOAD];

    if (sbrk(extra_needed_size) == (void*)-1) {
        // sbrk() failed
        return NULL;
    }

    // blocks_count[TOTAL] doesn't change
    blocks_count[FREE]--;
    bytes_count[TOTAL] += extra_needed_size;
    bytes_count[FREE] -= wilderness_block_metadata->size[TOTAL_PAYLOAD];

    wilderness_block_metadata->is_free = false;
    wilderness_block_metadata->size[TOTAL_PAYLOAD] = payload_size;
    wilderness_block_metadata->size[ACTIVE_PAYLOAD] = payload_size;

    return wilderness_block_metadata->getPayloadBlockAddr();
}

void HeapBlocksList::releaseUsedBlock(void *payload_addr) {
    // here payload_addr != NULL

    auto *block_metadata = (MallocMetadata*)payload_addr - 1;
    if (block_metadata->is_free) {
        // we allow double free
        return;
    }

    bool combine_with_succ = block_metadata->next != NULL
                             && block_metadata->next->is_free;

    bool combine_with_pred = block_metadata->prev != NULL
                             && block_metadata->prev->is_free;

    if (combine_with_succ && combine_with_pred) {
        combineFreeBlockWithSuccAndPred(block_metadata);
    }
    else if (combine_with_succ) {
        combineFreeBlockWithSucc(block_metadata);
    }
    else if (combine_with_pred) {
        combineFreeBlockWithPred(block_metadata);
    }
    else {
        freeBlockWithoutCombining(block_metadata);
    }
}

void HeapBlocksList::combineFreeBlockWithSucc(MallocMetadata *block_metadata) {
    // here block_metadata->next != NULL

    MallocMetadata* succ_metadata = block_metadata->next;

    if (succ_metadata->next != NULL) {
        succ_metadata->next->prev = block_metadata;
    }
    block_metadata->next = succ_metadata->next;
    if (tail == succ_metadata) {
        tail = block_metadata;
    }

    blocks_count[TOTAL]--;
    // blocks_count[FREE] doesn't change
    // bytes_count[TOTAL] doesn't change
    bytes_count[FREE] += sizeof(MallocMetadata)
                         + block_metadata->size[TOTAL_PAYLOAD];

    block_metadata->is_free = true;
    block_metadata->size[TOTAL_PAYLOAD] += sizeof(MallocMetadata)
                                           + succ_metadata->size[TOTAL_PAYLOAD];
    block_metadata->size[ACTIVE_PAYLOAD] = 0;
}

void HeapBlocksList::combineFreeBlockWithPred(MallocMetadata *block_metadata) {
    // here block_metadata->prev != NULL
    MallocMetadata* pred_metadata = block_metadata->prev;

    if (block_metadata->next != NULL) {
        block_metadata->next->prev = pred_metadata;
    }
    pred_metadata->next = block_metadata->next;
    if (tail == block_metadata) {
        tail = pred_metadata;
    }

    pred_metadata->size[TOTAL_PAYLOAD] += sizeof(MallocMetadata)
                                          + block_metadata->size[TOTAL_PAYLOAD];

    blocks_count[TOTAL]--;
    // blocks_count[FREE] doesn't change
    // bytes_count[TOTAL] doesn't change
    bytes_count[FREE] += sizeof(MallocMetadata)
                         + block_metadata->size[TOTAL_PAYLOAD];
}

void HeapBlocksList::combineFreeBlockWithSuccAndPred(MallocMetadata *block_metadata) {
    combineFreeBlockWithSucc(block_metadata);
    combineFreeBlockWithPred(block_metadata);

    blocks_count[FREE]--;
    bytes_count[FREE] -= block_metadata->size[TOTAL_PAYLOAD];
}

void HeapBlocksList::freeBlockWithoutCombining(MallocMetadata* block_metadata) {
    block_metadata->is_free = true;
    block_metadata->size[ACTIVE_PAYLOAD] = 0;

    // blocks_count[TOTAL] doesn't change
    blocks_count[FREE]++;
    // bytes_count[TOTAL] doesn't change
    bytes_count[FREE] += block_metadata->size[TOTAL_PAYLOAD];
}

void *HeapBlocksList::allocateZeroedBlock(size_t payload_size) {
    void* payload_block_addr = allocateBlock(payload_size);

    if (payload_block_addr != NULL) {
        // zeroing only the part of block the user asked for
        memset(payload_block_addr, 0, payload_size);
    }

    return payload_block_addr;
}

void *HeapBlocksList::reallocateActiveBlock(void *old_payload_addr,
        size_t new_payload_size) {

    if (old_payload_addr == NULL) {
        // act like a call to smalloc(new_payload_size)
        return allocateBlock(new_payload_size);
    }

    auto *old_block_metadata = (MallocMetadata*)old_payload_addr - 1;

    if (old_block_metadata->size[TOTAL_PAYLOAD] >= new_payload_size) {
        // current block is large enough
        return reallocateWithSameBlock(old_block_metadata, new_payload_size);
    }
    if (tail == old_block_metadata
        && reallocateWildernessBlock(new_payload_size) != NULL) {
        // enlarge and use Wilderness block.
        // if sbrk() fails continue to other options
        return old_payload_addr;
    }
    if (canReallocateUsingPredOrSucc(old_block_metadata, new_payload_size)) {
        return reallocateUsingPredOrSucc(old_block_metadata, new_payload_size);
    }

    // otherwise, try to find a different block
    return reallocateToOtherBlock(old_block_metadata, old_payload_addr,
            new_payload_size);
}

void* HeapBlocksList::reallocateWithSameBlock(MallocMetadata *old_block_metadata,
        size_t new_payload_size) {

    size_t remaining_payload_size = 0;
    if (old_block_metadata->size[TOTAL_PAYLOAD] - new_payload_size > sizeof(MallocMetadata)) {
        remaining_payload_size = old_block_metadata->size[TOTAL_PAYLOAD]
                                 - new_payload_size
                                 - sizeof(MallocMetadata);
    }

    if (remaining_payload_size >= SPLITTING_THRESHOLD) {
        splitUsedBlock(old_block_metadata, new_payload_size,
                remaining_payload_size);
    }

    old_block_metadata->size[ACTIVE_PAYLOAD] = new_payload_size;

    return old_block_metadata->getPayloadBlockAddr();
}

void HeapBlocksList::splitUsedBlock(MallocMetadata *old_block_metadata,
        size_t new_payload_size, size_t remaining_payload_size) {
    // here remaining_payload_size >= 128
    auto* remaining_block_metadata =
            (MallocMetadata*)((char*)old_block_metadata
                              + sizeof(MallocMetadata)
                              + new_payload_size);

    old_block_metadata->size[TOTAL_PAYLOAD] = new_payload_size;

    remaining_block_metadata->is_free = true;
    remaining_block_metadata->size[TOTAL_PAYLOAD] = remaining_payload_size;
    remaining_block_metadata->size[ACTIVE_PAYLOAD] = 0;

    if (old_block_metadata->next != NULL) {
        old_block_metadata->next->prev = remaining_block_metadata;
    }
    remaining_block_metadata->prev = old_block_metadata;
    remaining_block_metadata->next = old_block_metadata->next;
    old_block_metadata->next = remaining_block_metadata;

    if (tail == old_block_metadata) {
        tail = remaining_block_metadata;
    }

    blocks_count[TOTAL]++;
    blocks_count[FREE]++;
    // bytes_count[TOTAL] doesn't change
    bytes_count[FREE] += remaining_payload_size;
}

void *HeapBlocksList::reallocateWildernessBlock(size_t new_payload_size) {
    // here Wilderness block is free but not large enough
    MallocMetadata* wilderness_block_metadata = tail;

    size_t extra_needed_size = new_payload_size
                               - wilderness_block_metadata->size[TOTAL_PAYLOAD];

    if (sbrk(extra_needed_size) == (void*)-1) {
        // sbrk() failed
        return NULL;
    }

    wilderness_block_metadata->size[TOTAL_PAYLOAD] = new_payload_size;
    wilderness_block_metadata->size[ACTIVE_PAYLOAD] = new_payload_size;

    // blocks_count[TOTAL] doesn't change
    // blocks_count[FREE] doesn't change
    bytes_count[TOTAL] += extra_needed_size;
    // bytes_count[FREE] doesn't change

    return wilderness_block_metadata->getPayloadBlockAddr();
}

bool HeapBlocksList::canReallocateUsingPredOrSucc(MallocMetadata *old_block_metadata,
        size_t new_payload_size) {
    // here old block isn't large enough alone
    // If old block is the wilderness block then we are here because enlarging it failed

    return canReallocateUsingPredOnly(old_block_metadata, new_payload_size)
        || canReallocateUsingSuccOnly(old_block_metadata, new_payload_size)
        || canReallocateUsingPredAndSucc(old_block_metadata, new_payload_size);
}

bool HeapBlocksList::canReallocateUsingPredOnly(MallocMetadata *old_block_metadata,
        size_t new_payload_size) {
    if (old_block_metadata->prev == NULL) {
        return false;
    }
    if (!old_block_metadata->prev->is_free) {
        return false;
    }

    size_t pred_payload_size = old_block_metadata->prev->size[TOTAL_PAYLOAD];
    size_t total_avail_payload_size = pred_payload_size
                                      + sizeof(MallocMetadata)
                                      + old_block_metadata->size[TOTAL_PAYLOAD];

    return total_avail_payload_size >= new_payload_size;
}

bool HeapBlocksList::canReallocateUsingSuccOnly(MallocMetadata *old_block_metadata,
        size_t new_payload_size) {
    if (old_block_metadata->next == NULL) {
        return false;
    }
    if (!old_block_metadata->next->is_free) {
        return false;
    }

    size_t succ_payload_size = old_block_metadata->next->size[TOTAL_PAYLOAD];
    size_t total_avail_payload_size = old_block_metadata->size[TOTAL_PAYLOAD]
                                      + sizeof(MallocMetadata)
                                      + succ_payload_size;

    return total_avail_payload_size >= new_payload_size;
}

bool HeapBlocksList::canReallocateUsingPredAndSucc(MallocMetadata *old_block_metadata,
        size_t new_payload_size) {
    if (old_block_metadata->prev == NULL || old_block_metadata->next == NULL) {
        return false;
    }
    if (!old_block_metadata->prev->is_free || !old_block_metadata->next->is_free) {
        return false;
    }

    size_t pred_payload_size = old_block_metadata->prev->size[TOTAL_PAYLOAD];
    size_t succ_payload_size = old_block_metadata->next->size[TOTAL_PAYLOAD];
    size_t total_avail_payload_size = pred_payload_size
            + sizeof(MallocMetadata) + old_block_metadata->size[TOTAL_PAYLOAD]
            + sizeof(MallocMetadata) + succ_payload_size;

    return total_avail_payload_size >= new_payload_size;
}

void* HeapBlocksList::reallocateUsingPredOrSucc(MallocMetadata *old_block_metadata,
        size_t new_payload_size) {
    // here we can reallocate using pred and/or succ

    // try using pred only
    if (canReallocateUsingPredOnly(old_block_metadata, new_payload_size)) {
        return reallocateUsingPredOnly(old_block_metadata, new_payload_size);
    }

    // if pred not large enough, try using succ only
    if (canReallocateUsingSuccOnly(old_block_metadata, new_payload_size)) {
        return reallocateUsingSuccOnly(old_block_metadata, new_payload_size);
    }

    // if succ alone not large enough, use both pred and succ
    // it is promised here that pred and succ combined are large enough
    return reallocateUsingPredAndSucc(old_block_metadata, new_payload_size);
}

void* HeapBlocksList::reallocateUsingPredOnly(MallocMetadata *old_block_metadata,
        size_t new_payload_size) {
    // here pred exists, free and large enough

    MallocMetadata* pred_metadata = old_block_metadata->prev;
    size_t pred_payload_size = pred_metadata->size[TOTAL_PAYLOAD];
    size_t total_avail_payload_size = pred_payload_size
                                      + sizeof(MallocMetadata)
                                      + old_block_metadata->size[TOTAL_PAYLOAD];

    pred_metadata->next = old_block_metadata->next;
    if (tail == old_block_metadata) {
        tail = pred_metadata;
    } else {
        old_block_metadata->next->prev = pred_metadata;
    }

    pred_metadata->is_free = false;
    pred_metadata->size[TOTAL_PAYLOAD] = total_avail_payload_size;
    pred_metadata->size[ACTIVE_PAYLOAD] = new_payload_size;

    size_t remaining_payload_size = 0;
    if (total_avail_payload_size - new_payload_size > sizeof(MallocMetadata)) {
        remaining_payload_size = total_avail_payload_size
                                 - new_payload_size
                                 - sizeof(MallocMetadata);
    }

    if (remaining_payload_size >= SPLITTING_THRESHOLD) {
        auto* remaining_block_metadata =
                (MallocMetadata*)((char*)pred_metadata
                                   + sizeof(MallocMetadata)
                                   + new_payload_size);
        remaining_block_metadata->is_free = true;
        remaining_block_metadata->size[ACTIVE_PAYLOAD] = 0;
        remaining_block_metadata->size[TOTAL_PAYLOAD] = remaining_payload_size;
        remaining_block_metadata->prev = pred_metadata;
        remaining_block_metadata->next = pred_metadata->next;

        if (pred_metadata == tail) {
            tail = remaining_block_metadata;
        } else{
            pred_metadata->next->prev = remaining_block_metadata;
        }
        pred_metadata->next = remaining_block_metadata;

        // blocks_count[TOTAL] doesn't change
        // blocks_count[FREE] doesn't change
        // bytes_count[TOTAL] doesn't change
        bytes_count[FREE] -= pred_payload_size;
        bytes_count[FREE] += remaining_payload_size;
    } else {
        blocks_count[TOTAL]--;
        blocks_count[FREE]--;
        // bytes_count[TOTAL] doesn't change
        bytes_count[FREE] -= pred_payload_size;
    }

    memmove(pred_metadata->getPayloadBlockAddr(),
           old_block_metadata->getPayloadBlockAddr(),
           old_block_metadata->size[TOTAL_PAYLOAD]); // instead of ACTIVE_PAYLOAD

    return pred_metadata->getPayloadBlockAddr();
}

void* HeapBlocksList::reallocateUsingSuccOnly(MallocMetadata *old_block_metadata,
        size_t new_payload_size) {
    // here succ exists, free and large enough

    MallocMetadata* succ_metadata = old_block_metadata->next;
    size_t original_succ_payload_size = succ_metadata->size[TOTAL_PAYLOAD];
    size_t total_avail_payload_size = old_block_metadata->size[TOTAL_PAYLOAD]
                                      + sizeof(MallocMetadata)
                                      + original_succ_payload_size;

    old_block_metadata->next = succ_metadata->next;
    if (tail == succ_metadata) {
        tail = old_block_metadata;
    } else {
        succ_metadata->next->prev = old_block_metadata;
    }

    old_block_metadata->size[TOTAL_PAYLOAD] = total_avail_payload_size;
    old_block_metadata->size[ACTIVE_PAYLOAD] = new_payload_size;

    size_t remaining_payload_size = 0;
    if (total_avail_payload_size - new_payload_size > sizeof(MallocMetadata)) {
        remaining_payload_size = total_avail_payload_size
                                 - new_payload_size
                                 - sizeof(MallocMetadata);
    }

    if (remaining_payload_size >= SPLITTING_THRESHOLD) {
        auto* remaining_block_metadata =
                (MallocMetadata*)((char*)old_block_metadata
                                  + sizeof(MallocMetadata)
                                  + new_payload_size);
        remaining_block_metadata->is_free = true;
        remaining_block_metadata->size[ACTIVE_PAYLOAD] = 0;
        remaining_block_metadata->size[TOTAL_PAYLOAD] = remaining_payload_size;
        remaining_block_metadata->prev = old_block_metadata;
        remaining_block_metadata->next = old_block_metadata->next;

        if (old_block_metadata == tail) {
            tail = remaining_block_metadata;
        } else {
            old_block_metadata->next->prev = remaining_block_metadata;
        }
        old_block_metadata->next = remaining_block_metadata;

        // blocks_count[TOTAL] doesn't change
        // blocks_count[FREE] doesn't change
        // bytes_count[TOTAL] doesn't change
        bytes_count[FREE] -= original_succ_payload_size;
        bytes_count[FREE] += remaining_payload_size;
    } else {
        blocks_count[TOTAL]--;
        blocks_count[FREE]--;
        // bytes_count[TOTAL] doesn't change
        bytes_count[FREE] -= original_succ_payload_size;
    }

    // no memmove needed

    return old_block_metadata->getPayloadBlockAddr();
}

void *HeapBlocksList::reallocateUsingPredAndSucc(MallocMetadata *old_block_metadata,
        size_t new_payload_size) {
    // here pred and succ exists, free and large enough

    MallocMetadata* pred_metadata = old_block_metadata->prev;
    MallocMetadata* succ_metadata = old_block_metadata->next;
    size_t original_pred_payload_size = pred_metadata->size[TOTAL_PAYLOAD];
    size_t original_succ_payload_size = succ_metadata->size[TOTAL_PAYLOAD];

    size_t total_avail_payload_size =
            original_pred_payload_size
            + sizeof(MallocMetadata) + old_block_metadata->size[TOTAL_PAYLOAD]
            + sizeof(MallocMetadata) + original_succ_payload_size;

    pred_metadata->next = succ_metadata->next;
    if (tail == succ_metadata) {
        tail = pred_metadata;
    } else {
        succ_metadata->next->prev = pred_metadata;
    }

    pred_metadata->is_free = false;
    pred_metadata->size[TOTAL_PAYLOAD] = total_avail_payload_size;
    pred_metadata->size[ACTIVE_PAYLOAD] = new_payload_size;

    size_t remaining_payload_size = 0;
    if (total_avail_payload_size - new_payload_size > sizeof(MallocMetadata)) {
        remaining_payload_size = total_avail_payload_size
                                 - new_payload_size
                                 - sizeof(MallocMetadata);
    }

    if (remaining_payload_size >= SPLITTING_THRESHOLD) {
        auto* remaining_block_metadata =
                (MallocMetadata*)((char*)pred_metadata
                                  + sizeof(MallocMetadata)
                                  + new_payload_size);
        remaining_block_metadata->is_free = true;
        remaining_block_metadata->size[ACTIVE_PAYLOAD] = 0;
        remaining_block_metadata->size[TOTAL_PAYLOAD] = remaining_payload_size;
        remaining_block_metadata->prev = pred_metadata;
        remaining_block_metadata->next = pred_metadata->next;

        if (pred_metadata == tail) {
            tail = remaining_block_metadata;
        } else {
            pred_metadata->next->prev = remaining_block_metadata;
        }
        pred_metadata->next = remaining_block_metadata;

        blocks_count[TOTAL] += -2 + 1;
        blocks_count[FREE] += -2 + 1;
        // bytes_count[TOTAL] doesn't change
        bytes_count[FREE] -= original_pred_payload_size + original_succ_payload_size;
        bytes_count[FREE] += remaining_payload_size;
    } else {
        blocks_count[TOTAL] -= 2;
        blocks_count[FREE] -= 2;
        // bytes_count[TOTAL] doesn't change
        bytes_count[FREE] -= original_pred_payload_size + original_succ_payload_size;
    }

    memmove(pred_metadata->getPayloadBlockAddr(),
           old_block_metadata->getPayloadBlockAddr(),
           old_block_metadata->size[TOTAL_PAYLOAD]); // instead of ACTIVE_PAYLOAD

    return pred_metadata->getPayloadBlockAddr();
}

void *HeapBlocksList::reallocateToOtherBlock(MallocMetadata *old_block_metadata,
        void* old_payload_addr, size_t new_payload_size) {

    MallocMetadata* free_block_metadata = findFreeBlock(new_payload_size);
    void* new_payload_block_addr = NULL;

    if (free_block_metadata == NULL) { // use sbrk
        new_payload_block_addr = createNewBlock(new_payload_size);
        if (new_payload_block_addr == NULL) {
            // sbrk() failed
            return NULL;
        }
    } else { // use free block
        new_payload_block_addr = useFreeBlock(free_block_metadata,
                                              new_payload_size);
    }

    memmove(new_payload_block_addr,
           old_payload_addr,
           old_block_metadata->size[TOTAL_PAYLOAD]); // instead of ACTIVE_PAYLOAD
    releaseUsedBlock(old_payload_addr);

    return new_payload_block_addr;
}

// ----------------------------------------------------------------------------

class MMappedBlocksManager{
public:
    size_t total_blocks_count;
    size_t total_bytes_count;

    MMappedBlocksManager();

    void* allocateBlock(size_t payload_size);

    void* createNewBlock(size_t payload_size);

    void setNewBlockMetaData(size_t payload_size,
            MallocMetadata* block_metadata);

    void releaseUsedBlock(void* payload_addr);

    void* reallocateActiveBlock(void* old_payload_addr,
            size_t new_payload_size);
};

// ----------------------------------------------------------------------------

MMappedBlocksManager::MMappedBlocksManager()
        : total_blocks_count(0), total_bytes_count(0)
{}

void *MMappedBlocksManager::allocateBlock(size_t payload_size) {
    // only option is to allocate a new block with mmap
    return createNewBlock(payload_size);
}

void *MMappedBlocksManager::createNewBlock(size_t payload_size) {
    size_t needed_allocation_size = payload_size + sizeof(MallocMetadata);

    /* If total_needed_size isn't a page size multiple, it will be rounded up
     * to page size multiple */
    void* block_addr = mmap(NULL, needed_allocation_size,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS,
                            -1,
                            0);
    if (block_addr == (void*)-1) {
        // mmap failed
        return NULL;
    }

    auto* metadata_addr = (MallocMetadata*)block_addr;
    setNewBlockMetaData(payload_size, metadata_addr);

    total_blocks_count++;
    total_bytes_count += needed_allocation_size; // including metadata

    return metadata_addr->getPayloadBlockAddr();
}

void MMappedBlocksManager::setNewBlockMetaData(size_t payload_size,
        MallocMetadata *block_metadata) {
    block_metadata->size[TOTAL_PAYLOAD] = payload_size;
    block_metadata->size[ACTIVE_PAYLOAD] = payload_size;
}

void MMappedBlocksManager::releaseUsedBlock(void *payload_addr) {
    // here payload_addr != NULL

    auto* block_metadata = (MallocMetadata*)payload_addr - 1;

    size_t block_size = block_metadata->size[TOTAL_PAYLOAD]
                        + sizeof(MallocMetadata);
    munmap((void*)block_metadata, block_size);
    total_blocks_count--;
    total_bytes_count -= block_size;
}

void *MMappedBlocksManager::reallocateActiveBlock(void *old_payload_addr,
        size_t new_payload_size) {
    MallocMetadata* old_block_metadata = NULL;
    if (old_payload_addr != NULL) {
        old_block_metadata = (MallocMetadata*)old_payload_addr - 1;
    }

    // always reallocate to a new block
    void* new_payload_addr = allocateBlock(new_payload_size);
    if (new_payload_addr != NULL && old_payload_addr != NULL) {
        memmove(new_payload_addr, old_payload_addr,
                min(new_payload_size, old_block_metadata->size[ACTIVE_PAYLOAD]));
        releaseUsedBlock(old_payload_addr);
    }

    return new_payload_addr;
}

// ----------------------------------------------------------------------------

class MemoryManager {
public:
    HeapBlocksList heap_blocks_list;
    MMappedBlocksManager mmapped_blocks;

    void* allocateBlock(size_t payload_size);

    void* allocateZeroedBlock(size_t payload_size);

    void releaseUsedBlock(void* payload_addr);

    void* reallocateActiveBlock(void* old_payload_addr, size_t new_payload_size);

    size_t getBlocksCount(BytesType type);

    size_t getBytesCount(BytesType type);

    size_t getMetaDataSize();
};

void *MemoryManager::allocateBlock(size_t payload_size) {
    if (payload_size >= 128 * KB) {
        return mmapped_blocks.allocateBlock(payload_size);
    } else {
        return heap_blocks_list.allocateBlock(payload_size);
    }
}

void *MemoryManager::allocateZeroedBlock(size_t payload_size) {
    if (payload_size >= 128 * KB) {
        return mmapped_blocks.allocateBlock(payload_size);
    } else {
        return heap_blocks_list.allocateZeroedBlock(payload_size);
    }
}

void MemoryManager::releaseUsedBlock(void *payload_addr) {
    auto* block_metadata = (MallocMetadata*)payload_addr - 1;

    if (block_metadata->size[ACTIVE_PAYLOAD] < 128 * KB) {
        heap_blocks_list.releaseUsedBlock(payload_addr);
    } else {
        mmapped_blocks.releaseUsedBlock(payload_addr);
    }
}

void *MemoryManager::reallocateActiveBlock(void *old_payload_addr,
        size_t new_payload_size) {
    if (new_payload_size >= 128 * KB) {
        return mmapped_blocks.reallocateActiveBlock(old_payload_addr,
                new_payload_size);
    } else {
        return heap_blocks_list.reallocateActiveBlock(old_payload_addr,
                new_payload_size);
    }
}

size_t MemoryManager::getBlocksCount(BytesType type) {
    if (type == FREE) {
        return heap_blocks_list.blocks_count[FREE];
    } else {
        return heap_blocks_list.blocks_count[TOTAL]
               + mmapped_blocks.total_blocks_count;
    }
}

size_t MemoryManager::getBytesCount(BytesType type) {
    if (type == FREE) {
        return heap_blocks_list.bytes_count[FREE];
    } else {
        return heap_blocks_list.bytes_count[TOTAL] + mmapped_blocks.total_bytes_count
               - _num_meta_data_bytes();
    }
}

size_t MemoryManager::getMetaDataSize() {
    return sizeof(MallocMetadata);
}

// ----------------------------------------------------------------------------

MemoryManager memory_manager;

// malloc family of functions implementations

void* smalloc(size_t size) {
    if (size == 0 || size > 1e8) {
        return NULL;
    }

    return memory_manager.allocateBlock(size);
}

void* scalloc(size_t num, size_t size) {
    if (size == 0 || num == 0 || size > 1e8 || num > 1e8 || size*num > 1e8) {
        return NULL;
    }

    return memory_manager.allocateZeroedBlock(size*num);
}

void sfree(void* p) {
    if (p == NULL) {
        return;
    }

    memory_manager.releaseUsedBlock(p);
}

void* srealloc(void* oldp, size_t size) {
    if (size == 0 || size > 1e8) {
        return NULL;
    }

    return memory_manager.reallocateActiveBlock(oldp, size);
}

// ----------------------------------------------------------------------------

// private functions for testing prototypes

size_t _num_free_blocks() {
    return memory_manager.getBlocksCount(FREE);
}

size_t _num_free_bytes() {
    return memory_manager.getBytesCount(FREE);
}

size_t _num_allocated_blocks() {
    return memory_manager.getBlocksCount(TOTAL);
}

size_t _num_allocated_bytes() {
    return memory_manager.getBytesCount(TOTAL);
}

size_t _num_meta_data_bytes() {
    return memory_manager.getBlocksCount(TOTAL) * memory_manager.getMetaDataSize();
}

size_t _size_meta_data() {
    return memory_manager.getMetaDataSize();
}
