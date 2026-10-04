/* insmod.c — minimal static module loader for the FlashAgent initramfs
 * Usage: insmod <module.ko>
 * Uses the finit_module(2) syscall directly — no kmod library needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/types.h>

int main(int argc, char **argv){
  if(argc<2){ fprintf(stderr,"usage: insmod <module.ko>\n"); return 1; }
  int fd=open(argv[1],O_RDONLY);
  if(fd<0){ fprintf(stderr,"insmod: cannot open %s: %s\n",argv[1],strerror(errno)); return 1; }
  long flags=0;
  if(argc>2 && !strcmp(argv[2],"force")) flags=1; /* force: skip module version check */
  if(syscall(SYS_finit_module,fd,"",flags)<0){
    fprintf(stderr,"insmod: finit_module(%s): %s\n",argv[1],strerror(errno));
    close(fd);
    return 1;
  }
  fprintf(stderr,"insmod: loaded %s\n",argv[1]);
  close(fd);
  return 0;
}
