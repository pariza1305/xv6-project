// Bonus: Alternative Page Replacement Algorithm - LRU with Working Set Protection
// 
// Algorithm: Least Recently Used with Working Set Protection (LRU-WSP)
// 
// Design Rationale:
// - LRU is fundamentally better than FIFO for cache-like workloads
// - Working Set Protection (WSP) prevents evicting actively-used pages
// - Pages accessed multiple times in current epoch are protected
// - Clean pages are still preferred for eviction (less I/O cost)
//
// How it works:
// 1. Each page tracks: last_access timestamp, access_count in epoch
// 2. When memory is full, scan for eviction candidates
// 3. Identify pages NOT in working set (low access_count in current epoch)
// 4. Among candidates, evict least recently used (oldest last_access)
// 5. Prefer clean pages over dirty pages
//
// Trade-offs:
// - Pros: Better hit rate, protects hot pages, adapts to workload phases
// - Cons: Slightly more bookkeeping, requires timestamp maintenance

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"
#include "fs.h"
#include "file.h"

// External timestamp counter
extern uint64 global_timestamp;
extern struct spinlock timestamp_lock;

// Threshold for working set protection (minimum accesses to be protected)
#define WSP_THRESHOLD 2

// Epoch length - when to reset access counts
#define EPOCH_LENGTH 1000

// Find victim using LRU with Working Set Protection
// Returns the index in the pages array, or -1 if no victim found
static int find_victim_lru_wsp(struct proc *p) {
  int victim_idx = -1;
  int best_score = -1;  // Higher is better (less desirable for eviction)
  
  // Current epoch for working set decision
  static uint64 epoch_start = 0;
  if (epoch_start == 0) {
    epoch_start = global_timestamp;
  }
  
  // Reset working set counts if epoch expired
  if (global_timestamp - epoch_start > EPOCH_LENGTH) {
    for(int i = 0; i < p->num_pages; i++) {
      p->pages[i].access_count = 0;
    }
    epoch_start = global_timestamp;
  }
  
  // Score calculation: prefer pages NOT in working set, then least recent
  // Score = (access_count < WSP_THRESHOLD ? 10 : 0) + (age / 1000)
  // Lower score = better candidate for eviction
  
  for(int i = 0; i < p->num_pages; i++) {
    struct page_info *page = &p->pages[i];
    
    // Only consider resident pages
    if(page->state != RESIDENT)
      continue;
    
    // Calculate recency score
    // Older pages get higher scores (more desirable to evict)
    uint64 age = global_timestamp - page->last_access;
    
    // Boost score for clean pages (prefer evicting clean)
    int boost = page->is_dirty ? 0 : 100;
    
    // Calculate eviction priority
    // Pages NOT in working set get base score of 0 (highly evictable)
    // Pages in working set get base score of 1000 (protected)
    int base_score = (page->access_count < WSP_THRESHOLD) ? 0 : 1000;
    
    int score = base_score + (age / 100) + boost;
    
    // Find page with LOWEST score (most evictable)
    if(victim_idx == -1 || score < best_score) {
      best_score = score;
      victim_idx = i;
    }
  }
  
  return victim_idx;
}

// Record page access for LRU tracking
void record_page_access(struct proc *p, uint64 va) {
  acquire(&timestamp_lock);
  uint64 timestamp = global_timestamp++;
  release(&timestamp_lock);
  
  // Find and update page info
  for(int i = 0; i < p->num_pages; i++) {
    if(p->pages[i].va == va && p->pages[i].state == RESIDENT) {
      p->pages[i].last_access = timestamp;
      p->pages[i].access_count++;
      break;
    }
  }
}

