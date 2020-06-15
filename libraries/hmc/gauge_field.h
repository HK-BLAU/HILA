#ifndef GAUGE_FIELD_H
#define GAUGE_FIELD_H





/// Calculate the Polyakov loop for a given gauge field.
template<int N>
double polyakov_loop(direction dir, field<SU<N>> (&gauge)[NDIM]){
  // This implementation uses the onsites() to cycle through the
  // NDIM-1 dimensional planes. This is probably not the most
  // efficient implementation.
  coordinate_vector vol = lattice->size();
  field<SU<N>> polyakov; polyakov[ALL] = 1;
  for(int t=0; t<vol[dir]; t++){
    onsites(ALL){
      if(X.coordinates()[dir]==(t+1)%vol[dir]){
        polyakov[X] = polyakov[X] * gauge[dir][X-dir];
      }
    }
  }

  double poly=0;
  onsites(ALL){
    if(X.coordinates()[dir]==0){
      poly += polyakov[X].trace().re;
    }
  }

  double v3 = lattice->volume()/vol[dir];
  return poly/(N*v3);
}





/// Calculate the sum of staples connected to links in direction dir 
template<typename SUN>
field<SUN> calc_staples(field<SUN> (&U)[NDIM], direction dir)
{
  field<SUN> staple_sum;
  static field<SUN> down_staple;
  staple_sum[ALL] = 0;
  foralldir(dir2){
    //Calculate the down side staple.
    //This will be communicated up.
    down_staple[ALL] = U[dir2][X+dir].conjugate()
                     * U[dir][X].conjugate()
                     * U[dir2][X];
    // Forward staple
    staple_sum[ALL]  = staple_sum[X]
                     + U[dir2][X+dir]
                     * U[dir][X+dir2].conjugate()
                     * U[dir2][X].conjugate();
    // Add the down staple
    staple_sum[ALL] = staple_sum[X] + down_staple[X-dir2];
  }
  return staple_sum;
}




/// Measure the plaquette
template<int N, typename radix>
double plaquette_sum(field<SU<N,radix>> *U){
  double Plaq=0;
  foralldir(dir1) foralldir(dir2) if(dir2 < dir1){
    onsites(ALL){
      element<SU<N,radix>> temp;
      temp = U[dir1][X] * U[dir2][X+dir1]
           * U[dir1][X+dir2].conjugate()
           * U[dir2][X].conjugate();
      Plaq += 1-temp.trace().re/N;
    }
  }
  return Plaq;
}

template<int N, typename radix>
double plaquette_sum(field<squarematrix<N,radix>> *U){
  double Plaq=0;
  foralldir(dir1) foralldir(dir2) if(dir2 < dir1){
    onsites(ALL){
      element<SU<N,radix>> temp;
      temp = U[dir1][X] * U[dir2][X+dir1]
           * U[dir1][X+dir2].conjugate()
           * U[dir2][X].conjugate();
      Plaq += 1-temp.trace()/N;
    }
  }
  return Plaq;
}


template<typename SUN>
double plaquette(field<SUN> *gauge){
  return plaquette_sum(gauge)/(lattice->volume()*NDIM*(NDIM-1)/2);
}










// A conveniance class for a gauge field.
// Contains an SU(N) matrix in each direction for the
// gauge field and for the momentum
template<int N,typename radix=double>
struct gauge_field {
  using gauge_type = SU<N,radix>;
  field<SU<N,double>> gauge[NDIM];
  field<SU<N,double>> momentum[NDIM];
  field<SU<N,double>> gauge_backup[NDIM];

  // Set the gauge field to unity
  void set_unity(){
    foralldir(dir){
      onsites(ALL){
        gauge[dir][X] = 1;
      }
    }
  }

  /// Gaussian random momentum for each element
  void draw_momentum(){
    foralldir(dir) {
      onsites(ALL){
        if(disable_avx[X]==0){};
        momentum[dir][X].gaussian_algebra();
      }
    }
  }

