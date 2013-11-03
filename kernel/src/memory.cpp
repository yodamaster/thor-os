#include "memory.hpp"

namespace {

struct bios_mmap_entry {
    uint32_t base_low;
    uint32_t base_high;
    uint32_t length_low;
    uint32_t length_high;
    uint16_t type;
    uint16_t acpi;
    uint32_t damn_padding;
} __attribute__((packed));

std::size_t e820_failed = 0;
std::size_t entry_count = 0;
bios_mmap_entry* e820_address = 0;

mmapentry e820_mmap[32];

void mmap_query(std::size_t cmd, std::size_t* result){
    std::size_t tmp;
    __asm__ __volatile__ ("mov r8, %0; int 62; mov %1, rax" : : "dN" (cmd), "a" (tmp));
    *result = tmp;
}

std::size_t _available_memory;
std::size_t _used_memory;

struct malloc_header_chunk {
    std::size_t size;
    malloc_header_chunk* next;
    malloc_header_chunk* prev;
};

struct malloc_footer_chunk {
    std::size_t size;
};

struct fake_head {
    std::size_t size;
    malloc_header_chunk* next;
    malloc_header_chunk* prev;
    std::size_t size_2;
};

const std::size_t META_SIZE = sizeof(malloc_header_chunk) + sizeof(malloc_footer_chunk);
const std::size_t MIN_SPLIT = 32;
const std::size_t BLOCK_SIZE = 4096;
const std::size_t MIN_BLOCKS = 4;

fake_head head;
malloc_header_chunk* malloc_head = 0;

typedef std::size_t* page_entry;
typedef page_entry* pt_t;
typedef pt_t* pdt_t;
typedef pdt_t* pdpt_t;
typedef pdpt_t* pml4t_t;

mmapentry* current_mmap_entry = nullptr;
std::size_t current_mmap_entry_position;

std::size_t pml4t_index = 0;
std::size_t pdpt_index = 0;
std::size_t pdt_index = 0;
std::size_t pt_index = 256;

std::size_t* allocate_block(std::size_t blocks){
    if(!current_mmap_entry){
        for(std::size_t i = 0; i < entry_count; ++i){
            auto& entry = e820_mmap[i];

            if(entry.type == 1 && entry.base >= 0x100000 && entry.size >= 16384){
                current_mmap_entry = &entry;
                current_mmap_entry_position = entry.base;
                break;
            }
        }
    }

    if(!current_mmap_entry){
        return nullptr;
    }

    pml4t_t pml4t = reinterpret_cast<pml4t_t>(0x70000);
    auto pdpt = reinterpret_cast<pdpt_t>(reinterpret_cast<uintptr_t>(pml4t[pml4t_index]) & ~0xFFF);
    auto pdt = reinterpret_cast<pdt_t>(reinterpret_cast<uintptr_t>(pdpt[pdpt_index]) & ~0xFFF);
    auto pt = reinterpret_cast<pt_t>(reinterpret_cast<uintptr_t>(pdt[pdt_index]) & ~0xFFF);

    if(pt_index + blocks >= 512){
        //TODO Go to a new page table
    }

    for(std::size_t i = 0; i < blocks; ++i){
        pt[pt_index + i] = reinterpret_cast<page_entry>((current_mmap_entry_position + i * BLOCK_SIZE) | 0x3);
    }

    auto block = reinterpret_cast<std::size_t*>(current_mmap_entry_position);

    pt_index += blocks;
    current_mmap_entry_position += blocks * BLOCK_SIZE;

    return block;
}

} //end of anonymous namespace

void init_memory_manager(){
    //Init the fake head
    head.size = 0;
    head.next = nullptr;
    head.prev = nullptr;
    head.size_2 = 0;

    malloc_head = reinterpret_cast<malloc_header_chunk*>(&head);

    auto block = allocate_block(MIN_BLOCKS);
    auto header = reinterpret_cast<malloc_header_chunk*>(block);

    header->size = MIN_BLOCKS * BLOCK_SIZE - META_SIZE;
    header->next = malloc_head;
    header->prev = malloc_head;

    auto footer = reinterpret_cast<malloc_footer_chunk*>(
        reinterpret_cast<uintptr_t>(block) + header->size + sizeof(malloc_header_chunk));
    footer->size = header->size;

    malloc_head->next = header;
    malloc_head->prev = header;
}

