/*! \file IntegratorMetaDynamics.cc
    \brief Implements the IntegratorMetaDynamics class
 */

#include "IntegratorMetaDynamics.h"

#include <stdio.h>
#include <iomanip>
#include <sstream>

using namespace std;

#include <boost/python.hpp>
#include <boost/filesystem.hpp>

using namespace boost::python;
using namespace boost::filesystem;


IntegratorMetaDynamics::IntegratorMetaDynamics(boost::shared_ptr<SystemDefinition> sysdef,
            Scalar deltaT,
            Scalar W,
            Scalar T_shift,
            unsigned int stride,
            bool add_hills,
            const std::string& filename,
            bool overwrite,
            bool use_grid)
    : IntegratorTwoStep(sysdef, deltaT),
      m_W(W),
      m_T_shift(T_shift),
      m_stride(stride),
      m_num_update_steps(0),
      m_curr_bias_potential(0.0),
      m_is_initialized(false),
      m_filename(filename),
      m_overwrite(overwrite),
      m_is_appending(false),
      m_delimiter("\t"),
      m_use_grid(use_grid),
      m_add_hills(add_hills),
      m_restart_filename("")
    {
    assert(m_T_shift>0);
    assert(m_W > 0);

    m_log_names.push_back("bias_potential");
    }

void IntegratorMetaDynamics::openOutputFile()
    {
    if (exists(m_filename) && !m_overwrite)
        {
        m_exec_conf->msg->notice(3) << "integrate.mode_metadynamics: Appending log to existing file \"" << m_filename << "\"" << endl;
        m_file.open(m_filename.c_str(), ios_base::in | ios_base::out | ios_base::ate);
        m_is_appending = true;
        }
    else
        {
        m_exec_conf->msg->notice(3) << "integrate.mode_metadynamics: Creating new log in file \"" << m_filename << "\"" << endl;
        m_file.open(m_filename.c_str(), ios_base::out);
        m_is_appending = false;
        }
    if (!m_file.good())
        {
        m_exec_conf->msg->error() << "integrate.mode_metadynamics: Error opening log file " << m_filename << endl;
        throw runtime_error("Error initializing IntegratorMetadynamics");
        }
    }

void IntegratorMetaDynamics::writeFileHeader()
    {
    assert(m_variables.size());
    assert(m_file);

    m_file << "timestep" << m_delimiter << "W" << m_delimiter;

    std::vector<CollectiveVariableItem>::iterator it;
    for (it = m_variables.begin(); it != m_variables.end(); ++it)
        {
        m_file << it->m_cv->getName();
        m_file << m_delimiter << "sigma_" << it->m_cv->getName();

        if (it != m_variables.end())
            m_file << m_delimiter;
        }

    m_file << endl;
    }

void IntegratorMetaDynamics::prepRun(unsigned int timestep)
    {
    // Set up file output
    if (! m_is_initialized && m_filename != "")
        {
        openOutputFile();
        if (! m_is_appending)
            writeFileHeader();
        }
    
 
    // Set up colllective variables
    if (! m_is_initialized)
        {
        m_cv_values.resize(m_variables.size());
        std::vector< std::vector<Scalar> >::iterator it;

        for (it = m_cv_values.begin(); it != m_cv_values.end(); ++it)
            it->clear();

        m_num_update_steps = 0;
        m_bias_potential.clear();
        }

    // Set up grid if necessary
    if (! m_is_initialized && m_use_grid)
        {
        setupGrid();

        if (m_restart_filename != "")
            {
            // restart from file
            m_exec_conf->msg->notice(2) << "integrate.mode_metadynamics: Restarting from grid file \"" << m_restart_filename << "\"" << endl;

            readGrid(m_restart_filename);

            m_restart_filename = "";
            }
        }

    m_is_initialized = true;

    // initial update of the potential
    updateBiasPotential(timestep);

    IntegratorTwoStep::prepRun(timestep);
    }

