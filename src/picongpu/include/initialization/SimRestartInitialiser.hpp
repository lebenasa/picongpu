/**
 * Copyright 2013 Axel Huebl, Felix Schmitt, Heiko Burau, Rene Widera
 *
 * This file is part of PIConGPU. 
 * 
 * PIConGPU is free software: you can redistribute it and/or modify 
 * it under the terms of the GNU General Public License as published by 
 * the Free Software Foundation, either version 3 of the License, or 
 * (at your option) any later version. 
 * 
 * PIConGPU is distributed in the hope that it will be useful, 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the 
 * GNU General Public License for more details. 
 * 
 * You should have received a copy of the GNU General Public License 
 * along with PIConGPU.  
 * If not, see <http://www.gnu.org/licenses/>. 
 */



#ifndef SIMRESTARTINITIALISER_HPP
#define	SIMRESTARTINITIALISER_HPP

#include "types.h"
#include "simulation_defines.hpp"
#include "particles/frame_types.hpp"

#include "dataManagement/AbstractInitialiser.hpp"
#include "dataManagement/DataConnector.hpp"

#include "dimensions/DataSpace.hpp"

#include "dimensions/GridLayout.hpp"
#include "fields/FieldE.hpp"
#include "fields/FieldB.hpp"

#include <splash/splash.h>

#include "simulationControl/MovingWindow.hpp"

#include <string>
#include <sstream>

namespace picongpu
{

using namespace PMacc;
using namespace splash;

/**
 * Helper class for SimRestartInitialiser for loading
 * DIM-dimensional particle data.
 * 
 * @tparam BufferType type of particles buffer
 * @tparam DIM dimension of data to load
 */
template<class BufferType, unsigned DIM>
class RestartParticleLoader
{
public:
    static void loadParticles(uint32_t simulationStep,
                              DomainCollector& dataCollector,
                              DataSpace<DIM> gridPosition,
                              std::string prefix,
                              BufferType& particles);
};

template<class BufferType>
class RestartParticleLoader<BufferType, DIM3>
{
private:

    template<class TYPE>
    static void loadParticleData(TYPE **dst,
                                 uint32_t simulationStep,
                                 ParallelDomainCollector& dataCollector,
                                 Dimensions &dataSize,
                                 CollectionType&,
                                 std::string name,
                                 uint64_t numParticles,
                                 uint64_t particlesLoadOffset)
    {        
        // allocate memory for particles
        *dst = new TYPE[numParticles];
        memset(*dst, 0, sizeof (TYPE) * numParticles);
        
        // read particles from file 
        dataCollector.read(simulationStep,
                Dimensions(numParticles, 1, 1),
                Dimensions(particlesLoadOffset, 0, 0),
                name.c_str(),
                dataSize,
                *dst
                );
    }

public:

