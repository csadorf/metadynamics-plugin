#include "OrderParameterMeshGPU.cuh"

__constant__ Scalar sinc_coeff[6];

const Scalar coeff[] = {Scalar(1.0), Scalar(-1.0/6.0), Scalar(1.0/120.0), Scalar(-1.0/5040.0),
                        Scalar(1.0/362880.0), Scalar(-1.0/39916800.0)};

/*! \param x Distance on mesh in units of the mesh size
 */
__device__ Scalar assignTSC(Scalar x)
    {
    Scalar xsq = x*x;
    Scalar xabs = sqrtf(xsq);

    if (xsq <= Scalar(1.0/4.0))
        return Scalar(3.0/4.0) - xsq;
    else if (xsq <= Scalar(9.0/4.0))
        return Scalar(1.0/2.0)*(Scalar(3.0/2.0)-xabs)*(Scalar(3.0/2.0)-xabs);
    else
        return Scalar(0.0);
    }

/*! \param k Wave vector times mesh size
 * \returns the Fourier transformed TSC assignment function
 */
__device__ Scalar assignTSCFourier(Scalar k)
    {
    Scalar fac = 0;

    if (k*k <= Scalar(1.0))
        {
        Scalar term = Scalar(1.0);
        for (unsigned int i = 0; i < 6; ++i)
            {
            fac += sinc_coeff[i] * term;
            term *= k*k/Scalar(4.0);
            }
        }
    else
        {
        fac = Scalar(2.0)*sinf(k*Scalar(1.0/2.0))/k;
        }

    return fac*fac*fac;
    }


texture<unsigned int, 1, cudaReadModeElementType> cadj_tex;

//! Assignment of particles to mesh using three-point scheme (triangular shaped cloud)
/*! This is a second order accurate scheme with continuous value and continuous derivative
 */
__global__ void gpu_assign_particles_kernel(const unsigned int N,
                                       const Scalar4 *d_postype,
                                       cufftComplex *d_mesh,
                                       const Index3D mesh_idx,
                                       const Scalar *d_mode,
                                       const unsigned int *d_cadj,
                                       const Index2D cadji,
                                       const BoxDim box)
    {
    unsigned int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= N) return;

    uint3 dim = make_uint3(mesh_idx.getW(), mesh_idx.getH(), mesh_idx.getD());

    Scalar V_cell = box.getVolume()/(Scalar)mesh_idx.getNumElements();
 
    // inverse dimensions
    Scalar3 dim_inv = make_scalar3(Scalar(1.0)/(Scalar)dim.x,
                                   Scalar(1.0)/(Scalar)dim.y,
                                   Scalar(1.0)/(Scalar)dim.z);

    Scalar4 postype = d_postype[idx];

    Scalar3 pos = make_scalar3(postype.x, postype.y, postype.z);
    unsigned int type = __float_as_int(postype.w);
    Scalar mode = d_mode[type];

    // compute coordinates in units of the mesh size
    Scalar3 f = box.makeFraction(pos);
    Scalar3 reduced_pos = make_scalar3(f.x * (Scalar) dim.x,
                                       f.y * (Scalar) dim.y,
                                       f.z * (Scalar) dim.z);

    // find cell the particle is in
    unsigned int ix = reduced_pos.x;
    unsigned int iy = reduced_pos.y;
    unsigned int iz = reduced_pos.z;

    // handle particles on the boundary
    if (ix == mesh_idx.getW())
        ix = 0;
    if (iy == mesh_idx.getH())
        iy = 0;
    if (iz == mesh_idx.getD()) 
        iz = 0;

    // center of cell (in units of the mesh size)
    unsigned int my_cell = mesh_idx(ix,iy,iz);

    // assign particle to cell and next neighbors
    for (unsigned int k = 0; k < cadji.getW(); ++k)
        {
        unsigned int neigh_cell = tex1Dfetch(cadj_tex,cadji(k, my_cell));
        uint3 cell_coord = mesh_idx.getTriple(neigh_cell); 

        Scalar3 neigh_frac = make_scalar3((Scalar) cell_coord.x + Scalar(0.5),
                                         (Scalar) cell_coord.y + Scalar(0.5),
                                         (Scalar) cell_coord.z + Scalar(0.5));
       
        // coordinates of the neighboring cell between 0..1 
        Scalar3 neigh_frac_box = neigh_frac * dim_inv;
        Scalar3 neigh_pos = box.makeCoordinates(neigh_frac_box);

        // compute distance between particle and cell center in fractional coordinates using minimum image
        Scalar3 dx = box.minImage(neigh_pos - pos);
        Scalar3 dx_frac_box = box.makeFraction(dx) - make_scalar3(0.5,0.5,0.5);
        Scalar3 dx_frac = dx_frac_box*make_scalar3(dim.x,dim.y,dim.z);

        // compute fraction of particle density assigned to cell
        Scalar density_fraction = assignTSC(dx_frac.x)*assignTSC(dx_frac.y)*assignTSC(dx_frac.z)/V_cell;
        unsigned int cell_idx = cell_coord.z + dim.z * (cell_coord.y + dim.y * cell_coord.x);

        d_mesh[cell_idx].x += mode*density_fraction;
        }
         
    }

