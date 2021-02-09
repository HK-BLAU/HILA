#ifndef CUDA_BACKEND
#define CUDA_BACKEND

#include "../defs.h"
#include "../field_storage.h"


/* CUDA implementations */
template<typename T>
void field_storage<T>::allocate_field(lattice_struct * lattice) {
  // Allocate space for the field of the device
  auto status = cudaMalloc( (void **)&fieldbuf, sizeof(T) * lattice->field_alloc_size() );
  check_cuda_error_code(status, "Allocate field memory");
  if (fieldbuf == nullptr) {
    std::cout << "Failure in field memory allocation\n";
  }
  assert(fieldbuf != nullptr);
}

template<typename T>
void field_storage<T>::free_field() {
  if (fieldbuf != nullptr){
    auto status = cudaFree(fieldbuf);
    check_cuda_error_code(status, "Free field memory");
  }
  fieldbuf = nullptr;
}



// Only attempt to compile with CUDA compiler.
// Hilapp will skip these.
#ifdef __CUDACC__


// These are used in device code. Can be called directly in a kernel.
template<typename T> 
__device__ auto field_storage<T>::get(const unsigned i, const unsigned field_alloc_size) const {
  assert( i < field_alloc_size);
  using base_type = typename base_type_struct<T>::type;
  constexpr unsigned n_elements = sizeof(T) / sizeof(base_type);
  T value;
  base_type * value_f = (base_type *)&value;
  base_type * fp = (base_type *)(fieldbuf);
  for (unsigned e=0; e<n_elements; e++) {
    value_f[e] = fp[e*field_alloc_size + i];
  }
  return value;
}


template<typename T>
template<typename A>
__device__ inline void field_storage<T>::set(const A &value, const unsigned i, const unsigned field_alloc_size){
  assert( i < field_alloc_size);
  using base_type = typename base_type_struct<T>::type;
  constexpr unsigned n_elements =sizeof(T) / sizeof(base_type);
  const base_type * value_f = (base_type *)&value;
  base_type * fp = (base_type *)(fieldbuf);
  for (unsigned e=0; e<n_elements; e++) {
    fp[e*field_alloc_size + i] = value_f[e];
  }
}



/// Get a single element from the field outside a loop. Slow, should only be used for setup
template <typename T>
__global__ void get_element_kernel( field_storage<T> field, char *buffer, unsigned i, const unsigned field_alloc_size )
{
  *((T*) buffer) = field.get(i, field_alloc_size);
}


template<typename T>
auto field_storage<T>::get_element( const unsigned i, const lattice_struct * RESTRICT lattice) const {
  char * d_buffer;
  T value;
  
  // Call the kernel to collect the element
  cudaMalloc( (void **)&(d_buffer), sizeof(T));
  get_element_kernel<<< 1, 1 >>>(*this, d_buffer, i, lattice->field_alloc_size() );
  
  // Copy the result to the host
  cudaMemcpy((char *) (&value), d_buffer, sizeof(T), cudaMemcpyDeviceToHost );
  cudaFree(d_buffer);
  return value;
}


/// Set a single element from outside a loop. Slow, should only be used for setup
template <typename T>
__global__ void set_element_kernel( field_storage<T> field, char *buffer, unsigned i, const unsigned field_alloc_size )
{
  field.set( (T*) buffer, i, field_alloc_size);
}

template<typename T>
template<typename A>
void field_storage<T>::set_element(A &value, const unsigned i, const lattice_struct * RESTRICT lattice) {
  char * d_buffer;
  T t_value = value;

  // Allocate space and copy the buffer to the device
  cudaMalloc( (void **)&(d_buffer), sizeof(T));
  cudaMemcpy( d_buffer, (char*) &t_value, sizeof(T), cudaMemcpyHostToDevice );

  // call the kernel to set correct indexes
  set_element_kernel<<< 1, 1 >>>( *this, d_buffer, i, lattice->field_alloc_size() );
  cudaFree(d_buffer);
}





/// A kernel that gathers elements
template <typename T>
__global__ void gather_elements_kernel( field_storage<T> field, T *buffer, unsigned * site_index, const int n, const unsigned field_alloc_size )
{
  unsigned Index = threadIdx.x + blockIdx.x * blockDim.x;
  if( Index < n ) {
    buffer[Index] = field.get(site_index[Index], field_alloc_size);
  }
}