    static void loadParticles(uint32_t simulationStep,
                              ParallelDomainCollector& dataCollector,
                              std::string prefix,
                              BufferType& particles,
                              DataSpace<DIM3> globalDomainOffset,
                              DataSpace<DIM3> localDomainSize,
                              DataSpace<DIM3> logicalToPhysicalOffset
                              )
    {
        GridController<simDim> &gc = GridController<simDim>::getInstance();
        
        // first, load all data arrays from hdf5 file
        CollectionType *ctFloat;
        CollectionType *ctInt;

        ctFloat = new ColTypeFloat();
        ctInt = new ColTypeInt();

        const std::string name_lookup[] = {"x", "y", "z"};

        Dimensions dim_pos(0, 0, 0);
        Dimensions dim_cell(0, 0, 0);
        Dimensions dim_mom(0, 0, 0);
        Dimensions dim_weighting(0, 0, 0);
#if(ENABLE_RADIATION == 1)
        Dimensions dim_mom_mt1(0, 0, 0);

#endif

        typedef float* ptrFloat;
        typedef int* ptrInt;

        ptrFloat relativePositions[simDim];
        ptrInt cellPositions[simDim];
        ptrFloat momentums[simDim];
        ptrFloat weighting = NULL;
        
        /* load particles info table entry for this process
           particlesInfo is (part-count, scalar pos, x, y, z) */
        typedef uint64_t uint64Quint[5];
        uint64Quint particlesInfo[gc.getGlobalSize()];
        Dimensions particlesInfoSizeRead;
        
        dataCollector.read(simulationStep, (std::string(prefix) + std::string("_particles_info")).c_str(),
                particlesInfoSizeRead, particlesInfo);
        
        assert(particlesInfoSizeRead[0] == gc.getGlobalSize());
        
        /* search my entry (using my scalar position) in particlesInfo */
        uint64_t particleOffset = 0;
        uint64_t particleCount = 0;
        uint64_t myScalarPos = gc.getScalarPosition();
        
        for (size_t i = 0; i < particlesInfoSizeRead[0]; ++i)
        {                        
            if (particlesInfo[i][1] == myScalarPos)
            {
                particleCount = particlesInfo[i][0];
                break;
            }
            
            particleOffset += particlesInfo[i][0];
        }
        
        log<picLog::INPUT_OUTPUT > ("Loading %1% particles from offset %2%") %
            (long long unsigned)particleCount % (long long unsigned)particleOffset;
        
#if(ENABLE_RADIATION == 1)
        ptrFloat momentums_mt1[simDim];
#if(RAD_MARK_PARTICLE>1) || (RAD_ACTIVATE_GAMMA_FILTER!=0)
        Dimensions dim_radiationFlag(0, 0, 0);
        CollectionType *ctBool = new ColTypeBool();
        typedef bool* ptrBool;
        ptrBool radiationFlag = NULL;
        loadParticleData<bool> (&radiationFlag, simulationStep, dataCollector,
                                dim_radiationFlag, *ctBool, prefix + std::string("_radiationFlag"),
                                particleCount, particleOffset);
#endif
#endif

        
        loadParticleData<float> (&weighting, simulationStep, dataCollector,
                                 dim_weighting, *ctFloat, prefix + std::string("_weighting"),
                                 particleCount, particleOffset);

        assert(weighting != NULL);

        for (uint32_t i = 0; i < simDim; ++i)
        {
            relativePositions[i] = NULL;
            cellPositions[i] = NULL;
            momentums[i] = NULL;
#if(ENABLE_RADIATION == 1)
            momentums_mt1[i] = NULL;
#endif

            // read relative positions for particles in cells
            loadParticleData<float> (&(relativePositions[i]), simulationStep, dataCollector,
                                     dim_pos, *ctFloat, prefix + std::string("_position_") + name_lookup[i],
                                     particleCount, particleOffset);

            // read simulation relative cell positions
            loadParticleData<int > (&(cellPositions[i]), simulationStep, dataCollector,
                                    dim_cell, *ctInt, prefix + std::string("_globalCellIdx_") + name_lookup[i],
                                    particleCount, particleOffset);

            // update simulation relative cell positions from file to 
            // gpu-relative positions for new configuration
            //if (gridPosition[i] > 0)
            for (uint32_t elem = 0; elem < dim_cell.getScalarSize(); ++elem)
                cellPositions[i][elem] -= logicalToPhysicalOffset[i];


            // read momentum of particles
            loadParticleData<float> (&(momentums[i]), simulationStep, dataCollector,
                                     dim_mom, *ctFloat, prefix + std::string("_momentum_") + name_lookup[i],
                                     particleCount, particleOffset);

#if(ENABLE_RADIATION == 1)
            // read old momentum of particles
            loadParticleData<float> (&(momentums_mt1[i]), simulationStep, dataCollector,
                                     dim_mom_mt1, *ctFloat, prefix + std::string("_momentumPrev1_") + name_lookup[i],
                                     particleCount, particleOffset);
#endif

            assert(dim_pos[0] == dim_cell[0] && dim_cell[0] == dim_mom[0]);
            assert(dim_pos[0] == dim_pos.getScalarSize() &&
                   dim_cell[0] == dim_cell.getScalarSize() &&
                   dim_mom[0] == dim_mom.getScalarSize());

            assert(relativePositions[i] != NULL);
            assert(cellPositions[i] != NULL);
            assert(momentums[i] != NULL);

#if(ENABLE_RADIATION == 1)
            assert(momentums_mt1[i] != NULL);
#endif
        }

        // now create frames from loaded data
        typename BufferType::ParticlesBoxType particlesBox = particles.getHostParticlesBox();

        typename BufferType::FrameType * frame(NULL);

        DataSpace<DIM3> superCellsCount = particles.getParticlesBuffer().getSuperCellsCount();
        DataSpace<DIM3> superCellSize = particles.getParticlesBuffer().getSuperCellSize();

        // copy all read data to frames
        DataSpace<DIM3> oldSuperCellPos(-1, -1, -1);
        uint32_t localId = 0;

        for (uint32_t i = 0; i < dim_pos.getScalarSize(); i++)
        {
            // get super cell

            // gpu-global cell position
            DataSpace<DIM3> cellPosOnGPU(cellPositions[0][i], cellPositions[1][i], cellPositions[2][i]);

            /*
            cellPosOnGPU.x() += TILE_WIDTH * GUARD_SIZE;
            cellPosOnGPU.y() += TILE_HEIGHT * GUARD_SIZE;
            cellPosOnGPU.z() += TILE_DEPTH * GUARD_SIZE;
             */
            // gpu-global super cell position
            DataSpace<DIM3> superCellPos = (cellPosOnGPU / superCellSize);

            // get gpu-global super cell offset in cells
            DataSpace<DIM3> superCellOffset = superCellPos * superCellSize; //without guarding (need to calculate cell in supercell)
            // cell position in super cell
            DataSpace<DIM3> cellPosInSuperCell = cellPosOnGPU - superCellOffset;


            superCellPos = superCellPos + GUARD_SIZE; //add GUARD supercells

            assert(superCellPos.x() < superCellsCount.x() &&
                   superCellPos.y() < superCellsCount.y() &&
                   superCellPos.z() < superCellsCount.z());



            // grab next empty frame
            if (superCellPos != oldSuperCellPos || localId == TILE_SIZE)
            {
                localId = 0;
                frame = &(particlesBox.getEmptyFrame());
                particlesBox.setAsLastFrame(*frame, superCellPos);
                oldSuperCellPos = superCellPos;
            }



            if (!((uint32_t) (cellPosInSuperCell.x()) < TILE_WIDTH && (uint32_t) (cellPosInSuperCell.y()) < TILE_HEIGHT &&
                  (uint32_t) (cellPosInSuperCell.z()) < TILE_DEPTH))
            {
                assert((uint32_t) (cellPosInSuperCell.x()) < TILE_WIDTH && (uint32_t) (cellPosInSuperCell.y()) < TILE_HEIGHT &&
                       (uint32_t) (cellPosInSuperCell.z()) < TILE_DEPTH);
            }
            PMacc::lcellId_t localCellId = cellPosInSuperCell.z() * superCellSize.x() * superCellSize.y() +
                cellPosInSuperCell.y() * superCellSize.x() +
                cellPosInSuperCell.x();

            // write to frame
            assert(localId < TILE_SIZE);
            assert((uint32_t) (localCellId) < TILE_SIZE);

            PMACC_AUTO(particle, ((*frame)[localId]));

            particle[localCellIdx_] = localCellId;
            particle[multiMask_] = 1;
            particle[weighting_] = weighting[i];
#if(ENABLE_RADIATION == 1) && ((RAD_MARK_PARTICLE>1) || (RAD_ACTIVATE_GAMMA_FILTER!=0))
            particle[radiationFlag_] = (radiationFlag[i]);
#endif
            for (uint32_t d = 0; d < simDim; ++d)
            {
                particle[position_][d] = relativePositions[d][i];

                particle[momentum_][d] = momentums[d][i];
#if(ENABLE_RADIATION == 1)
                //!\todo: only use Momentum_mt1 if particle type is electrons
                particle[momentumPrev1_][d] = momentums_mt1[d][i];
#endif

            }
            // increase current id/index in frame (0-255)
            localId++;

        }

        particles.syncToDevice();
        particles.fillAllGaps();

        __getTransactionEvent().waitForFinished();

        // cleanup
        for (uint32_t i = 0; i < simDim; ++i)
        {
            delete momentums[i];
            delete cellPositions[i];
            delete relativePositions[i];
        }

        delete weighting;

        delete ctInt;
        delete ctFloat;
#if(ENABLE_RADIATION == 1)
        for (uint32_t i = 0; i < simDim; ++i)
        {
            delete momentums_mt1[i];
        }
#if(RAD_MARK_PARTICLE>1) || (RAD_ACTIVATE_GAMMA_FILTER!=0)
        delete radiationFlag;
        delete ctBool;
#endif
#endif
    }
};

/**
 * Simulation restart initialiser.
 * 
 * Initialises a new simulation from stored data in HDF5 files.
 * 
 * @tparam EBuffer type for Electrons (see MySimulation)
 * @tparam IBuffer type for Ions (see MySimulation)
 * @tparam DIM dimension of the simulation (2-3)
 */
template <class EBuffer, class IBuffer, unsigned DIM>
class SimRestartInitialiser : public AbstractInitialiser
{
public:

