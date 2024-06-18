#include "hila.h"
#include "gauge/polyakov.h"
#include <string>

using ftype=double;
using mygroup=SU<NCOLOR,ftype>;

template<typename ... Args>
std::string string_format(const std::string& format,Args ... args) {
    // wrapper for std::snprintf which sets up buffer of required size

    // determine required buffer size :
    int size_s=std::snprintf(nullptr,0,format.c_str(),args ...)+1;
    if(size_s<=0) { 
        throw std::runtime_error("Error during formatting."); 
    }

    // allocate buffer :
    auto size=static_cast<size_t>(size_s);
    std::unique_ptr<char[]> buf(new char[size]);

    // write formatted string to buffer :
    std::snprintf(buf.get(),size,format.c_str(),args ...);

    return std::string(buf.get(),buf.get()+size-1); 
}

// define a struct to hold the input parameters: this
// makes it simpler to pass the values around
struct parameters {
    ftype beta;          // inverse gauge coupling
    ftype c11;           // improved gauge action plaquette weight
    ftype c12;           // improved gauge action 1x2-rectangle weight
    ftype dt;            // HMC time step
    int trajlen;         // number of HMC time steps per trajectory
    int n_traj;          // number of trajectories to generate
    int n_therm;         // number of thermalization trajectories (counts only accepted traj.)
    int wflow_freq;      // number of trajectories between wflow measurements
    ftype wflow_max_l;   // flow scale at which wilson flow stops
    ftype wflow_l_step;  // flow scale interval between flow measurements
    ftype wflow_a_accu;  // desired absolute accuracy of wilson flow integration steps
    ftype wflow_r_accu;  // desired relative accuracy of wilson flow integration steps
    int n_save;          // number of trajectories between config. check point 
    std::string config_file;
    ftype time_offset;
};


///////////////////////////////////////////////////////////////////////////////////
// general functions

template <typename T>
void staplesum(const GaugeField<T>& U,Field<T>& staples,Direction d1,Parity par=ALL) {

    Field<T> lower;

    bool first=true;
    foralldir(d2) if(d2!=d1) {

        // anticipate that these are needed
        // not really necessary, but may be faster
        U[d2].start_gather(d1,ALL);
        U[d1].start_gather(d2,par);

        // calculate first lower 'U' of the staple sum
        // do it on opp parity
        onsites(opp_parity(par)) {
            lower[X]=(U[d1][X]*U[d2][X+d1]).dagger()*U[d2][X];
        }

        // calculate then the upper 'n', and add the lower
        // lower could also be added on a separate loop
        if(first) {
            onsites(par) {
                staples[X]=U[d2][X+d1]*(U[d2][X]*U[d1][X+d2]).dagger()+lower[X-d2];
            }
            first=false;
        } else {
            onsites(par) {
                staples[X]+=U[d2][X+d1]*(U[d2][X]*U[d1][X+d2]).dagger()+lower[X-d2];
            }
        }
    }
}

template <typename group>
ftype measure_plaq(const GaugeField<group>& U) {
    // measure the Wilson plaquette action
    Reduction<ftype> plaq=0;
    plaq.allreduce(false).delayed(true);
    foralldir(dir1) foralldir(dir2) if(dir1<dir2) {
        onsites(ALL) {
            plaq+=1.0-real(trace(U[dir1][X]*U[dir2][X+dir1]*
                U[dir1][X+dir2].dagger()*U[dir2][X].dagger()))/group::size();
        }
    }
    return plaq.value();
}

template <typename group,typename atype=hila::arithmetic_type<group>>
atype measure_e2(const VectorField<Algebra<group>>& E) {
    // compute gauge kinetic energy from momentum field E
    Reduction<atype> e2=0;
    e2.allreduce(false).delayed(true);
    foralldir(d) {
        onsites(ALL) e2+=E[d][X].squarenorm();
    }
    return e2.value()/2;
}

template <typename group,int L>
void get_wilson_line_b(const GaugeField<group>& U,const Direction(&path)[L],Field<group>& R) {
    // compute the Wilson line defined by the list of directions "path"
    CoordinateVector v=0;
    // initialize R with first link of the Wilson line:
    if(path[0]<NDIM) {
        // link points in positive direction
        onsites(ALL) R[X]=U[path[0]][X];
        v+=path[0];
    } else {
        // link points in negative direction
        v+=path[0];
        onsites(ALL) R[X]=U[opp_dir(path[0])][X+v].dagger();
    }

    // multiply R successively with the remaining links of the Wilson line
    for(int i=1; i<L; ++i) {
        if(path[i]<NDIM) {
            // link points in positive direction
            onsites(ALL) R[X]*=U[path[i]][X+v];
            v+=path[i];
        } else {
            // link points in negative direction
            v+=path[i];
            onsites(ALL) R[X]*=U[opp_dir(path[i])][X+v].dagger();
        }
    }
}

template <typename group,int L>
void get_wloop_force_add_b(const GaugeField<group>& U,const Direction(&path)[L],ftype eps,VectorField<Algebra<group>>& K) {
    // compute gauge force of Wilson loop defined by "path" and add result to vector field "K"
    Field<group> R;
    get_wilson_line_b(U,path,R);

    CoordinateVector v=0;
    for(int i=0; i<L; ++i) {
        if(path[i]<NDIM) {
            // link points in positive direction
            onsites(ALL) K[path[i]][X]-=R[X-v].project_to_algebra_scaled(eps);

            onsites(ALL) R[X]=U[path[i]][X+v].dagger()*R[X]*U[path[i]][X+v];

            v+=path[i];
        } else {
            // link points in negative direction
            v+=path[i];

            onsites(ALL) R[X]=U[opp_dir(path[i])][X+v]*R[X]*U[opp_dir(path[i])][X+v].dagger();

            onsites(ALL) K[opp_dir(path[i])][X]+=R[X-v].project_to_algebra_scaled(eps);
        }
    }
}

