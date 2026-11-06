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
*   Kernel 13 -- 2-D PIC (Particle In Cell)
*******************************************************************
*    DO  13     L= 1,Loop
*    DO  13    ip= 1,n
*              i1= P(1,ip)
*              j1= P(2,ip)
*              i1=        1 + MOD2N(i1,64)
*              j1=        1 + MOD2N(j1,64)
*         P(3,ip)= P(3,ip)  + B(i1,j1)
*         P(4,ip)= P(4,ip)  + C(i1,j1)
*         P(1,ip)= P(1,ip)  + P(3,ip)
*         P(2,ip)= P(2,ip)  + P(4,ip)
*              i2= P(1,ip)
*              j2= P(2,ip)
*              i2=            MOD2N(i2,64)
*              j2=            MOD2N(j2,64)
*         P(1,ip)= P(1,ip)  + Y(i2+32)
*         P(2,ip)= P(2,ip)  + Z(j2+32)
*              i2= i2       + E(i2+32)
*              j2= j2       + F(j2+32)
*        H(i2,j2)= H(i2,j2) + 1.0
* 13 CONTINUE
*/

std::vector<std::vector<int> > Kernel13()
{
#if 1
    int i1, j1, i2, j2;
    int i = 0, n = 5000;
    std::vector<int> y(n+32, 1);
    std::vector<int> z(n+32, 1);
    std::vector<int> e(n+32, 1);
    std::vector<int> f(n+32, 1);
    InitializeLoop(y, 1);
    InitializeLoop(z, 1);
    InitializeLoop(e, 1);
    InitializeLoop(f, 1);
    std::vector<std::vector<int> > b(n, std::vector<int>(n, 1));
    std::vector<std::vector<int> > c(n, std::vector<int>(n, 3));
    std::vector<std::vector<int> > p(n, std::vector<int>(4, 0));
    std::vector<std::vector<int> > h(n, std::vector<int>(n, 0));
    Initialize2DLoop(p, 0.8);
    Initialize2DLoop(h, 0.2);

    do {
        //#pragma omp parallel for private(i1,j1,j2,i2)
        for (int ip = 0 ; ip < n; ip++) 
        {
           i1 = p[ip][0];
           j1 = p[ip][1];
           i1 &= 64-1;
           j1 &= 64-1;
           p[ip][2] += b[j1][i1];
           p[ip][3] += c[j1][i1];
           p[ip][0] += p[ip][2];
           p[ip][1] += p[ip][3];
           i2 = p[ip][0];
           j2 = p[ip][1];
           i2 = ( i2 & (64-1) ) - 1 ;
           j2 = ( j2 & (64-1) ) - 1 ;
           p[ip][0] += y[i2+32];
           p[ip][1] += z[j2+32];
           i2 += e[i2+32];
           j2 += f[j2+32];
           //#pragma omp atomic
           h[j2][i2] += 1.0;
        }
    } while( i++ < iter );
    // Uncomment the function Print1DArrray could print out each array element
    // Compared to results with different threads we will find the differences
    // Print2DArray(h); 
    return h;
#endif
}

int main(int argc, char **argv){
	#ifdef TIMING
	_entering();
	#endif
	long double sum = 0;
	
	auto k13 = Kernel13();
    if (argc > 1){
        Print2DArray(k13);
    }

	#ifdef TIMING
	_exiting();
	#endif
	return 0;
}
