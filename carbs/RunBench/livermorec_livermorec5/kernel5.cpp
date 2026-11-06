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
*   Kernel 5 -- tri-diagonal elimination, below diagonal
*******************************************************************
*    DO 5 L = 1,Loop
*    DO 5 i = 2,n
*  5    X(i)= Z(i)*(Y(i) - X(i-1))
*/

std::vector<float> Kernel5()
{
    int j = 0, n = 100000;
    std::vector<float> x(n, 0);
    std::vector<float> y(n, 0);
    std::vector<float> z(n, 0);
    InitializeLoop(y, 0.0000305);
    InitializeLoop(z, 0.0000023);
    do {
        // This loop can't be parallelize because the loop dependence on x[i] and x[i-1]
        //#pragma omp parallel for
        for (int i = 1; i < n; i++ ) 
        {
           x[i] = z[i] * (y[i] - x[i-1]);
        }
    } while(++j < 100);
    // Uncomment the function Print1DArrray could print out each array element
    // Compared to results with different threads we will find the differences
    // Print1DArray(x); 
    return x;
}

int main(int argc, char **argv){
	#ifdef TIMING
	_entering();
	#endif
	long double sum = 0;
	
	auto k5 = Kernel5();
    if (argc > 1){
        Print1DArray(k5);
    }

	#ifdef TIMING
	_exiting();
	#endif
	return sum;
}