std::size_t* k_malloc(std::size_t bytes){
    auto current = malloc_head->next;

    while(true){
        if(current == malloc_head){
            //There are no blocks big enough to hold this request

            std::size_t* block = allocate_block(MIN_BLOCKS);
            auto header = reinterpret_cast<malloc_header_chunk*>(block);
            header->size = MIN_BLOCKS * BLOCK_SIZE - META_SIZE;

            current->next->prev = header;
            current->next = header;

            header->next  = current->next;
            header->prev = current;

            auto footer = reinterpret_cast<malloc_footer_chunk*>(
                reinterpret_cast<uintptr_t>(block) + header->size + sizeof(malloc_header_chunk));
            footer->size = header->size;
        } else if(current->size >= bytes){
            //This block is big enough

            //Is it worth splitting the block ?
            if(current->size - bytes - META_SIZE > MIN_SPLIT){
                auto new_block_size = current->size - bytes - META_SIZE;

                //Set the new size;
                current->size = bytes;

                auto footer = reinterpret_cast<malloc_footer_chunk*>(
                    reinterpret_cast<uintptr_t>(current) + bytes + sizeof(malloc_header_chunk));
                footer->size = bytes;

                auto new_block = reinterpret_cast<malloc_header_chunk*>(
                    reinterpret_cast<uintptr_t>(current) + bytes + META_SIZE);

                new_block->size = new_block_size;
                new_block->next = current->next;
                new_block->prev = current->prev;
                current->prev->next = new_block;
                current->next->prev = new_block;

                auto new_footer = reinterpret_cast<malloc_footer_chunk*>(
                    reinterpret_cast<uintptr_t>(new_block) + new_block_size + sizeof(malloc_header_chunk));
                new_footer->size = new_block_size;

                break;
            } else {
                //Remove this node from the free list
                current->prev->next = current->next;
                current->next->prev = current->prev;

                break;
            }
        }

        current = current->next;
    }

    _used_memory += bytes + META_SIZE;

    //Make sure the node is clean
    current->prev = nullptr;
    current->next = nullptr;

    return reinterpret_cast<std::size_t*>(
        reinterpret_cast<uintptr_t>(current) + sizeof(malloc_header_chunk));
}

void k_free(std::size_t* block){
    auto free_header = reinterpret_cast<malloc_header_chunk*>(
        reinterpret_cast<uintptr_t>(block) - sizeof(malloc_header_chunk));

    _used_memory -= free_header->size + META_SIZE;

    auto header = malloc_head;

    free_header->prev = header;
    free_header->next = header->next;

    header->next->prev = free_header;
    header->next = free_header;
}

void load_memory_map(){
    mmap_query(0, &e820_failed);
    mmap_query(1, &entry_count);
    mmap_query(2, reinterpret_cast<std::size_t*>(&e820_address));

    if(!e820_failed && e820_address){
        for(std::size_t i = 0; i < entry_count; ++i){
            auto& bios_entry = e820_address[i];
            auto& os_entry = e820_mmap[i];

            std::size_t base = bios_entry.base_low + (static_cast<std::size_t>(bios_entry.base_high) << 32);
            std::size_t length = bios_entry.length_low + (static_cast<std::size_t>(bios_entry.length_high) << 32);

            os_entry.base = base;
            os_entry.size = length;
            os_entry.type = bios_entry.type;

            if(os_entry.base == 0 && os_entry.type == 1){
                os_entry.type = 7;
            }

            if(os_entry.type == 1){
                _available_memory += os_entry.size;
            }
        }
    }
}

std::size_t mmap_entry_count(){
    return entry_count;
}

bool mmap_failed(){
    return e820_failed;
}

const mmapentry& mmap_entry(std::size_t i){
    return e820_mmap[i];
}

const char* str_e820_type(std::size_t type){
    switch(type){
        case 1:
            return "Free";
        case 2:
            return "Reserved";
        case 3:
        case 4:
            return "ACPI";
        case 5:
            return "Unusable";
        case 6:
            return "Disabled";
        case 7:
            return "Kernel";
        default:
            return "Unknown";
    }
}

std::size_t available_memory(){
    return _available_memory;
}

std::size_t used_memory(){
    return _used_memory;
}

std::size_t free_memory(){
    return _available_memory - _used_memory;
}