  void gauge_update(double eps){
    foralldir(dir){
      onsites(ALL){
        element<SU<N,double>> momexp = eps*momentum[dir][X];
        momexp.exp();
        gauge[dir][X] = momexp*gauge[dir][X];
      }
    }
  }

  void add_momentum(field<SU<N,double>> (&force)[NDIM]){
    foralldir(dir){
      onsites(ALL){
        project_antihermitean(force[dir][X]);
        momentum[dir][X] = momentum[dir][X] + force[dir][X];
      }
    }
  }

  // Make a copy of fields updated in a trajectory
  void backup(){
    foralldir(dir) gauge_backup[dir] = gauge[dir];
  }

  // Restore the previous backup
  void restore_backup(){
    foralldir(dir) gauge[dir] = gauge_backup[dir];
  }

  // Read the gauge field from a file
  void read_file(std::string filename){
    std::ifstream inputfile;
    inputfile.open(filename, std::ios::in | std::ios::binary);
    foralldir(dir){
      read_fields(inputfile, gauge[dir]);
    }
    inputfile.close();
  }

  // Write the gauge field to a file
  void write_file(std::string filename){
    std::ofstream outputfile;
    outputfile.open(filename, std::ios::out | std::ios::trunc | std::ios::binary);
    foralldir(dir){
      write_fields(outputfile, gauge[dir]);
    }
    outputfile.close();
  }


};


template<typename repr>
struct represented_gauge_field {
  using gauge_type = repr;
  using fund_type = typename repr::sun;
  using basetype = typename repr::base_type;
  static constexpr int Nf = fund_type::size;
  static constexpr int Nr = repr::size;

  gauge_field<Nf,basetype> &fundamental;

  field<repr> gauge[NDIM];

  represented_gauge_field(gauge_field<Nf,basetype>  &f) : fundamental(f){}
  represented_gauge_field(represented_gauge_field &r)
    : represented_gauge_field(r){}



  void represent(){
    foralldir(dir){
      onsites(ALL){
        if(disable_avx[X]==0){};
        gauge[dir][X].represent(fundamental.gauge[dir][X]);
      }
    }
  }

  void add_momentum(field<squarematrix<Nf,basetype>> (&force)[NDIM]){
    foralldir(dir){
      onsites(ALL){
        project_antihermitean(force[dir][X]);
        element<fund_type> fforce;
        fforce = repr::project_force(force[dir][X]);
        fundamental.momentum[dir][X] = fundamental.momentum[dir][X] + fforce[dir][X];
      }
    }
  }

  /// This gets called if there is a represented gauge action term.
  /// If there is also a fundamental term, it gets called twice... 
  void draw_momentum(){
    fundamental.draw_momentum();
  }

  // Make a backup of the fundamental gauge field
  // Again, this may get called twice.
  void backup(){
    fundamental.backup();
  }

  // Restore the previous backup
  void restore_backup(){
    fundamental.restore_backup();
  }

};






template<typename gauge_field>
class gauge_action {
  public:
    using gauge_mat = typename gauge_field::gauge_type;
    static constexpr int N = gauge_mat::size;

    gauge_field &gauge;
    field<gauge_mat> gauge_copy[NDIM];
    double beta;

    gauge_action(gauge_field &g, double b) 
    : gauge(g), beta(b){}

    gauge_action(gauge_action &ga)
    : gauge(ga.gauge), beta(ga.beta) {}

    //The gauge action
    double action(){
      double Sa = 0;
      double Sg = beta*plaquette_sum(gauge.gauge);
      foralldir(dir) {
        onsites(ALL){
          Sa += gauge.momentum[dir][X].algebra_norm();
        }
      }
      return Sg+Sa;
    }

    /// Gaussian random momentum for each element
    void draw_gaussian_fields(){
      gauge.draw_momentum();
    }

