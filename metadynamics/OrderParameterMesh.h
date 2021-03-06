#ifndef __ORDER_PARAMETER_MESH_H__
#define __ORDER_PARAMETER_MESH_H__

#include "CollectiveVariable.h"

#include <hoomd/md/CommunicatorGrid.h>

#ifdef ENABLE_MPI
#include <hoomd/extern/dfftlib/src/dfft_host.h>
#endif

#include <hoomd/extern/kiss_fftnd.h>

/*! Order parameter evaluated using the particle mesh method
 */
class OrderParameterMesh : public CollectiveVariable
    {
    public:
        //! Constructor
        OrderParameterMesh(std::shared_ptr<SystemDefinition> sysdef,
                           const unsigned int nx,
                           const unsigned int ny,
                           const unsigned int nz,
                           const std::vector<Scalar> mode,
                           const std::vector<int3> zero_modes = std::vector<int3>());
        virtual ~OrderParameterMesh();

        Scalar getCurrentValue(unsigned int timestep);

        /*! Returns the names of provided log quantities.
         */
        std::vector<std::string> getProvidedLogQuantities()
            {
            std::vector<std::string> list = CollectiveVariable::getProvidedLogQuantities();
            for (std::vector<std::string>::iterator it = m_log_names.begin(); it != m_log_names.end(); ++it)
                {
                list.push_back(*it);
                }
            return list;
            }

        /*! Returns the value of a specific log quantity.
         * \param quantity The name of the quantity to return the value of
         * \param timestep The current value of the time step
         */
        Scalar getLogValue(const std::string& quantity, unsigned int timestep);

        /*! Set the convolution kernel table
         * \param K convolution kernel as function of k
         * \param d_K derivative of convolution kernel
         * \param kmin Minimum wave vector for table
         * \param kmax Maximum wave vector for table
         */
        void setTable(const std::vector<Scalar> &K,
                      const std::vector<Scalar> &d_K,
                      Scalar kmin, Scalar kmax);

        /*! Set flag whether to use a convolution kernel table
         */
        void setUseTable(bool use_table)
            {
            m_use_table = use_table;
            }

    protected:
        /*! Compute the biased forces for this collective variable.
            The force that is written to the force arrays must be
            multiplied by the bias factor.

            \param timestep The current value of the time step
         */
        void computeBiasForces(unsigned int timestep);

        Scalar3 m_mesh_size;                //!< The dimensions of a single cell along every coordinate
        uint3 m_mesh_points;                //!< Number of sub-divisions along one coordinate
        uint3 m_n_ghost_cells;              //!< Number of ghost cells along every axis
        uint3 m_grid_dim;                   //!< Grid dimensions (including ghost cells)
        Scalar3 m_ghost_width;              //!< Dimensions of the ghost layer
        unsigned int m_ghost_offset;       //!< Offset in mesh due to ghost cells
        unsigned int m_n_cells;             //!< Total number of inner cells
        unsigned int m_radius;              //!< Stencil radius (in units of mesh size)
        unsigned int m_n_inner_cells;       //!< Number of inner mesh points (without ghost cells)
        GlobalArray<Scalar> m_mode;            //!< Per-type scalar multiplying density ("charges")
        Scalar m_mode_sq;                   //!< Sum of squared mode amplitudes
        GlobalArray<Scalar> m_inf_f;           //!< Fourier representation of the influence function (real part)
        GlobalArray<Scalar> m_interpolation_f; //!< Fourier representation of the interpolation function
        GlobalArray<Scalar3> m_k;              //!< Mesh of k values
        Scalar m_qstarsq;                   //!< Short wave length cut-off squared for density harmonics
        bool m_is_first_step;               //!< True if we have not yet computed the influence function
        unsigned int m_cv_last_updated;     //!< Timestep of last update of collective variable
        bool m_box_changed;                 //!< True if box has changed since last compute
        Scalar m_cv;                        //!< Current value of collective variable

        GlobalArray<Scalar> m_virial_mesh;     //!< k-space mesh of virial tensor values

        unsigned int m_q_max_last_computed;        //!< Last time step at which q max was computed
        Scalar3 m_q_max;                           //!< Current wave vector with maximum amplitude
        Scalar m_sq_max;                           //!< Maximum structure factor

        GlobalArray<int3> m_zero_modes;        //!< Fourier modes that should be zeroed

        Scalar m_k_min;                             //!< Minimum k of tabulated convolution kernel
        Scalar m_k_max;                             //!< Maximum k of tabulated convolution kernel
        Scalar m_delta_k;                           //!< Spacing between k values
        GlobalArray<Scalar> m_table;                   //!< Tabulated kernel
        GlobalArray<Scalar> m_table_d;                 //!< Tabulated kernel
        bool m_use_table;                           //!< Whether to use the tabulated kernel

        //! Helper function to be called when box changes
        void setBoxChange()
            {
            m_box_changed = true;
            }

        //! Helper function to setup the mesh indices
        void setupMesh();

        //! Helper function to setup FFT and allocate the mesh arrays
        virtual void initializeFFT();

        //! Compute the optimal influence function
        virtual void computeInfluenceFunction();

        //! The TSC (triangular-shaped cloud) charge assignment function
        Scalar assignTSC(Scalar x);

        //! Derivative of the TSC (triangular-shaped cloud) charge assignment function
        Scalar assignTSCderiv(Scalar x);

        //! Fourier representation of the TSC (triangular-shaped cloud) charge assignment function
        Scalar assignTSCfourier(Scalar k);

        //! Helper function to assign particle coordinates to mesh
        virtual void assignParticles();

        //! Helper function to update the mesh arrays
        virtual void updateMeshes();

        //! Helper function to interpolate the forces
        virtual void interpolateForces();

        //! Helper function to calculate value of collective variable
        virtual Scalar computeCV();

        //! Helper function to compute the virial
        virtual void computeVirial();

        //! Helper function to compute q vector with maximum amplitude
        virtual void computeQmax(unsigned int timestep);

    private:
        kiss_fftnd_cfg m_kiss_fft;         //!< The FFT configuration
        kiss_fftnd_cfg m_kiss_ifft;        //!< Inverse FFT configuration

        #ifdef ENABLE_MPI
        dfft_plan m_dfft_plan_forward;     //!< Distributed FFT for forward transform
        dfft_plan m_dfft_plan_inverse;     //!< Distributed FFT for inverse transform
        std::unique_ptr<CommunicatorGrid<kiss_fft_cpx> > m_grid_comm_forward; //!< Communicator for charge mesh
        std::unique_ptr<CommunicatorGrid<kiss_fft_cpx> > m_grid_comm_reverse; //!< Communicator for inv fourier mesh
        #endif

        bool m_kiss_fft_initialized;               //!< True if a local KISS FFT has been set up

        GlobalArray<kiss_fft_cpx> m_mesh;             //!< The particle density mesh
        GlobalArray<kiss_fft_cpx> m_fourier_mesh;     //!< The fourier transformed mesh
        GlobalArray<kiss_fft_cpx> m_fourier_mesh_G;   //!< Fourier transformed mesh times the influence function
        GlobalArray<kiss_fft_cpx> m_inv_fourier_mesh; //!< The inverse-Fourier transformed mesh

        std::vector<std::string> m_log_names;           //!< Name of the log quantity

        bool m_dfft_initialized;                   //! True if host dfft has been initialized

        //! Compute virial on mesh
        void computeVirialMesh();

        //! Compute number of ghost cellso
        uint3 computeGhostCellNum();
    };

void export_OrderParameterMesh(pybind11::module& m);

#endif
