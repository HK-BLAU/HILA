#ifndef SU_VEC
#define SU_VEC

#include "cmplx.h"

template<int N, typename radix>
class SU_vector {
    public:
    using base_type = typename base_type_struct<radix>::type;
    cmplx<radix> c[N];
};

template<typename radix> 
class SU2; 

template<typename radix> 
class SU2vector : public SU_vector<2, radix> {
    public:
    using base_type = typename base_type_struct<radix>::type;
    SU2vector(const SU2<radix> & m){
        this->c[0].re = m.b;				    
        this->c[0].im = m.a;			       	
        this->c[1].re = m.d;			       	
        this->c[1].im =-m.c;
    }
};

#endif