template <typename group,int L>
void get_wilson_line(const GaugeField<group>& U,const Direction(&path)[L],Field<group>& R) {
    // compute the Wilson line defined by the list of directions "path"
    int i,ip;

    int udirs[NDIRS]={0};
    for(i=0; i<L; ++i) {
        if((udirs[(int)path[i]]++)==0) {
            if(path[i]<NDIM) {
                U[path[i]].start_gather(opp_dir(path[i]),ALL);
            }
        }
    }

    Field<group> R0[2];

    i=0;
    ip=0;
    // initialize R0[0] with first link variable of the Wilson line:
    if(path[i]<NDIM) {
        // link points in positive direction
        onsites(ALL) R0[ip][X]=U[path[i]][X-path[i]];
    } else {
        // link points in negative direction
        onsites(ALL) R0[ip][X]=U[opp_dir(path[i])][X].dagger();
    }

    // multiply R0[ip] successively with the L-2 intermediate link variables of the Wilson line
    // and store the result in R0[1-ip]
    for(i=1; i<L-1; ++i) {
        //R0[ip].start_gather(opp_dir(path[i]),ALL);
        if(path[i]<NDIM) {
            // link points in positive direction
            onsites(ALL) mult(R0[ip][X-path[i]],U[path[i]][X-path[i]],R0[1-ip][X]);
        } else {
            // link points in negative direction
            onsites(ALL) mult(R0[ip][X-path[i]],U[opp_dir(path[i])][X].dagger(),R0[1-ip][X]);
        }
        ip=1-ip;
    }

    // multiply R0[ip] by the last link variable of the Wilson line and store the result in R
    //R0[ip].start_gather(opp_dir(path[i]),ALL);
    if(path[i]<NDIM) {
        // link points in positive direction
        onsites(ALL) mult(R0[ip][X-path[i]],U[path[i]][X-path[i]],R[X]);
    } else {
        // link points in negative direction
        onsites(ALL) mult(R0[ip][X-path[i]],U[opp_dir(path[i])][X].dagger(),R[X]);
    }
}

template <typename group,int L>
void get_wloop_force_add(const GaugeField<group>& U,const Direction(&path)[L],const Field<group>& W,ftype eps,VectorField<Algebra<group>>& K) {
    // compute gauge force of Wilson loop "W", corresponding to "path" and add result to vector field "K"
    Field<group> R=W;
    Field<group> R0;
    int udirs[NDIRS]={0};
    for(int i=0; i<L; ++i) {
        if((udirs[(int)path[i]]++)==0) {
            if(path[i]<NDIM) {
                U[path[i]].start_gather(opp_dir(path[i]),ALL);
            }
        }
    }

    for(int i=0; i<L; ++i) {
        //R.start_gather(opp_dir(path[i]),ALL);
        if(path[i]<NDIM) {
            // link points in positive direction
            onsites(ALL) K[path[i]][X]-=R[X].project_to_algebra_scaled(eps);

            onsites(ALL) mult(U[path[i]][X-path[i]].dagger(),R[X-path[i]],R0[X]);
            onsites(ALL) mult(R0[X],U[path[i]][X-path[i]],R[X]);

            //onsites(ALL) R0[X]=U[path[i]][X-path[i]].dagger()*R[X-path[i]];
            //onsites(ALL) R[X]=R0[X]*U[path[i]][X-path[i]];
        } else {
            // link points in negative direction
            onsites(ALL) mult(U[opp_dir(path[i])][X],R[X-path[i]],R0[X]);
            onsites(ALL) mult(R0[X],U[opp_dir(path[i])][X].dagger(),R[X]);

            //onsites(ALL) R0[X]=U[opp_dir(path[i])][X]*R[X-path[i]];
            //onsites(ALL) R[X]=R0[X]*U[opp_dir(path[i])][X].dagger();

            onsites(ALL) K[opp_dir(path[i])][X]+=R[X].project_to_algebra_scaled(eps);
        }
    }
}

template <typename group,int L>
void get_wloop_force_add(const GaugeField<group>& U,const Direction(&path)[L],ftype eps,VectorField<Algebra<group>>& K) {
    // compute gauge force of Wilson loop along "path" and add result to vector field "K"
    Field<group> R;
    Field<group> R0;
    get_wilson_line(U,path,R);

    int udirs[NDIRS]={0};
    for(int i=0; i<L; ++i) {
        if((udirs[(int)path[i]]++)==0) {
            if(path[i]<NDIM) {
                U[path[i]].start_gather(opp_dir(path[i]),ALL);
            } 
        }
    }

    for(int i=0; i<L; ++i) {
        //R.start_gather(opp_dir(path[i]),ALL);
        if(path[i]<NDIM) {
            // link points in positive direction
            onsites(ALL) K[path[i]][X]-=R[X].project_to_algebra_scaled(eps);

            onsites(ALL) mult(U[path[i]][X-path[i]].dagger(),R[X-path[i]],R0[X]);
            onsites(ALL) mult(R0[X],U[path[i]][X-path[i]],R[X]);

            //onsites(ALL) R0[X]=U[path[i]][X-path[i]].dagger()*R[X-path[i]];
            //onsites(ALL) R[X]=R0[X]*U[path[i]][X-path[i]];
        } else {
            // link points in negative direction
            onsites(ALL) mult(U[opp_dir(path[i])][X],R[X-path[i]],R0[X]);
            onsites(ALL) mult(R0[X],U[opp_dir(path[i])][X].dagger(),R[X]);

            //onsites(ALL) R0[X]=U[opp_dir(path[i])][X]*R[X-path[i]];
            //onsites(ALL) R[X]=R0[X]*U[opp_dir(path[i])][X].dagger();

            onsites(ALL) K[opp_dir(path[i])][X]+=R[X].project_to_algebra_scaled(eps);
        }
    }
}

template <typename group,int L>
void get_wloop_force(const GaugeField<group>& U,const Direction(&path)[L],ftype eps,VectorField<Algebra<group>>& K) {
    // compute gauge force of Wilson loop defined by "path" and store result in vector field "K"
    foralldir(d1) {
        K[d1][ALL]=0;
    }
    get_wloop_froce_add(U,path,eps,K);
}

template <typename group>
ftype measure_rect12(const GaugeField<group>& U) {
    // measure the 1x2-rectangle action
    Reduction<ftype> plaq=0;
    plaq.allreduce(false).delayed(true);
    Field<group> R=0;
    foralldir(dir1) foralldir(dir2) if(dir1!=dir2) {
        Direction path[6]={dir1,dir2,dir2,opp_dir(dir1),opp_dir(dir2),opp_dir(dir2)};
        get_wilson_line(U,path,R);
        onsites(ALL) {
            plaq+=1.0-real(trace(R[X]))/group::size();
        }
    }
    return plaq.value();
}