/// CUDA implementation of gather_elements without CUDA aware MPI
template<typename T>
void field_storage<T>::gather_elements( T * RESTRICT buffer, 
                                        const unsigned * RESTRICT index_list, int n,
                                        const lattice_struct * RESTRICT lattice) const {
  unsigned *d_site_index;
  T * d_buffer;
  
  // Copy the list of boundary site indexes to the device
  cudaMalloc( (void **)&(d_site_index), n*sizeof(unsigned));
  cudaMemcpy( d_site_index, index_list, n*sizeof(unsigned), cudaMemcpyHostToDevice );

  // Call the kernel to build the list of elements
  cudaMalloc( (void **)&(d_buffer), n*sizeof(T));
  int N_blocks = n/N_threads + 1;
  gather_elements_kernel<<< N_blocks, N_threads >>>(*this, d_buffer, d_site_index, n, lattice->field_alloc_size() );
  
  // Copy the result to the host
  cudaMemcpy( (char *) buffer, d_buffer, n*sizeof(T), cudaMemcpyDeviceToHost );

  cudaFree(d_site_index);
  cudaFree(d_buffer);
}


/// A kernel that gathers elements negated
// requires unary - 
template <typename T>
__global__ void gather_elements_negated_kernel( field_storage<T> field, T *buffer, unsigned * site_index, 
                                                const int n, const unsigned field_alloc_size )
{
  unsigned Index = threadIdx.x + blockIdx.x * blockDim.x;
  if( Index < n ) {
    buffer[Index] = - field.get(site_index[Index], field_alloc_size);
  }
}

/// CUDA implementation of gather_elements_negated without CUDA aware MPI
// The only difference to the above is the kernel that is called
template<typename T>
void field_storage<T>::gather_elements_negated(T * RESTRICT buffer, 
                                        const unsigned * RESTRICT index_list, int n,
                                        const lattice_struct * RESTRICT lattice) const {
  unsigned *d_site_index;
  T * d_buffer;
  
  if constexpr (!has_unary_minus<T>::value) {
    assert(sizeof(T) < 0 && "Unary 'operatur- ()' must be defined for Field variable for antiperiodic b.c.");
  }

  // Copy the list of boundary site indexes to the device
  cudaMalloc( (void **)&(d_site_index), n*sizeof(unsigned));
  cudaMemcpy( d_site_index, index_list, n*sizeof(unsigned), cudaMemcpyHostToDevice );

  // Call the kernel to build the list of elements
  cudaMalloc( (void **)&(d_buffer), n*sizeof(T));
  int N_blocks = n/N_threads + 1;
  gather_elements_negated_kernel<<< N_blocks, N_threads >>>(*this, d_buffer, d_site_index, n, lattice->field_alloc_size() );
  
  // Copy the result to the host
  cudaMemcpy( buffer, d_buffer, n*sizeof(T), cudaMemcpyDeviceToHost );

  cudaFree(d_site_index);
  cudaFree(d_buffer);
}


template <typename T>
__global__ void gather_comm_elements_kernel( field_storage<T> field, T *buffer, unsigned * site_index, const int n, const unsigned field_alloc_size )
{
  unsigned Index = threadIdx.x + blockIdx.x * blockDim.x;
  if( Index < n ) {
    using base_type = typename base_type_struct<T>::type;
    constexpr unsigned n_elements = sizeof(T) / sizeof(base_type);
    T element = field.get(site_index[Index], field_alloc_size);
    base_type * ep = (base_type *)&element;
    base_type * fp = (base_type *)(buffer);
    for (unsigned e=0; e<n_elements; e++) {
      fp[Index+n*e] = ep[e];
    }
  }
}

template <typename T>
__global__ void gather_comm_elements_negated_kernel( field_storage<T> field, T *buffer, unsigned * site_index, const int n, const unsigned field_alloc_size )
{
  unsigned Index = threadIdx.x + blockIdx.x * blockDim.x;
  if( Index < n ) {
    using base_type = typename base_type_struct<T>::type;
    constexpr unsigned n_elements = sizeof(T) / sizeof(base_type);
    T element = -field.get(site_index[Index], field_alloc_size);
    base_type * ep = (base_type *)&element;
    base_type * fp = (base_type *)(buffer);
    for (unsigned e=0; e<n_elements; e++) {
      fp[Index+n*e] = ep[e];
    }
  }
}