void IntegratorMetaDynamics::update(unsigned int timestep)
    {
    // issue a warning if no integration methods are set
    if (!m_gave_warning && m_methods.size() == 0)
        {
        cout << "***Warning! No integration methods are set, continuing anyways." << endl;
        m_gave_warning = true;
        }
    
    // ensure that prepRun() has been called
    assert(this->m_prepared);
    
    if (m_prof)
        m_prof->push("Integrate");
    
    // perform the first step of the integration on all groups
    std::vector< boost::shared_ptr<IntegrationMethodTwoStep> >::iterator method;
    for (method = m_methods.begin(); method != m_methods.end(); ++method)
        (*method)->integrateStepOne(timestep);

    // Update the rigid body particle positions and velocities if they are present
    if (m_sysdef->getRigidData()->getNumBodies() > 0)
        m_sysdef->getRigidData()->setRV(true);

    if (m_prof)
        m_prof->pop();

    // update bias potential
    updateBiasPotential(timestep+1);

    // compute the net force on all particles
#ifdef ENABLE_CUDA
    if (exec_conf->exec_mode == ExecutionConfiguration::GPU)
        computeNetForceGPU(timestep+1);
    else
#endif
        computeNetForce(timestep+1);

    if (m_prof)
        m_prof->push("Integrate");

    // if the virial needs to be computed and there are rigid bodies, perform the virial correction
    PDataFlags flags = m_pdata->getFlags();
    if (flags[pdata_flag::isotropic_virial] && m_sysdef->getRigidData()->getNumBodies() > 0)
        m_sysdef->getRigidData()->computeVirialCorrectionStart();

    // perform the second step of the integration on all groups
    for (method = m_methods.begin(); method != m_methods.end(); ++method)
        (*method)->integrateStepTwo(timestep);

    // Update the rigid body particle velocities if they are present
    if (m_sysdef->getRigidData()->getNumBodies() > 0)
       m_sysdef->getRigidData()->setRV(false);

    // if the virial needs to be computed and there are rigid bodies, perform the virial correction
    if (flags[pdata_flag::isotropic_virial] && m_sysdef->getRigidData()->getNumBodies() > 0)
        m_sysdef->getRigidData()->computeVirialCorrectionEnd(m_deltaT/2.0);

    if (m_prof)
        m_prof->pop();
    } 

