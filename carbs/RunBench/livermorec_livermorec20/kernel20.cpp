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
 *   Kernel 20 -- Discrete ordinates transport, conditional recurrence on xx
 *******************************************************************
 *    DO 20 L= 1,Loop
 *    DO 20 k= 1,n
 *         DI= Y(k)-G(k)/( XX(k)+DK)
 *         DN= 0.2
 *         IF( DI.NE.0.0) DN= MAX( S,MIN( Z(k)/DI, T))
 *       X(k)= ((W(k)+V(k)*DN)* XX(k)+U(k))/(VX(k)+V(k)*DN)
 *    XX(k+1)= (X(k)- XX(k))*DN+ XX(k)
 * 20 CONTINUE
 */
std::vector<float> Kernel20()
{
   int i = 0, n = 10000;
   float s = 1.0, t = 0.5, dk = 1.0;
   std::vector<float> x(n, 0);
   std::vector<float> xx(n+1, 0);
   std::vector<float> y(n, 0);
   std::vector<float> g(n, 0);
   std::vector<float> z(n, 0);
   std::vector<float> u(n, 0);
   std::vector<float> v(n, 0);
   std::vector<float> w(n, 0);
   std::vector<float> vx(n, 0);
   InitializeLoop(x, 0.6);
   InitializeLoop(xx, 0.2);
   InitializeLoop(y, 0.76);
   InitializeLoop(g, 0.001);
   InitializeLoop(z, 0.00015);
   InitializeLoop(u, 0.0003);
   InitializeLoop(v, 0.008);
   InitializeLoop(w, 0.0001);
   InitializeLoop(vx, 0.0025);
    do {
        // The parallel optimization doesn't work since xx[k+1] depends on xx[k].
        // And every piece of code inside this for loop is potentially related to 
        // xx[k] so we can not even do partial optimization. 
        //#pragma omp parallel for
        for (int k = 1; k < n; k++) 
        {
            float di = y[k] - g[k] / ( xx[k] + dk );
            float dn = 0.2;
            if ( di > 0) 
            {
                dn = z[k]/di ;
                if ( t < dn ) dn = t;
                if ( s > dn ) dn = s;
            }
            x[k] = ( ( w[k] + v[k]*dn )* xx[k] + u[k] ) / ( vx[k] + v[k]*dn );
            xx[k+1] = ( x[k] - xx[k] )* dn + xx[k];
        } 
    } while(++i < 10);
    // Uncomment the function Print1DArrray could print out each array element
    // Compared to results with different threads we will find the differences
    // Print1DArray(xx);
    return xx;
}

int main(int argc, char **argv){
	#ifdef TIMING
	_entering();
	#endif
	long double sum = 0;
	
	auto k1 = Kernel20();
    if (argc > 1){
        Print1DArray(k1);
    }

	#ifdef TIMING
	_exiting();
	#endif
	return sum;
}