// Index list is constant? Map each cpu pointer to a device pointer and copy just once
struct cuda_comm_node_struct{
  const unsigned *cpu_index;
  unsigned *gpu_index;
  int n;
};

inline unsigned * get_site_index(const lattice_struct::comm_node_struct &to_node, parity par, int &n){
  static std::vector<struct cuda_comm_node_struct> comm_nodes;

  const unsigned * cpu_index = to_node.get_sitelist(par,n);
  for(struct cuda_comm_node_struct comm_node : comm_nodes){
    if(cpu_index == comm_node.cpu_index && n == comm_node.n){
      return comm_node.gpu_index;
    }
  }
  struct cuda_comm_node_struct comm_node;
  comm_node.cpu_index = cpu_index;
  comm_node.n = n;
  cudaMalloc( (void **)&(comm_node.gpu_index), n*sizeof(unsigned));
  cudaMemcpy( comm_node.gpu_index, cpu_index, n*sizeof(unsigned), cudaMemcpyHostToDevice );
  comm_nodes.push_back(comm_node);
  return comm_node.gpu_index;
}


// MPI buffer on the device. Use the gather_elements and gather_elements_negated
// kernels to fill the buffer.
template<typename T>
void field_storage<T>::gather_comm_elements(T * RESTRICT buffer, 
                                            const lattice_struct::comm_node_struct & to_node,
                                            parity par, 
                                            const lattice_struct * RESTRICT lattice,
                                            bool antiperiodic) const {
  int n;
  unsigned *d_site_index = get_site_index(to_node, par, n);
  static T * d_buffer;

  #ifdef CUDA_AWARE_MPI
    // The buffer is already on the device
    d_buffer = buffer;
  #else
    // Allocate a buffer on the device
    cudaMalloc( (void **)&(d_buffer), n*sizeof(T));
  #endif
  
  // Call the kernel to build the list of elements
  int N_blocks = n/N_threads + 1;
  if(antiperiodic){
    gather_comm_elements_negated_kernel<<< N_blocks, N_threads >>>(*this, d_buffer, d_site_index, n, lattice->field_alloc_size() );
  } else {
    gather_comm_elements_kernel<<< N_blocks, N_threads >>>(*this, d_buffer, d_site_index, n, lattice->field_alloc_size() );
  }
  
  #ifndef CUDA_AWARE_MPI
  // Copy the result to the host
  cudaMemcpy( (char *) buffer, d_buffer, n*sizeof(T), cudaMemcpyDeviceToHost );
  cudaFree(d_buffer);
  #endif
}





/// A kernel that scatters the elements
template <typename T>
__global__ void place_elements_kernel( field_storage<T> field, T * buffer, unsigned * site_index, const int n, const unsigned field_alloc_size )
{
  unsigned Index = threadIdx.x + blockIdx.x * blockDim.x;
  if( Index < n ) {
    field.set( buffer[Index], site_index[Index], field_alloc_size);
  }
}

/// CUDA implementation of place_elements without CUDA aware MPI
template<typename T>
void field_storage<T>::place_elements(T * RESTRICT buffer, 
                                      const unsigned * RESTRICT index_list, int n,
                                      const lattice_struct * RESTRICT lattice) {
  unsigned *d_site_index;
  T * d_buffer;

  // Allocate space and copy the buffer to the device
  cudaMalloc( (void **)&(d_buffer), n*sizeof(T));
  cudaMemcpy( d_buffer, buffer, n*sizeof(T), cudaMemcpyHostToDevice );

  // Copy the list of boundary site indexes to the device
  cudaMalloc( (void **)&(d_site_index), n*sizeof(unsigned));
  cudaMemcpy( d_site_index, index_list, n*sizeof(unsigned), cudaMemcpyHostToDevice );

  // Call the kernel to place the elements 
  int N_blocks = n/N_threads + 1;
  place_elements_kernel<<< N_blocks, N_threads >>>( *this, d_buffer, d_site_index, n, lattice->field_alloc_size() );

  cudaFree(d_buffer);
  cudaFree(d_site_index);
}




template <typename T>
__global__ void set_local_boundary_elements_kernel( field_storage<T> field, unsigned offset, unsigned * site_index, const int n, const unsigned field_alloc_size )
{
  unsigned Index = threadIdx.x + blockIdx.x * blockDim.x;
  if( Index < n ) {
    T value;
    value = - field.get(site_index[Index], field_alloc_size);
    field.set( value, offset+Index, field_alloc_size);
  }
}


