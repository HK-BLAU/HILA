#ifndef LATTICE_H
#define LATTICE_H

#include <sstream>
#include <iostream>
#include <fstream>
#include <array>
#include <vector>

// SUBNODE_LAYOUT is now defined in main.mk
// #define SUBNODE_LAYOUT

// TODO: assertion moved somewhere where basic params
#undef NDEBUG
#include <assert.h>
#include "plumbing/defs.h"
#include "plumbing/coordinates.h"
#include "plumbing/timing.h"

#ifdef SUBNODE_LAYOUT
#ifndef VECTOR_SIZE
#ifdef CUDA
#define VECTOR_SIZE 8 // Size of float, length 1 vectors
#else
#define VECTOR_SIZE (256 / 8) // this is for AVX2
#endif
#endif
// This is the vector size used to determine the layout
constexpr unsigned number_of_subnodes = VECTOR_SIZE / sizeof(float);
#endif

/// list boundary conditions - used only if SPECIAL_BOUNDARY_CONDITIONS defined
enum class BoundaryCondition { PERIODIC, ANTIPERIODIC, FIXED };

void test_std_gathers();
void report_too_large_node(); // report on too large node

/// useful information about a node
struct node_info {
    CoordinateVector min, size;
    unsigned evensites, oddsites;
};

/// Some backends need specialized lattice data
/// in loops. Forward declaration here and
/// implementations in backend headers.
/// Loops generated by Transformer can access
/// this through lattice->backend_lattice.
struct backend_lattice_struct;

/// The lattice struct defines the lattice geometry ans sets up MPI communication patterns
class lattice_struct {
  private:
    // Use ints instead of unsigned, just to avoid surprises in arithmetics
    // I shall assume here that int is 32 bits, and int64_t 64 bits.
    CoordinateVector l_size;
    size_t l_volume;

  public:
    /// Information about the node stored on this process
    struct node_struct {
        int rank; // rank of this node
        size_t sites, evensites, oddsites;
        size_t field_alloc_size;    // how many sites/node in allocations
        CoordinateVector min, size; // node local coordinate ranges
        unsigned nn[NDIRS];         // nn-node of node down/up to dirs
        bool first_site_even;       // is location min even or odd?
        std::vector<CoordinateVector> coordinates;

        void setup(node_info &ni, lattice_struct &lattice);

#ifdef SUBNODE_LAYOUT
        /// If we have vectorized-style layout, we introduce "subnodes"
        /// size is mynode.size/subnodes.divisions, which is not
        /// constant across nodes
        struct subnode_struct {
            CoordinateVector divisions, size; // div to subnodes to directions, size
            size_t sites, evensites, oddsites;
            Direction merged_subnodes_dir;

            void setup(const node_struct &tn);
        } subnodes;
#endif

        unsigned volume() { return sites; }

    } mynode;

    /// information about all nodes
    struct allnodes {
        int number;                 // number of nodes
        unsigned n_divisions[NDIM]; // number of node divisions to dir
        // lattice division: div[d] will have num_dir[d]+1 elements, last size
        // TODO: is this needed at all?
        std::vector<unsigned> divisors[NDIM];
        CoordinateVector max_size; // size of largest node

        std::vector<node_info> nodelist;

        unsigned *RESTRICT map_array;   // mapping (optional)
        unsigned *RESTRICT map_inverse; // inv of it

        void create_remap();                // create remap_node
        unsigned remap(unsigned i);         // use remap
        unsigned inverse_remap(unsigned i); // inverse remap

    } nodes;

    /// Information necessary to communicate with a node
    struct comm_node_struct {
        unsigned rank; // rank of communicated with node
        size_t sites, evensites, oddsites;
        size_t buffer; // offset from the start of field array
        unsigned *sitelist;

        // Get a vector containing the sites of parity par and number of elements
        const unsigned *RESTRICT get_sitelist(Parity par, int &size) const {
            if (par == ALL) {
                size = sites;
                return sitelist;
            } else if (par == EVEN) {
                size = evensites;
                return sitelist;
            } else {
                size = oddsites;
                return sitelist + evensites;
            }
        }

        // The number of sites that need to be communicated
        unsigned n_sites(Parity par) const {
            if (par == ALL) {
                return sites;
            } else if (par == EVEN) {
                return evensites;
            } else {
                return oddsites;
            }
        }

        // The local index of a site that is sent to neighbour
        unsigned site_index(int site, Parity par) const {
            if (par == ODD) {
                return sitelist[evensites + site];
            } else {
                return sitelist[site];
            }
        }

        // The offset of the halo from the start of the field array
        unsigned offset(Parity par) const {
            if (par == ODD) {
                return buffer + evensites;
            } else {
                return buffer;
            }
        }
    };

    /// nn-communication has only 1 node to talk to
    struct nn_comminfo_struct {
        unsigned *index;
        comm_node_struct from_node, to_node;
        unsigned receive_buf_size; // only for general gathers
    };

    /// general communication
    struct gen_comminfo_struct {
        unsigned *index;
        std::vector<comm_node_struct> from_node;
        std::vector<comm_node_struct> to_node;
        size_t receive_buf_size;
    };

    /// nearest neighbour comminfo struct
    std::array<nn_comminfo_struct, NDIRS> nn_comminfo;

    /// Main neighbour index array
    unsigned *RESTRICT neighb[NDIRS];

