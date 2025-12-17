#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "fs.h"
#include "file.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;   
    if((*pte & PTE_V) == 0)  // has physical page been allocated?
      continue;
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      continue;   // page table entry hasn't been allocated
    if((*pte & PTE_V) == 0)
      continue;   // physical page hasn't been allocated
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;
  
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
      
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// Handle page fault for lazily-allocated page
// Returns physical address if successful, 0 on failure or invalid access
uint64
vmfault(pagetable_t pagetable, uint64 va, int write)
{
  struct proc *p = myproc();
  uint64 mem;
  struct page_info *page;
  
  va = PGROUNDDOWN(va);
  
  // Check if page is already mapped
  if(ismapped(pagetable, va)) {
    return walkaddr(pagetable, va); // Already mapped, return physical address
  }
  
  // Determine access type and cause
  char *access_type = write ? "write" : "read";
  char *cause = "invalid";
  int is_exec = 0;
  
  // Check if address is valid and determine cause
  // NULL pointer check - addresses below PGSIZE are always invalid
  if(va < PGSIZE) {
    printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=invalid\n", p->pid, va, access_type);
    printf("[pid %d] KILL invalid-access va=0x%lx access=%s\n", p->pid, va, access_type);
    return 0;
  }
  
  if(va >= p->text_start && va < p->text_end) {
    cause = "exec";
    is_exec = 1;
  } else if(va >= p->data_start && va < p->data_end) {
    cause = "exec";
    is_exec = 1;
  } else if(va >= p->heap_start && va < p->sz) {
    // This range includes both heap and stack
    // We distinguish by checking if it's near the top (stack) or bottom (heap)
    // Stack grows down from stack_top, heap grows up from heap_start
    uint64 stack_bottom = p->stack_top - USERSTACK * PGSIZE;
    if(va >= stack_bottom && va < p->stack_top) {
      cause = "stack";
    } else {
      cause = "heap";
    }
  } else {
    // Invalid access
    printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=invalid\n", p->pid, va, access_type);
    printf("[pid %d] KILL invalid-access va=0x%lx access=%s\n", p->pid, va, access_type);
    return 0;
  }
  
  // Find or create page info
  page = find_page_info(p, va);
  if(!page) {
    return 0; // Cannot track more pages
  }
  
  // Check if page was swapped out
  if(page->state == SWAPPED) {
    // Need to reload from swap
    cause = "swap";
    printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=swap\n", p->pid, va, access_type);
    
    // Allocate physical memory
    mem = (uint64)kalloc();
    if(mem == 0) {
      // Out of memory - try page replacement
      printf("[pid %d] MEMFULL\n", p->pid);
      
      struct page_info *victim = find_victim_page(p);
      if(!victim) {
        return 0; // No page to evict
      }
      
      printf("[pid %d] VICTIM va=0x%lx seq=%d algo=FIFO\n", 
             p->pid, victim->va, victim->fifo_seq);
      
      if(evict_page(p, victim) != 0) {
        return 0;
      }
      
      // Try allocating again
      mem = (uint64)kalloc();
      if(mem == 0) {
        return 0; // Still no memory
      }
    }
    
    // Read page from swap
    if(read_from_swap(p, va, page->swap_slot, mem) != 0) {
      kfree((void*)mem);
      return 0;
    }
    
    // Map the page
    if(mappages(pagetable, va, PGSIZE, mem, PTE_R|PTE_W|PTE_U) != 0) {
      kfree((void*)mem);
      return 0;
    }
    
    printf("[pid %d] SWAPIN va=0x%lx slot=%d\n", p->pid, va, page->swap_slot);
    
    // Update page state
    page->state = RESIDENT;
    page->fifo_seq = p->next_fifo_seq++;
    page->swap_slot = -1;
    page->is_dirty = 0;
    
    printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, page->fifo_seq);
    
    return walkaddr(pagetable, va);
  }
  
  // Log page fault (not swapped case)
  if(page->state != SWAPPED) {
    printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=%s\n", p->pid, va, access_type, cause);
  }
  
  // Handle different causes
  if(is_exec) {
    // Load from executable
    if(load_exec_page(p, va, p->exec_inode) != 0) {
      return 0;
    }
    printf("[pid %d] LOADEXEC va=0x%lx\n", p->pid, va);
  } else {
    // Allocate zero-filled page for heap/stack
    mem = (uint64)kalloc();
    if(mem == 0) {
      // Out of memory - try page replacement
      printf("[pid %d] MEMFULL\n", p->pid);
      
      struct page_info *victim = find_victim_page(p);
      if(!victim) {
        return 0; // No page to evict
      }
      
      printf("[pid %d] VICTIM va=0x%lx seq=%d algo=FIFO\n", 
             p->pid, victim->va, victim->fifo_seq);
      
      if(evict_page(p, victim) != 0) {
        return 0;
      }
      
      // Try allocating again
      mem = (uint64)kalloc();
      if(mem == 0) {
        return 0; // Still no memory
      }
    }
    memset((void*)mem, 0, PGSIZE);
    
    if(mappages(pagetable, va, PGSIZE, mem, PTE_R|PTE_W|PTE_U) != 0) {
      kfree((void*)mem);
      return 0;
    }
    printf("[pid %d] ALLOC va=0x%lx\n", p->pid, va);
    mem = 0; // Don't return this value, we'll get it from walkaddr
  }
  
  // Update page info
  page->state = RESIDENT;
  page->fifo_seq = p->next_fifo_seq++;
  page->is_dirty = 0;
  
  // Log resident
  printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, page->fifo_seq);
  
  // Return physical address
  return walkaddr(pagetable, va);
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}