void IntegratorMetaDynamics::updateBiasPotential(unsigned int timestep)
    {
    // exit early if there are no collective variables
    if (m_variables.size() == 0)
        return;

    // collect values of collective variables
    std::vector< Scalar> current_val;
    std::vector<CollectiveVariableItem>::iterator it;
    for (it = m_variables.begin(); it != m_variables.end(); ++it)
        {
        unsigned int cv_index = it - m_variables.begin();
        Scalar val = it->m_cv->getCurrentValue(timestep);

        if (! m_use_grid)
            {
            // append to history
            m_cv_values[cv_index].push_back(val);
            }

        current_val.push_back(val);
        }

    if (m_prof)
        m_prof->push("Metadynamics");

    // update biasing weights by summing up partial derivivatives of Gaussians deposited every m_stride steps
    m_curr_bias_potential = 0.0;
    std::vector<double> bias(m_variables.size(), 0.0); 

    if (m_use_grid)
        {
        // interpolate current value of bias potential
        Scalar V = interpolateBiasPotential(current_val);
        m_curr_bias_potential = V;

        if (m_add_hills && (m_num_update_steps % m_stride == 0))
            {
            // add Gaussian to grid
            
            // scaling factor for well-tempered MetaD
            Scalar scal = exp(-V/m_T_shift);

            // loop over grid
            unsigned int len = m_grid_index.getNumElements();
            std::vector<unsigned int> coords(m_grid_index.getDimension()); 
            for (unsigned int grid_idx = 0; grid_idx < len; grid_idx++)
                {
                // obtain d-dimensional coordinates
                m_grid_index.getCoordinates(grid_idx, coords);

                Scalar gauss_exp(0.0);
                // evaluate Gaussian on grid point
                for (unsigned int cv_idx = 0; cv_idx < m_variables.size(); ++cv_idx)
                    {
                    Scalar delta = (m_variables[cv_idx].m_cv_max - m_variables[cv_idx].m_cv_min)/
                                   (m_variables[cv_idx].m_num_points - 1);
                    Scalar val = m_variables[cv_idx].m_cv_min + coords[cv_idx]*delta;
                    Scalar sigma = m_variables[cv_idx].m_sigma;
                    double d = val - current_val[cv_idx];
                    gauss_exp += d*d/2.0/sigma/sigma;
                    }
                double gauss = exp(-gauss_exp);

                // add Gaussian to grid
                m_grid[grid_idx] += m_W*scal*gauss;
                }
            }

        // calculate partial derivatives numerically
        for (unsigned int cv_idx = 0; cv_idx < m_variables.size(); ++cv_idx)
            {
            Scalar delta = (m_variables[cv_idx].m_cv_max - m_variables[cv_idx].m_cv_min)/
                           (m_variables[cv_idx].m_num_points - 1);
            if (current_val[cv_idx] - delta < m_variables[cv_idx].m_cv_min) 
                {
                // forward difference
                std::vector<Scalar> val2 = current_val;
                val2[cv_idx] += delta;

                Scalar y2 = interpolateBiasPotential(val2);
                Scalar y1 = interpolateBiasPotential(current_val);
                bias[cv_idx] = (y2-y1)/delta;
                }
            else if (current_val[cv_idx] + delta > m_variables[cv_idx].m_cv_max)
                {
                // backward difference
                std::vector<Scalar> val2 = current_val;
                val2[cv_idx] -= delta;
                Scalar y1 = interpolateBiasPotential(val2);
                Scalar y2 = interpolateBiasPotential(current_val);
                bias[cv_idx] = (y2-y1)/delta;
                }
            else
                {
                // central difference
                std::vector<Scalar> val2 = current_val;
                std::vector<Scalar> val1 = current_val;
                val1[cv_idx] -= delta;
                val2[cv_idx] += delta;
                Scalar y1 = interpolateBiasPotential(val1);
                Scalar y2 = interpolateBiasPotential(val2);
                bias[cv_idx] = (y2 - y1)/(Scalar(2.0)*delta);
                }
            }

        } 
    else
        {
        // sum up all Gaussians accumulated until now
        for (unsigned int step = 0; step < m_bias_potential.size()*m_stride; step += m_stride)
            {
            double gauss_exp = 0.0;
            // calculate Gaussian contribution from t'=step
            std::vector<Scalar>::iterator val_it;
            for (val_it = current_val.begin(); val_it != current_val.end(); ++val_it)
                {
                Scalar val = *val_it;
                unsigned int cv_index = val_it - current_val.begin();
                Scalar sigma = m_variables[cv_index].m_sigma;
                double delta = val - m_cv_values[cv_index][step];
                gauss_exp += delta*delta/2.0/sigma/sigma;
                }
            double gauss = exp(-gauss_exp);

            // calculate partial derivatives
            std::vector<CollectiveVariableItem>::iterator cv_item;
            Scalar scal = exp(-m_bias_potential[step/m_stride]/m_T_shift);
            for (cv_item = m_variables.begin(); cv_item != m_variables.end(); ++cv_item)
                {
                unsigned int cv_index = cv_item - m_variables.begin();
                Scalar val = current_val[cv_index];
                Scalar sigma = m_variables[cv_index].m_sigma;
                bias[cv_index] -= m_W*scal/sigma/sigma*(val - m_cv_values[cv_index][step])*gauss;
                }

            m_curr_bias_potential += m_W*scal*gauss;
            }
        }

    // write hills information
    if (m_is_initialized && (m_num_update_steps % m_stride == 0) && m_add_hills)
        {
        Scalar W = m_W*exp(-m_curr_bias_potential/m_T_shift);
        m_file << setprecision(10) << timestep << m_delimiter;
        m_file << setprecision(10) << W << m_delimiter;

        std::vector<Scalar>::iterator cv;
        for (cv = current_val.begin(); cv != current_val.end(); ++cv)
            {
            unsigned int cv_index = cv - current_val.begin();
            m_file << setprecision(10) << *cv << m_delimiter;
            m_file << setprecision(10) << m_variables[cv_index].m_sigma;
            if (cv != current_val.end() -1) m_file << m_delimiter;
            }

        m_file << endl;
        }
   
    if (m_add_hills && (! m_use_grid) && (m_num_update_steps % m_stride == 0))
        m_bias_potential.push_back(m_curr_bias_potential);

    // update current bias potential derivative for every collective variable
    std::vector<CollectiveVariableItem>::iterator cv_item;
    for (cv_item = m_variables.begin(); cv_item != m_variables.end(); ++cv_item)
        {
        unsigned int cv_index = cv_item - m_variables.begin();
        cv_item->m_cv->setBiasFactor(bias[cv_index]);
        }

    // increment number of updated steps
    m_num_update_steps++;

    if (m_prof)
        m_prof->pop();
    }