template <typename group>
void get_force_rect12(const GaugeField<group>& U,VectorField<Algebra<group>>& K) {
    // compute the force for the 1-2-rectangle term
    foralldir(d1) {
        K[d1][ALL]=0;
    }
    foralldir(dir1) foralldir(dir2) if(dir1!=dir2) {
        Direction path[6]={dir1,dir2,dir2,opp_dir(dir1),opp_dir(dir2),opp_dir(dir2)};
        get_wloop_force_add(U,path,1.0,K);
    }
}

template <typename group,typename atype=hila::arithmetic_type<group>>
void update_E_impr(const GaugeField<group>& U,VectorField<Algebra<group>>& E,const parameters& p,atype delta) {
    // compute the force for the improved action -S_{impr}=\beta/N*(c11*ReTr(plaq)+c12*ReTr(rect))
    // and use it to evolve the momentum field E
    
    auto eps=delta*p.beta/group::size();
    
    // plaquette part:
    foralldir(dir1) foralldir(dir2) if(dir1<dir2) {
        Direction path[4]={dir1,dir2,opp_dir(dir1),opp_dir(dir2)};
        get_wloop_force_add(U,path,eps*p.c11,E);
    }

    if(p.c12!=0) {
        // rectangle part:
        foralldir(dir1) foralldir(dir2) if(dir1!=dir2) {
            Direction path[6]={dir1,dir2,dir2,opp_dir(dir1),opp_dir(dir2),opp_dir(dir2)};
            get_wloop_force_add(U,path,eps*p.c12,E);
        }
    }
}

template <typename group,typename atype=hila::arithmetic_type<group>>
void get_force_impr(const GaugeField<group>& U,VectorField<Algebra<group>>& K,const parameters& p) {
    // compute the force for the improved action -S_{impr}=\beta/N*(c11*ReTr(plaq)+c12*ReTr(rect))
    // and write it to K

    foralldir(d1) {
        K[d1][ALL]=0;
    }

    // plaquette part:
    foralldir(dir1) foralldir(dir2) if(dir1<dir2) {
        Direction path[4]={dir1,dir2,opp_dir(dir1),opp_dir(dir2)};
        get_wloop_force_add(U,path,p.c11,K);
    }

    if(p.c12!=0) {
        // rectangle part:
        foralldir(dir1) foralldir(dir2) if(dir1!=dir2) {
            Direction path[6]={dir1,dir2,dir2,opp_dir(dir1),opp_dir(dir2),opp_dir(dir2)};
            get_wloop_force_add(U,path,p.c12,K);
        }
    }
}

template <typename group,typename atype=hila::arithmetic_type<group>>
void get_force_impr_f(const GaugeField<group>& U,VectorField<Algebra<group>>& K,const parameters& p) {
    // compute the force for the improved action -S_{impr}=\beta/N*(c11*ReTr(plaq)+c12*ReTr(rect))
    // in a faster way and write the results to K

    foralldir(d1) {
        K[d1][ALL]=0;
    }

    Field<group> ustap;
    Field<group> lstap;

    foralldir(dir1) foralldir(dir2) if(dir1!=dir2) {
        U[dir2].start_gather(dir1,ALL);
        U[dir1].start_gather(dir2,ALL);
        // get upper (dir1,dir2) and lower (dir1,-dir2) staples
        onsites(ALL) {
            lstap[X]=(U[dir1][X]*U[dir2][X+dir1]).dagger()*U[dir2][X];
            ustap[X]=U[dir2][X+dir1]*(U[dir2][X]*U[dir1][X+dir2]).dagger();
        }

        lstap.start_gather(opp_dir(dir2),ALL);

        // compute plaquette contribution to force and add it to K
        onsites(ALL) {
            K[dir1][X]-=(U[dir1][X]*(ustap[X]+lstap[X-dir2])).project_to_algebra_scaled(p.c11);
        }
        
        if(p.c12!=0) {
            // rectangle contribution to force
            Direction path[6]={opp_dir(dir2),dir1,dir2,dir2,opp_dir(dir1),opp_dir(dir2)};

            // compose rectangle and store it in ustap
            onsites(ALL) ustap[X]=lstap[X-dir2].dagger()*ustap[X];

            // compute rectangle force and add it to K
            get_wloop_force_add(U,path,ustap,p.c12,K);
        }
    }
}

template <typename group,typename atype=hila::arithmetic_type<group>>
void get_force_impr_f2(const GaugeField<group>& U,VectorField<Algebra<group>>& K,const parameters& p) {
    // compute the force for the improved action -S_{impr}=\beta/N*(c11*ReTr(plaq)+c12*ReTr(rect))
    // in a faster way and write the results to K

    foralldir(d1) {
        K[d1][ALL]=0;
    }

    Field<group> ustap;
    Field<group> lstap;
    Field<group> tstap;

    bool first=true;
    foralldir(dir1) {
        first=true;
        foralldir(dir2) if(dir1!=dir2) {
            U[dir2].start_gather(dir1,ALL);
            U[dir1].start_gather(dir2,ALL);

            // get upper (dir1,dir2) and lower (dir1,-dir2) staples
            onsites(ALL) {
                lstap[X]=(U[dir1][X]*U[dir2][X+dir1]).dagger()*U[dir2][X];
                ustap[X]=U[dir2][X+dir1]*(U[dir2][X]*U[dir1][X+dir2]).dagger();
            }

            lstap.start_gather(opp_dir(dir2),ALL);

            if(first) {
                onsites(ALL) {
                    tstap[X]=ustap[X];
                    tstap[X]+=lstap[X-dir2];
                }
            } else {
                onsites(ALL) {
                    tstap[X]+=ustap[X];
                    tstap[X]+=lstap[X-dir2];
                }
            }

            if(p.c12!=0) {
                // rectangle contribution to force
                Direction path[6]={opp_dir(dir2),dir1,dir2,dir2,opp_dir(dir1),opp_dir(dir2)};

                // compose rectangle and store it in ustap
                onsites(ALL) ustap[X]=lstap[X-dir2].dagger()*ustap[X];

                // compute rectangle force and add it to K
                get_wloop_force_add(U,path,ustap,p.c12,K);
            }
            first=false;
        }
        // compute plaquette contribution to force and add it to K
        onsites(ALL) {
            K[dir1][X]-=(U[dir1][X]*tstap[X]).project_to_algebra_scaled(p.c11);
        }
    }
}