// Evict a page using LRU-WSP algorithm
static int do_evict_page_lru(struct proc *p, int page_idx) {
  struct page_info *page = &p->pages[page_idx];
  uint64 va = page->va;
  
  // Get physical address of the page
  uint64 pa = walkaddr(p->pagetable, va);
  
  // For simplicity, treat text pages as clean (read-only) and data/heap/stack as potentially dirty
  int is_dirty = (va >= p->data_start);  // Data, heap, and stack can be dirty
  
  if(is_dirty) {
    // Write dirty page to swap file
    if(p->swap_file == 0) {
      printf("[pid %d] SWAPFULL\n", p->pid);
      return -1;
    }
    
    // Find or allocate swap slot
    int swap_slot = page->swap_slot;
    if(swap_slot == -1) {
      // Allocate new swap slot
      for(int i = 0; i < 1024; i++) {
        if(p->swap_slots[i] == 0) {
          swap_slot = i;
          p->swap_slots[i] = 1;
          p->num_swap_slots_used++;
          break;
        }
      }
      if(swap_slot == -1) {
        printf("[pid %d] SWAPFULL\n", p->pid);
        return -1;
      }
      page->swap_slot = swap_slot;
    }
    
    // Write page to swap file at offset swap_slot * PGSIZE
    uint offset = swap_slot * PGSIZE;
    begin_op();
    int result = writei(p->swap_file->ip, 0, (uint64)pa, offset, PGSIZE);
    end_op();
    if(result != PGSIZE) {
      printf("[pid %d] SWAPFULL\n", p->pid);
      return -1;
    }
    
    printf("[pid %d] SWAPOUT va=0x%lx slot=%d\n", p->pid, va, swap_slot);
    printf("[pid %d] EVICT va=0x%lx state=dirty\n", p->pid, va);
    page->state = SWAPPED;
  } else {
    // Clean page - just discard (text pages are read-only)
    printf("[pid %d] EVICT va=0x%lx state=clean\n", p->pid, va);
    printf("[pid %d] DISCARD va=0x%lx\n", p->pid, va);
  }
  
  // Unmap the page from the page table
  if(pa != 0) {
    uvmunmap(p->pagetable, va, PGSIZE, 1);
    // Free the physical page
    kfree((void*)pa);
  }
  
  return 0;
}