void IntegratorMetaDynamics::setupGrid()
    {
    assert(! m_is_initialized);
    assert(m_variables.size());

    std::vector< CollectiveVariableItem >::iterator it;

    std::vector< unsigned int > lengths(m_variables.size());

    for (it = m_variables.begin(); it != m_variables.end(); ++it)
        {
        lengths[it - m_variables.begin()] = it->m_num_points;
        }

    m_grid_index.setLengths(lengths);
    m_grid.resize(m_grid_index.getNumElements(),0.0);
    } 

Scalar IntegratorMetaDynamics::interpolateBiasPotential(const std::vector<Scalar>& val)
    {
    assert(val.size() == m_grid_index.getDimension());

    // find closest d-dimensional sub-block
    std::vector<Scalar> lower_x(m_grid_index.getDimension());
    std::vector<Scalar> upper_x(m_grid_index.getDimension());
    std::vector<unsigned int> lower_idx(m_grid_index.getDimension());
    std::vector<unsigned int> upper_idx(m_grid_index.getDimension());
    std::vector<Scalar> rel_delta(m_grid_index.getDimension());

    for (unsigned int i = 0; i < m_grid_index.getDimension(); i++)
        {
        bool found = false;
        unsigned int h;
        Scalar lower_bound(0.0), upper_bound(0.0);
        Scalar delta = (m_variables[i].m_cv_max - m_variables[i].m_cv_min)/(m_variables[i].m_num_points - 1);
        upper_bound = m_variables[i].m_cv_min;
        for (h=0; h < m_variables[i].m_num_points-1; h++)
            {
            lower_bound = upper_bound;
            upper_bound += delta;

            if ( (val[i] >= lower_bound) && (val[i] < upper_bound))
                {
                found = true;
                break;
                }
            }

        if (!found)
            {
            m_exec_conf->msg->warning() << "integrate.mode_metadynamics: Value " << val[i]
                                        << " of collective variable " << i << " out of bounds." << endl
                                        << "Assuming bias potential of zero." << endl;
            return Scalar(0.0);
            }
        
        lower_x[i] = lower_bound;
        upper_x[i] = upper_bound;
        rel_delta[i] = (val[i]-lower_bound)/(upper_bound-lower_bound);

        lower_idx[i] = h;
        upper_idx[i] = h+1;
        }

    // construct multilinear interpolation
    unsigned int n_term = 1 << m_grid_index.getDimension();
    Scalar res(0.0);
    for (unsigned int bits = 0; bits < n_term; ++bits)
        {
        std::vector<unsigned int> coords(m_grid_index.getDimension());
        Scalar term(1.0);
        for (unsigned int i = 0; i < m_grid_index.getDimension(); i++)
            {
            if (bits & (1 << i))
                {
                coords[i] = lower_idx[i];
                term *= (Scalar(1.0) - rel_delta[i]);
                }
            else
                {
                coords[i] = upper_idx[i];
                term *= rel_delta[i];
                }
            }
        
        term *= m_grid[m_grid_index.getIndex(coords)];
        res += term;
        }

    return res;
    }

void IntegratorMetaDynamics::setGrid(bool use_grid)
    {
    if (m_is_initialized)
        {
        m_exec_conf->msg->error() << "integrate.mode_metadynamics: Cannot change grid mode after initialization." << endl;
        throw std::runtime_error("Error setting up metadynamics parameters.");
        }

    m_use_grid = use_grid;

    if (use_grid)
        {
        // Check for some input errors
        std::vector<CollectiveVariableItem>::iterator it;

        for (it = m_variables.begin(); it != m_variables.end(); ++it)
            {
            if (it->m_cv_min >= it->m_cv_max)
                {
                m_exec_conf->msg->error() << "integrate.mode_metadyanmics: Maximum grid value of collective variable has to be greater than minimum value.";
                throw std::runtime_error("Error creating collective variable.");
                
                }

            if (it->m_num_points < 2)
                {
                m_exec_conf->msg->error() << "integrate.mode_metadynamics: Number of grid points for collective variable has to be at least two.";
                throw std::runtime_error("Error creating collective variable.");
                }
            }
        }
    }