template <typename group,typename atype=hila::arithmetic_type<group>>
atype measure_action_impr(const GaugeField<group>& U,const VectorField<Algebra<group>>& E,const parameters& p) {
    // measure the total action, consisting of plaquette, rectangle (if present), and momentum term
    atype plaq=p.c11*measure_plaq(U);
    if(p.c12!=0) {
        plaq+=p.c12*measure_rect12(U);
    }
    atype e2=measure_e2(E);
    return p.beta*plaq+e2/2;
}

template <typename group,typename atype=hila::arithmetic_type<group>>
void update_U(GaugeField<group>& U,const VectorField<Algebra<group>>& E,atype delta) {
    // evolve U with momentum E over time step delta 
    foralldir(d) {
        onsites(ALL) U[d][X]=chexp(E[d][X]*delta)*U[d][X];
    }
}

template <typename group,typename atype=hila::arithmetic_type<group>>
void measure_topo_charge_and_energy_log(const GaugeField<group>& U, atype& qtopo_out, atype& energy_out) {
    // measure topological charge and field strength energy of the gauge field, using the
    // matrix logarithms of the plaquettes as components of the field strength tensor

    Reduction<atype> qtopo=0;
    Reduction<atype> energy=0;
    qtopo.allreduce(false).delayed(true);
    energy.allreduce(false).delayed(true);

#if NDIM == 4
    Field<group> F[6];
    // F[0]: F[0][1], F[1]: F[0][2], F[2]: F[0][3],
    // F[3]: F[1][2], F[4]: F[1][3], F[5]: F[2][3]
    Field<group> tF0,tF1;

    int k=0;
    // (Note: by replacing log(...).expand() in the following by anti-hermitian projection,
    // one would recover the clover F[dir1][dir2])
    foralldir(dir1) foralldir(dir2) if(dir1<dir2) {
        U[dir2].start_gather(dir1,ALL);
        U[dir1].start_gather(dir2,ALL);

        onsites(ALL) {
            // log of dir1-dir2-plaquette that starts and ends at X; corresponds to F[dir1][dir2]
            // at center location X+dir1/2+dir2/2 of plaquette:
            tF0[X]=log((U[dir1][X]*U[dir2][X+dir1]*(U[dir2][X]*U[dir1][X+dir2]).dagger())).expand();
            // parallel transport to X+dir1
            tF1[X]=U[dir1][X].dagger()*tF0[X]*U[dir1][X];
        }

        tF1.start_gather(opp_dir(dir1),ALL);
        onsites(ALL) {
            tF0[X]+=tF1[X-dir1];
        }
        U[dir2].start_gather(opp_dir(dir2),ALL);
        tF0.start_gather(opp_dir(dir2),ALL);
        onsites(ALL) {
            // get F[dir1][dir2] at X from average of the (parallel transported) F[dir1][dir2] from 
            // the centers of all dir1-dir2-plaquettes that touch X :
            F[k][X]=(tF0[X]+U[dir2][X-dir2].dagger()*tF0[X-dir2]*U[dir2][X-dir2])*0.25;
        }
        ++k;
    }
    onsites(ALL) {
        qtopo+=real(trace(F[0][X]*F[5][X]));
        qtopo+=-real(trace(F[1][X]*F[4][X]));
        qtopo+=real(trace(F[2][X]*F[3][X]));

        energy+=F[0][X].squarenorm();
        energy+=F[1][X].squarenorm();
        energy+=F[2][X].squarenorm();
        energy+=F[3][X].squarenorm();
        energy+=F[4][X].squarenorm();
        energy+=F[5][X].squarenorm();
    }
#endif
    qtopo_out=qtopo.value()/(4.0*M_PI*M_PI);
    energy_out=energy.value();
}

template <typename group,typename atype=hila::arithmetic_type<group>>
void measure_topo_charge_and_energy_clover(const GaugeField<group>& U,atype& qtopo_out,atype& energy_out) {
    // measure topological charge and field strength energy of the gauge field, using the
    // clover matrices as components of the field strength tensor

    Reduction<atype> qtopo=0;
    Reduction<atype> energy=0;
    qtopo.allreduce(false).delayed(true);
    energy.allreduce(false).delayed(true);

#if NDIM == 4
    Field<group> F[6];
    // F[0]: F[0][1], F[1]: F[0][2], F[2]: F[0][3],
    // F[3]: F[1][2], F[4]: F[1][3], F[5]: F[2][3]
    int k=0;
    foralldir(dir1) foralldir(dir2) if(dir1<dir2) {
        onsites(ALL) {
            // clover operator as in eq. (2.9) of Nuclear Physics B259 (1985) 572-596
            F[k][X]=0.25*(U[dir1][X]*U[dir2][X+dir1]*(U[dir2][X]*U[dir1][X+dir2]).dagger()
                -(U[dir1][X-dir1-dir2]*U[dir2][X-dir2]).dagger()*U[dir2][X-dir1-dir2]*U[dir1][X-dir1]
                +U[dir2][X]*(U[dir2][X-dir1]*U[dir1][X-dir1+dir2]).dagger()*U[dir1][X-dir1]
                -U[dir1][X]*(U[dir1][X-dir2]*U[dir2][X+dir1-dir2]).dagger()*U[dir2][X-dir2]);
            F[k][X]=0.5*(F[k][X]-F[k][X].dagger()); // anti-hermitian projection
        }
        ++k;
    }
    onsites(ALL) {
        qtopo+=real(trace(F[0][X]*F[5][X]));
        qtopo+=-real(trace(F[1][X]*F[4][X]));
        qtopo+=real(trace(F[2][X]*F[3][X]));

        energy+=F[0][X].squarenorm();
        energy+=F[1][X].squarenorm();
        energy+=F[2][X].squarenorm();
        energy+=F[3][X].squarenorm();
        energy+=F[4][X].squarenorm();
        energy+=F[5][X].squarenorm();
    }
#endif
    qtopo_out=qtopo.value()/(4.0*M_PI*M_PI);
    energy_out=energy.value();
}


