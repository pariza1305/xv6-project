#ifndef MEMSTAT_H
#define MEMSTAT_H

#include "types.h"

#define MAX_PAGES_INFO 128 // Max pages to report per syscall

// Page states
#define UNMAPPED 0 
#define RESIDENT 1 
#define SWAPPED  2

struct page_stat {
  uint va;       // Virtual address
  int state;     // Page state (UNMAPPED/RESIDENT/SWAPPED)
  int is_dirty;  // Whether page has been written to
  int seq;       // FIFO sequence number
  int swap_slot; // Swap slot number if swapped
};

struct proc_mem_stat {
  int pid;
  int num_pages_total;     // Total virtual pages allocated
  int num_resident_pages;  // Pages currently in memory
  int num_swapped_pages;   // Pages currently swapped out
  int next_fifo_seq;       // Next FIFO sequence number
  struct page_stat pages[MAX_PAGES_INFO];
};

#endif