    /*! Restart a simulation from a hdf5 dump
     * This class can't restart simulation with active moving (sliding) window
     */
    SimRestartInitialiser(std::string filename, DataSpace<DIM> localGridSize) :
    filename(filename),
    simulationStep(0),
    localGridSize(localGridSize)
    {
        GridController<simDim> &gc = GridController<simDim>::getInstance();
        const uint32_t maxOpenFilesPerNode = 4;
        
        Dimensions mpiSizeHdf5(1, 1, 1);
        for (uint32_t i = 0; i < simDim; ++i)
            mpiSizeHdf5[i] = gc.getGpuNodes()[i];
        
        dataCollector = new ParallelDomainCollector(
                gc.getCommunicator().getMPIComm(),
                gc.getCommunicator().getMPIInfo(),
                mpiSizeHdf5,
                maxOpenFilesPerNode);
    }

    virtual ~SimRestartInitialiser()
    {
        // cleanup
        if (dataCollector != NULL)
        {
            delete dataCollector;
            dataCollector = NULL;
        }
    }

    uint32_t setup()
    {
        // call super class
        AbstractInitialiser::setup();
        
        GridController<DIM> &gc = GridController<DIM>::getInstance();
        DataSpace<DIM> mpiPos = gc.getPosition();
        DataSpace<DIM> mpiSize = gc.getGpuNodes();

        DataCollector::FileCreationAttr attr;
        DataCollector::initFileCreationAttr(attr);
        /** \todo add hdf5.restartMerged flag
         *        OR detect automatically, if merge is really necessary
         *        in that case, set the fileAccType to FAT_READ_MERGED
         *        (useful for changed MPI setting for restart)
         */
        attr.fileAccType = DataCollector::FAT_READ;
        attr.mpiSize.set(mpiSize[0], mpiSize[1], mpiSize[2]);
        attr.mpiPosition.set(mpiPos[0], mpiPos[1], mpiPos[2]);

        dataCollector->open(filename.c_str(), attr);

        // maxID holds the last iteration written to the dataCollector's file
        simulationStep = dataCollector->getMaxID();

        log<picLog::INPUT_OUTPUT > ("Loading from simulation step %1% in file set '%2%'") %
                simulationStep % filename;
        
        /* load number of slides to initialize MovingWindow */        
        int slides = 0;
        dataCollector->readAttribute(simulationStep, NULL, "sim_slides", &slides);
        
        /* apply slides to set gpus to last/written configuration */
        MovingWindow::getInstance().setSlideCounter((uint32_t) slides);
        gc.setNumSlides(slides);
        
        gridPosition = SubGrid<simDim>::getInstance().getSimulationBox().getGlobalOffset();

        return simulationStep + 1;
    }