// end general functions
///////////////////////////////////////////////////////////////////////////////////
// non-bulk-prevention functions

template <typename group>
void get_force(const GaugeField<group>& U,VectorField<Algebra<group>>& K) {
    // compute the force for the plaquette action and write it to K
    Field<group> staple;
    foralldir(d) {
        staplesum(U,staple,d);
        onsites(ALL) {
            K[d][X]=(U[d][X]*staple[X]).project_to_algebra_scaled(-1.0);
        }
    }
}

template <typename group,typename atype=hila::arithmetic_type<group>>
void update_E(const GaugeField<group>& U,VectorField<Algebra<group>>& E,const parameters& p,atype delta) {
    // compute the force for the plaquette action and use it to evolve E
    Field<group> staple;
    auto eps=delta*p.beta/group::size();
    foralldir(d) {
        staplesum(U,staple,d);
        onsites(ALL) {
            E[d][X]-=(U[d][X]*staple[X]).project_to_algebra_scaled(eps);
        }
    }
}

template <typename group,typename atype=hila::arithmetic_type<group>>
atype measure_action(const GaugeField<group>& U,const VectorField<Algebra<group>>& E,const parameters& p) {
    // measure the total action, consisting of plaquette and momentum term
    atype plaq=measure_plaq(U);
    atype e2=measure_e2(E);
    return p.beta*plaq+e2/2;
}

template <typename group>
void do_trajectory(GaugeField<group>& U,VectorField<Algebra<group>>& E,const parameters& p) {
    // leap frog integration for normal action

    // start trajectory: advance U by half a time step
    update_U(U,E,p.dt/2);
    // main trajectory integration:
    for(int n=0; n<p.trajlen-1; n++) {
        update_E(U,E,p,p.dt);
        update_U(U,E,p.dt);
    }
    // end trajectory: bring U and E to the same time
    update_E(U,E,p,p.dt);
    update_U(U,E,p.dt/2);
    
    U.reunitarize_gauge();
}

// end non-bulk-prevention functions
///////////////////////////////////////////////////////////////////////////////////
// bulk-prevention functions, cf. arXiv:2306.14319 (with n=2)

template <typename T>
T ch_inv(const T& U) {
    // compute inverse of the square matrix U, using the 
    // Cayley-Hamilton theorem (Faddeev-LeVerrier algorithm)
    T tB[2];
    int ip=0;
    tB[ip]=1.;
    auto tc=trace(U);
    tB[1-ip]=U;
    for(int k=2; k<=T::size(); ++k) {
        tB[1-ip]-=tc;
        mult(U,tB[1-ip],tB[ip]);
        tc=trace(tB[ip])/k;
        ip=1-ip;
    }
    return tB[ip]/tc;
}

template <typename T>
T bp_UAmat(const T& U) {
    // compute U*A(U) with the A-matrix from Eq. (B3) of arXiv:2306.14319 for n=2
    T tA1=U;
    tA1+=1.;
    tA1*=0.5;
    T tA2=ch_inv(tA1); 
    tA1=tA2*tA2.dagger();
    return U*tA1*tA1*tA2;
}

template <typename T>
T bp_iOsqmat(const T& U) {
    // compute matrix inside the trace on r.h.s. of Eq. (B1) of arXiv:2306.14319 for n=2
    T tA1=U;
    tA1+=1.;
    tA1*=0.5;
    T tA2=tA1.dagger()*tA1;
    tA1=ch_inv(tA2);
    return tA1*tA1-1.;
}

template <typename group>
void get_force_bp(const GaugeField<group>& U,VectorField<Algebra<group>>& K) {
    // compute force for BP action for n=2 according to Eq. (B5) of arXiv:2306.14319 and write it to K
    Field<group> fmatp;
    Field<group> fmatmd1;
    Field<group> fmatmd2;
    auto eps=2.0;
    bool first=true;
    foralldir(d1) {
        K[d1][ALL]=0;
    }
    foralldir(d1) {
        foralldir(d2) if(d2>d1) {
            U[d2].start_gather(d1,ALL);
            U[d1].start_gather(d2,ALL);
            onsites(ALL) {
                fmatp[X]=bp_UAmat(U[d1][X]*U[d2][X+d1]*(U[d2][X]*U[d1][X+d2]).dagger());
                fmatmd1[X]=(fmatp[X]*U[d2][X]).dagger()*U[d2][X]; // parallel transport fmatp[X].dagger() to X+d2
                fmatmd2[X]=U[d1][X].dagger()*fmatp[X]*U[d1][X]; // parallel transport fmatp[X] to X+d1
            }
            fmatmd1.start_gather(opp_dir(d2),ALL);
            fmatmd2.start_gather(opp_dir(d1),ALL);
            onsites(ALL) {
                K[d1][X]-=(fmatmd1[X-d2]+fmatp[X]).project_to_algebra_scaled(eps);
                K[d2][X]-=(fmatmd2[X-d1]-fmatp[X]).project_to_algebra_scaled(eps);
            }
        }
    }
}

template <typename group,typename atype=hila::arithmetic_type<group>>
void update_E_bp(const GaugeField<group>& U,VectorField<Algebra<group>>& E,const parameters& p,atype delta) {
    // compute force for BP action for n=2 according to Eq. (B5) of arXiv:2306.14319 and use it to evolve E
    Field<group> fmatp;
    Field<group> fmatmd1;
    Field<group> fmatmd2;
    auto eps=delta*2.0*p.beta/group::size();
    foralldir(d1) {
        foralldir(d2) if(d2>d1) {
            U[d2].start_gather(d1,ALL);
            U[d1].start_gather(d2,ALL);
            onsites(ALL) {
                fmatp[X]=bp_UAmat(U[d1][X]*U[d2][X+d1]*(U[d2][X]*U[d1][X+d2]).dagger());
                fmatmd1[X]=(fmatp[X]*U[d2][X]).dagger()*U[d2][X]; // parallel transport fmatp[X].dagger() to X+d2
                fmatmd2[X]=U[d1][X].dagger()*fmatp[X]*U[d1][X]; // parallel transport fmatp[X] to X+d1
            }
            fmatmd1.start_gather(opp_dir(d2),ALL);
            fmatmd2.start_gather(opp_dir(d1),ALL);

            onsites(ALL) {
                E[d1][X]-=(fmatmd1[X-d2]+fmatp[X]).project_to_algebra_scaled(eps);
                E[d2][X]-=(fmatmd2[X-d1]-fmatp[X]).project_to_algebra_scaled(eps);
            }
        }
    }
}

