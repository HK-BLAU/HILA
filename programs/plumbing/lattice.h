#ifndef LATTICE_H
#define LATTICE_H

#include <iostream>
#include <fstream>
#include <array>
#include <vector>


// TODO: assertion moved somewhere where basic params
#undef NDEBUG
#include <assert.h>
#include "../plumbing/defs.h"
#include "../plumbing/inputs.h"

struct node_info {
  coordinate_vector min,size;
  unsigned evensites, oddsites;
};

/* Some backends need specialized lattice data
 * in loops. Forward declaration here and
 * implementations in backend headers.
 * Loops generated by Transformer can access
 * this through lattice->backend_lattice.
 */
struct backend_lattice_struct;






class lattice_struct {
private:
  // expose these directly, by far the simplest interface - who cares about c++ practices
  // use also ints instead of unsigned, just to avoid surprises in arithmetics
  // I shall assume here that int is 32 bits, and long long 64 bits.  I guess these are
  // pretty much standard for now
  // Alternative: int_32t and int_64t (or int_fast_32t  and int_fast_64t, even more generally) 
  int l_size[NDIM];
  long long l_volume;

  // Information about the node stored on this process
  struct node_struct {
    unsigned index;
    unsigned sites, evensites, oddsites;
    unsigned field_alloc_size;          // how many sites/node in allocations 
    coordinate_vector min, size;                 // node local coordinate ranges
    unsigned nn[NDIRS];                 // nn-node of node down/up to dirs
    bool first_site_even;               // is location min even or odd?
    std::vector<coordinate_vector> coordinates;

    void setup(node_info & ni, lattice_struct & lattice);
  } this_node;

  // information about all nodes
  struct allnodes {
    unsigned number;
    unsigned ndir[NDIM];  // number of node divisions to dir
    // lattice division: div[d] will have num_dir[d]+1 elements, last size
    // TODO: is this needed at all?
    std::vector<unsigned> divisors[NDIM];
    std::vector<node_info> nodelist;

    unsigned * map_array;                  // mapping (optional)
    unsigned * map_inverse;                // inv of it
    
    void create_remap();                   // create remap_node
    unsigned remap(unsigned i);            // use remap
    unsigned inverse_remap(unsigned i);    // inverse remap
    
  } nodes;

public:

  struct comm_node_struct {
    unsigned index;
    unsigned sites, evensites, oddsites;
    unsigned buffer;
    std::vector<unsigned>  sitelist;

    // The number of sites that need to be communicated
    unsigned n_sites(parity par){
      if(par == ALL){
        return sites;
      } else if(par == EVEN){
        return evensites;
      } else {
        return oddsites;
      }
    }

    // The local index of a site that is sent to neighbour
    unsigned site_index(int site, parity par){
      if(par == ODD){
        return sitelist[evensites+site];
      } else {
        return sitelist[site];
      }
    }

    // The offset of the halo from the start of the field array
    unsigned offset(parity par){
      if(par == ODD){
        return buffer + evensites;
      } else {
        return buffer;
      }
    }
  };

  struct comminfo_struct {
    int label;
    unsigned * index;
    std::vector<comm_node_struct> from_node;
    std::vector<comm_node_struct> to_node;
  };

  std::vector<comminfo_struct> comminfo;

  unsigned * neighb[NDIRS];
  unsigned char *wait_arr_;

  backend_lattice_struct *backend_lattice;

  void setup(int siz[NDIM], int &argc, char **argv);
  void setup(input & inputs);
  void setup_layout();
  void setup_nodes();
  
  #if NDIM == 4
  void setup(int nx, int ny, int nz, int nt, int &argc, char **argv);
  #elif NDIM == 3  
  void setup(int nx, int ny, int nz, int &argc, char **argv);
  #elif NDIM == 2
  void setup(int nx, int ny, int &argc, char **argv);
  #elif NDIM == 1
  void setup(int nx, int &argc, char **argv); 
  #endif


  void teardown();

  int size(direction d) { return l_size[d]; }
  int size(int d) { return l_size[d]; }
  int local_size(int d) { return this_node.size[d]; }
  long long volume() { return l_volume; }
  int node_number() { return this_node.index; }
  int n_nodes() { return nodes.number; }
  long long local_volume() {return this_node.sites;}
  
  bool is_on_node(const coordinate_vector & c);
  unsigned node_number(const coordinate_vector & c);
  unsigned site_index(const coordinate_vector & c);
  unsigned site_index(const coordinate_vector & c, const unsigned node);
  coordinate_vector site_location(unsigned index);
  unsigned field_alloc_size() {return this_node.field_alloc_size; }
  void create_std_gathers();
  
  bool first_site_even() { return this_node.first_site_even; };

  unsigned remap_node(const unsigned i);
  
  #ifdef EVENFIRST
  int loop_begin( parity P) const {
    if(P==ODD){
      return this_node.evensites;
    } else {
      return 0;
    }
  }
  int loop_end( parity P) const {
    if(P==EVEN){
      return this_node.evensites;
    } else {
      return this_node.sites;
    }
  }
  #else
  int loop_begin( parity P) const {
    if(P==EVEN){
      return this_node.evensites;
    } else {
      return 0;
    }
  }
  int loop_end( parity P) const {
    if(P==ODD){
      return this_node.evensites;
    } else {
      return this_node.sites;
    }
  }
  #endif

  coordinate_vector coordinates( unsigned idx ){
    return site_location(idx);
  }

  coordinate_vector local_coordinates( unsigned idx ){
    coordinate_vector l = site_location(idx);
    foralldir(d)
      l[d] = l[d] - this_node.min[d];
    return l;
  }

  lattice_struct::comminfo_struct get_comminfo(int d){
    return comminfo[d];
  }

  /* MPI functions and variables. Define here in lattice? */
  void initialize_wait_arrays();
  #ifdef USE_MPI
  MPI_Comm mpi_comm_lat;
  #endif

  template <typename T>
  void reduce_node_sum(T & value, bool distribute);

  template <typename T>
  void reduce_node_product(T & value, bool distribute);

  // Guarantee 64 bits for these - 32 can overflow!
  unsigned long long n_gather_done = 0, n_gather_avoided = 0;

};

/// global handle to lattice
extern lattice_struct * lattice;


// Keep track of defined lattices
extern std::vector<lattice_struct*> lattices;





#ifdef VANILLA
#include "backend_cpu/lattice.h"
#elif defined(CUDA)
#include "backend_cuda/lattice.h"
#elif defined(VECTORIZED)
#include "backend_vector/lattice.h"
#endif




#endif