    void teardown()
    {
        dataCollector->close();

        // call super class
        AbstractInitialiser::teardown();
    }

    void init(uint32_t id, ISimulationData& data, uint32_t)
    {
        switch (id)
        {
#if (ENABLE_ELECTRONS == 1)       
        case PAR_ELECTRONS:
        {
            VirtualWindow window = MovingWindow::getInstance().getVirtualWindow(simulationStep);
            DataSpace<DIM3> globalDomainOffset(gridPosition);
            DataSpace<DIM3> logicalToPhysicalOffset(gridPosition - window.globalSimulationOffset);

            /*domains are allways positiv*/
            if (globalDomainOffset.y() == 0)
                globalDomainOffset.y() = window.globalSimulationOffset.y();

            DataSpace<DIM3> localDomainSize(window.localSize);

            log<picLog::INPUT_OUTPUT > ("Begin loading electrons");
            RestartParticleLoader<EBuffer, DIM>::loadParticles(
                                                               simulationStep,
                                                               *dataCollector,
                                                               EBuffer::FrameType::getName(),
                                                               static_cast<EBuffer&> (data),
                                                               globalDomainOffset,
                                                               localDomainSize,
                                                               logicalToPhysicalOffset
                                                               );
            log<picLog::INPUT_OUTPUT > ("Finished loading electrons");


            if (MovingWindow::getInstance().isSlidingWindowActive())
            {
                {
                    log<picLog::INPUT_OUTPUT > ("Begin loading electrons bottom");
                    globalDomainOffset = gridPosition;
                    globalDomainOffset.y() += window.localSize.y();

                    localDomainSize = window.localFullSize;
                    localDomainSize.y() -= window.localSize.y();

                    {
                        DataSpace<simDim> particleOffset = gridPosition;
                        particleOffset.y() = -window.localSize.y();
                        RestartParticleLoader<EBuffer, DIM>::loadParticles(
                                                                           simulationStep,
                                                                           *dataCollector,
                                                                           "_bottom_e",
                                                                           static_cast<EBuffer&> (data),
                                                                           globalDomainOffset,
                                                                           localDomainSize,
                                                                           particleOffset
                                                                           );
                    }
                    log<picLog::INPUT_OUTPUT > ("Finished loading electrons bottom");
                }
            }
        }
            break;
#endif
#if (ENABLE_IONS == 1)
        case PAR_IONS:
        {
            VirtualWindow window = MovingWindow::getInstance().getVirtualWindow(simulationStep);
            DataSpace<DIM3> globalDomainOffset(gridPosition);
            DataSpace<DIM3> logicalToPhysicalOffset(gridPosition - window.globalSimulationOffset);

            /*domains are allways positiv*/
            if (globalDomainOffset.y() == 0)
                globalDomainOffset.y() = window.globalSimulationOffset.y();

            DataSpace<DIM3> localDomainSize(window.localSize);

            log<picLog::INPUT_OUTPUT > ("Begin loading ions");
            RestartParticleLoader<IBuffer, DIM>::loadParticles(
                                                               simulationStep,
                                                               *dataCollector,
                                                               IBuffer::FrameType::getName(),
                                                               static_cast<IBuffer&> (data),
                                                               globalDomainOffset,
                                                               localDomainSize,
                                                               logicalToPhysicalOffset
                                                               );
            log<picLog::INPUT_OUTPUT > ("Finished loading ions");
            if (MovingWindow::getInstance().isSlidingWindowActive())
            {
                {
                    log<picLog::INPUT_OUTPUT > ("Begin loading ions bottom");
                    globalDomainOffset = gridPosition;
                    globalDomainOffset.y() += window.localSize.y();

                    localDomainSize = window.localFullSize;
                    localDomainSize.y() -= window.localSize.y();

                    {
                        DataSpace<simDim> particleOffset = gridPosition;
                        particleOffset.y() = -window.localSize.y();
                        RestartParticleLoader<IBuffer, DIM>::loadParticles(
                                                                           simulationStep,
                                                                           *dataCollector,
                                                                           "_bottom_i",
                                                                           static_cast<IBuffer&> (data),
                                                                           globalDomainOffset,
                                                                           localDomainSize,
                                                                           particleOffset
                                                                           );
                    }
                    log<picLog::INPUT_OUTPUT > ("Finished loading ions bottom");
                }
            }
        }
            break;

#endif
        case FIELD_E:
            initField(static_cast<FieldE&> (data).getGridBuffer(), FieldE::getName());
            break;

        case FIELD_B:
            initField(static_cast<FieldB&> (data).getGridBuffer(), FieldB::getName());
            //copy field B to Bavg (this is not exact but a good approximation)
            //cloneField(static_cast<FieldB&> (data).getGridBufferBavg(), static_cast<FieldB&> (data).getGridBuffer(), "Bavg");
            //this copy is only needed if we not write Bavg in HDF5 file
            //send all B fields thus simulation are of neighbors is on all gpus
            static_cast<FieldB&> (data).asyncCommunication(__getTransactionEvent()).waitForFinished();
            break;
        }
    }

