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

// Find the victim page using FIFO (oldest resident page)
// Returns the index in the pages array, or -1 if no victim found
static int find_victim_fifo(struct proc *p) {
  int victim_idx = -1;
  int min_fifo_seq = 2147483647;  // Max int value
  
  // Find the page with the smallest FIFO sequence number that is resident
  for(int i = 0; i < p->num_pages; i++) {
    if(p->pages[i].state == RESIDENT && p->pages[i].fifo_seq < min_fifo_seq) {
      min_fifo_seq = p->pages[i].fifo_seq;
      victim_idx = i;
    }
  }
  
  return victim_idx;
}

// Evict a page to swap file (internal version)
static int do_evict_page(struct proc *p, int page_idx) {
  struct page_info *page = &p->pages[page_idx];
  uint64 va = page->va;
  
  // Get physical address of the page
  uint64 pa = walkaddr(p->pagetable, va);
  
  // For simplicity, treat text pages as clean (read-only) and data/heap/stack as potentially dirty
  // We would need to track actual dirty bits in a real implementation
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

// Reload a page from swap file
static int reload_page_from_swap(struct proc *p, uint64 va, struct page_info *page) {
  if(page->state != SWAPPED || page->swap_slot == -1) {
    return -1;  // Page not in swap
  }
  
  if(p->swap_file == 0) {
    return -1;  // No swap file
  }
  
  // Allocate a physical page
  char *mem = kalloc();
  if(mem == 0) {
    return -1;  // Out of memory
  }
  
  // Read page from swap file
  uint offset = page->swap_slot * PGSIZE;
  begin_op();
  int result = readi(p->swap_file->ip, 0, (uint64)mem, offset, PGSIZE);
  end_op();
  if(result != PGSIZE) {
    kfree(mem);
    return -1;  // Read failed
  }
  
  // Map the page back into the page table
  // Determine permissions based on memory region
  int perm = PTE_U | PTE_R | PTE_W;  // Default to writable
  if(va >= p->text_start && va < p->text_end) {
    perm = PTE_U | PTE_R | PTE_X;  // Text is read-only executable
  }
  
  if(mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm) != 0) {
    kfree(mem);
    return -1;  // Mapping failed
  }
  
  // Update page state
  page->state = RESIDENT;
  int old_swap_slot = page->swap_slot;
  page->fifo_seq = p->next_fifo_seq++;
  
  // Free the swap slot
  if(page->swap_slot >= 0 && page->swap_slot < 1024) {
    p->swap_slots[page->swap_slot] = 0;
    p->num_swap_slots_used--;
    printf("[pid %d] SWAPIN va=0x%lx slot=%d\n", p->pid, va, old_swap_slot);
    page->swap_slot = -1;
  }
  
  printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, page->fifo_seq);
  
  return 0;
}

