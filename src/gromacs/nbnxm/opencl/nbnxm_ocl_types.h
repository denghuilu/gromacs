/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2012,2013,2014,2015,2016 by the GROMACS development team.
 * Copyright (c) 2017,2018,2019,2020, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */

/*! \internal \file
 *  \brief
 *  Data types used internally in the nbnxm_ocl module.
 *
 *  \author Anca Hamuraru <anca@streamcomputing.eu>
 *  \author Szilárd Páll <pszilard@kth.se>
 *  \ingroup module_nbnxm
 */

#ifndef GMX_NBNXM_NBNXM_OPENCL_TYPES_H
#define GMX_NBNXM_NBNXM_OPENCL_TYPES_H

#include "gromacs/gpu_utils/devicebuffer.h"
#include "gromacs/gpu_utils/gmxopencl.h"
#include "gromacs/gpu_utils/gputraits_ocl.h"
#include "gromacs/gpu_utils/oclutils.h"
#include "gromacs/mdtypes/interaction_const.h"
#include "gromacs/nbnxm/gpu_types_common.h"
#include "gromacs/nbnxm/nbnxm.h"
#include "gromacs/nbnxm/nbnxm_gpu.h"
#include "gromacs/nbnxm/pairlist.h"
#include "gromacs/utility/enumerationhelpers.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/real.h"

#include "nbnxm_ocl_consts.h"

struct gmx_wallclock_gpu_nbnxn_t;

/* kernel does #include "gromacs/math/utilities.h" */
/* Move the actual useful stuff here: */

//! Define 1/sqrt(pi)
#define M_FLOAT_1_SQRTPI 0.564189583547756f

/*! \brief Constants for platform-dependent defaults for the prune kernel's j4 processing concurrency.
 *
 *  Initialized using macros that can be overridden at compile-time (using #GMX_NBNXN_PRUNE_KERNEL_J4_CONCURRENCY).
 */
/*! @{ */
const int c_oclPruneKernelJ4ConcurrencyDEFAULT = GMX_NBNXN_PRUNE_KERNEL_J4_CONCURRENCY_DEFAULT;
/*! @} */

/*! \brief Pruning kernel flavors.
 *
 * The values correspond to the first call of the pruning post-list generation
 * and the rolling pruning, respectively.
 */
enum ePruneKind
{
    epruneFirst,
    epruneRolling,
    ePruneNR
};

/*! \internal
 * \brief Staging area for temporary data downloaded from the GPU.
 *
 *  The energies/shift forces get downloaded here first, before getting added
 *  to the CPU-side aggregate values.
 */
struct nb_staging_t
{
    //! LJ energy
    float* e_lj = nullptr;
    //! electrostatic energy
    float* e_el = nullptr;
    //! float3 buffer with shift forces
    float (*fshift)[3] = nullptr;
};

/*! \internal
 * \brief Nonbonded atom data - both inputs and outputs.
 */
typedef struct cl_atomdata
{
    //! number of atoms
    int natoms;
    //! number of local atoms
    int natoms_local;
    //! allocation size for the atom data (xq, f)
    int nalloc;

    //! float4 buffer with atom coordinates + charges, size natoms
    DeviceBuffer<float> xq;

    //! float3 buffer with force output array, size natoms
    DeviceBuffer<float> f;

    //! LJ energy output, size 1
    DeviceBuffer<float> e_lj;
    //! Electrostatics energy input, size 1
    DeviceBuffer<float> e_el;

    //! float3 buffer with shift forces
    DeviceBuffer<float> fshift;

    //! number of atom types
    int ntypes;
    //! int buffer with atom type indices, size natoms
    DeviceBuffer<int> atom_types;
    //! float2 buffer with sqrt(c6),sqrt(c12), size natoms
    DeviceBuffer<float> lj_comb;

    //! float3 buffer with shifts values
    DeviceBuffer<float> shift_vec;

    //! true if the shift vector has been uploaded
    bool bShiftVecUploaded;
} cl_atomdata_t;

/*! \internal
 * \brief Data structure shared between the OpenCL device code and OpenCL host code
 *
 * Must not contain OpenCL objects (buffers)
 * TODO: review, improve */