void gpu_assign_particles(const unsigned int N,
                          const Scalar4 *d_postype,
                          cufftComplex *d_mesh,
                          const Index3D& mesh_idx,
                          const Scalar *d_mode,
                          const unsigned int *d_cadj,
                          const Index2D& cadji,
                          const BoxDim& box)
    {

    cadj_tex.normalized = false;
    cadj_tex.filterMode = cudaFilterModePoint;
    cudaBindTexture(0, cadj_tex, d_cadj, sizeof(unsigned int)*cadji.getNumElements());

    unsigned int block_size = 512;

    gpu_assign_particles_kernel<<<N/block_size+1, block_size>>>(N,
                                                                d_postype,
                                                                d_mesh,
                                                                mesh_idx,
                                                                d_mode,
                                                                d_cadj,
                                                                cadji,
                                                                box);
    }

__global__ void gpu_update_meshes_kernel(const unsigned int n_wave_vectors,
                                         cufftComplex *d_fourier_mesh,
                                         cufftComplex *d_fourier_mesh_G,
                                         const Scalar *d_inf_f,
                                         const Scalar3 *d_k,
                                         const Scalar V_cell,
                                         cufftComplex *d_fourier_mesh_x,
                                         cufftComplex *d_fourier_mesh_y,
                                         cufftComplex *d_fourier_mesh_z)
    {
    unsigned int k = blockDim.x * blockIdx.x + threadIdx.x;

    if (k >= n_wave_vectors) return;

    d_fourier_mesh[k].x *= V_cell;
    d_fourier_mesh[k].y *= V_cell;

    cufftComplex fourier_G;
    fourier_G.x =d_fourier_mesh[k].x * d_inf_f[k];
    fourier_G.y =d_fourier_mesh[k].y * d_inf_f[k];

    Scalar3 kval = d_k[k];
    d_fourier_mesh_x[k].x = -fourier_G.y*kval.x;
    d_fourier_mesh_x[k].y = fourier_G.x*kval.x;

    d_fourier_mesh_y[k].x = -fourier_G.y*kval.y;
    d_fourier_mesh_y[k].y = fourier_G.x*kval.y;

    d_fourier_mesh_z[k].x = -fourier_G.y*kval.z;
    d_fourier_mesh_z[k].y = fourier_G.x*kval.z;

    d_fourier_mesh_G[k] = fourier_G;
    }