// Helper functions for lazy allocation and page tracking

// Find or create page info for a virtual address
struct page_info*
find_page_info(struct proc *p, uint64 va)
{
  va = PGROUNDDOWN(va);
  
  // Look for existing page info
  for(int i = 0; i < p->num_pages; i++) {
    if(p->pages[i].va == va) {
      return &p->pages[i];
    }
  }
  
  // Create new page info if space available
  if(p->num_pages < MAX_PROC_PAGES) {
    struct page_info *page = &p->pages[p->num_pages++];
    page->va = va;
    page->state = UNMAPPED;
    page->is_dirty = 0;
    page->fifo_seq = 0;
    page->swap_slot = -1;
    return page;
  }
  
  return 0; // No space for tracking
}

// Load a page from executable file
int
load_exec_page(struct proc *p, uint64 va, struct inode *ip)
{
  if(!ip) return -1;
  
  va = PGROUNDDOWN(va);
  
  // Lock the inode for reading
  ilock(ip);
  
  // Find the program header that contains this address
  struct elfhdr elf;
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf)) {
    iunlock(ip);
    return -1;
  }
    
  struct proghdr ph;
  int i, off;
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph)) {
      iunlock(ip);
      return -1;
    }
    if(ph.type != ELF_PROG_LOAD)
      continue;
      
    if(va >= ph.vaddr && va < ph.vaddr + ph.memsz) {
      // Found the segment, allocate and load page
      char *mem = kalloc();
      if(mem == 0) {
        // Out of memory - try page replacement
        printf("[pid %d] MEMFULL\n", p->pid);
        
        struct page_info *victim = find_victim_page(p);
        if(!victim) {
          iunlock(ip);
          return -1; // No page to evict
        }
        
        printf("[pid %d] VICTIM va=0x%lx seq=%d algo=FIFO\n", 
               p->pid, victim->va, victim->fifo_seq);
        
        if(evict_page(p, victim) != 0) {
          iunlock(ip);
          return -1;
        }
        
        // Try allocating again
        mem = kalloc();
        if(mem == 0) {
          iunlock(ip);
          return -1; // Still no memory
        }
      }
      
      memset(mem, 0, PGSIZE);
      
      // Calculate file offset and size to read
      uint64 file_offset = ph.off + (va - ph.vaddr);
      uint64 bytes_to_read = PGSIZE;
      if(va + PGSIZE > ph.vaddr + ph.filesz) {
        if(va >= ph.vaddr + ph.filesz) {
          bytes_to_read = 0; // BSS section
        } else {
          bytes_to_read = ph.vaddr + ph.filesz - va;
        }
      }
      
      // Read from file if needed
      if(bytes_to_read > 0) {
        if(readi(ip, 0, (uint64)mem, file_offset, bytes_to_read) != bytes_to_read) {
          kfree(mem);
          iunlock(ip);
          return -1;
        }
      }
      
      // Unlock inode before mapping (to avoid holding locks too long)
      iunlock(ip);
      
      // Map page with appropriate permissions
      int perm = PTE_U;
      if(ph.flags & ELF_PROG_FLAG_READ) perm |= PTE_R;
      if(ph.flags & ELF_PROG_FLAG_WRITE) perm |= PTE_W;
      if(ph.flags & ELF_PROG_FLAG_EXEC) perm |= PTE_X;
      
      if(mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm) != 0) {
        kfree(mem);
        return -1;
      }
      
      return 0;
    }
  }
  
  iunlock(ip);
  return -1; // Address not found in any segment
}

// Check if address is valid for the process
int
is_valid_address(struct proc *p, uint64 va)
{
  // Check text segment
  if(va >= p->text_start && va < p->text_end)
    return 1;
    
  // Check data segment  
  if(va >= p->data_start && va < p->data_end)
    return 1;
    
  // Check heap
  if(va >= p->heap_start && va < p->sz)
    return 1;
    
  // Check stack (within one page below current stack pointer)
  uint64 sp = p->trapframe->sp;
  if(va >= sp - PGSIZE && va < p->stack_top)
    return 1;
    
  return 0;
}

