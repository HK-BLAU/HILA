#ifndef SUNM
#define SUNM

#include "cmplx.h"
#include "general_matrix.h"
#include "../plumbing/defs.h"
#include "../plumbing/mersenne.h" //has to be included
#include <cmath>

// matrix multiplication routines ------------------------

#define nn_a(x,y) (x.d*y.a + x.a*y.d - x.b*y.c + x.c*y.b)
#define nn_b(x,y) (x.d*y.b + x.b*y.d - x.c*y.a + x.a*y.c)
#define nn_c(x,y) (x.d*y.c + x.c*y.d - x.a*y.b + x.b*y.a)
#define nn_d(x,y) (x.d*y.d - x.a*y.a - x.b*y.b - x.c*y.c)

#define na_a(x,y) (-x.d*y.a + x.a*y.d + x.b*y.c - x.c*y.b)
#define na_b(x,y) (-x.d*y.b + x.b*y.d + x.c*y.a - x.a*y.c)
#define na_c(x,y) (-x.d*y.c + x.c*y.d + x.a*y.b - x.b*y.a)
#define na_d(x,y) ( x.d*y.d + x.a*y.a + x.b*y.b + x.c*y.c)

#define an_a(x,y) ( x.d*y.a - x.a*y.d + x.b*y.c - x.c*y.b)
#define an_b(x,y) ( x.d*y.b - x.b*y.d + x.c*y.a - x.a*y.c)
#define an_c(x,y) ( x.d*y.c - x.c*y.d + x.a*y.b - x.b*y.a)
#define an_d(x,y) ( x.d*y.d + x.a*y.a + x.b*y.b + x.c*y.c)

#define aa_a(x,y) (-x.d*y.a - x.a*y.d - x.b*y.c + x.c*y.b)
#define aa_b(x,y) (-x.d*y.b - x.b*y.d - x.c*y.a + x.a*y.c)
#define aa_c(x,y) (-x.d*y.c - x.c*y.d - x.a*y.b + x.b*y.a)
#define aa_d(x,y) ( x.d*y.d - x.a*y.a - x.b*y.b - x.c*y.c)

// gaussian rng generation routines ----------------------

#define VARIANCE 0.5
#define MYSINF(X) sin(X)
#define MYCOSF(X) cos(X)

template<typename radix>
radix gaussian_ran2 (radix* out2) 
{
  double phi, urnd, r;
  phi = 2.0 * 3.141592654 * (double) hila_random();
  urnd = (double)(1.0-hila_random());
  r  = sqrt( -log(urnd) * (2.0 * VARIANCE) );
  *out2 = (r*MYCOSF(phi));
  return (radix)(r*MYSINF(phi));
}

//--------------------------------------------------------

//////////////////
/// SU(N) matrix class
/// Implementations are found in this header file, since non-specialized
/// templates have to be visible to the tranlation units that use them
//////////////////

template<int n, typename radix>
class SU : public matrix<n,n,cmplx<radix>>{};

template<typename radix> 
class SU2; 
//adjoint su2 matrix 
template<typename radix> 
class adjoint; 

template<typename radix> 
class SU2vector {
    public:
    cmplx<radix> c[2];
    SU2vector(const SU2<raxix> & m){
        v.c[0].re = m.b;				    
        v.c[0].im = m.a;			       	
        v.c[1].re = m.d;			       	
        v.c[1].im =-m.c;
    }
};

template<typename radix>
class SU2 { 
    public: 

        SU2() : a(0), b(0), c(0), d(1) {}

        SU2(radix * vals) : a(vals[0]), b(vals[1]), c(vals[2]), d(vals[3]) { normalize(); }

        SU2(const SU2vector & rhs){
            b = v.c[0].re;				
            a = v.c[0].im;				
            d = v.c[1].re;				
            c =-v.c[1].im;           
        };

        SU2(const SU2<raxix> & rhs){
            b = rhs.b;				
            a = rhs.a;				
            d = rhs.d;				
            c = rhs.c;           
        };

        friend SU2<radix> operator * (const SU2<radix> & x, const SU2<radix> & y){
            SU2<radix> r;
            r.a = nn_a(x,y); r.b = nn_b(x,y); 
            r.c = nn_c(x,y); r.d = nn_d(x,y);
            return r;
        }
 
        friend SU2<radix> operator * (const SU2<radix> & x, const adjoint<radix> & y){
            SU2<radix> r;
            r.a = na_a(x,y.ref); r.b = na_b(x,y.ref); \
            r.c = na_c(x,y.ref); r.d = na_d(x,y.ref);
            return r;
        }
 
        friend SU2<radix> operator * (const adjoint<radix> & x, const SU2<radix> & y){
            SU2<radix> r;
            r.a = an_a(x.ref,y); r.b = an_b(x.ref,y); \
            r.c = an_c(x.ref,y); r.d = an_d(x.ref,y);
            return r;
        }

