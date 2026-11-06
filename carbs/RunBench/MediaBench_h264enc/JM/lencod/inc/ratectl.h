
/*!
 ***************************************************************************
 * \file
 *    ratectl.h
 *
 * \author
 *    Zhengguo LI
 *
 * \date
 *    14 Jan 2003
 *
 * \brief
 *    Headerfile for rate control 
 **************************************************************************
 */

#ifndef _RATE_CTL_H_
#define _RATE_CTL_H_


#define MIN(a,b)  (((a)<(b)) ? (a) : (b))//LIZG 28/10/2002
#define MAX(a,b)  (((a)<(b)) ? (b) : (a))//LIZG 28/10/2002

extern double bit_rate; 
extern double frame_rate;
extern double GAMMAP;//LIZG, JVT019r1
extern double BETAP;//LIZG, JVT019r1

extern int RC_MAX_QUANT;//LIZG 28/10/2002
extern int RC_MIN_QUANT;//LIZG 28/10/2002

extern double BufferSize; //LIZG 25/10/2002
extern double GOPTargetBufferLevel;
extern double CurrentBufferFullness; //LIZG 25/10/2002
extern double TargetBufferLevel;//LIZG 25/10/2002
extern double PreviousBit_Rate;//LIZG  25/10/2002
extern double AWp;
extern double AWb;
extern int MyInitialQp;
extern int PAverageQp;

/*LIZG JVT50V2 distortion prediction model*/
/*coefficients of the prediction model*/
extern double PreviousPictureMAD;
extern double MADPictureC1;
extern double MADPictureC2;
extern double PMADPictureC1;
extern double PMADPictureC2;
/* LIZG JVT50V2 picture layer MAD */
extern Boolean PictureRejected[21];
extern double PPictureMAD[21];
extern double PictureMAD[21];
extern double ReferenceMAD[21];

/*quadratic rate-distortion model*/
extern Boolean   m_rgRejected[21];
extern double  m_rgQp[21];
extern double m_rgRp[21];
extern double m_X1;
extern double m_X2;
extern int m_Qc;
extern double m_Qstep;
extern int m_Qp;
extern int Pm_Qp;
extern int PreAveMBHeader;
extern int CurAveMBHeader;
extern int PPreHeader;
extern int PreviousQp1;
extern int PreviousQp2;
extern int NumberofBFrames;
/*basic unit layer rate control*/
extern int TotalFrameQP;
extern int NumberofBasicUnit;
extern int PAveHeaderBits1;
extern int PAveHeaderBits2;
extern int PAveHeaderBits3;
extern int PAveFrameQP;
extern int TotalNumberofBasicUnit;
extern int CodedBasicUnit;
extern double MINVALUE;
extern double CurrentFrameMAD;
extern double CurrentBUMAD;
extern double TotalBUMAD;
extern double PreviousFrameMAD;
extern int m_Hp;
extern int m_windowSize;
extern int MADm_windowSize;
extern int DDquant;
extern int MBPerRow;
extern double AverageMADPreviousFrame;
extern int TotalBasicUnitBits;
extern int QPLastPFrame;
extern int QPLastGOP;
//int MADn_windowSize;
//int n_windowSize;

extern double Pm_rgQp[20];
extern double Pm_rgRp[20];
extern double Pm_X1;
extern double Pm_X2;
extern int Pm_Hp;
/* adaptive field/frame coding*/
extern int FieldQPBuffer;
extern int FrameQPBuffer;
extern int FrameAveHeaderBits;
extern int FieldAveHeaderBits;
extern double *BUPFMAD;
extern double *BUCFMAD;
extern double *FCBUCFMAD;
extern double *FCBUPFMAD;

extern Boolean GOPOverdue;


//comput macroblock activity for rate control
extern int diffy[16][16];
extern int diffyy[16][16];
extern int diffy8[16][16];//for P8X8 mode 

extern int Iprev_bits;
extern int Pprev_bits;

void rc_alloc(void);
void rc_free(void);

void rc_init_seq(void);
void rc_init_GOP(int np, int nb);
void rc_update_pict_frame(int nbits);
void rc_init_pict(int fieldpic,int topfield, int targetcomputation);
void rc_update_pict(int nbits);
void setbitscount(int nbits);

int updateQuantizationParameter(int topfield);
void updateRCModel (void);
void updateMADModel (void);
Boolean skipThisFrame (void);
void RCModelEstimator (int n_windowSize);
void MADModelEstimator (int n_windowSize);
double calc_MAD(void);
double ComputeFrameMAD(void);
int Qstep2QP( double Qstep );
double QP2Qstep( int QP );

#endif

