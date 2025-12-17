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
#include "stat.h"

// static int loadseg(pde_t *, uint64, struct inode *, uint, uint);  // Not used in lazy mode

// map ELF permissions to PTE permission bits.
int flags2perm(int flags)
{
    int perm = 0;
    if(flags & 0x1)
      perm = PTE_X;
    if(flags & 0x2)
      perm |= PTE_W;
    return perm;
}

//
// the implementation of the exec() system call
//
int
kexec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();

  begin_op();

  // Open the executable file.
  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);

  // Read the ELF header.
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  // Is this really an ELF file?
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Initialize lazy allocation fields
  p->text_start = 0;
  p->text_end = 0;
  p->data_start = 0;
  p->data_end = 0;
  p->next_fifo_seq = 1;
  p->num_pages = 0;

  // Keep reference to executable inode for lazy loading
  idup(ip);
  p->exec_inode = ip;

  // Parse program headers but don't load segments yet
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    
    // Track segment boundaries for lazy allocation
    if(ph.flags & ELF_PROG_FLAG_EXEC) {
      // Text segment
      p->text_start = ph.vaddr;
      p->text_end = ph.vaddr + ph.memsz;
    } else {
      // Data segment
      if(p->data_start == 0) {
        p->data_start = ph.vaddr;
      }
      p->data_end = ph.vaddr + ph.memsz;
    }
    
    // Update process size without allocating
    if(ph.vaddr + ph.memsz > sz)
      sz = ph.vaddr + ph.memsz;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  p = myproc();
  uint64 oldsz = p->sz;
  
  // Create swap file for this process
  // Format: /pgswpXXXXX where XXXXX is the PID (zero-padded)
  char swap_path[32];
  int pid_digits[5];
  int temp_pid = p->pid;
  for(int i = 4; i >= 0; i--) {
    pid_digits[i] = temp_pid % 10;
    temp_pid /= 10;
  }
  swap_path[0] = '/';
  swap_path[1] = 'p';
  swap_path[2] = 'g';
  swap_path[3] = 's';
  swap_path[4] = 'w';
  swap_path[5] = 'p';
  for(int i = 0; i < 5; i++) {
    swap_path[6 + i] = '0' + pid_digits[i];
  }
  swap_path[11] = 0;
  
  // Save swap path in process
  for(int i = 0; i < 32 && swap_path[i]; i++) {
    p->swap_path[i] = swap_path[i];
  }
  p->swap_path[31] = 0;
  
  // Create the swap file
  begin_op();
  struct inode *swap_inode;
  if((swap_inode = create(swap_path, T_FILE, 0, 0)) == 0) {
    end_op();
    goto bad;
  }
  
  // Open it for reading and writing
  if((p->swap_file = filealloc()) == 0) {
    iunlockput(swap_inode);
    end_op();
    goto bad;
  }
  p->swap_file->type = FD_INODE;
  p->swap_file->ip = swap_inode;
  p->swap_file->off = 0;
  p->swap_file->readable = 1;
  p->swap_file->writable = 1;
  iunlock(swap_inode);
  end_op();

  // Set up stack boundaries without allocating physical pages
  sz = PGROUNDUP(sz);
  p->heap_start = sz;
  sz += (USERSTACK+1)*PGSIZE;
  p->stack_top = sz;
  sp = sz;
  stackbase = sp - USERSTACK*PGSIZE;
  
  // Log the lazy allocation initialization (after all boundaries are set)
  printf("[pid %d] INIT-LAZYMAP text=[0x%lx,0x%lx) data=[0x%lx,0x%lx) heap_start=0x%lx stack_top=0x%lx\n",
         p->pid, p->text_start, p->text_end, p->data_start, p->data_end, 
         p->heap_start, p->stack_top);

  // We need to allocate at least one stack page for arguments
  // This is the minimal allocation needed for exec to work
  char *stack_mem = kalloc();
  if(stack_mem == 0)
    goto bad;
  memset(stack_mem, 0, PGSIZE);
  
  // Map the top stack page
  if(mappages(pagetable, sz-PGSIZE, PGSIZE, (uint64)stack_mem, PTE_R|PTE_W|PTE_U) != 0){
    kfree(stack_mem);
    goto bad;
  }
  
  // Track this page in page_info array
  if(p->num_pages < MAX_PROC_PAGES) {
    p->pages[p->num_pages].va = PGROUNDDOWN(sz-PGSIZE);
    p->pages[p->num_pages].state = RESIDENT;
    p->pages[p->num_pages].is_dirty = 0;
    p->pages[p->num_pages].fifo_seq = p->next_fifo_seq++;
    p->pages[p->num_pages].swap_slot = -1;
    p->num_pages++;
  }

  // Copy argument strings into new stack, remember their
  // addresses in ustack[].
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push a copy of ustack[], the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // a0 and a1 contain arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  p->trapframe->a1 = sp;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
    
  // Commit to the user image.
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
  p->trapframe->epc = elf.entry;  // initial program counter = main
  p->trapframe->sp = sp; // initial stack pointer
  proc_freepagetable(oldpagetable, oldsz);
  
  // Log lazy mapping initialization
  printf("[pid %d] INIT-LAZYMAP text=[0x%lx,0x%lx) data=[0x%lx,0x%lx) heap_start=0x%lx stack_top=0x%lx\n",
          p->pid, p->text_start, p->text_end, p->data_start, p->data_end, p->heap_start, p->stack_top);

  return argc; // this ends up in a0, the first argument to main(argc, argv)

 bad:
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  if(p && p->exec_inode) {
    iput(p->exec_inode);
    p->exec_inode = 0;
  }
  return -1;
}

// Load an ELF program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
// NOTE: This function is not used in lazy allocation mode
/*
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  for(i = 0; i < sz; i += PGSIZE){
    pa = walkaddr(pagetable, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}
*/