        friend SU2<radix> operator * (const adjoint<radix> & x, const adjoint<radix> & y){
            SU2<radix> r;
            r.a = aa_a(x.ref, y.ref); r.b = aa_b(x.ref, y.ref); \
            r.c = aa_c(x.ref, y.ref); r.d = aa_d(x.ref, y.ref);
            return r;
        }

        SU2<radix> & operator = (const SU2<radix> &);
        SU2<radix> & operator = (const adjoint<radix> &);
        SU2<radix> & operator = (const SU2vector<radix> &);
        SU2<radix> & normalize();
        SU2<radix> & reunitarize();  
        SU2<radix> & random(); 
        SU2<radix> & inv(); 
        SU2<radix> & adj(); 
        
        radix sqr() const;
        radix tr() const;
        radix det() const; 

        SU2<radix> operator + (const SU2<radix> &); //basic operations. These versions return new matrix as result
        SU2<radix> operator - (const SU2<radix> &);
        SU2<radix> & operator += (const SU2<radix> &); //same ops as above, except store result in lhs matrix
        SU2<radix> & operator -= (const SU2<radix> &);
        SU2<radix> & operator *= (const SU2<radix> &);

        SU2<radix> & operator += (const adjoint<radix> &); //same ops as above, except store result in lhs matrix
        SU2<radix> & operator -= (const adjoint<radix> &);
        SU2<radix> & operator *= (const adjoint<radix> &);

        SU2<radix> & operator *= (const radix &);

        SU2<radix> operator * (const radix &);
    private:
        radix a, b, c, d;  
};

template<typename radix>
class adjoint {
    public:
    adjoint (const SU2<radix> & rhs) : ref(rhs) {} ;
    const SU2<radix> & ref;
};

template<typename radix>
radix SU2<radix>::sqr() const {
    return a*a + b*b + c*c + d*d;
}

template<typename radix>
radix SU2<radix>::det() const {
    return a*a + b*b + c*c + d*d;
}

template<typename radix>
radix SU2<radix>::tr() const {
    return 2*d;
}

template<typename radix>
SU2<radix> & SU2<radix>::normalize(){
    radix sq = sqrt(this->sqr());
    a /= sq;
    b /= sq;
    c /= sq;
    d /= sq;   
    return *this;
}

template<typename radix>
SU2<radix> & SU2<radix>::reunitarize(){
    return this->normalize();
}

template<typename radix>
SU2<radix> & SU2<radix>::random(){
    radix one, two;
    one = gaussian_ran2(&two);
    a = one;
    b = two;
    one = gaussian_ran2(&two);
    c = one;
    d = two;
    return this->normalize();
}

template<typename radix>
SU2<radix> & SU2<radix>::inv(){
    a *= static_cast<radix>(-1);
    b *= static_cast<radix>(-1);
    c *= static_cast<radix>(-1);
    d *= static_cast<radix>(-1);
    return *this;
}

template<typename radix>
SU2<radix> & SU2<radix>::operator = (const SU2<radix> & rhs){
    a = rhs.a;
    b = rhs.b;
    c = rhs.c;
    d = rhs.d;
    return *this;
};

template<typename radix>
SU2<radix> & SU2<radix>::operator = (const adjoint<radix> & rhs){
    a = -rhs.a;
    b = -rhs.b;
    c = -rhs.c;
    d = rhs.d;
    return *this;
};

template<typename radix>
SU2<radix> & SU2<radix>::operator *= (const SU2<radix> & y){
    a = nn_a((*this),y); 
    b = nn_b((*this),y); 
    c = nn_c((*this),y); 
    d = nn_d((*this),y); 
    return *this;
}

template<typename radix>
SU2<radix> & SU2<radix>::operator *= (const adjoint<radix> & y){
    a = na_a((*this),y); 
    b = na_b((*this),y); 
    c = na_c((*this),y); 
    d = na_d((*this),y); 
    return *this;
}

template<typename radix, typename radix2>
SU2<radix> & SU2<radix>::operator *= (const radix2 & rhs){ 
    a *= static_cast<radix>(rhs);
    b *= static_cast<radix>(rhs);
    c *= static_cast<radix>(rhs);
    d *= static_cast<radix>(rhs);
    return *this;
};

template<typename radix>
SU2<radix> SU2<radix>::operator * (const radix & rhs){ 
    SU2<radix> r;
    r.a = a*static_cast<radix>(rhs);
    r.b = b*static_cast<radix>(rhs);
    r.c = c*static_cast<radix>(rhs);
    r.d = d*static_cast<radix>(rhs);
    return r;
};

SU2<radix> operator + (const SU2<radix> & y){
    SU2<radix> r;
    r.a = x.a + y.a; r.b = x.b + y.b; \
    r.c = x.c + y.c; r.d = x.d + y.d;
    return r;
};

SU2<radix> operator - (const SU2<radix> &){
    SU2<radix> r;
    r.a = x.a - y.a; r.b = x.b - y.b; \
    r.c = x.c - y.c; r.d = x.d - y.d;
    return r;
};

#endif 