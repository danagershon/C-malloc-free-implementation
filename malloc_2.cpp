#include <unistd.h>
#include <string.h>

// malloc family of functions prototypes

void* smalloc(size_t size);

void* scalloc(size_t num, size_t size);

void sfree(void* p);

void* srealloc(void* oldp, size_t size);

// ----------------------------------------------------------------------------

// private functions for testing prototypes

size_t _num_free_blocks();

size_t _num_free_bytes();

size_t _num_allocated_blocks();

size_t _num_allocated_bytes();

size_t _num_meta_data_bytes();

size_t _size_meta_data();

// ----------------------------------------------------------------------------

// BlocksList class definition

typedef enum {
    FREE = 0,
    TOTAL = 1
} BytesType;

typedef enum {
    INITIAL = 0,
    ACTIVE = 1
} SizeType;

class BlocksList {
public:
    class MallocMetadata {
    public:
        size_t payload_size[2];
        bool is_free;
        MallocMetadata* next;
        MallocMetadata* prev; // no use

        MallocMetadata() = default;
    };

    MallocMetadata *head, *tail;
    size_t blocks_count[2];
    size_t bytes_count[2];

    BlocksList();

    /* search and return a pointer to the metadata of a free block whose size is
     * at least @payload_size. If not found, return NULL */
    MallocMetadata* findFreeBlock(size_t payload_size);

    /* for smalloc. Allocate a block on heap with active size @payload_size
     * and return a pointer to the start of payload block. Use an inactive
     * free block or create a new block.
     * If cannot allocate (because sbrk() fails) return NULL */
    void* allocateBlock(size_t payload_size);

    /* for calloc. Allocate like allocateBlock but zero out the payload block
     * with memset */
    void* allocateZeroedBlock(size_t payload_size);

    /* create a new block on heap using sbrk(). This block will become
     * the last block in the blocks list */
    void* createNewBlock(size_t payload_size);

    /* @free_block_metadata is of a block whose size is at least
     * @new_active_payload_size. Update the metadata of the block so it
     * becomes allocated with @new_active_payload_size */
    void* useFreeBlock(MallocMetadata* free_block_metadata,
            size_t new_active_payload_size);

    void setNewBlockMetaData(size_t payload_size,
            MallocMetadata* block_metadata);

    void* getPayloadBlockAddr(MallocMetadata* block_metadata);

    // for sfree
    void releaseUsedBlock(void* payload_addr);

    void* reallocateActiveBlock(void* old_payload_addr,
            size_t new_payload_size);

    void* reallocateBlockToOtherBlock(MallocMetadata*old_block_metadata,
            void* old_payload_addr, size_t new_payload_size);

    // size in bytes
    size_t getMetaDataSize();
};

// ----------------------------------------------------------------------------

// BlockList implementation

BlocksList::BlocksList()
    : head(NULL), tail(NULL)
{
    blocks_count[FREE] = 0;
    blocks_count[TOTAL] = 0;

    bytes_count[FREE] = 0;
    bytes_count[TOTAL] = 0;
}

size_t BlocksList::getMetaDataSize() {
    return sizeof(MallocMetadata);
}

BlocksList::MallocMetadata* BlocksList::findFreeBlock(size_t payload_size) {
    MallocMetadata* curr_block_metadata = head;

    while (curr_block_metadata != NULL) {
        if (curr_block_metadata->is_free
            && curr_block_metadata->payload_size[INITIAL] >= payload_size) {
            break;
        }
        curr_block_metadata = curr_block_metadata->next;
    }

    return curr_block_metadata;
}

void *BlocksList::allocateBlock(size_t payload_size) {
    MallocMetadata* free_block_metadata = findFreeBlock(payload_size);
    void* payload_block_addr = NULL;

    if (free_block_metadata == NULL) {
        // couldn't find free block large enough so use sbrk()
        payload_block_addr = createNewBlock(payload_size);
    } else {
        // found a large enough free block so use it
        payload_block_addr = useFreeBlock(free_block_metadata, payload_size);
    }

    return payload_block_addr;
}

