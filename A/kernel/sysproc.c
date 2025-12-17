#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"
#include "memstat.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;
  struct proc *p = myproc();

  argint(0, &n);

  addr = p->sz;
  if(n > 0) {
    p->sz += n;
  } else if (n < 0) {
    // Deallocate pages if shrinking memory, but we don't do that here.
    // For lazy allocation, we just adjust the size.
    if (p->sz + n < p->data_end) {
      // Don't allow shrinking below the end of the data segment
      return -1;
    }
    uvmdealloc(p->pagetable, p->sz, p->sz + n);
    p->sz += n;
  }
  
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// Get memory statistics for the current process
uint64
sys_memstat(void)
{
  uint64 addr;
  struct proc *p = myproc();
  struct proc_mem_stat stat;
  
  // Get the user-space address where we should write the result
  argaddr(0, &addr);
  
  // Fill in the statistics
  stat.pid = p->pid;
  stat.num_pages_total = p->num_pages;
  stat.num_resident_pages = 0;
  stat.num_swapped_pages = 0;
  stat.next_fifo_seq = p->next_fifo_seq;
  
  // Count resident and swapped pages, and copy page info
  int pages_copied = 0;
  for(int i = 0; i < p->num_pages && pages_copied < MAX_PAGES_INFO; i++) {
    if(p->pages[i].state == RESIDENT) {
      stat.num_resident_pages++;
    } else if(p->pages[i].state == SWAPPED) {
      stat.num_swapped_pages++;
    }
    
    // Copy page info
    stat.pages[pages_copied].va = p->pages[i].va;
    stat.pages[pages_copied].state = p->pages[i].state;
    stat.pages[pages_copied].is_dirty = p->pages[i].is_dirty;
    stat.pages[pages_copied].seq = p->pages[i].fifo_seq;
    stat.pages[pages_copied].swap_slot = p->pages[i].swap_slot;
    pages_copied++;
  }
  
  // Copy the result to user space
  if(copyout(p->pagetable, addr, (char *)&stat, sizeof(stat)) < 0)
    return -1;
  
  return 0;
}