void gpu_update_meshes(const unsigned int n_wave_vectors,
                         cufftComplex *d_fourier_mesh,
                         cufftComplex *d_fourier_mesh_G,
                         const Scalar *d_inf_f,
                         const Scalar3 *d_k,
                         const Scalar V_cell,
                         cufftComplex *d_fourier_mesh_x,
                         cufftComplex *d_fourier_mesh_y,
                         cufftComplex *d_fourier_mesh_z)
    {
    const unsigned int block_size = 512;

    gpu_update_meshes_kernel<<<n_wave_vectors/block_size+1, block_size>>>(n_wave_vectors,
                                                                          d_fourier_mesh,
                                                                          d_fourier_mesh_G,
                                                                          d_inf_f,
                                                                          d_k,
                                                                          V_cell,
                                                                          d_fourier_mesh_x,
                                                                          d_fourier_mesh_y,
                                                                          d_fourier_mesh_z);
    }

//! Texture for reading particle positions
texture<Scalar4, 1, cudaReadModeElementType> force_mesh_tex;

__global__ void gpu_coalesce_forces_kernel(const unsigned int n_wave_vectors,
                                       const cufftComplex *d_force_mesh_x,
                                       const cufftComplex *d_force_mesh_y,
                                       const cufftComplex *d_force_mesh_z,
                                       Scalar4 *d_force_mesh)
    {
    unsigned int k = blockIdx.x*blockDim.x+threadIdx.x;

    if (k >= n_wave_vectors) return;

    d_force_mesh[k] = make_scalar4(d_force_mesh_x[k].x,
                                   d_force_mesh_y[k].x,
                                   d_force_mesh_z[k].x,
                                   0.0);
    }

__global__ void gpu_interpolate_forces_kernel(const unsigned int N,
                                       const Scalar4 *d_postype,
                                       Scalar4 *d_force,
                                       const Scalar bias,
                                       const Index3D mesh_idx,
                                       const Scalar *d_mode,
                                       const unsigned int *d_cadj,
                                       const Index2D cadji,
                                       const BoxDim box)
    {
    unsigned int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= N) return;

    Scalar V = box.getVolume();
 
    uint3 dim = make_uint3(mesh_idx.getW(), mesh_idx.getH(), mesh_idx.getD());

    // inverse dimensions
    Scalar3 dim_inv = make_scalar3(Scalar(1.0)/(Scalar)mesh_idx.getW(),
                                   Scalar(1.0)/(Scalar)mesh_idx.getH(),
                                   Scalar(1.0)/(Scalar)mesh_idx.getD());

    Scalar4 postype = d_postype[idx];

    Scalar3 pos = make_scalar3(postype.x, postype.y, postype.z);
    unsigned int type = __float_as_int(postype.w);
    Scalar mode = d_mode[type];

    // compute coordinates in units of the mesh size
    Scalar3 f = box.makeFraction(pos);
    Scalar3 reduced_pos = make_scalar3(f.x * (Scalar) dim.x,
                                       f.y * (Scalar) dim.y,
                                       f.z * (Scalar) dim.z);

    // find cell the particle is in
    unsigned int ix = reduced_pos.x;
    unsigned int iy = reduced_pos.y;
    unsigned int iz = reduced_pos.z;

    // handle particles on the boundary
    if (ix == mesh_idx.getW())
        ix = 0;
    if (iy == mesh_idx.getH())
        iy = 0;
    if (iz == mesh_idx.getD()) 
        iz = 0;

    // center of cell (in units of the mesh size)
    unsigned int my_cell = mesh_idx(ix,iy,iz);

    Scalar3 force = make_scalar3(0.0,0.0,0.0);

    // assign particle to cell and next neighbors
    for (unsigned int k = 0; k < cadji.getW(); ++k)
        {
        unsigned int neigh_cell = tex1Dfetch(cadj_tex,cadji(k, my_cell));
        uint3 cell_coord = mesh_idx.getTriple(neigh_cell); 


        Scalar3 neigh_frac = make_scalar3((Scalar) cell_coord.x + Scalar(0.5),
                                         (Scalar) cell_coord.y + Scalar(0.5),
                                         (Scalar) cell_coord.z + Scalar(0.5));
       
        // coordinates of the neighboring cell between 0..1 
        Scalar3 neigh_frac_box = neigh_frac * dim_inv;
        Scalar3 neigh_pos = box.makeCoordinates(neigh_frac_box);

        // compute distance between particle and cell center in fractional coordinates using minimum image
        Scalar3 dx = box.minImage(neigh_pos - pos);
        Scalar3 dx_frac_box = box.makeFraction(dx) - make_scalar3(0.5,0.5,0.5);
        Scalar3 dx_frac = dx_frac_box*make_scalar3(dim.x,dim.y,dim.z);

        unsigned int cell_idx = cell_coord.z + dim.z * (cell_coord.y + dim.y * cell_coord.x);

        Scalar4 mesh_force = tex1Dfetch(force_mesh_tex,cell_idx);

        force += -assignTSC(dx_frac.x)*assignTSC(dx_frac.y)*assignTSC(dx_frac.z)*mode*
                 make_scalar3(mesh_force.x,mesh_force.y,mesh_force.z);
        }  

    // Multiply with bias potential derivative
    force *= bias/V;

    d_force[idx] = make_scalar4(force.x,force.y,force.z,0.0);
    }