template <typename group,typename atype=hila::arithmetic_type<group>>
atype measure_plaq_bp(const GaugeField<group>& U) {
    // measure the BP plaquette action
    Reduction<hila::arithmetic_type<group>> plaq=0;
    plaq.allreduce(false).delayed(true);
    foralldir(dir1) foralldir(dir2) if(dir1<dir2) {
        U[dir2].start_gather(dir1,ALL);
        U[dir1].start_gather(dir2,ALL);
        onsites(ALL) {
            plaq+=real(trace(bp_iOsqmat(U[dir1][X]*U[dir2][X+dir1]*
                (U[dir2][X]*U[dir1][X+dir2]).dagger())))/group::size();
        }
    }
    return plaq.value();
}

template <typename group,typename atype=hila::arithmetic_type<group>>
atype measure_action_bp(const GaugeField<group>& U,const VectorField<Algebra<group>>& E,const parameters& p) {
    // measure the total BP action, consisting of BP-plaquette and momentum term
    auto plaq=measure_plaq_bp(U);
    auto e2=measure_e2(E);
    return p.beta*plaq+e2/2;
}

template <typename group>
void do_trajectory_bp(GaugeField<group>& U,VectorField<Algebra<group>>& E,const parameters& p) {
    // leap frog integration for BP action

    // start trajectory: advance U by half a time step
    update_U(U,E,p.dt/2);
    // main trajectory integration:
    for(int n=0; n<p.trajlen-1; n++) {
        update_E_bp(U,E,p,p.dt);
        update_U(U,E,p.dt);
    }
    // end trajectory: bring U and E to the same time
    update_E_bp(U,E,p,p.dt);
    update_U(U,E,p.dt/2);

    U.reunitarize_gauge();
}

// end bulk-prevention functions
///////////////////////////////////////////////////////////////////////////////////
// measurement functions

template <typename group>
void measure_stuff(const GaugeField<group>& U,const VectorField<Algebra<group>>& E) {
    // perform measurements on current gauge and momentum pair (U, E) and
    // print results in formatted form to standard output
    static bool first=true;
    if(first) {
        // print legend for measurement output
        hila::out0<<"LMEAS:     BP-plaq          plaq          rect           E^2        P.real        P.imag\n";
        first=false;
    }
    auto plaqbp=measure_plaq_bp(U)/(lattice.volume()*NDIM*(NDIM-1)/2);
    auto plaq=measure_plaq(U)/(lattice.volume()*NDIM*(NDIM-1)/2);
    auto rect=measure_rect12(U)*0.25/(lattice.volume()*NDIM*(NDIM-1));
    auto e2=measure_e2(E)/(lattice.volume()*NDIM);
    auto poly=measure_polyakov(U,e_t);
    hila::out0<<string_format("MEAS % 0.6e % 0.6e % 0.6e % 0.6e % 0.6e % 0.6e",plaqbp,plaq,rect,e2,poly.real(),poly.imag())<<'\n';
}

// end measurement functions
///////////////////////////////////////////////////////////////////////////////////
// Wilson flow functions

template <typename group,typename atype=hila::arithmetic_type<group>>
void measure_wflow_stuff(const GaugeField<group>& V, atype flow_l, atype t_step) {
    // perform measurements on flowed gauge configuration V at flow scale flow_l
    // [t_step is the flow time integration step size used in last gradient flow step]
    // and print results in formatted form to standard output
    static bool first=true;
    if(first) {
        // print legend for flow measurement output
        hila::out0<<"LWFLMEAS   flscale     BP-S-plaq        S-plaq        S-rect    t^2*E_plaq     t^2*E_log     Qtopo_log    t^2*E_clov    Qtopo_clov   [t step size]\n";
        first=false;
    }
    auto plaqbp=measure_plaq_bp(V)/(lattice.volume()*NDIM*(NDIM-1)/2); // average bulk-prevention plaquette action
    auto eplaq=measure_plaq(V)*2.0/lattice.volume();
    auto plaq=eplaq/(NDIM*(NDIM-1)); // average wilson plaquette action
    auto rect=measure_rect12(V)*0.25/(lattice.volume()*NDIM*(NDIM-1)); // average 1-2-rectangle action

    eplaq*=group::size(); // average naive energy density (based on wilson plaquette action)

    // average energy density and toplogical charge from 
    // symmetric log definition of field strength tensor :
    atype qtopolog,elog;
    measure_topo_charge_and_energy_log(V,qtopolog,elog);
    elog/=lattice.volume();

    // average energy density and toplogical charge from 
    // clover definition of field strength tensor :
    atype qtopocl,ecl;
    measure_topo_charge_and_energy_clover(V,qtopocl,ecl);
    ecl/=lattice.volume();

    // print formatted results to standard output :
    hila::out0<<string_format("WFLMEAS  % 9.3f % 0.6e % 0.6e % 0.6e % 0.6e % 0.6e % 0.6e % 0.6e % 0.6e       [%0.5f]",flow_l,plaqbp,plaq,rect,pow(flow_l,4)/64.0*eplaq,pow(flow_l,4)/64.0*elog,qtopolog,pow(flow_l,4)/64.0*ecl,qtopocl,t_step)<<'\n';
}

template <typename group>
void get_wf_force(const GaugeField<group>& U,VectorField<Algebra<group>>& E,const parameters& p) {
    // force computation routine to be used to integrate wilson flow
    //get_force(U,E); // force for Wilson plaquette action
    //get_force_bp(U,E); // froce for bulk-preventing action
    get_force_impr_f2(U,E,p); // force for improved action -S_{impr} = \p.beta/N*( p.c11*\sum_{plaquettes P} ReTr(P) + p.c12*\sum_{1x2-rectangles R}  ReTr(R) )
}