void IntegratorMetaDynamics::dumpGrid(const std::string& filename)
    {
    if (! m_use_grid)
        {
        m_exec_conf->msg->error() << "integrate.mode_metadynamics: Grid information can only be dumped if grid is enabled.";
        throw std::runtime_error("Error dumping grid.");
        }
 
    std::ofstream file;

    // open output file
    file.open(filename.c_str(), ios_base::out);

    // write file header
    file << "#n_cv: " << m_grid_index.getDimension() << std::endl;
    file << "#dim:";
    
    for (unsigned int i= 0; i < m_grid_index.getDimension(); i++)
        file << " " << m_grid_index.getLength(i);

    file << std::endl;

    file << "grid_value";

    for (unsigned int i = 0; i < m_grid_index.getDimension(); i++)
        file << m_delimiter << "cv" << i;

    file << std::endl;

    // loop over grid
    unsigned int len = m_grid_index.getNumElements();
    std::vector<unsigned int> coords(m_grid_index.getDimension()); 
    for (unsigned int grid_idx = 0; grid_idx < len; grid_idx++)
        {
        // obtain d-dimensional coordinates
        m_grid_index.getCoordinates(grid_idx, coords);

        file << setprecision(10) << m_grid[grid_idx];

        for (unsigned int cv_idx = 0; cv_idx < m_variables.size(); ++cv_idx)
            {
            Scalar delta = (m_variables[cv_idx].m_cv_max - m_variables[cv_idx].m_cv_min)/
                           (m_variables[cv_idx].m_num_points - 1);
            Scalar val = m_variables[cv_idx].m_cv_min + coords[cv_idx]*delta;

            file << m_delimiter << setprecision(10) << val;
            }

        file << std::endl;
        }

    file.close();
    }

void IntegratorMetaDynamics::readGrid(const std::string& filename)
    {
    if (! m_use_grid)
        {
        m_exec_conf->msg->error() << "integrate.mode_metadynamics: Grid information can only be read if grid is enabled.";
        throw std::runtime_error("Error reading grid.");
        }
    std::ifstream file;

    // open grid file
    file.open(filename.c_str());

    std::string line; 

    // Skip file header
    getline(file, line);
    getline(file, line);
    getline(file, line);

    unsigned int len = m_grid_index.getNumElements();
    std::vector<unsigned int> coords(m_grid_index.getDimension()); 
    for (unsigned int grid_idx = 0; grid_idx < len; grid_idx++)
        {
        if (! file.good())
            {
            m_exec_conf->msg->error() << "integrate.mode_metadynamics: Premature end of grid file.";
            throw std::runtime_error("Error reading grid.");
            }
     
        getline(file, line);
        istringstream iss(line);
        iss >> m_grid[grid_idx];
        }
    
    file.close();
    } 

void export_IntegratorMetaDynamics()
    {
    class_<IntegratorMetaDynamics, boost::shared_ptr<IntegratorMetaDynamics>, bases<IntegratorTwoStep>, boost::noncopyable>
    ("IntegratorMetaDynamics", init< boost::shared_ptr<SystemDefinition>,
                          Scalar,
                          Scalar,
                          Scalar,
                          unsigned int,
                          bool,
                          const std::string&,
                          bool>())
    .def("registerCollectiveVariable", &IntegratorMetaDynamics::registerCollectiveVariable)
    .def("removeAllVariables", &IntegratorMetaDynamics::removeAllVariables)
    .def("isInitialized", &IntegratorMetaDynamics::isInitialized)
    .def("setGrid", &IntegratorMetaDynamics::setGrid)
    .def("dumpGrid", &IntegratorMetaDynamics::dumpGrid)
    .def("restartFromGridFile", &IntegratorMetaDynamics::restartFromGridFile)
    .def("setAddHills", &IntegratorMetaDynamics::setAddHills)
    ;
    }