void gpu_interpolate_forces(const unsigned int N,
                             const Scalar4 *d_postype,
                             Scalar4 *d_force,
                             const Scalar bias,
                             const cufftComplex *d_force_mesh_x,
                             const cufftComplex *d_force_mesh_y,
                             const cufftComplex *d_force_mesh_z,
                             Scalar4 *d_force_mesh,
                             const Index3D& mesh_idx,
                             const Scalar *d_mode,
                             const unsigned int *d_cadj,
                             const Index2D& cadji,
                             const BoxDim& box)
    {
    const unsigned int block_size = 512;

    unsigned int num_cells = mesh_idx.getNumElements();

    gpu_coalesce_forces_kernel<<<num_cells/block_size+1, block_size>>>(num_cells,
                                                                 d_force_mesh_x,
                                                                 d_force_mesh_y,
                                                                 d_force_mesh_z,
                                                                 d_force_mesh);
    cadj_tex.normalized = false;
    cadj_tex.filterMode = cudaFilterModePoint;
    cudaBindTexture(0, cadj_tex, d_cadj, sizeof(unsigned int)*cadji.getNumElements());

    force_mesh_tex.normalized = false;
    force_mesh_tex.filterMode = cudaFilterModePoint;
    cudaBindTexture(0, force_mesh_tex, d_force_mesh, sizeof(Scalar4)*num_cells);

    gpu_interpolate_forces_kernel<<<N/block_size+1,block_size>>>(N,
                                                                 d_postype,
                                                                 d_force,
                                                                 bias,
                                                                 mesh_idx,
                                                                 d_mode,
                                                                 d_cadj,
                                                                 cadji,
                                                                 box);
    }

__global__ void kernel_calculate_cv_partial(
            int n_wave_vectors,
            Scalar *sum_partial,
            const cufftComplex *d_fourier_mesh,
            const cufftComplex *d_fourier_mesh_G,
            const Scalar *d_E_self)
    {
    extern __shared__ Scalar sdata[];

    unsigned int tidx = threadIdx.x;

    int j = blockIdx.x * blockDim.x + threadIdx.x;

    Scalar mySum = Scalar(0.0);

    if (j < n_wave_vectors) {
        mySum = d_fourier_mesh[j].x * d_fourier_mesh_G[j].x + d_fourier_mesh[j].y * d_fourier_mesh_G[j].y - d_E_self[j];
        }
    sdata[tidx] = mySum;

   __syncthreads();

    // reduce the sum
    int offs = blockDim.x >> 1;
    while (offs > 0)
        {
        if (tidx < offs)
            {
            sdata[tidx] += sdata[tidx + offs];
            }
        offs >>= 1;
        __syncthreads();
        }

    // write result to global memeory
    if (tidx == 0)
       sum_partial[blockIdx.x] = sdata[0];
    }