typedef struct cl_nbparam_params
{

    //! type of electrostatics
    enum Nbnxm::ElecType elecType;
    //! type of VdW impl.
    enum Nbnxm::VdwType vdwType;

    //! charge multiplication factor
    float epsfac;
    //! Reaction-field/plain cutoff electrostatics const.
    float c_rf;
    //! Reaction-field electrostatics constant
    float two_k_rf;
    //! Ewald/PME parameter
    float ewald_beta;
    //! Ewald/PME correction term subtracted from the direct-space potential
    float sh_ewald;
    //! LJ-Ewald/PME correction term added to the correction potential
    float sh_lj_ewald;
    //! LJ-Ewald/PME coefficient
    float ewaldcoeff_lj;

    //! Coulomb cut-off squared
    float rcoulomb_sq;

    //! VdW cut-off squared
    float rvdw_sq;
    //! VdW switched cut-off
    float rvdw_switch;
    //! Full, outer pair-list cut-off squared
    float rlistOuter_sq;
    //! Inner, dynamic pruned pair-list cut-off squared
    float rlistInner_sq;

    //! VdW shift dispersion constants
    shift_consts_t dispersion_shift;
    //! VdW shift repulsion constants
    shift_consts_t repulsion_shift;
    //! VdW switch constants
    switch_consts_t vdw_switch;

    /* Ewald Coulomb force table data - accessed through texture memory */
    //! table scale/spacing
    float coulomb_tab_scale;
} cl_nbparam_params_t;


/** \internal
 * \brief Typedef of actual timer type.
 */
typedef struct Nbnxm::gpu_timers_t cl_timers_t;

/*! \internal
 * \brief Main data structure for OpenCL nonbonded force calculations.
 */
struct NbnxmGpu
{
    /* \brief OpenCL device context
     *
     * \todo Make it constant reference, once NbnxmGpu is a proper class.
     */
    const DeviceContext* deviceContext_;
    //! OpenCL runtime data (context, kernels)
    struct gmx_device_runtime_data_t* dev_rundata = nullptr;

    /**< Pointers to non-bonded kernel functions
     * organized similar with nb_kfunc_xxx arrays in nbnxn_ocl.cpp */
    ///@{
    cl_kernel kernel_noener_noprune_ptr[Nbnxm::c_numElecTypes][Nbnxm::c_numVdwTypes] = { { nullptr } };
    cl_kernel kernel_ener_noprune_ptr[Nbnxm::c_numElecTypes][Nbnxm::c_numVdwTypes] = { { nullptr } };
    cl_kernel kernel_noener_prune_ptr[Nbnxm::c_numElecTypes][Nbnxm::c_numVdwTypes] = { { nullptr } };
    cl_kernel kernel_ener_prune_ptr[Nbnxm::c_numElecTypes][Nbnxm::c_numVdwTypes] = { { nullptr } };
    ///@}
    //! prune kernels, ePruneKind defined the kernel kinds
    cl_kernel kernel_pruneonly[ePruneNR] = { nullptr };

    //! true if prefetching fg i-atom LJ parameters should be used in the kernels
    bool bPrefetchLjParam = false;

    /**< auxiliary kernels implementing memset-like functions */
    ///@{
    cl_kernel kernel_memset_f      = nullptr;
    cl_kernel kernel_memset_f2     = nullptr;
    cl_kernel kernel_memset_f3     = nullptr;
    cl_kernel kernel_zero_e_fshift = nullptr;
    ///@}

    //! true if doing both local/non-local NB work on GPU
    bool bUseTwoStreams = false;
    //! true indicates that the nonlocal_done event was enqueued
    bool bNonLocalStreamActive = false;

    //! atom data
    cl_atomdata_t* atdat = nullptr;
    //! parameters required for the non-bonded calc.
    NBParamGpu* nbparam = nullptr;
    //! pair-list data structures (local and non-local)
    gmx::EnumerationArray<Nbnxm::InteractionLocality, Nbnxm::gpu_plist*> plist = { nullptr };
    //! staging area where fshift/energies get downloaded
    nb_staging_t nbst;

    //! local and non-local GPU queues
    gmx::EnumerationArray<Nbnxm::InteractionLocality, const DeviceStream*> deviceStreams;

    /*! \brief Events used for synchronization */
    /*! \{ */
    /*! \brief Event triggered when the non-local non-bonded
     * kernel is done (and the local transfer can proceed) */
    cl_event nonlocal_done = nullptr;
    /*! \brief Event triggered when the tasks issued in the local
     * stream that need to precede the non-local force or buffer
     * operation calculations are done (e.g. f buffer 0-ing, local
     * x/q H2D, buffer op initialization in local stream that is
     * required also by nonlocal stream ) */
    cl_event misc_ops_and_local_H2D_done = nullptr;
    /*! \} */

    //! True if there has been local/nonlocal GPU work, either bonded or nonbonded, scheduled
    //  to be executed in the current domain. As long as bonded work is not split up into
    //  local/nonlocal, if there is bonded GPU work, both flags will be true.
    gmx::EnumerationArray<Nbnxm::InteractionLocality, bool> haveWork;


    //! True if event-based timing is enabled.
    bool bDoTime = false;
    //! OpenCL event-based timers.
    cl_timers_t* timers = nullptr;
    //! Timing data. TODO: deprecate this and query timers for accumulated data instead
    gmx_wallclock_gpu_nbnxn_t* timings = nullptr;
};

#endif /* NBNXN_OPENCL_TYPES_H */
