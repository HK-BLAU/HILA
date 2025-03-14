#include <cmath>
#include "hila.h"
#include "plumbing/random.h"

/////////////////////////////////////////////////////////////////////////

#include <random>

// #ifndef OPENMP

// static variable which holds the random state
// Use 64-bit mersenne twister
static std::mt19937_64 mersenne_twister_gen;

// random numbers are in interval [0,1)
static std::uniform_real_distribution<double> real_rnd_dist(0.0, 1.0);

// #endif


// In GPU code hila::random() defined in hila_gpu.cpp
#if !defined(CUDA) && !defined(HIP)
double hila::random() {
    return real_rnd_dist(mersenne_twister_gen);
}

#endif

// Generate random number in non-kernel (non-loop) code.  Not meant to
// be used in "user code"
double hila::host_random() {
    return real_rnd_dist(mersenne_twister_gen);
}

/////////////////////////////////////////////////////////////////////////

static bool rng_is_initialized = false;

namespace hila {

/**
 * @brief Random shuffling of rng seed for MPI nodes.
 * @details Do it in a manner makes it difficult to give the same seed by mistake
 * and also avoids giving the same seed for 2 nodes
 * For single MPI node seed remains unchanged
 */
uint64_t shuffle_rng_seed(uint64_t seed) {

    uint64_t n = hila::myrank();
    if (hila::partitions.number() > 1)
        n += hila::partitions.mylattice() * hila::number_of_nodes();

    return (seed + n) ^ (n << 31);
}


/**
 *@param seed unsigned int_64
 *@note On MPI, this function shuffles different seed values for all MPI ranks.
 */
void initialize_host_rng(uint64_t seed) {

    seed = hila::shuffle_rng_seed(seed);

    // #if !defined(OPENMP)
    mersenne_twister_gen.seed(seed);
    // warm it up
    for (int i = 0; i < 9000; i++)
        mersenne_twister_gen();
    // #endif
}

} // namespace hila


/**
 *@details The optional 2nd argument indicates whether to initialize the RNG on GPU device:
 * `hila::device_rng_on` (default) or `hila::device_rng_off`.  This argument does nothing if no GPU
 * platform.  If `hila::device_rng_off` is used, `onsites()` -loops cannot contain random number
 *calls (Runtime error will be flagged and program exits).
 *
 * Seed is shuffled so that different nodes
 * get different rng seeds.  If `seed == 0`,
 * seed is generated through using the `time()` -function.
 */
void hila::seed_random(uint64_t seed, bool device_init) {

    rng_is_initialized = true;

    if (!lattice.is_initialized()) {
        hila::error("lattice.initialize() must be called before hila::seed_random()");
    }


    uint64_t n = hila::myrank();
    if (hila::partitions.number() > 1)
        n += hila::partitions.mylattice() * hila::number_of_nodes();

    if (seed == 0) {
        // get seed from time
        if (hila::myrank() == 0) {
            struct timespec tp;

            clock_gettime(CLOCK_MONOTONIC, &tp);
            seed = tp.tv_sec;
            seed = (seed << 30) ^ tp.tv_nsec;
            hila::out0 << "Random seed from time: " << seed << '\n';
        }
        hila::broadcast(seed);
    }

    if (hila::partitions.number() > 1)
        seed = seed ^ ((static_cast<uint64_t>(hila::partitions.mylattice())) << 28);

#ifndef SITERAND

    hila::out0 << "Using node random numbers, seed for node 0: " << seed << std::endl;

    hila::initialize_host_rng(seed);

#if defined(CUDA) || defined(HIP)

    // we can use the same seed, the generator is different
    if (device_init) {
        hila::initialize_device_rng(seed);
    } else {
        hila::out0 << "Not initializing GPU random numbers\n";
    }

#endif

    // taus_initialize();

#else

    // TODO: SITERAND is not yet implemented!
    // This is usually used only for occasional benchmarking, where identical output
    // independent of the node number is desired

    hila::out0 << "*** SITERAND is in use!\n";

    random_seed_arr = (unsigned short(*)[3])memalloc(3 * node.sites * sizeof(unsigned short));
    forallsites(i) {
        random_seed_arr[i][0] = (unsigned short)(seed + site[i].index);
        random_seed_arr[i][1] = (unsigned short)(seed + 2 * site[i].index);
        random_seed_arr[i][2] = (unsigned short)(seed + 3 * site[i].index);
    }

    random_seed_ptr = random_seed_arr[0];

#endif
}

////////////////////////////////////////////////////////////////////
// Def here gpu rng functions for non-gpu
////////////////////////////////////////////////////////////////////

#if !(defined(CUDA) || defined(HIP))

/**
 *@details `hila::random()` does not work inside `onsites()` after this,
 *unless seeded again using `initialize_device_rng()`. Frees the memory RNG takes on the device.
 */
void hila::free_device_rng() {}

/**
 *@details Returns `true` on non-GPU archs.
 */
bool hila::is_device_rng_on() {
    return true;
}

/**
 *@details This function shuffles the seed for different MPI ranks on MPI.  Called by
 *`seed_random()` unless its 2nd argument is `hila::device_rng_off`. This can reinitialize device
 *RNG free'd by `free_device_rng()`.
 */
void hila::initialize_device_rng(uint64_t seed) {}

#endif


#define VARIANCE 1.0

double hila::gaussrand2(double &out2) {

    double phi, urnd, r;
    phi = 2.0 * M_PI * hila::random();

    // this should not really trigger
    do {
        urnd = 1.0 - hila::random();
    } while (urnd == 0.0);

    r = sqrt(-::log(urnd) * (2.0 * VARIANCE));
    out2 = r * cos(phi);
    return r * sin(phi);
}

#if !defined(CUDA) && !defined(HIP)

/**
 * @details By default these gives random numbers with variance \f$1.0\f$ and expectation value
 * \f$0.0\f$, i.e. \f[ e^{-(\frac{x^{2}}{2})} \f] with variance \f[ < x^{2}-0.0> = 1 \f]
 *
 * If you want random numbers with variance \f$ \sigma^{2} \f$, multiply the
 * result by \f$ \sqrt{\sigma^{2}} \f$ i.e.,
 * \code {.cpp}
 *       sqrt(sigma * sigma) * gaussrand();
 * \endcode
 *
 * @return double
 */
double hila::gaussrand() {
    static double second;
    static bool draw_new = true;
    if (draw_new) {
        draw_new = false;
        return hila::gaussrand2(second);
    }
    draw_new = true;
    return second;
}

#else

// Cuda and other stuff which does not accept
// static variables - just throw away another gaussian number.

// #pragma hila loop function contains rng
double hila::gaussrand() {
    double second;
    return hila::gaussrand2(second);
}

#endif

/**
 *@returns bool
 */
bool hila::is_rng_seeded() {
    return rng_is_initialized;
}


///////////////////////////////////////////////////////////////
// RNG initialization check - emitted on loops
///////////////////////////////////////////////////////////////

/**
 *@details program quits with error message if RNG is not initialized.
 *It also quit with error messages if the device RNG is not initialized.
 */
void hila::check_that_rng_is_initialized() {
    bool isinit;

    if (!rng_is_initialized) {
        hila::out0 << "ERROR: trying to use random numbers without initializing the generator"
                   << std::endl;
        hila::terminate(1);
    }
#if defined(CUDA) || defined(HIP)
    if (!hila::is_device_rng_on()) {
        hila::out0 << "ERROR: GPU random number generator is not initialized and onsites()-loop is "
                      "using random numbers"
                   << std::endl;
        hila::terminate(1);
    }
#endif
}