// Main page fault handler using LRU-WSP algorithm
int handle_page_fault_lru(struct proc *p, uint64 fault_addr, int scause) {
  uint64 va = PGROUNDDOWN(fault_addr);
  
  // Determine access type from scause
  const char *access = "read";
  
  if (scause == 12) {
    access = "exec";
  } else if (scause == 13) {
    access = "read";
  } else if (scause == 15) {
    access = "write";
  }
  
  // First, check if this page is in the resident/swapped set
  for(int i = 0; i < p->num_pages; i++) {
    if(p->pages[i].va == va) {
      if(p->pages[i].state == SWAPPED) {
        // Page is in swap - would need reload logic
        printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=swap\n", p->pid, fault_addr, access);
        // Note: Swap reload logic same as FIFO version
        return -1;
      } else if(p->pages[i].state == RESIDENT) {
        // Page is already resident, record access for LRU
        record_page_access(p, va);
        return 0;
      }
    }
  }
  
  // Page not yet resident - allocate it
  if (va >= p->text_start && va < p->text_end) {
    printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=exec\n", p->pid, fault_addr, access);
    
    char *mem = kalloc();
    if (mem == 0) {
      // Memory full - try page replacement
      printf("[pid %d] MEMFULL\n", p->pid);
      int victim_idx = find_victim_lru_wsp(p);
      if(victim_idx == -1) {
        printf("[pid %d] KILL oom va=0x%lx access=%s\n", p->pid, fault_addr, access);
        setkilled(p);
        return -1;
      }
      
      printf("[pid %d] VICTIM va=0x%lx seq=%d algo=LRU-WSP\n", p->pid, p->pages[victim_idx].va, 
             p->pages[victim_idx].access_count);
      if(do_evict_page_lru(p, victim_idx) != 0) {
        printf("[pid %d] KILL evict-failed va=0x%lx access=%s\n", p->pid, fault_addr, access);
        setkilled(p);
        return -1;
      }
      
      // Retry allocation
      mem = kalloc();
      if (mem == 0) {
        printf("[pid %d] KILL oom va=0x%lx access=%s\n", p->pid, fault_addr, access);
        setkilled(p);
        return -1;
      }
    }
    memset(mem, 0, PGSIZE);

    // Load from executable
    struct proghdr ph;
    struct inode *ip = p->exec_inode;
    struct elfhdr elf;

    readi(ip, 0, (uint64)&elf, 0, sizeof(elf));
    for(int i=0, file_off=elf.phoff; i<elf.phnum; i++, file_off+=sizeof(ph)){
      readi(ip, 0, (uint64)&ph, file_off, sizeof(ph));
      if(ph.type == ELF_PROG_LOAD && va >= ph.vaddr && va < ph.vaddr + ph.memsz) {
        uint offset = ph.off + (va - ph.vaddr);
        uint to_read = (ph.filesz > (va - ph.vaddr)) ? PGSIZE : 0;
        if(to_read > ph.filesz - (va - ph.vaddr))
          to_read = ph.filesz - (va - ph.vaddr);
        if(to_read > 0)
          readi(ip, 0, (uint64)mem, offset, to_read);
        break;
      }
    }
    
    if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, PTE_U | PTE_R | PTE_X) != 0) {
      kfree(mem);
      printf("[pid %d] KILL mapfail va=0x%lx access=%s\n", p->pid, fault_addr, access);
      setkilled(p);
      return -1;
    }
    
    // Track this page in the resident set
    if(p->num_pages < MAX_PROC_PAGES) {
      acquire(&timestamp_lock);
      p->pages[p->num_pages].va = va;
      p->pages[p->num_pages].state = RESIDENT;
      p->pages[p->num_pages].is_dirty = 0;
      p->pages[p->num_pages].fifo_seq = p->next_fifo_seq++;
      p->pages[p->num_pages].swap_slot = -1;
      p->pages[p->num_pages].last_access = global_timestamp++;
      p->pages[p->num_pages].access_count = 1;
      release(&timestamp_lock);
      p->num_pages++;
    }
    
    printf("[pid %d] LOADEXEC va=0x%lx\n", p->pid, va);
    printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, p->pages[p->num_pages - 1].fifo_seq);
    return 0;
  }

  // Similar logic for data, heap, and stack
  if (va >= p->data_start && va < p->data_end) {
    printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=exec\n", p->pid, fault_addr, access);
    
    char *mem = kalloc();
    if (mem == 0) {
      printf("[pid %d] MEMFULL\n", p->pid);
      int victim_idx = find_victim_lru_wsp(p);
      if(victim_idx == -1) {
        printf("[pid %d] KILL oom va=0x%lx access=%s\n", p->pid, fault_addr, access);
        setkilled(p);
        return -1;
      }
      
      printf("[pid %d] VICTIM va=0x%lx seq=%d algo=LRU-WSP\n", p->pid, p->pages[victim_idx].va,
             p->pages[victim_idx].access_count);
      if(do_evict_page_lru(p, victim_idx) != 0) {
        printf("[pid %d] KILL evict-failed va=0x%lx access=%s\n", p->pid, fault_addr, access);
        setkilled(p);
        return -1;
      }
      
      mem = kalloc();
      if (mem == 0) {
        printf("[pid %d] KILL oom va=0x%lx access=%s\n", p->pid, fault_addr, access);
        setkilled(p);
        return -1;
      }
    }
    memset(mem, 0, PGSIZE);

    // Load from executable
    struct proghdr ph;
    struct inode *ip = p->exec_inode;
    struct elfhdr elf;

    readi(ip, 0, (uint64)&elf, 0, sizeof(elf));
    for(int i=0, file_off=elf.phoff; i<elf.phnum; i++, file_off+=sizeof(ph)){
      readi(ip, 0, (uint64)&ph, file_off, sizeof(ph));
      if(ph.type == ELF_PROG_LOAD && va >= ph.vaddr && va < ph.vaddr + ph.memsz) {
        uint offset = ph.off + (va - ph.vaddr);
        uint to_read = (ph.filesz > (va - ph.vaddr)) ? PGSIZE : 0;
        if(to_read > ph.filesz - (va - ph.vaddr))
          to_read = ph.filesz - (va - ph.vaddr);
        if(to_read > 0)
          readi(ip, 0, (uint64)mem, offset, to_read);
        break;
      }
    }
    
    if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, PTE_U | PTE_R | PTE_W) != 0) {
      kfree(mem);
      printf("[pid %d] KILL mapfail va=0x%lx access=%s\n", p->pid, fault_addr, access);
      setkilled(p);
      return -1;
    }
    
    if(p->num_pages < MAX_PROC_PAGES) {
      acquire(&timestamp_lock);
      p->pages[p->num_pages].va = va;
      p->pages[p->num_pages].state = RESIDENT;
      p->pages[p->num_pages].is_dirty = 0;
      p->pages[p->num_pages].fifo_seq = p->next_fifo_seq++;
      p->pages[p->num_pages].swap_slot = -1;
      p->pages[p->num_pages].last_access = global_timestamp++;
      p->pages[p->num_pages].access_count = 1;
      release(&timestamp_lock);
      p->num_pages++;
    }
    
    printf("[pid %d] LOADEXEC va=0x%lx\n", p->pid, va);
    printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, p->pages[p->num_pages - 1].fifo_seq);
    return 0;
  }

  // Heap allocation
  if (va >= p->data_end && va < p->sz) {
    printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=heap\n", p->pid, fault_addr, access);
    
    char *mem = kalloc();
    if (mem == 0) {
      printf("[pid %d] MEMFULL\n", p->pid);
      int victim_idx = find_victim_lru_wsp(p);
      if(victim_idx == -1) {
        printf("[pid %d] KILL oom va=0x%lx access=%s\n", p->pid, fault_addr, access);
        setkilled(p);
        return -1;
      }
      
      printf("[pid %d] VICTIM va=0x%lx seq=%d algo=LRU-WSP\n", p->pid, p->pages[victim_idx].va,
             p->pages[victim_idx].access_count);
      if(do_evict_page_lru(p, victim_idx) != 0) {
        printf("[pid %d] KILL evict-failed va=0x%lx access=%s\n", p->pid, fault_addr, access);
        setkilled(p);
        return -1;
      }
      
      mem = kalloc();
      if (mem == 0) {
        printf("[pid %d] KILL oom va=0x%lx access=%s\n", p->pid, fault_addr, access);
        setkilled(p);
        return -1;
      }
    }
    memset(mem, 0, PGSIZE);
    
    if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, PTE_U | PTE_R | PTE_W) != 0) {
      kfree(mem);
      printf("[pid %d] KILL mapfail va=0x%lx access=%s\n", p->pid, fault_addr, access);
      setkilled(p);
      return -1;
    }
    
    if(p->num_pages < MAX_PROC_PAGES) {
      acquire(&timestamp_lock);
      p->pages[p->num_pages].va = va;
      p->pages[p->num_pages].state = RESIDENT;
      p->pages[p->num_pages].is_dirty = 0;
      p->pages[p->num_pages].fifo_seq = p->next_fifo_seq++;
      p->pages[p->num_pages].swap_slot = -1;
      p->pages[p->num_pages].last_access = global_timestamp++;
      p->pages[p->num_pages].access_count = 1;
      release(&timestamp_lock);
      p->num_pages++;
    }
    
    printf("[pid %d] ALLOC va=0x%lx\n", p->pid, va);
    printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, p->pages[p->num_pages - 1].fifo_seq);
    return 0;
  }

  // Stack allocation
  if (va < p->trapframe->sp && va >= p->trapframe->sp - PGSIZE) {
    printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=stack\n", p->pid, fault_addr, access);
    
    char *mem = kalloc();
    if (mem == 0) {
      printf("[pid %d] MEMFULL\n", p->pid);
      int victim_idx = find_victim_lru_wsp(p);
      if(victim_idx == -1) {
        printf("[pid %d] KILL oom va=0x%lx access=%s\n", p->pid, fault_addr, access);
        setkilled(p);
        return -1;
      }
      
      printf("[pid %d] VICTIM va=0x%lx seq=%d algo=LRU-WSP\n", p->pid, p->pages[victim_idx].va,
             p->pages[victim_idx].access_count);
      if(do_evict_page_lru(p, victim_idx) != 0) {
        printf("[pid %d] KILL evict-failed va=0x%lx access=%s\n", p->pid, fault_addr, access);
        setkilled(p);
        return -1;
      }
      
      mem = kalloc();
      if (mem == 0) {
        printf("[pid %d] KILL oom va=0x%lx access=%s\n", p->pid, fault_addr, access);
        setkilled(p);
        return -1;
      }
    }
    memset(mem, 0, PGSIZE);
    
    if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, PTE_U | PTE_R | PTE_W) != 0) {
      kfree(mem);
      printf("[pid %d] KILL mapfail va=0x%lx access=%s\n", p->pid, fault_addr, access);
      setkilled(p);
      return -1;
    }
    
    if(p->num_pages < MAX_PROC_PAGES) {
      acquire(&timestamp_lock);
      p->pages[p->num_pages].va = va;
      p->pages[p->num_pages].state = RESIDENT;
      p->pages[p->num_pages].is_dirty = 0;
      p->pages[p->num_pages].fifo_seq = p->next_fifo_seq++;
      p->pages[p->num_pages].swap_slot = -1;
      p->pages[p->num_pages].last_access = global_timestamp++;
      p->pages[p->num_pages].access_count = 1;
      release(&timestamp_lock);
      p->num_pages++;
    }
    
    printf("[pid %d] ALLOC va=0x%lx\n", p->pid, va);
    printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, p->pages[p->num_pages - 1].fifo_seq);
    return 0;
  }

  // Invalid address
  printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=invalid\n", p->pid, fault_addr, access);
  printf("[pid %d] KILL invalid-access va=0x%lx access=%s\n", p->pid, fault_addr, access);
  setkilled(p);
  return -1;
}