// Find victim page using FIFO (oldest resident page)
struct page_info*
find_victim_page(struct proc *p)
{
  struct page_info *victim = 0;
  int min_seq = -1;
  
  for(int i = 0; i < p->num_pages; i++) {
    if(p->pages[i].state == RESIDENT) {
      if(min_seq == -1 || p->pages[i].fifo_seq < min_seq) {
        min_seq = p->pages[i].fifo_seq;
        victim = &p->pages[i];
      }
    }
  }
  
  return victim;
}

// Evict a page from memory
// Returns 0 on success, -1 on failure
int
evict_page(struct proc *p, struct page_info *victim)
{
  if(!victim || victim->state != RESIDENT) {
    return -1;
  }
  
  uint64 va = victim->va;
  
  // Log eviction
  printf("[pid %d] EVICT  va=0x%lx state=%s\n", p->pid, va, 
         victim->is_dirty ? "dirty" : "clean");
  
  // Get the PTE to check permissions and determine if page needs backing
  pte_t *pte = walk(p->pagetable, va, 0);
  if(!pte || !(*pte & PTE_V)) {
    return -1;
  }
  
  // Determine if page needs to be swapped or can be discarded
  int can_discard = 0;
  
  // Clean pages from text/data can be discarded (reloaded from executable)
  if(!victim->is_dirty && 
     ((va >= p->text_start && va < p->text_end) ||
      (va >= p->data_start && va < p->data_end))) {
    can_discard = 1;
  }
  
  if(can_discard) {
    // Page can be reloaded from executable, just discard
    printf("[pid %d] DISCARD va=0x%lx\n", p->pid, va);
    victim->state = UNMAPPED;
    victim->swap_slot = -1;
  } else {
    // Dirty page or heap/stack - write to swap
    int slot = write_to_swap(p, va);
    if(slot < 0) {
      return -1; // Swap failed (full or error)
    }
    printf("[pid %d] SWAPOUT va=0x%lx slot=%d\n", p->pid, va, slot);
    victim->state = SWAPPED;
    victim->swap_slot = slot;
  }
  
  // Unmap and free the physical page
  uint64 pa = PTE2PA(*pte);
  *pte = 0;
  kfree((void*)pa);
  
  return 0;
}

// Swap management functions

// Allocate a swap slot
// Returns slot number (0-1023) or -1 if no free slots
int
alloc_swap_slot(struct proc *p)
{
  if(p->num_swap_slots_used >= 1024) {
    return -1; // Swap full
  }
  
  for(int i = 0; i < 1024; i++) {
    if(p->swap_slots[i] == 0) {
      p->swap_slots[i] = 1;
      p->num_swap_slots_used++;
      return i;
    }
  }
  return -1;
}

// Free a swap slot
void
free_swap_slot(struct proc *p, int slot)
{
  if(slot >= 0 && slot < 1024 && p->swap_slots[slot]) {
    p->swap_slots[slot] = 0;
    p->num_swap_slots_used--;
  }
}

// Write a page to swap
// Returns slot number or -1 on error
int
write_to_swap(struct proc *p, uint64 va)
{
  if(!p->swap_file) {
    return -1;
  }
  
  // Allocate a swap slot
  int slot = alloc_swap_slot(p);
  if(slot < 0) {
    printf("[pid %d] SWAPFULL\n", p->pid);
    printf("[pid %d] KILL swap-exhausted\n", p->pid);
    setkilled(p);
    return -1;
  }
  
  // Get physical address
  uint64 pa = walkaddr(p->pagetable, va);
  if(pa == 0) {
    free_swap_slot(p, slot);
    return -1;
  }
  
  // Write page to swap file at appropriate offset
  begin_op();
  ilock(p->swap_file->ip);
  if(writei(p->swap_file->ip, 0, pa, slot * PGSIZE, PGSIZE) != PGSIZE) {
    iunlock(p->swap_file->ip);
    end_op();
    free_swap_slot(p, slot);
    return -1;
  }
  iunlock(p->swap_file->ip);
  end_op();
  
  return slot;
}

// Read a page from swap
// Returns 0 on success, -1 on error
int
read_from_swap(struct proc *p, uint64 va, int slot, uint64 pa)
{
  if(!p->swap_file || slot < 0 || slot >= 1024) {
    return -1;
  }
  
  // Read page from swap file
  begin_op();
  ilock(p->swap_file->ip);
  if(readi(p->swap_file->ip, 0, pa, slot * PGSIZE, PGSIZE) != PGSIZE) {
    iunlock(p->swap_file->ip);
    end_op();
    return -1;
  }
  iunlock(p->swap_file->ip);
  end_op();
  
  // Free the swap slot
  free_swap_slot(p, slot);
  
  return 0;
}
