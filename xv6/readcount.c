#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

int
main(int argc, char *argv[])
{
  int fd;
  char buf[100];
  int before, after;

  // call syscall first time
  before = getreadcount();
  printf("Initial readcount: %d\n", before);

  // open a file
  fd = open("README", 0);   // you can replace "README" with any file that exists
  if(fd < 0){
    printf("readcount: cannot open file\n");
    exit(1);
  }

  // read 100 bytes
  read(fd, buf, sizeof(buf));
  close(fd);

  // call syscall again
  after = getreadcount();
  printf("Readcount after reading 100 bytes: %d\n", after);

  exit(0);
}
