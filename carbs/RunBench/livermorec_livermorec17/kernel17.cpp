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
 *   Kernel 17 -- implicit, conditional computation
 *******************************************************************
 *          DO 62 L= 1,Loop
 *                i= n
 *                j= 1
 *              INK= -1
 *            SCALE= 5./3.
 *              XNM= 1./3.
 *               E6= 1.03/3.07
 *                   GO TO 61
 *60             E6= XNM*VSP(i)+VSTP(i)
 *          VXNE(i)= E6
 *              XNM= E6
 *           VE3(i)= E6
 *                i= i+INK
 *               IF( i.EQ.j) GO TO  62
 *61             E3= XNM*VLR(i) +VLIN(i)
 *             XNEI= VXNE(i)
 *          VXND(i)= E6
 *              XNC= SCALE*E3
 *               IF( XNM .GT.XNC) GO TO  60
 *               IF( XNEI.GT.XNC) GO TO  60
 *           VE3(i)= E3
 *               E6= E3+E3-XNM
 *          VXNE(i)= E3+E3-XNEI
 *              XNM= E6
 *                i= i+INK
 *               IF( i.NE.j) GO TO 61
 * 62 CONTINUE
 */
std::vector<float> Kernel17()
{
    // This kernel loop is not suitable for parallel execution
    int ii = 0, n = 100000;
    std::vector<float> vsp(n, 1.2);
    std::vector<float> vstp(n, 0.3);
    std::vector<float> vxne(n, 0.5);
    std::vector<float> ve3(n, 2.3);
    std::vector<float> vlr(n, 0.2);
    std::vector<float> vlin(n, 1.3);
    std::vector<float> vxnd(n, 3.0);
    do{
      float scale = 5.0 / 3.0;
      float xnm = 1.0 / 3.0;
      float e6 = 1.03 / 3.07;
      //The following parallel optimization doesn't work since the current 
      //iteration relies on xnm and e6, which are calculated from previous
      //iteration.
      //#pragma omp parallel for
      for(int i = n-1; i > 0; i--) {
         float e3 = xnm*vlr[i] + vlin[i];
         float xnei = vxne[i];
         vxnd[i] = e6;
         float xnc = scale*e3;
         if( xnm > xnc || xnei > xnc ) {
            e6 = xnm*vsp[i] + vstp[i]; 
            vxne[i] = e6;
            xnm = e6;
            ve3[i] = e6;
         } else {
            ve3[i] = e3;
            e6 = e3 + e3 - xnm;
            vxne[i] = e3 + e3 - xnei;
            xnm = e6; 
         }
      }
   } while ( ii++ < iter);
   return ve3;
}

int main(int argc, char **argv){
	#ifdef TIMING
	_entering();
	#endif
	long double sum = 0;
	
	auto k17 = Kernel17();
	if (argc > 1){
        Print1DArray(k17);
    }

	#ifdef TIMING
	_exiting();
	#endif
	return sum;
}