void *BlocksList::createNewBlock(size_t payload_size) {
    void* old_prog_break = sbrk(0);
    size_t total_allocation_size = getMetaDataSize() + payload_size;

    if (sbrk(static_cast<int>(total_allocation_size)) == (void*)-1) {
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

    return getPayloadBlockAddr(new_block_metadata);
}

void BlocksList::setNewBlockMetaData(size_t payload_size,
        MallocMetadata* block_metadata) {
    block_metadata->is_free = false;

    block_metadata->payload_size[INITIAL] = payload_size;
    block_metadata->payload_size[ACTIVE] = payload_size;

    block_metadata->next = NULL;
    block_metadata->prev = tail;
}

void *BlocksList::getPayloadBlockAddr(MallocMetadata *block_metadata) {
    return (void*)(block_metadata + 1);
    // equivalent to (char*)block_metadata + getMetaDataSize()
}

void *BlocksList::useFreeBlock(MallocMetadata* free_block_metadata,
        size_t new_active_payload_size) {
    free_block_metadata->is_free = false;
    free_block_metadata->payload_size[ACTIVE] = new_active_payload_size;

    // blocks_count[TOTAL] doesn't change
    blocks_count[FREE]--;
    // bytes_count[TOTAL] doesn't change
    bytes_count[FREE] -= free_block_metadata->payload_size[INITIAL];

    return getPayloadBlockAddr(free_block_metadata);
}

void BlocksList::releaseUsedBlock(void *payload_addr) {
    // here payload_addr != NULL

    auto *block_metadata = (MallocMetadata*)(payload_addr) - 1;
    if (block_metadata->is_free) {
        // we allow double free
        return;
    }

    block_metadata->is_free = true;
    block_metadata->payload_size[ACTIVE] = 0;
    // block_metadata->payload_size[INITIAL] doesn't change

    // blocks_count[TOTAL] doesn't change
    blocks_count[FREE]++;
    // bytes_count[TOTAL] doesn't change
    bytes_count[FREE] += block_metadata->payload_size[INITIAL];
}

void *BlocksList::allocateZeroedBlock(size_t payload_size) {
    void* payload_block_addr = allocateBlock(payload_size);

    if (payload_block_addr != NULL) {
        // zeroing only the part of block the user asked for
        memset(payload_block_addr, 0, payload_size);
    }

    return payload_block_addr;
}

void *BlocksList::reallocateActiveBlock(void *old_payload_addr,
        size_t new_payload_size) {

    if (old_payload_addr == NULL) {
        // act like a call to smalloc(new_payload_size)
        return allocateBlock(new_payload_size);
    }

    auto *old_block_metadata = (MallocMetadata*)old_payload_addr - 1;
    // equivalent to (MallocMetadata*)((char*)old_payload_addr - getMetaDataSize())

    if (old_block_metadata->payload_size[INITIAL] >= new_payload_size) {
        // current block is large enough
        old_block_metadata->payload_size[ACTIVE] = new_payload_size;
        return old_payload_addr;
    }else {
        // current block not large enough
        return reallocateBlockToOtherBlock(old_block_metadata, old_payload_addr, new_payload_size);
    }
}

void *BlocksList::reallocateBlockToOtherBlock(MallocMetadata *old_block_metadata,
        void* old_payload_addr, size_t new_payload_size) {

    MallocMetadata* free_block_metadata = findFreeBlock(new_payload_size);
    void* new_payload_block_addr = NULL;

    if (free_block_metadata == NULL) { // use sbrk
        new_payload_block_addr = createNewBlock(new_payload_size);
        if (new_payload_block_addr == NULL) {
            return NULL;
        }
    } else { // use free block
        new_payload_block_addr = useFreeBlock(free_block_metadata,
                new_payload_size);
    }

    memmove(new_payload_block_addr, old_payload_addr,
            old_block_metadata->payload_size[ACTIVE]);
    releaseUsedBlock(old_payload_addr);

    return new_payload_block_addr;
}

// ----------------------------------------------------------------------------

// malloc family of functions implementations

// global blocks list, free (inactive) and allocated (active)
BlocksList blocks_list;

void* smalloc(size_t size) {
    if (size == 0 || size > 1e8) {
        return NULL;
    }

    return blocks_list.allocateBlock(size);
}

void* scalloc(size_t num, size_t size) {
    if (size == 0 || num == 0 || size*num > 1e8) {
        return NULL;
    }

    return blocks_list.allocateZeroedBlock(size*num);
}

void sfree(void* p) {
    if (p == NULL) {
        return;
    }

    blocks_list.releaseUsedBlock(p);
}

void* srealloc(void* oldp, size_t size) {
    if (size == 0 || size > 1e8) {
        return NULL;
    }

    return blocks_list.reallocateActiveBlock(oldp, size);
}

// ----------------------------------------------------------------------------

// private functions for testing prototypes

size_t _num_free_blocks() {
    return blocks_list.blocks_count[FREE];
}

size_t _num_free_bytes() {
    return blocks_list.bytes_count[FREE];
}

size_t _num_allocated_blocks() {
    return blocks_list.blocks_count[TOTAL];
}

size_t _num_allocated_bytes() {
    return blocks_list.bytes_count[TOTAL] - _num_meta_data_bytes();
}

size_t _num_meta_data_bytes() {
    return blocks_list.blocks_count[TOTAL] * blocks_list.getMetaDataSize();
}

size_t _size_meta_data() {
    return blocks_list.getMetaDataSize();
}
