#ifndef __COLLECTIVE_VARIABLE_H__
#define __COLLECTIVE_VARIABLE_H__

/*! \file CollectiveVariable.h
    \brief Declares the CollectiveVariable abstract class
 */

#include <hoomd/hoomd.h>

#include <string.h>

/*! Abstract interface for a collective variable
 
    All C++ implementations of collective variables inherit from this class.
    A CollectiveVariable is an extension of a ForceCompute,
    and can compute forces.

    The force generated by a collective variable (i.e. its negative derivative
    with respect to particle positions) must be multiplied
    by a bias factor (the partial derivative of the biasing potential with
    respect to the collective variable). The bias factor is set using
    the method setBiasFactor().

    Collective variables should have a potential energy of zero,
    since they are not directly added to the Hamiltonian (only via the
    biasing potential). Instead, the value of the collective variable
    can be queried using getCurrentValue().

 */
class CollectiveVariable : public ForceCompute
    {
    public:
        /*! Constructs a collective variable
            \param sysdef The system definition
            \param name The name of this collective variable
         */
        CollectiveVariable(boost::shared_ptr<SystemDefinition> sysdef, const std::string& name);
        virtual ~CollectiveVariable() {}

        /*! Returns the current value of the collective variable
         *  \param timestep The currnt value of the timestep
         */
        virtual Scalar getCurrentValue(unsigned int timestep) = 0;

        /*! Set the current value of the bias factor.
            This routine has to be called before force evaluation
            by the integrator.

            \param bias The value that multiplies the force
         */
        virtual void setBiasFactor(Scalar bias)
            {
            m_bias = bias;
            }

        /*! Evaluate a harmonic potential function of the collective variable
         * \param harmonic True if harmonic potential should be active
         */
        void setHarmonic(bool harmonic)
            {
            m_harmonic = harmonic;
            }

        /*! Set spring constant for harmonic potential
         * \param kappa Spring constant (in units energy/c.v.^2)
         */
        void setKappa(Scalar kappa)
            {
            m_kappa = kappa;
            }

        /*! Set minimum position of harmonic potential
         * \param cv0 Minimum position (units of c.v.)
         */ 
        void setMinimum(Scalar cv0)
            {
            m_cv0 = cv0;
            } 

        /*! Returns the name of the collective variable
         */
        std::string getName()
            {
            return m_cv_name;
            }

        /*! Computes the derivative of the collective variable w.r.t. the particle coordinates
         * and stores them in the force array.
         */
        void computeDerivatives(unsigned int timestep)
            {
            m_bias = Scalar(1.0);

            computeBiasForces(timestep);
            }

        /*! Returns the value of the harmonic umbrella potential
         * \param timestep
         */
        Scalar getUmbrellaPotential(unsigned int timestep);

    protected:
        /*! \param timestep The current value of the time step
         */
        void computeForces(unsigned int timestep);

        /*! Compute the biased forces for this collective variable.
            The force that is written to the force arrays must be
            multiplied by the bias factor.

            \param timestep The current value of the time step
         */
        virtual void computeBiasForces(unsigned int timestep) = 0;

        Scalar m_bias;         //!< The bias factor multiplying the force

        std::string m_cv_name; //!< Name of the collective variable

    private:
        bool m_harmonic;       //!< True if a harmonic potential of the collective variable is evaluated
        Scalar m_cv0;          //!< Minimum position of harmonic potential
        Scalar m_kappa;        //!< Stiffness of harmonic potential
    };

//! Export the CollectiveVariable class to python
void export_CollectiveVariable();

#endif // __COLLECTIVE_VARIABLE_H__