template <typename group,typename atype=hila::arithmetic_type<group>>
atype do_wilson_flow_adapt(GaugeField<group>& V,atype l_start,atype l_end,const parameters& p,atype tstep=0.001) {
    // wilson flow integration from flow scale l_start to l_end using 3rd order
    // 3-step Runge-Kutta (RK3) from arXiv:1006.4518 (cf. appendix C of
    // arXiv:2101.05320 for derivation of this Runge-Kutta method)
    // and embedded RK2 for adaptive step size

    atype atol=p.wflow_a_accu;
    atype rtol=p.wflow_r_accu;

    // translate flow scale interval [l_start,l_end] to corresponding
    // flow time interval [t,tmax] :
    atype t=l_start*l_start/8.0;
    atype tmax=l_end*l_end/8.0;
    atype lstab=0.095; // stability limit
    atype step=min(min(tstep,0.51*(tmax-t)),lstab);  //initial step size

    VectorField<Algebra<group>> k1,k2;
    GaugeField<group> V2,V0;
    Field<atype> reldiff;
    atype maxreldiff,maxstep;

    // RK3 coefficients from arXiv:1006.4518 :
    // correspond to standard RK3 with Butcher-tableau 
    // (cf. arXiv:2101.05320, Appendix C)
    //  0  |   0     0     0
    //  #  |  1/4    0     0
    //  #  | -2/9   8/9    0
    // -------------------------
    //     |  1/4    0    3/4
    //
    atype a11=0.25;
    atype a21=-17.0/36.0,a22=8.0/9.0;
    atype a33=0.75;

    // RK2 coefficients :
    // cf. Alg. 6 and Eqn. (13)-(14) in arXiv:2101.05320 to see
    // how these are obtained from standard RK2 with Butcher-tableau
    //  0  |   0     0
    //  #  |  1/4    0
    // -----------------
    //     |  -1     2
    //
    atype b21=-1.25,b22=2.0;

    V0=V;
    bool stop=false;

    while(t<tmax&&!stop) {

        if(t+step>=tmax) {
            step=tmax-t;
            stop=true;
        } else {
            if(t+2.0*step>=tmax) {
                step=0.51*(tmax-t);
            }
        }

        get_wf_force(V,k1,p);
        foralldir(d) onsites(ALL) {
            // first steps of RK3 and RK2 are the same :
            V[d][X]=chexp(k1[d][X]*(step*a11))*V[d][X];
        }

        get_wf_force(V,k2,p);
        foralldir(d) onsites(ALL) {
            // second step of RK2 :
            V2[d][X]=chexp(k2[d][X]*(step*b22)+k1[d][X]*(step*b21))*V[d][X];

            // second step of RK3 :
            k2[d][X]=k2[d][X]*(step*a22)+k1[d][X]*(step*a21);
            V[d][X]=chexp(k2[d][X])*V[d][X];
        }

        get_wf_force(V,k1,p);
        foralldir(d) onsites(ALL) {
            // third step of RK3 :
            V[d][X]=chexp(k1[d][X]*(step*a33)-k2[d][X])*V[d][X];
        }

        // determine maximum difference between RK3 and RK2, 
        // relative to desired accuracy :
        maxreldiff=0;
        foralldir(d) {
            reldiff[ALL]=(V[d][X]-V2[d][X]).norm()/(atol+rtol*V[d][X].norm());
            maxreldiff=max(maxreldiff,reldiff.max());
        }
        maxreldiff/=(atype)(group::size()*group::size());

        // max. allowed step size to achieve desired accuracy :
        maxstep=min(step/pow(maxreldiff,1.0/3.0),1.0);
        if(step>maxstep) {
            // repeat current iteration if step size was larger than maxstep
            V=V0;
        } else {
            // proceed to next iteration
            t+=step;
            V0=V;
        }
        // adjust step size for next iteration to better match accuracy goal :
        step=min(0.9*maxstep,lstab);
    }

    V.reunitarize_gauge();
    return step;
}


// end Wilson flow functions
///////////////////////////////////////////////////////////////////////////////////
// load/save config functions

template <typename group>
void checkpoint(const GaugeField<group>& U,int trajectory,const parameters& p) {
    double t=hila::gettime();
    // save config
    U.config_write(p.config_file);
    // write run_status file
    if(hila::myrank()==0) {
        std::ofstream outf;
        outf.open("run_status",std::ios::out|std::ios::trunc);
        outf<<"trajectory  "<<trajectory+1<<'\n';
        outf<<"seed        "<<static_cast<uint64_t>(hila::random()*(1UL<<61))<<'\n';
        outf<<"time        "<<hila::gettime()<<'\n';
        outf.close();
    }
    std::stringstream msg;
    msg<<"Checkpointing, time "<<hila::gettime()-t;
    hila::timestamp(msg.str().c_str());
}

template <typename group>
bool restore_checkpoint(GaugeField<group>& U,int& trajectory,parameters& p) {
    uint64_t seed;
    bool ok=true;
    p.time_offset=0;
    hila::input status;
    if(status.open("run_status",false,false)) {
        hila::out0<<"RESTORING FROM CHECKPOINT:\n";
        trajectory=status.get("trajectory");
        seed=status.get("seed");
        p.time_offset=status.get("time");
        status.close();
        hila::seed_random(seed);
        U.config_read(p.config_file);
        ok=true;
    } else {
        std::ifstream in;
        in.open(p.config_file,std::ios::in|std::ios::binary);
        if(in.is_open()) {
            in.close();
            hila::out0<<"READING initial config\n";
            U.config_read(p.config_file);
            ok=true;
        } else {
            ok=false;
        }
    }
    return ok;
}

// end load/save config functions
///////////////////////////////////////////////////////////////////////////////////



