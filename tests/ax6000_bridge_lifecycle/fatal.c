/* Actual fatal entry, replacing only arch/IRQ edges in an isolated process. */
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define __noreturn __attribute__((noreturn))
struct shared_state { int fatal, error, restart, irqs_off, preemption_off, continued; bool configured; };
static struct shared_state *shared;
#define cpu_profile_fatal shared->fatal
#define cpu_profile_error shared->error
#define cpu_profile_configured shared->configured
#define atomic_cmpxchg(p,o,n) __sync_val_compare_and_swap((p),(o),(n))
#define smp_store_release(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
static bool irq_disabled, preempt_disabled;
static void local_irq_disable(void) {irq_disabled=true;__sync_fetch_and_add(&shared->irqs_off,1);}
static void preempt_disable(void) {preempt_disabled=true;__sync_fetch_and_add(&shared->preemption_off,1);}
static void cpu_relax(void) {__asm__ __volatile__("" ::: "memory");}
enum mtk_cpu_dma_fault { MTK_CPU_DMA_STOP_FAILED=1, MTK_CPU_DMA_CONTROL_RACE=2 };
enum { REBOOT_HARD=3 };
static int reboot_mode;
static void machine_restart(const char *command) {
 assert(command==NULL && irq_disabled && preempt_disabled && reboot_mode==REBOOT_HARD);
 __sync_fetch_and_add(&shared->restart,1);
 /* Deliberately return: production must park rather than execute cleanup. */
}
#include "actual-fatal.inc"
int main(void) {
 shared=mmap(NULL,sizeof(*shared),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
 assert(shared!=MAP_FAILED);shared->configured=true;
 int start[2];assert(!pipe(start));pid_t child[2];
 for(int i=0;i<2;i++) {
  child[i]=fork();assert(child[i]>=0);
  if(!child[i]) {
   char byte;close(start[1]);assert(read(start[0],&byte,1)==1);
   mtk_cpu_dma_fatal(MTK_CPU_DMA_CONTROL_RACE,17);
   __sync_fetch_and_add(&shared->continued,1);_exit(3);
  }
 }
 close(start[0]);assert(write(start[1],"go",2)==2);close(start[1]);
 for(int retries=0;retries<500 &&
     (__atomic_load_n(&shared->preemption_off,__ATOMIC_ACQUIRE)<2 ||
      __atomic_load_n(&shared->restart,__ATOMIC_ACQUIRE)<1);retries++)usleep(1000);
 assert(shared->irqs_off==2 && shared->preemption_off==2);
 assert(shared->fatal==MTK_CPU_DMA_CONTROL_RACE && shared->error==-EIO && !shared->configured);
 assert(shared->restart==1 && shared->continued==0);
 for(int i=0;i<2;i++) {
  int status;assert(waitpid(child[i],&status,WNOHANG)==0);
  assert(!kill(child[i],SIGKILL));assert(waitpid(child[i],&status,0)==child[i]);assert(WIFSIGNALED(status));
 }
 assert(shared->continued==0);assert(!munmap(shared,sizeof(*shared)));
 puts("actual fatal entry: IRQ/preemption before election, single reset executor, returned arch reset never reaches cleanup");
 return 0;
}