    /// implement waiting using mask_t - unsigned char is good for up to 4 dim.
    dir_mask_t *RESTRICT wait_arr_;

#ifdef SPECIAL_BOUNDARY_CONDITIONS
    /// special boundary pointers are needed only in cases neighbour
    /// pointers must be modified (new halo elements). That is known only during runtime.
    /// is_on_edget is the only "general" info element here, true if the node to Direction
    /// dir is on lattice edge.
    struct special_boundary_struct {
        unsigned *neighbours;
        unsigned *move_index;
        size_t offset, n_even, n_odd, n_total;
        bool is_needed;
        bool is_on_edge;
    };
    // holder for nb ptr info
    special_boundary_struct special_boundaries[NDIRS];
#endif

#if 0
// mpi_column not needed any more
#ifdef USE_MPI
    /// Information about the column of nodes to a given directions
    /// (nodes that share all coordinates except 1)
    struct mpi_column_struct {
        std::vector<int> nodelist;
        MPI_Comm column_communicator = nullptr;
        int my_column_rank;
        bool init = true;
    };

    /// Get an MPI column in Direction dir, build if necessary
    static mpi_column_struct get_mpi_column(Direction dir);
#endif
#endif

#ifndef VANILLA
    backend_lattice_struct *backend_lattice;
#endif

    void setup(const CoordinateVector & siz);
    void setup_layout();
    void setup_nodes();

    void teardown();

    // Std accessors:
    // volume
    int64_t volume() const { return l_volume; }

    // size routines
    int size(Direction d) const { return l_size[d]; }
    int size(int d) const { return l_size[d]; }
    CoordinateVector size() const { return l_size; }

    CoordinateVector mod_size(const CoordinateVector &v) const { return mod(v, l_size); }

    int node_rank() const { return mynode.rank; }
    int n_nodes() const { return nodes.number; }
    // std::vector<node_info> nodelist() { return nodes.nodelist; }
    // CoordinateVector min_coordinate() const { return mynode.min; }
    // int min_coordinate(Direction d) const { return mynode.min[d]; }

    bool is_on_mynode(const CoordinateVector &c);
    int node_rank(const CoordinateVector &c);
    unsigned site_index(const CoordinateVector &c);
    unsigned site_index(const CoordinateVector &c, const unsigned node);
    unsigned field_alloc_size() const { return mynode.field_alloc_size; }

    void create_std_gathers();
    gen_comminfo_struct create_general_gather(const CoordinateVector &r);
    std::vector<comm_node_struct> create_comm_node_vector(CoordinateVector offset,
                                                          unsigned *index, bool receive);

    bool first_site_even() { return mynode.first_site_even; };

#ifdef SPECIAL_BOUNDARY_CONDITIONS
    void init_special_boundaries();
    void setup_special_boundary_array(Direction d);

    const unsigned *get_neighbour_array(Direction d, BoundaryCondition bc);
#else
    const unsigned *get_neighbour_array(Direction d, BoundaryCondition bc) {
        return neighb[d];
    }
#endif

    unsigned remap_node(const unsigned i);

#ifdef EVEN_SITES_FIRST
    int loop_begin(Parity P) const {
        if (P == ODD) {
            return mynode.evensites;
        } else {
            return 0;
        }
    }
    int loop_end(Parity P) const {
        if (P == EVEN) {
            return mynode.evensites;
        } else {
            return mynode.sites;
        }
    }
#else

    int loop_begin(Parity P) const {
        if (P == EVEN) {
            return mynode.evensites;
        } else {
            return 0;
        }
    }
    int loop_end(Parity P) const {
        if (P == ODD) {
            return mynode.evensites;
        } else {
            return mynode.sites;
        }
    }
#endif

    inline const CoordinateVector &coordinates(unsigned idx) const {
        return mynode.coordinates[idx];
    }

    inline int coordinate(unsigned idx, Direction d) const {
        return mynode.coordinates[idx][d];
    }

    inline Parity site_parity(unsigned idx) const {
#ifdef EVEN_SITES_FIRST
        if (idx < mynode.evensites)
            return EVEN;
        else
            return ODD;
#else
        return coordinates(idx).parity();
#endif
    }

    CoordinateVector local_coordinates(unsigned idx) const {
        return coordinates(idx) - mynode.min;
    }

    lattice_struct::nn_comminfo_struct get_comminfo(int d) { return nn_comminfo[d]; }

    /* MPI functions and variables. Define here in lattice? */
    void initialize_wait_arrays();

#ifdef USE_MPI
    MPI_Comm mpi_comm_lat;

    // Guarantee 64 bits for these - 32 can overflow!
    int64_t n_gather_done = 0, n_gather_avoided = 0;

    template <typename T> 
    void reduce_node_sum(T *value, int N, bool distribute = true);
    
    template <typename T>
    void reduce_node_product(T *value, int N, bool distribute = true);

#else

    // define to nothing
    template <typename T> void reduce_node_sum(T *value, int N, bool distribute) {}
    template <typename T> void reduce_node_product(T *value, int N, bool distribute) {}

#endif

    // simple reduce_node_sum for single variable
    template <typename T> T reduce_node_sum(T &value, bool distribute = true) {
        if (!hila::check_input) 
            reduce_node_sum(&value, 1, distribute);
        return value;
    }
};

/// global handle to lattice
extern lattice_struct *lattice;

// Keep track of defined lattices
extern std::vector<lattice_struct *> lattices;

// and the MPI tag generator
int get_next_msg_tag();

/// let us house the sublattices-struct here
struct sublattices_struct {
    unsigned number, mylattice;
    bool sync;
};

extern sublattices_struct sublattices;

#ifdef VANILLA
#include "plumbing/backend_cpu/lattice.h"
#elif defined(CUDA)
#include "plumbing/backend_cuda/lattice.h"
#elif defined(VECTORIZED)
#include "plumbing/backend_vector/lattice_vector.h"
#endif

#endif
