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
 *   Kernel 23 -- 2-D implicit hydrodynamics fragment
 *******************************************************************
 *    DO 23  L= 1,Loop
 *    DO 23  j= 2,6
 *    DO 23  k= 2,n
 *          QA= ZA(k,j+1)*ZR(k,j) +ZA(k,j-1)*ZB(k,j) +
 *   .          ZA(k+1,j)*ZU(k,j) +ZA(k-1,j)*ZV(k,j) +ZZ(k,j)
 * 23  ZA(k,j)= ZA(k,j) +.175*(QA -ZA(k,j))
 */
std::vector<std::vector<float>> Kernel23()
{
   int i = 0, n = 30000;
   std::vector<std::vector<float>> za(7, std::vector<float>(n+1, 1.2));
   std::vector<std::vector<float>> zb(7, std::vector<float>(n+1, 1.7));
   std::vector<std::vector<float>> zr(7, std::vector<float>(n+1, 2.2));
   std::vector<std::vector<float>> zu(7, std::vector<float>(n+1, 3.6));
   std::vector<std::vector<float>> zv(7, std::vector<float>(n+1, 4.2));
   std::vector<std::vector<float>> zz(7, std::vector<float>(n+1, 0.6));
   do{
      //The following parallel optimization is faster but leads to incorrect
      //results since for each iteration, the calculation depends on the values
      //calculated by previous iterations.
      //#pragma omp parallel for collapse(2)
      for(int j = 1; j<6; j++) {
         for( int k=1; k<n; ++k) {
            int qa = za[j+1][k]*zr[j][k] + za[j-1][k]*zb[j][k] +
               za[j][k+1]*zu[j][k] + za[j][k-1]*zv[j][k] + zz[j][k];
            za[j][k] += 0.175*( qa - za[j][k] );
         }
      }
   } while(i++ <iter);
   return za;
}

int main(int argc, char **argv){
	#ifdef TIMING
	_entering();
	#endif
	long double sum = 0;
	
	std::vector<std::vector<float>> k23 = Kernel23();
    if (argc > 1)
    {
        Print2DArray(k23);
    }

	#ifdef TIMING
	_exiting();
	#endif
	return 0;
}
