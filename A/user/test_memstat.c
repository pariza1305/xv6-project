#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/memstat.h"

int main() {
  struct proc_mem_stat stat;
  int result = memstat(&stat);
  
  if (result < 0) {
    printf("memstat failed\n");
    exit(1);
  }
  
  printf("PID: %d\n", stat.pid);
  printf("Total pages: %d\n", stat.num_pages_total);
  printf("Resident pages: %d\n", stat.num_resident_pages);
  printf("Swapped pages: %d\n", stat.num_swapped_pages);
  printf("Next FIFO seq: %d\n", stat.next_fifo_seq);
  printf("Pages info:\n");
  
  for(int i = 0; i < stat.num_pages_total && i < MAX_PAGES_INFO; i++) {
    printf("  Page %d: va=0x%x state=%d dirty=%d seq=%d swap_slot=%d\n",
           i, stat.pages[i].va, stat.pages[i].state, stat.pages[i].is_dirty,
           stat.pages[i].seq, stat.pages[i].swap_slot);
  }
  
  exit(0);
}