    uint32_t getSimulationStep()
    {
        return simulationStep;
    }

private:

    template<class Data>
    void initField(Data& field, std::string objectName)
    {

        log<picLog::INPUT_OUTPUT > ("Begin loading field '%1%'") % objectName;
        DataSpace<DIM> field_grid = field.getGridLayout().getDataSpace();
        DataSpace<DIM> field_data = field.getGridLayout().getDataSpaceWithoutGuarding();
        DataSpace<DIM> field_guard = field.getGridLayout().getGuard();

        VirtualWindow window = MovingWindow::getInstance().getVirtualWindow(simulationStep);

        size_t elements = field_grid.productOfComponents();
        float3_X *ptr = field.getHostBuffer().getDataBox().getPointer();
        memset(ptr, 0, elements * sizeof (float3_X));

        const std::string name_lookup[] = {"x", "y", "z"};

        /* globalSlideOffset due to gpu slides between origin at time step 0
         * and origin at current time step
         * ATTENTION: splash offset are globalSlideOffset + picongpu offsets
         */
        DataSpace<simDim> globalSlideOffset(0,
                                            window.slides * window.localFullSize.y(),
                                            0);

        DataSpace<DIM> globalOffset(SubGrid<DIM>::getInstance().getSimulationBox().getGlobalOffset());
        Dimensions domain_offset(globalOffset.x() + globalSlideOffset.x(),
                                 globalOffset.y() + globalSlideOffset.y(),
                                 globalOffset.z() + globalSlideOffset.z());

        if (GridController<simDim>::getInstance().getPosition().y() == 0)
            domain_offset[1] += window.globalSimulationOffset.y();

        Dimensions domain_size(window.localSize.x(),
                               window.localSize.y(),
                               window.localSize.z()
                               );
        for (uint32_t i = 0; i < DIM; ++i)
        {
            // Read the subdomain which belongs to our mpi position.
            // The total grid size must match the grid size of the stored data.
            log<picLog::INPUT_OUTPUT > ("Read from domain: offset=%1% size=%2%") % domain_offset.toString() % domain_size.toString();
            DomainCollector::DomDataClass data_class;
            DataContainer *field_container =
                dataCollector->readDomain(simulationStep,
                                          (objectName + std::string("_") + name_lookup[i]).c_str(),
                                          domain_offset,
                                          domain_size,
                                          &data_class);

            for (uint32_t z = 0; z < domain_size[2]; ++z)
                for (uint32_t y = 0; y < domain_size[1]; ++y)
                    for (uint32_t x = 0; x < domain_size[0]; ++x)
                    {
                        // src is domain_size large, dst is field_grid large

                        int src_index = z * domain_size[0] * domain_size[1] +
                            y * domain_size[0] + x;
                        int dst_index = (z + field_guard[2] + window.localOffset.z()) * field_grid[0] * field_grid[1] +
                            (y + field_guard[1] + window.localOffset.y()) * field_grid[0] + x + field_guard[0] + window.localOffset.x();

                        *(((float*) (ptr + dst_index)) + i) =
                            ((float*) (field_container->getIndex(0)->getData()))[src_index];
                    }

            delete field_container;
        }

        field.hostToDevice();

        __getTransactionEvent().waitForFinished();

        log<picLog::INPUT_OUTPUT > ("Read from domain: offset=%1% size=%2%") % domain_offset.toString() % domain_size.toString();
        log<picLog::INPUT_OUTPUT > ("Finished loading field '%1%'") % objectName;
    }

    template<class Data>
    void cloneField(Data& fieldDest, Data& fieldSrc, std::string objectName)
    {
        log<picLog::INPUT_OUTPUT > ("Begin cloning field '%1%'") % objectName;
        DataSpace<DIM> field_grid = fieldDest.getGridLayout().getDataSpace();

        size_t elements = field_grid.productOfComponents();
        float3_X *ptrDest = fieldDest.getHostBuffer().getDataBox().getPointer();
        float3_X *ptrSrc = fieldSrc.getHostBuffer().getDataBox().getPointer();

        for (size_t k = 0; k < elements; ++k)
        {
            ptrDest[k] = ptrSrc[k];
        }

        fieldDest.hostToDevice();

        __getTransactionEvent().waitForFinished();

        log<picLog::INPUT_OUTPUT > ("Finished cloning field '%1%'") % objectName;
    }

    DataSpace<DIM> gridPosition;
    DataSpace<DIM> localGridSize;
    std::string filename;
    uint32_t simulationStep;
    ParallelDomainCollector *dataCollector;
};
}

#endif	/* SIMRESTARTINITIALISER_HPP */

