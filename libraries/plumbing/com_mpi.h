#ifndef COMM_MPI_H
#define COMM_MPI_H

#include "plumbing/defs.h"

#ifdef USE_MPI

#include "plumbing/lattice.h"

/// let us house the sublattices-struct here
struct sublattices_struct {
    unsigned number, mylattice;
    bool sync;
};

extern sublattices_struct sublattices;


extern hila::timer start_send_timer, wait_send_timer, post_receive_timer,
    wait_receive_timer, synchronize_timer, reduction_timer, reduction_wait_timer,
    broadcast_timer, send_timer, cancel_send_timer, cancel_receive_timer,
    sublattice_sync_timer;

///***********************************************************
/// Implementations of communication routines.
///

// The MPI tag generator
int get_next_msg_tag();

namespace hila {

/// Broadcast template for standard type
template <typename T>
void broadcast(T &var, int rank = 0) {
    static_assert(std::is_trivial<T>::value, "broadcast(var) must use trivial type");
    if (hila::check_input)
        return;

    assert(0 <= rank && rank < hila::number_of_nodes() && "Invalid sender rank in broadcast()");

    broadcast_timer.start();
    MPI_Bcast(&var, sizeof(T), MPI_BYTE, 0, lattice->mpi_comm_lat);
    broadcast_timer.stop();
}

/// Broadcast for std::vector
template <typename T>
void broadcast(std::vector<T> &list, int rank = 0) {

    static_assert(std::is_trivial<T>::value,
                  "broadcast(vector<T>) must have trivial T");

    if (hila::check_input)
        return;

    broadcast_timer.start();

    int size = list.size();
    MPI_Bcast(&size, sizeof(int), MPI_BYTE, rank, lattice->mpi_comm_lat);
    if (hila::myrank() != rank) {
        list.resize(size);
    }

    // move vectors directly to the storage
    MPI_Bcast((void *)list.data(), sizeof(T) * size, MPI_BYTE, rank,
              lattice->mpi_comm_lat);

    broadcast_timer.stop();
}

template <typename T>
void broadcast(T *var) {
    static_assert(sizeof(T) > 0 &&
                  "Do not use pointers to broadcast()-function. Use 'broadcast(T* arr, "
                  "int size)' to broadcast an array");
}

/// Broadcast for arrays where size must be known and same for all nodes
template <typename T>
void broadcast_array(T *var, int n, int rank = 0) {

    if (hila::check_input)
        return;

    broadcast_timer.start();
    MPI_Bcast((void *)var, sizeof(T) * n, MPI_BYTE, rank, lattice->mpi_comm_lat);
    broadcast_timer.stop();
}

// DO string bcasts separately
void broadcast(std::string &r,int rank = 0);
void broadcast(std::vector<std::string> &l, int rank = 0);

/// and broadcast with two values
template <typename T, typename U>
void broadcast(T &t, U &u, int rank = 0) {

    if (hila::check_input)
        return;

    struct {
        T tv;
        U uv;
    } s = {t, u};

    hila::broadcast(s,rank);
    t = s.tv;
    u = s.uv;
}

} // namespace hila

// try to get the basic data type of the message
// this is just to enable a bit larger messages
template <typename T>
MPI_Datatype get_MPI_number_type(int &size, bool with_int = false) {

    if (std::is_same<hila::number_type<T>, int>::value) {
        size = sizeof(int);
        return with_int ? MPI_2INT : MPI_INT;
    } else if (std::is_same<hila::number_type<T>, unsigned>::value) {
        size = sizeof(unsigned);
        return with_int ? MPI_2INT : MPI_UNSIGNED;  // MPI does not contain MPI_UNSIGNED_INT
    } else if (std::is_same<hila::number_type<T>, long>::value) {
        size = sizeof(long);
        return with_int ? MPI_LONG_INT : MPI_LONG;
    } else if (std::is_same<hila::number_type<T>, int64_t>::value) {
        size = sizeof(int64_t);
        return with_int ? MPI_LONG_INT : MPI_INT64_T;   // need to use LONG_INT 
    } else if (std::is_same<hila::number_type<T>, uint64_t>::value) {
        size = sizeof(uint64_t);
        return with_int ? MPI_LONG_INT : MPI_UINT64_T;  // ditto
    } else if (std::is_same<hila::number_type<T>, float>::value) {
        size = sizeof(float);
        return with_int ? MPI_FLOAT_INT : MPI_FLOAT;
    } else if (std::is_same<hila::number_type<T>, double>::value) {
        size = sizeof(double);
        return with_int ? MPI_DOUBLE_INT : MPI_DOUBLE;
    } else if (std::is_same<hila::number_type<T>, long double>::value) {
        size = sizeof(long double);
        return with_int ? MPI_LONG_DOUBLE_INT : MPI_LONG_DOUBLE;
    }

    size = 1;
    return MPI_BYTE;
}

// Reduction templates

template <typename T>
void lattice_struct::reduce_node_sum(T *value, int N, bool distribute) {
    T work[N];
    MPI_Datatype dtype;
    int size;

    dtype = get_MPI_number_type<T>(size);

    reduction_timer.start();
    if (distribute) {
        MPI_Allreduce((void *)value, (void *)work,
                      N * sizeof(T) / sizeof(hila::number_type<T>), dtype, MPI_SUM,
                      lattice->mpi_comm_lat);
        for (int i = 0; i < N; i++)
            value[i] = work[i];
    } else {
        MPI_Reduce((void *)value, (void *)work,
                   N * sizeof(T) / sizeof(hila::number_type<T>), dtype, MPI_SUM, 0,
                   lattice->mpi_comm_lat);
        if (hila::myrank() == 0)
            for (int i = 0; i < N; i++)
                value[i] = work[i];
    }
    reduction_timer.stop();
}

// Product reduction template - so far only for int, float, dbl

template <typename T>
void lattice_struct::reduce_node_product(T *value, int N, bool distribute) {
    T work[N];
    MPI_Datatype dtype;

    if (std::is_same<T, int>::value) {
        dtype = MPI_INT;
    } else if (std::is_same<T, float>::value) {
        dtype = MPI_FLOAT;
    } else if (std::is_same<T, double>::value) {
        dtype = MPI_DOUBLE;
    } else {
        static_assert(sizeof(T) > 0, "Unknown number_type in reduce_node_product");
    }

    reduction_timer.start();
    if (distribute) {
        MPI_Allreduce((void *)value, (void *)work, N, dtype, MPI_PROD,
                      lattice->mpi_comm_lat);
        for (int i = 0; i < N; i++)
            value[i] = work[i];
    } else {
        MPI_Reduce((void *)value, (void *)work, N, dtype, MPI_PROD, 0,
                   lattice->mpi_comm_lat);
        if (hila::myrank() == 0)
            for (int i = 0; i < N; i++)
                value[i] = work[i];
    }
    reduction_timer.stop();
}

void hila_reduce_double_setup(double *d, int n);
void hila_reduce_float_setup(float *d, int n);
void hila_reduce_sums();

namespace hila {
void set_allreduce(bool on = true);
bool get_allreduce();
} // namespace hila

template <typename T>
void lattice_struct::reduce_sum_setup(T *value) {

    using b_t = hila::number_type<T>;
    if (std::is_same<b_t, double>::value) {
        hila_reduce_double_setup((double *)value, sizeof(T) / sizeof(double));
    } else if (std::is_same<b_t, float>::value) {
        hila_reduce_float_setup((float *)value, sizeof(T) / sizeof(float));
    } else {
        reduce_node_sum(value, 1, hila::get_allreduce());
    }
}


#endif // USE_MPI

#endif // COMM_MPI_H