__global__ void kernel_final_reduce_cv(Scalar* sum_partial,
                                       unsigned int nblocks,
                                       Scalar *sum)
    {
    extern __shared__ Scalar smem[];

    if (threadIdx.x == 0)
       *sum = Scalar(0.0);

    for (int start = 0; start< nblocks; start += blockDim.x)
        {
        __syncthreads();
        if (start + threadIdx.x < nblocks)
            smem[threadIdx.x] = sum_partial[start + threadIdx.x];
        else
            smem[threadIdx.x] = Scalar(0.0);

        __syncthreads();

        // reduce the sum
        int offs = blockDim.x >> 1;
        while (offs > 0)
            {
            if (threadIdx.x < offs)
                smem[threadIdx.x] += smem[threadIdx.x + offs];
            offs >>= 1;
            __syncthreads();
            }

         if (threadIdx.x == 0)
            {
            *sum += smem[0];
            }
        }
    }

void gpu_compute_cv(unsigned int n_wave_vectors,
                           Scalar *d_sum_partial,
                           Scalar *d_sum,
                           const cufftComplex *d_fourier_mesh,
                           const cufftComplex *d_fourier_mesh_G,
                           const Scalar *d_E_self,
                           const unsigned int block_size)
    {
    unsigned int n_blocks = n_wave_vectors/block_size + 1;

    unsigned int shared_size = block_size * sizeof(Scalar);
    kernel_calculate_cv_partial<<<n_blocks, block_size, shared_size>>>(
               n_wave_vectors,
               d_sum_partial,
               d_fourier_mesh,
               d_fourier_mesh_G,
               d_E_self);

    // calculate final S(q) values 
    const unsigned int final_block_size = 512;
    shared_size = final_block_size*sizeof(Scalar);
    kernel_final_reduce_cv<<<1, final_block_size,shared_size>>>(d_sum_partial,
                                                                  n_blocks,
                                                                  d_sum);
    }

__global__ void gpu_compute_influence_function_kernel(const Index3D mesh_idx,
                                          const unsigned int N,
                                          Scalar *d_inf_f,
                                          Scalar3 *d_k,
                                          Scalar *d_E_self,
                                          const Scalar3 b1,
                                          const Scalar3 b2,
                                          const Scalar3 b3,
                                          const BoxDim box,
                                          const Scalar qstarsq)
    {
    int l = blockIdx.x * blockDim.x + threadIdx.x;
    int m = blockIdx.y * blockDim.y + threadIdx.y;
    int n = blockIdx.z * blockDim.z + threadIdx.z;

    uint3 dim = make_uint3(mesh_idx.getW(), mesh_idx.getH(), mesh_idx.getD());
    if (l >= dim.x/2+1 || m >= dim.y/2+1 || m >= dim.z/2+1) return;

    // maximal integer indices of inner loop
    const int maxnx = 2;
    const int maxny = 2;
    const int maxnz = 2;

    Scalar3 dim_inv = make_scalar3(Scalar(1.0)/(Scalar)dim.x,
                                   Scalar(1.0)/(Scalar)dim.y,
                                   Scalar(1.0)/(Scalar)dim.z);

    Scalar V_box = box.getVolume();


    Scalar3 kval = (Scalar)l*b1+(Scalar)m*b2+(Scalar)n*b3;
    Scalar ksq = dot(kval,kval);

    // accumulate self energy
    Scalar E_self = V_box*exp(-ksq/qstarsq*Scalar(1.0/2.0))/(Scalar) N;

    Scalar3 UsqR = make_scalar3(0.0,0.0,0.0);
    Scalar Usq = Scalar(0.0);

    for (int nx = -maxnx; nx <= maxnx; ++nx)
        for (int ny = -maxny; ny <= maxny; ++ny)
            for (int nz = -maxnz; nz <= maxnz; ++nz)
                {
                Scalar3 kn = kval + (Scalar)dim.x*(Scalar)nx*b1+
                               + (Scalar)dim.y*(Scalar)ny*b2+
                               + (Scalar)dim.z*(Scalar)nz*b3;
                Scalar3 knH = Scalar(2.0*M_PI)*(make_scalar3(l,m,n)*dim_inv+make_scalar3(nx,ny,nz));
                Scalar U = assignTSCFourier(knH.x)*assignTSCFourier(knH.y)*assignTSCFourier(knH.z);
                Scalar knsq = dot(kn,kn);
                UsqR += U*U*kn*exp(-knsq/qstarsq*Scalar(1.0/2.0))/(Scalar)N/(Scalar)N*V_box;
                Usq += U*U;
                }

    Scalar num = dot(kval,UsqR);
    Scalar3 kH = Scalar(2.0*M_PI)*make_scalar3(l,m,n)*dim_inv;

    Scalar denom = ksq*Usq*Usq;

    for (int i = 0; i < 2; ++i)
        {
        l *= -1;
        for (int j = 0; j < 2; ++j)
            {
            m *= -1;
            for (int k = 0; k < 2; ++k)
                {
                n *= -1;

                // determine cell idx
                unsigned int ix, iy, iz;
                if (l < 0)
                    ix = l + dim.x;
                else
                    ix = l;

                if (m < 0)
                    iy = m + dim.y;
                else
                    iy = m;

                if (n < 0)
                    iz = n + dim.z;
                else
                    iz = n;
                
                unsigned int cell_idx = iz + dim.z * (iy + dim.y * ix);

                if ((l != 0) || (m != 0) || (n!=0))
                    d_inf_f[cell_idx] = num/denom;
                else
                    // avoid divide by zero
                    d_inf_f[cell_idx] = Scalar(1.0)/(Scalar)N/(Scalar)N*V_box;

                kval = (Scalar)l*b1+(Scalar)m*b2+(Scalar)n*b3;
                d_k[cell_idx] = kval;

                d_E_self[cell_idx] = E_self;
                }
            }
        }
    }

