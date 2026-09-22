// Compile ONLY the render entry points to audit their entire call graph.
#include "ymfm_precision.h"
extern "C" bool render32(ymfm::precision::fm_engine<float,128>* engine,
                         float* left,float* right,std::size_t frames)
{ return engine->render(left,right,frames); }
extern "C" bool render64(ymfm::precision::fm_engine<double,128>* engine,
                         double* left,double* right,std::size_t frames)
{ return engine->render(left,right,frames); }
