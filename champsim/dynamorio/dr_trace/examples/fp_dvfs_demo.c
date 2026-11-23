#include <stdio.h>
#include <stdint.h>
#include "../../../../flagbench/kernels/dvfs_hint.h"
#ifndef HINT
#define HINT 1
#endif
#define N 4096
#define BIG (1<<21)
static double a[N], b[N];
static uint32_t idx[1<<18]; static uint32_t big[BIG];
double acc=0; uint64_t m=0;
__attribute__((noinline)) void roi(void){
  volatile uint32_t *pb = big;
  for(int rep=0;rep<4;rep++){
    for(int i=0;i<(1<<16);i++) m+=pb[idx[i]];          /* memory-bound: random gathers */
    if(HINT) DVFS_HINT(3);
    for(int k=0;k<60;k++) for(int i=0;i<N;i++) acc = acc*0.999999 + a[i]*b[i]; /* FP-bound */
    if(HINT) DVFS_HINT(0);
  }
}
int main(void){
  for(int i=0;i<N;i++){a[i]=i*0.5+1;b[i]=1.0/(i+1);}
  for(int i=0;i<BIG;i++) big[i]=i;
  uint32_t s=1; for(int i=0;i<(1<<18);i++){s^=s<<13;s^=s>>17;s^=s<<5;idx[i]=s&(BIG-1);}
  roi(); printf("%f %llu\n",acc,(unsigned long long)m); return 0;
}
