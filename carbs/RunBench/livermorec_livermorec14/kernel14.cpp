#include <iostream>
#include <cstring>
#include <string>
#include <cmath>
#include <vector>
#include <chrono>
#include <ctime>
#include <numeric>  // std::accumulate()
//#include <omp.h>

// const iteration time for every loop
extern const int iter = 100;
unsigned long long t2, t3, _total_count;
unsigned long long rdtsc(void){
        unsigned hi, lo;
        __asm__ __volatile__ ("mfence\n\t" "lfence\n\t" "rdtsc" : "=a"(lo), "=d"(hi));
        return ( (unsigned long long)lo)|( ((unsigned long long)hi)<<32 );
}
void _entering(){
        t2 = rdtsc();
}
void _exiting(){
        t3 = rdtsc();
        fprintf(stderr, "timing: %llu %llu\n", t2, t3);
        _total_count += 1;
        if(_total_count > 100)
                exit(0);
}

template <typename T> 
double CalculateSum(const std::vector<T> input)
{
    double sum = 0;
    // sum up all the elements in the vector
    ////#pragma omp parallel for reduction(+:sum)
    for (unsigned int i = 0; i < input.size(); i++)
    {
        sum += input[i];
    }
    return sum;
}

template <typename T> 
void InitializeLoop(std::vector<T>& input, const float mul)
{
    // Initialize the loop with different values
    ////#pragma omp parallel for
    for (unsigned i = 0; i < input.size(); i++)
    {
        input[i] = i * mul;
    }
}

template <typename T> 
void Initialize2DLoop(std::vector<std::vector<T> >& input, const float mul)
{
    // Initialize the loop with different values
    ////#pragma omp parallel for
    for (unsigned int i = 0; i < input.size(); i++)
    {
        for (unsigned int j = 0; j < input[i].size(); j++)
        {
            input[i][j] = i * mul * j;
        }
    }
}

template <typename T> 
void Print1DArray(const std::vector<T> input)
{
    for (unsigned int i = 0; i < input.size(); i++)
    {
        std::cout << input[i] << std::endl;
    }
}

template <typename T> 
void Print2DArray(const std::vector<std::vector<T> > input)
{
    for (unsigned int i = 0; i < input.size(); i++)
    {
        for (unsigned int j = 0; j < input[i].size(); j++)
        {
            std::cout << input[i][j] << std::endl;
        }
    }
}

/*
 *******************************************************************
 *   Kernel 14 -- 1-D PIC (Particle In Cell)
 *******************************************************************
 *    DO   14   L= 1,Loop
 *    DO   141  k= 1,n
 *          VX(k)= 0.0
 *          XX(k)= 0.0
 *          IX(k)= INT(  GRD(k))
 *          XI(k)= REAL( IX(k))
 *         EX1(k)= EX   ( IX(k))
 *        DEX1(k)= DEX  ( IX(k))
 *41  CONTINUE
 *    DO   142  k= 1,n
 *          VX(k)= VX(k) + EX1(k) + (XX(k) - XI(k))*DEX1(k)
 *          XX(k)= XX(k) + VX(k)  + FLX
 *          IR(k)= XX(k)
 *          RX(k)= XX(k) - IR(k)
 *          IR(k)= MOD2N(  IR(k),2048) + 1
 *          XX(k)= RX(k) + IR(k)
 *42  CONTINUE
 *    DO  14    k= 1,n
 *    RH(IR(k)  )= RH(IR(k)  ) + 1.0 - RX(k)
 *    RH(IR(k)+1)= RH(IR(k)+1) + RX(k)
 *14  CONTINUE
 */
std::vector<float> Kernel14()
{
#if 1
   int i = 0, n = 100000, flx = 1;
   std::vector<int> ir(n, 1.0);
   std::vector<long> ix(n, 1.0);
   std::vector<float> vx(n, 1.0);
   std::vector<float> xx(n, 1.0);
   std::vector<float> xi(n, 1.0);
   std::vector<float> grd(n, 1.0);
   std::vector<float> ex(n, 1.0);
   std::vector<float> dex(n, 1.0);
   std::vector<float> ex1(n, 1.0);
   std::vector<float> dex1(n, 1.0);
   std::vector<float> rx(n, 1.0);
   std::vector<float> rh(n, 1.0); 

   do {
      //#pragma omp parallel for
      for ( int k=0 ; k<n ; k++ ) {
           vx[k] = 0.0;
           xx[k] = 0.0;
           ix[k] = (long) grd[k];
           xi[k] = (float) ix[k];
           ex1[k] = ex[ ix[k] - 1 ];
           dex1[k] = dex[ ix[k] - 1 ];
       }
      //#pragma omp parallel for
       for ( int k=0 ; k<n ; k++ ) {
           vx[k] = vx[k] + ex1[k] + ( xx[k] - xi[k] )*dex1[k];
           xx[k] = xx[k] + vx[k]  + flx;
           ir[k] = xx[k];
           rx[k] = xx[k] - ir[k];
           ir[k] = ( ir[k] & (2048-1) ) + 1;
           xx[k] = rx[k] + ir[k];
       }
      //#pragma omp parallel for
       for ( int k=0 ; k<n ; k++ ) {
            rh[ ir[k]-1 ] += 1.0 - rx[k];
            rh[ ir[k]   ] += rx[k];
       } 
   } while( i++ < iter);
   return rh;
#endif
}

int main(int argc, char **argv){
	#ifdef TIMING
	_entering();
	#endif
	long double sum = 0;
	
	auto k14 = Kernel14();
    if (argc > 1){
        Print1DArray(k14);
    }

	#ifdef TIMING
	_exiting();
	#endif
	return 0;
}