void gpu_compute_influence_function(const Index3D& mesh_idx,
                                    const unsigned int N,
                                    Scalar *d_inf_f,
                                    Scalar3 *d_k,
                                    Scalar *d_E_self,
                                    const BoxDim& box,
                                    const Scalar qstarsq)
    { 
    cudaMemcpyToSymbol(sinc_coeff,coeff,6*sizeof(Scalar));

    // compute reciprocal lattice vectors
    Scalar3 a1 = box.getLatticeVector(0);
    Scalar3 a2 = box.getLatticeVector(1);
    Scalar3 a3 = box.getLatticeVector(2);

    Scalar V_box = box.getVolume();
    Scalar3 b1 = Scalar(2.0*M_PI)*make_scalar3(a2.y*a3.z-a2.z*a3.y, a2.z*a3.x-a2.x*a3.z, a2.x*a3.y-a2.y*a3.x)/V_box;
    Scalar3 b2 = Scalar(2.0*M_PI)*make_scalar3(a3.y*a1.z-a3.z*a1.y, a3.z*a1.x-a3.x*a1.z, a3.x*a1.y-a3.y*a1.x)/V_box;
    Scalar3 b3 = Scalar(2.0*M_PI)*make_scalar3(a1.y*a2.z-a1.z*a2.y, a1.z*a2.x-a1.x*a2.z, a1.x*a2.y-a1.y*a2.x)/V_box;

    unsigned int block_size = 8;
    
    dim3 blockDim(block_size,block_size,block_size);
    dim3 gridDim((mesh_idx.getW()/2+1)/block_size+1,
                 (mesh_idx.getH()/2+1)/block_size+1,
                 (mesh_idx.getD()/2+1)/block_size+1);
    gpu_compute_influence_function_kernel<<<gridDim,blockDim>>>(mesh_idx,
                                                                N,
                                                                d_inf_f,
                                                                d_k,
                                                                d_E_self,
                                                                b1,
                                                                b2,
                                                                b3,
                                                                box,
                                                                qstarsq);
    } 