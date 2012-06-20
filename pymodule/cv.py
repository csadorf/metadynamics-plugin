from hoomd_script import globals
from hoomd_script import util
from hoomd_script import data
from hoomd_script.force import _force

import _metadynamics
import hoomd


class _collective_variable(_force):
    def __init__(self, sigma, name=None):
        _force.__init__(self, name)

        self.sigma = sigma

        # default grid parameters
        self.cv_min = 0.0
        self.cv_max = 0.0
        self.num_points = 0
        
        self.use_grid = False

    def enable_grid(self,cv_min, cv_max, num_points):
        util.print_status_line()

        self.cv_min = cv_min
        self.cv_max = cv_max
        self.num_points = num_points

        self.use_grid = True

class lamellar(_collective_variable):

    def __init__(self, sigma, mode, lattice_vectors, phi, name=None):
        util.print_status_line()

        if name is not None:
            name = "_" + name
            suffix = name
        else:
            suffix = "" 

        _collective_variable.__init__(self, sigma, name)

        if len(lattice_vectors) == 0:
                globals.msg.error("cv.lamellar: List of supplied latice vectors is empty.\n")
                raise RuntimeEror('Error creating collective variable.')
     
        if type(mode) != type(dict()):
                globals.msg.error("cv.lamellar: Mode amplitudes specified incorrectly.\n")
                raise RuntimeEror('Error creating collective variable.')

        cpp_mode = hoomd.std_vector_float()
        for i in range(0, globals.system_definition.getParticleData().getNTypes()):
            t = globals.system_definition.getParticleData().getNameByType(i)

            if t not in mode.keys():
                globals.msg.error("cv.lamellar: Missing mode amplitude for particle type " + t + ".\n")
                raise RuntimeEror('Error creating collective variable.')
            cpp_mode.append(mode[t])

        cpp_lattice_vectors = _metadynamics.std_vector_int3()
        for l in lattice_vectors:
            if len(l) != 3:
                globals.msg.error("cv.lamellar: List of input lattice vectors not a list of triples.\n")
                raise RuntimeError('Error creating collective variable.')
            cpp_lattice_vectors.append(hoomd.make_int3(l[0], l[1], l[2]))

        cpp_phases = hoomd.std_vector_float()
        if len(phi) != len(lattice_vectors):
                globals.msg.error("cv.lamellar: List of phase shifts not equal to length of lattice vectors.\n")
                raise RuntimeError('Error creating collective variable.')

        for phase in phi:
            cpp_phases.append(phase)

        if not globals.exec_conf.isCUDAEnabled():
            self.cpp_force = _metadynamics.LamellarOrderParameter(globals.system_definition, cpp_mode, cpp_lattice_vectors, cpp_phases, suffix)
        else:
            self.cpp_force = _metadynamics.LamellarOrderParameterGPU(globals.system_definition, cpp_mode, cpp_lattice_vectors, cpp_phases, suffix)

        globals.system.addCompute(self.cpp_force, self.force_name)

    def update_coeffs(self):
        pass
