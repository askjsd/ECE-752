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
 *   Kernel 19 -- general linear recurrence equations
 *******************************************************************
 *               KB5I= 0
 *           DO 194 L= 1,Loop
 *           DO 191 k= 1,n
 *         B5(k+KB5I)= SA(k) +STB5*SB(k)
 *               STB5= B5(k+KB5I) -STB5
 *191        CONTINUE
 *192        DO 193 i= 1,n
 *                  k= n-i+1
 *         B5(k+KB5I)= SA(k) +STB5*SB(k)
 *               STB5= B5(k+KB5I) -STB5
 *193        CONTINUE
 *194 CONTINUE
 */
std::vector<float> Kernel19()
{
    int ii = 0, n = 100000;
    int kb5i = n;
    float stb5 = 0.0015;
    std::vector<float> b5(n+kb5i, 0);
    std::vector<float> sa(n, 0);
    std::vector<float> sb(n, 0);
    InitializeLoop(b5, 0.0001);
    InitializeLoop(sa, 0.00023);
    InitializeLoop(sb, 0.00016);
    do{
      //The following two parallel optimizations won't work
      //as we keep using stb5 calculated from previous iteration
      //so can't process them simultaneously.
        //#pragma omp parallel for
        for (int k = 0; k < n; k++) {
           b5[k+kb5i] = sa[k] + stb5 * sb[k];
           stb5 = b5[k+kb5i] - stb5;
        }
       //#pragma omp parallel for
        for (int i = 1; i <= n; i++) {
           int k = n - i ;
           b5[k+kb5i] = sa[k] + stb5 * sb[k];
           stb5 = b5[k+kb5i] - stb5;
        } 
    } while (ii++ < 10);
    // Uncomment the function Print1DArrray could print out each array element
    // Compared to results with different threads we will find the differences
    // Print1DArray(b5);
    return b5;
}

int main(int argc, char **argv){
	#ifdef TIMING
	_entering();
	#endif
	long double sum = 0;
	
	auto k19 = Kernel19();
    if (argc > 1){
        Print1DArray(k19);
    }

	#ifdef TIMING
	_exiting();
	#endif
	return 0;
}