int main(int argc,char** argv) {

    // hila::initialize should be called as early as possible
    hila::initialize(argc,argv);

    // hila provides an input class hila::input, which is
    // a convenient way to read in parameters from input files.
    // parameters are presented as key - value pairs, as an example
    //  " lattice size  64, 64, 64, 64"
    // is read below.
    //
    // Values are broadcast to all MPI nodes.
    //
    // .get() -method can read many different input types,
    // see file "input.h" for documentation

    parameters p;

    hila::out0<<"SU("<<mygroup::size()<<") HMC with bulk-prevention\n";

    hila::input par("parameters");

    CoordinateVector lsize;
    // reads NDIM numbers
    lsize=par.get("lattice size");
    // inverse gauge coupling
    p.beta=par.get("beta");
    // HMC step size
    p.dt=par.get("dt");
    // trajectory length in steps
    p.trajlen=par.get("trajectory length");
    // number of trajectories
    p.n_traj=par.get("number of trajectories");
    // number of thermalization trajectories
    p.n_therm=par.get("thermalization trajs");
    // wilson flow frequency (number of traj. between w. flow measurement)
    p.wflow_freq=par.get("wflow freq");
    // wilson flow max. flow-distance
    p.wflow_max_l=par.get("wflow max lambda");
    // wilson flow flow-distance step size
    p.wflow_l_step=par.get("wflow lambda step");
    // wilson flow absolute accuracy (per integration step)
    p.wflow_a_accu=par.get("wflow abs. accuracy");
    // wilson flow relative accuracy (per integration step)
    p.wflow_r_accu=par.get("wflow rel. accuracy");
    // random seed = 0 -> get seed from time
    long seed=par.get("random seed");
    // save config and checkpoint
    p.n_save=par.get("trajs/saved");
    // measure surface properties and print "profile"
    p.config_file=par.get("config name");

    par.close(); // file is closed also when par goes out of scope

    // DBW2 action parameters:
    //p.c12=-1.4088;     // rectangle weight
    //p.c11=1.0-8.0*p.c12; // plaquette weight

    // Iwasaki action parameters:
    p.c12=-0.331;     // rectangle weight
    p.c11=1.0-8.0*p.c12; // plaquette weight

    // LW action parameters:
    //p.c12=-1.0/12.0;     // rectangle weight
    //p.c11=1.0-8.0*p.c12; // plaquette weight

    // Wilson plaquette action parameters:
    //p.c12=0;
    //p.c11=1.0;

    // setting up the lattice is convenient to do after reading
    // the parameter
    lattice.setup(lsize);

    // We need random number here
    hila::seed_random(seed);

    // Alloc gauge field and momenta (E)
    GaugeField<mygroup> U;
    VectorField<Algebra<mygroup>> E;

    int start_traj=0;
    if(!restore_checkpoint(U,start_traj,p)) {
        U=1;
    }

    auto orig_dt=p.dt;
    auto orig_trajlen=p.trajlen;
    GaugeField<mygroup> U_old;
    int nreject=0;
    for(int trajectory=start_traj; trajectory<p.n_traj; ++trajectory) {
        if(trajectory<p.n_therm) {
            // during thermalization: start with 10% of normal step size (and trajectory length)
            // and increse linearly with number of accepted thermalization trajectories. normal
            // step size is reached after 3/4*p.n_therm accepted thermalization trajectories.
            if(trajectory<p.n_therm*3.0/4.0) {
                p.dt=orig_dt*(0.1+0.9*4.0/3.0*trajectory/p.n_therm);
                p.trajlen=orig_trajlen;
            } else {
                p.dt=orig_dt;
                p.trajlen=orig_trajlen;
            }
            if(nreject>1) {
                // if two consecutive thermalization trajectories are rejected, decrese the
                // step size by factor 0.5 (but keeping trajectory length constant, since
                // thermalization becomes inefficient if trajectory length decreases)
                for(int irej=0; irej<nreject-1; ++irej) {
                    p.dt*=0.5;
                    p.trajlen*=2;
                }
                hila::out0<<" thermalization step size(reduzed due to multiple reject) dt="<<std::setprecision(8)<<p.dt<<'\n';
            } else {
                hila::out0<<" thermalization step size dt="<<std::setprecision(8)<<p.dt<<'\n';
            }
        } else if(trajectory==p.n_therm) {
            p.dt=orig_dt;
            p.trajlen=orig_trajlen;
            hila::out0<<" normal stepsize dt="<<std::setprecision(8)<<p.dt<<'\n';
        }

        U_old=U;
        double ttime=hila::gettime();

        foralldir(d) onsites(ALL) E[d][X].gaussian_random();

        auto act_old=measure_action_bp(U,E,p);

        do_trajectory_bp(U,E,p);

        auto act_new=measure_action_bp(U,E,p);


        bool reject=hila::broadcast(exp(act_old-act_new)<hila::random());
        
        if(trajectory<p.n_therm) {
            // during thermalization: keep track of number of rejected trajectories
            if(reject) {
                ++nreject;
                --trajectory;
            } else {
                if(nreject>0) {
                    --nreject;
                }
            }
        }

        hila::out0<<std::setprecision(12)<<"HMC "<<trajectory
            <<(reject?" REJECT":" ACCEPT")<<" start "<<act_old<<" ds "
            <<std::setprecision(6)<<act_new-act_old;

        hila::out0<<" time "<<std::setprecision(3)<<hila::gettime()-ttime<<'\n';
        
        if(reject) {
            U=U_old;
        }

        hila::out0<<"Measure_start "<<trajectory<<'\n';

        measure_stuff(U,E);

        hila::out0<<"Measure_end "<<trajectory<<'\n';

        if(trajectory>=p.n_therm) {

            if(p.wflow_freq>0&&trajectory%p.wflow_freq==0) {
                int wtrajectory=trajectory/p.wflow_freq;
                if(p.wflow_l_step>0) {
                    int nflow_steps=(int)(p.wflow_max_l/p.wflow_l_step);

                    double wtime=hila::gettime();
                    hila::out0<<"Wflow_start "<<wtrajectory<<'\n';

                    GaugeField<mygroup> V=U;
                    ftype t_step=0.001;
                    for(int i=0; i<nflow_steps; ++i) {

                        t_step=do_wilson_flow_adapt(V,i*p.wflow_l_step,(i+1)*p.wflow_l_step,p,t_step);

                        measure_wflow_stuff(V,(i+1)*p.wflow_l_step,t_step);

                    }

                    hila::out0<<"Wflow_end "<<wtrajectory<<"    time "<<std::setprecision(3)<<hila::gettime()-wtime<<'\n';
                }
            }

        }

        if(p.n_save>0&&(trajectory+1)%p.n_save==0) {
            checkpoint(U,trajectory,p);
        }
    }

    hila::finishrun();
}