    // Update the momentum with the gauge field
    void force_step(double eps){
      field<gauge_mat> force[NDIM];
      foralldir(dir){
        force[dir] = calc_staples(gauge.gauge, dir);
        onsites(ALL){
          force[dir][X] = (-beta*eps/N)*gauge.gauge[dir][X]*force[dir][X];
        }
      }
      gauge.add_momentum(force);
    }

    // Draw a random gauge field
    void random(){
      foralldir(dir){
        onsites(ALL){
          gauge.gauge[dir][X].random();
        }
      }
    }


    // Called by HMC.
    // Make a copy of fields updated in a trajectory
    void backup_fields(){
      gauge.backup();
    }

    // Restore the previous backup
    void restore_backup(){
      gauge.restore_backup();
    }

    // Momentum step and step are required for an integrator.
    // A gauge action is also the lowest level of an
    // integrator hierarchy.
    
    // Update the gauge field with momentum
    void momentum_step(double eps){
      gauge.gauge_update(eps);
    }

    // A single gauge update
    void step(double eps){
      O2_step(*this, eps);
    }
};


// Represents a sum of two action terms. Useful for adding them
// to an integrator on the same level.
template<typename action_type_1, typename action_type_2>
class action_sum {
  public:
    action_type_1 a1;
    action_type_2 a2;

    action_sum(action_type_1 _a1, action_type_2 _a2) 
    : a1(_a1), a2(_a2){}

    action_sum(action_sum &asum) : a1(asum.a1), a2(asum.a2){}

    //The gauge action
    double action(){
      return a1.action() + a2.action();
    }

    /// Gaussian random momentum for each element
    void draw_gaussian_fields(){
      a1.draw_gaussian_fields();
      a2.draw_gaussian_fields();
    }

    // Update the momentum with the gauge field
    void force_step(double eps){
      a1.force_step(eps);
      a2.force_step(eps);
    }

    // Make a copy of fields updated in a trajectory
    void backup_fields(){
      a1.backup_fields();
      a2.backup_fields();
    }

    // Restore the previous backup
    void restore_backup(){
      a1.restore_backup();
      a2.restore_backup();
    }
};


// Sum operator for creating an action_sum object
template<typename gauge_field_1, typename gauge_field_2>
action_sum<gauge_action<gauge_field_1>, gauge_action<gauge_field_2>> operator+(gauge_action<gauge_field_1> a1, gauge_action<gauge_field_2> a2){
  action_sum<gauge_action<gauge_field_1>, gauge_action<gauge_field_2>> sum(a1, a2);
  return sum;
}



/// Define an integration step for a Molecular Dynamics
/// trajectory.
template<typename action_type, typename lower_integrator_type>
class integrator{
  public:
    action_type action_term;
    lower_integrator_type lower_integrator;

    integrator(action_type a, lower_integrator_type i)
    : action_term(a), lower_integrator(i) {}

    // The current total action of fields updated by this
    // integrator. This is kept constant up to order eps^3.
    double action(){
      return action_term.action() + lower_integrator.action();
    }

    // Refresh fields that can be drawn from a gaussian distribution
    // This is needed at the beginning of a trajectory
    void draw_gaussian_fields(){
      action_term.draw_gaussian_fields();
      lower_integrator.draw_gaussian_fields();
    }

    // Make a copy of fields updated in a trajectory
    void backup_fields(){
      action_term.backup_fields();
      lower_integrator.backup_fields();
    }

    // Restore the previous backup
    void restore_backup(){
      action_term.restore_backup();
      lower_integrator.restore_backup();
    }


    // Update the momentum with the gauge field
    void force_step(double eps){
      action_term.force_step(eps);
    }

    // Update the gauge field with momentum
    void momentum_step(double eps){
      lower_integrator.step(eps);
    }

    // A single gauge update
    void step(double eps){
      O2_step(*this, eps);
    }

};







#endif