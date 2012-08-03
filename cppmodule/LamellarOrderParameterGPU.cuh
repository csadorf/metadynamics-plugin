/*! \file LamellarOrderParameterGPU.cuh
 *  \brief Defines the GPU routines for LamellarOrderParameterGPU
 */
#include <hoomd/hoomd_config.h>
#include <hoomd/BoxDim.h>

/*! Calculates the fourier modes for the collective variable

    \param n_wave Number of modes
    \param d_wave_vectors Device array of wave vectors
    \param n_particles Number of particles
    \param d_postype Device array of particle positions and types
    \param d_mode Device array of per-type mode coefficients
    \param d_fourier_modes The fourier modes (Output device array)
    \param d_phases Device array of per-mode phase shifts

    \returns the CUDA status
 */
cudaError_t gpu_calculate_fourier_modes(unsigned int n_wave,
                                 Scalar3 *d_wave_vectors,
                                 unsigned int n_particles,
                                 Scalar4 *d_postype,
                                 Scalar *d_mode,
                                 Scalar2 *d_fourier_modes,
                                 Scalar *d_phases);

/*! Calculates the negative derivative of the collective variable with
    respect to particle positions
  
    \param N Number of particles
    \param d_postype Array of particle positions and types
    \param d_force Array of per-particle forces
    \param d_virial Array of virial matrix elements per-particle
    \param n_wave Number of modes
    \param d_wave_vectors Device array of wave vectors
    \param d_mode Device array of per-type mode coefficients
    \param global_box Dimensions of the global simulation box
    \param bias The bias factor to multiply the forces with
    \param d_phases Device array of per-mode phase shifts

    \returns the CUDA status
*/
cudaError_t gpu_compute_sq_forces(unsigned int N,
                                  Scalar4 *d_postype,
                                  Scalar4 *d_force,
                                  Scalar *d_virial,
                                  unsigned int n_wave,
                                  Scalar3 *d_wave_vectors,
                                  Scalar *d_mode,
                                  const BoxDim global_box,
                                  Scalar bias,
                                  Scalar *d_phases);