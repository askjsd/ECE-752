/* +++Date last modified: 05-Jul-1997 */

/*
**  BITCNTS.C - Test program for bit counting functions
**
**  public domain by Bob Stout & Auke Reitsma
*/

#include <stdio.h>
#include <stdlib.h>
#include "conio.h"
#include <limits.h>
#include <time.h>
#include <float.h>
#include "bitops.h"

#define FUNCS  7

static int CDECL bit_shifter(long int x);

int main(int argc, char *argv[])
{
  _entering();
  clock_t start;
  clock_t stop;
  double ct;
  double cmin = DBL_MAX;
  double cmax = 0;
  int i;
  int cminix;
  int cmaxix;
  long j;
  long n;
  long seed;
  int iterations;
  static int (*pBitCntFunc[7])(long) = {bit_count, bitcount, ntbl_bitcnt, ntbl_bitcount, BW_btbl_bitcount, AR_btbl_bitcount, bit_shifter};
  static char *text[7] = {"Optimized 1 bit/loop counter", "Ratko's mystery algorithm", "Recursive bit count by nybbles", "Non-recursive bit count by nybbles", "Non-recursive bit count by bytes (BW)", "Non-recursive bit count by bytes (AR)", "Shift and count bits"};
  if (argc < 2)
  {
    fprintf(stderr, "Usage: bitcnts <iterations>\n");
    exit(-1);
  }
  iterations = atoi(argv[1]);
  puts("Bit counter algorithm benchmark\n");
  for (i = 0; i < 7; i++)
  {
    start = clock();
    for (j = (n = 0), seed = rand(); j < iterations; j++, seed += 13)
      n += pBitCntFunc[i](seed);

    stop = clock();
    ct = (stop - start) / ((double) CLOCKS_PER_SEC);
    if (ct < cmin)
    {
      cmin = ct;
      cminix = i;
    }
    if (ct > cmax)
    {
      cmax = ct;
      cmaxix = i;
    }
    printf("%-38s> Time: ; Bits: %ld\n", text[i], n);
  }

  //printf("\nBest  > %s\n", text[cminix]);
  //printf("Worst > %s\n", text[cmaxix]);
  _exiting();
  return 0;
}

static int CDECL bit_shifter(long int x)
{
  int i, n;
  
  for (i = n = 0; x && (i < (sizeof(long) * CHAR_BIT)); ++i, x >>= 1)
    n += (int)(x & 1L);
  return n;
}