int handle_page_fault(struct proc *p, uint64 fault_addr, int scause) {
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
  // If it's swapped, reload it from swap
  for(int i = 0; i < p->num_pages; i++) {
    if(p->pages[i].va == va) {
      if(p->pages[i].state == SWAPPED) {
        // Page is in swap, reload it
        printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=swap\n", p->pid, fault_addr, access);
        if(reload_page_from_swap(p, va, &p->pages[i]) == 0) {
          return 0;
        } else {
          printf("[pid %d] KILL swapin-failed va=0x%lx access=%s\n", p->pid, fault_addr, access);
          setkilled(p);
          return -1;
        }
      } else if(p->pages[i].state == RESIDENT) {
        // Page is already resident, shouldn't get a fault
        return 0;
      }
    }
  }
  
  // Page not in resident/swapped set, allocate it
  
  // Check if the faulting address is within the text segment
  if (va >= p->text_start && va < p->text_end) {
    printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=exec\n", p->pid, fault_addr, access);
    
    char *mem = kalloc();
    if (mem == 0) {
      // Memory full - try page replacement
      printf("[pid %d] MEMFULL\n", p->pid);
      int victim_idx = find_victim_fifo(p);
      if(victim_idx == -1) {
        printf("[pid %d] KILL oom va=0x%lx access=%s\n", p->pid, fault_addr, access);
        setkilled(p);
        return -1;
      }
      
      printf("[pid %d] VICTIM va=0x%lx seq=%d algo=FIFO\n", p->pid, p->pages[victim_idx].va, p->pages[victim_idx].fifo_seq);
      if(do_evict_page(p, victim_idx) != 0) {
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

    // Find the program header for this address and load from file
    struct proghdr ph;
    struct inode *ip = p->exec_inode;
    struct elfhdr elf;

    readi(ip, 0, (uint64)&elf, 0, sizeof(elf));
    for(int i=0, file_off=elf.phoff; i<elf.phnum; i++, file_off+=sizeof(ph)){
      readi(ip, 0, (uint64)&ph, file_off, sizeof(ph));
      if(ph.type == ELF_PROG_LOAD && va >= ph.vaddr && va < ph.vaddr + ph.memsz) {
        // Load from file
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
      p->pages[p->num_pages].va = va;
      p->pages[p->num_pages].state = RESIDENT;
      p->pages[p->num_pages].is_dirty = 0;
      p->pages[p->num_pages].fifo_seq = p->next_fifo_seq++;
      p->pages[p->num_pages].swap_slot = -1;
      p->num_pages++;
    }
    
    printf("[pid %d] LOADEXEC va=0x%lx\n", p->pid, va);
    printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, p->pages[p->num_pages - 1].fifo_seq);
    return 0;
  }

  // Check if the faulting address is within the data segment
  if (va >= p->data_start && va < p->data_end) {
    printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=data\n", p->pid, fault_addr, access);
    
    char *mem = kalloc();
    if (mem == 0) {
      // Memory full - try page replacement
      printf("[pid %d] MEMFULL\n", p->pid);
      int victim_idx = find_victim_fifo(p);
      if(victim_idx == -1) {
        printf("[pid %d] KILL oom va=0x%lx access=%s\n", p->pid, fault_addr, access);
        setkilled(p);
        return -1;
      }
      
      printf("[pid %d] VICTIM va=0x%lx seq=%d algo=FIFO\n", p->pid, p->pages[victim_idx].va, p->pages[victim_idx].fifo_seq);
      if(do_evict_page(p, victim_idx) != 0) {
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

    // Find the program header for this address and load from file
    struct proghdr ph;
    struct inode *ip = p->exec_inode;
    struct elfhdr elf;

    readi(ip, 0, (uint64)&elf, 0, sizeof(elf));
    for(int i=0, file_off=elf.phoff; i<elf.phnum; i++, file_off+=sizeof(ph)){
      readi(ip, 0, (uint64)&ph, file_off, sizeof(ph));
      if(ph.type == ELF_PROG_LOAD && va >= ph.vaddr && va < ph.vaddr + ph.memsz) {
        // Load from file if within filesz
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
    
    // Track this page in the resident set
    if(p->num_pages < MAX_PROC_PAGES) {
      p->pages[p->num_pages].va = va;
      p->pages[p->num_pages].state = RESIDENT;
      p->pages[p->num_pages].is_dirty = 0;
      p->pages[p->num_pages].fifo_seq = p->next_fifo_seq++;
      p->pages[p->num_pages].swap_slot = -1;
      p->num_pages++;
    }
    
    printf("[pid %d] LOADEXEC va=0x%lx\n", p->pid, va);
    printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, p->pages[p->num_pages - 1].fifo_seq);
    return 0;
  }

  // Check if the faulting address is in the heap
  if (va >= p->data_end && va < p->sz) {
    printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=heap\n", p->pid, fault_addr, access);
    
    char *mem = kalloc();
    if (mem == 0) {
      // Memory full - try page replacement
      printf("[pid %d] MEMFULL\n", p->pid);
      int victim_idx = find_victim_fifo(p);
      if(victim_idx == -1) {
        printf("[pid %d] KILL oom va=0x%lx access=%s\n", p->pid, fault_addr, access);
        setkilled(p);
        return -1;
      }
      
      printf("[pid %d] VICTIM va=0x%lx seq=%d algo=FIFO\n", p->pid, p->pages[victim_idx].va, p->pages[victim_idx].fifo_seq);
      if(do_evict_page(p, victim_idx) != 0) {
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
    
    if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, PTE_U | PTE_R | PTE_W) != 0) {
      kfree(mem);
      printf("[pid %d] KILL mapfail va=0x%lx access=%s\n", p->pid, fault_addr, access);
      setkilled(p);
      return -1;
    }
    
    // Track this page in the resident set
    if(p->num_pages < MAX_PROC_PAGES) {
      p->pages[p->num_pages].va = va;
      p->pages[p->num_pages].state = RESIDENT;
      p->pages[p->num_pages].is_dirty = 0;
      p->pages[p->num_pages].fifo_seq = p->next_fifo_seq++;
      p->pages[p->num_pages].swap_slot = -1;
      p->num_pages++;
    }
    
    printf("[pid %d] ALLOC va=0x%lx\n", p->pid, va);
    printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, p->pages[p->num_pages - 1].fifo_seq);
    return 0;
  }

  // Check if the faulting address is for the stack
  // Stack grows downward; allow one page below current sp
  if (va < p->trapframe->sp && va >= p->trapframe->sp - PGSIZE) {
    printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=stack\n", p->pid, fault_addr, access);
    
    char *mem = kalloc();
    if (mem == 0) {
      // Memory full - try page replacement
      printf("[pid %d] MEMFULL\n", p->pid);
      int victim_idx = find_victim_fifo(p);
      if(victim_idx == -1) {
        printf("[pid %d] KILL oom va=0x%lx access=%s\n", p->pid, fault_addr, access);
        setkilled(p);
        return -1;
      }
      
      printf("[pid %d] VICTIM va=0x%lx seq=%d algo=FIFO\n", p->pid, p->pages[victim_idx].va, p->pages[victim_idx].fifo_seq);
      if(do_evict_page(p, victim_idx) != 0) {
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
    
    if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, PTE_U | PTE_R | PTE_W) != 0) {
      kfree(mem);
      printf("[pid %d] KILL mapfail va=0x%lx access=%s\n", p->pid, fault_addr, access);
      setkilled(p);
      return -1;
    }
    
    // Track this page in the resident set
    if(p->num_pages < MAX_PROC_PAGES) {
      p->pages[p->num_pages].va = va;
      p->pages[p->num_pages].state = RESIDENT;
      p->pages[p->num_pages].is_dirty = 0;
      p->pages[p->num_pages].fifo_seq = p->next_fifo_seq++;
      p->pages[p->num_pages].swap_slot = -1;
      p->num_pages++;
    }
    
    printf("[pid %d] ALLOC va=0x%lx\n", p->pid, va);
    printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, p->pages[p->num_pages - 1].fifo_seq);
    return 0;
  }

  // Invalid address - outside all valid ranges
  printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=invalid\n", p->pid, fault_addr, access);
  printf("[pid %d] KILL invalid-access va=0x%lx access=%s\n", p->pid, fault_addr, access);
  setkilled(p);
  return -1;
}