template<typename T>
void field_storage<T>::set_local_boundary_elements(direction dir, parity par,
                                                   const lattice_struct * RESTRICT lattice,
                                                   bool antiperiodic)
{
  // Only need to do something for antiperiodic boundaries
  if (antiperiodic) {
    unsigned n, start = 0;
    if (par == ODD) {
      n = lattice->special_boundaries[dir].n_odd;
      start = lattice->special_boundaries[dir].n_even;
    } else {
      if (par == EVEN) n = lattice->special_boundaries[dir].n_even;
      else n = lattice->special_boundaries[dir].n_total;
    }
    unsigned offset = lattice->special_boundaries[dir].offset + start;

    unsigned *d_site_index;
    check_cuda_error("earlier");
    cudaMalloc( (void **)(&d_site_index), n*sizeof(unsigned));
    check_cuda_error("set_local_boundary_elements: cudaMalloc");
    cudaMemcpy( d_site_index, lattice->special_boundaries[dir].move_index + start, n*sizeof(unsigned), cudaMemcpyHostToDevice );
    check_cuda_error("set_local_boundary_elements: cudaMemcpy");

    unsigned N_blocks = n/N_threads + 1;
    set_local_boundary_elements_kernel<<< N_blocks, N_threads >>>( *this, offset, 
      d_site_index, n, lattice->field_alloc_size());

    cudaFree(d_site_index);
    check_cuda_error("set_local_boundary_elements: cudaFree");
  }
  
}




// Place communicated elements to the field array
template <typename T>
__global__ void place_comm_elements_kernel( field_storage<T> field, T * buffer, unsigned offset, const int n, const unsigned field_alloc_size )
{
  unsigned Index = threadIdx.x + blockIdx.x * blockDim.x;
  if( Index < n ) {
    using base_type = typename base_type_struct<T>::type;
    constexpr unsigned n_elements = sizeof(T) / sizeof(base_type);
    T element;
    base_type * ep = (base_type *)&element;
    base_type * fp = (base_type *)(buffer);
    for (unsigned e=0; e<n_elements; e++) {
      ep[e] = fp[Index+n*e];
    }
    field.set( element, offset+Index, field_alloc_size);
  }
}

// Standard MPI, buffer is on the cpu and needs to be copied accross
template<typename T>
void field_storage<T>::place_comm_elements(direction d, parity par, T * RESTRICT buffer, 
                                           const lattice_struct::comm_node_struct & from_node, 
                                           const lattice_struct * RESTRICT lattice) {
  unsigned n = from_node.n_sites(par);
  T * d_buffer;

#ifdef CUDA_AWARE_MPI
  // MPI buffer is on device
  d_buffer = buffer;
#else
  // Allocate space and copy the buffer to the device
  cudaMalloc( (void **)&(d_buffer), n*sizeof(T));
  cudaMemcpy( d_buffer, buffer, n*sizeof(T), cudaMemcpyHostToDevice );
#endif

  unsigned N_blocks = n/N_threads + 1;
  place_comm_elements_kernel<<< N_blocks, N_threads >>>((*this), d_buffer, from_node.offset(par), n, lattice->field_alloc_size());

#ifndef CUDA_AWARE_MPI
  cudaFree(d_buffer);
#endif
}


#ifdef CUDA_AWARE_MPI

template <typename T>
void field_storage<T>::free_mpi_buffer( T * d_buffer){
  cudaFree(d_buffer);
}

template <typename T>
T * field_storage<T>::allocate_mpi_buffer( unsigned n ){
  T * d_buffer;
  cudaMalloc((void **)&(d_buffer), n*sizeof(T) );
  return d_buffer;
}

#else

template <typename T>
void field_storage<T>::free_mpi_buffer( T * buffer){
  std::free(buffer);
}

template <typename T>
T * field_storage<T>::allocate_mpi_buffer( unsigned n ){
  return (T *)memalloc( n * sizeof(T) );
}

#endif


#elif defined(HILAPP)

// Hilapp requires a dummy implementation for functions with auto return type
template<typename T> 
auto field_storage<T>::get(const unsigned i, const unsigned field_alloc_size) const {
  T value; return value;
}
template<typename T>
auto field_storage<T>::get_element( const unsigned i, const lattice_struct * RESTRICT lattice) const {
  T value; return value;
}

#endif //__CUDACC__

#endif