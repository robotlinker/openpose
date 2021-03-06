#ifndef CPU_ONLY
    #include <cuda.h>
    #include <cuda_runtime_api.h>
#endif
#include <openpose/pose/poseParameters.hpp>
#include <openpose/pose/renderPose.hpp>
#include <openpose/utilities/cuda.hpp>
#include <openpose/pose/poseGpuRenderer.hpp>

namespace op
{
    PoseGpuRenderer::PoseGpuRenderer(const Point<int>& heatMapsSize, const PoseModel poseModel,
                                     const std::shared_ptr<PoseExtractor>& poseExtractor, const float renderThreshold,
                                     const bool blendOriginalFrame, const float alphaKeypoint, const float alphaHeatMap,
                                     const unsigned int elementToRender) :
        // #body elements to render = #body parts (size()) + #body part pair connections + 3 (+whole pose +whole heatmaps +PAFs)
        // POSE_BODY_PART_MAPPING crashes on Windows, replaced by getPoseBodyPartMapping
        GpuRenderer{renderThreshold, alphaKeypoint, alphaHeatMap, blendOriginalFrame, elementToRender,
                    (unsigned int)(getPoseBodyPartMapping(poseModel).size() + POSE_BODY_PART_PAIRS[(int)poseModel].size()/2 + 3)}, // mNumberElementsToRender
        PoseRenderer{poseModel},
        mHeatMapsSize{heatMapsSize},
        spPoseExtractor{poseExtractor},
        pGpuPose{nullptr}
    {
    }

    PoseGpuRenderer::~PoseGpuRenderer()
    {
        try
        {
            // Free CUDA pointers - Note that if pointers are 0 (i.e. nullptr), no operation is performed.
            #ifndef CPU_ONLY
                cudaFree(pGpuPose);
            #endif
        }
        catch (const std::exception& e)
        {
            error(e.what(), __LINE__, __FUNCTION__, __FILE__);
        }
    }

    void PoseGpuRenderer::initializationOnThread()
    {
        try
        {
            log("Starting initialization on thread.", Priority::Low, __LINE__, __FUNCTION__, __FILE__);
            // GPU memory allocation for rendering
            #ifndef CPU_ONLY
                cudaMalloc((void**)(&pGpuPose), POSE_MAX_PEOPLE * POSE_NUMBER_BODY_PARTS[(int)mPoseModel] * 3 * sizeof(float));
                cudaCheck(__LINE__, __FUNCTION__, __FILE__);
            #endif
            log("Finished initialization on thread.", Priority::Low, __LINE__, __FUNCTION__, __FILE__);
        }
        catch (const std::exception& e)
        {
            error(e.what(), __LINE__, __FUNCTION__, __FILE__);
        }
    }

    std::pair<int, std::string> PoseGpuRenderer::renderPose(Array<float>& outputData, const Array<float>& poseKeypoints,
                                                            const float scaleNetToOutput)
    {
        try
        {
            // Security checks
            if (outputData.empty())
                error("Empty Array<float> outputData.", __LINE__, __FUNCTION__, __FILE__);
            // GPU rendering
            const auto elementRendered = spElementToRender->load();
            std::string elementRenderedName;
            #ifndef CPU_ONLY
                const auto numberPeople = poseKeypoints.getSize(0);
                if (numberPeople > 0 || elementRendered != 0 || !mBlendOriginalFrame)
                {
                    cpuToGpuMemoryIfNotCopiedYet(outputData.getPtr(), outputData.getVolume());
                    cudaCheck(__LINE__, __FUNCTION__, __FILE__);
                    const auto numberBodyParts = POSE_NUMBER_BODY_PARTS[(int)mPoseModel];
                    const auto numberBodyPartsPlusBkg = numberBodyParts+1;
                    const Point<int> frameSize{outputData.getSize(2), outputData.getSize(1)};
                    // Draw poseKeypoints
                    if (elementRendered == 0)
                    {
                        if (!poseKeypoints.empty())
                            cudaMemcpy(pGpuPose, poseKeypoints.getConstPtr(), numberPeople * numberBodyParts * 3 * sizeof(float),
                                       cudaMemcpyHostToDevice);
                        renderPoseKeypointsGpu(*spGpuMemory, mPoseModel, numberPeople, frameSize, pGpuPose,
                                               mRenderThreshold, mShowGooglyEyes, mBlendOriginalFrame, getAlphaKeypoint());
                    }
                    else
                    {
                        if (scaleNetToOutput == -1.f)
                            error("Non valid scaleNetToOutput.", __LINE__, __FUNCTION__, __FILE__);
                        // Draw specific body part or bkg
                        if (elementRendered <= numberBodyPartsPlusBkg)
                        {
                            elementRenderedName = mPartIndexToName.at(elementRendered-1);
                            renderPoseHeatMapGpu(*spGpuMemory, mPoseModel, frameSize, spPoseExtractor->getHeatMapCpuConstPtr(),
                                                 mHeatMapsSize, scaleNetToOutput, elementRendered,
                                                 (mBlendOriginalFrame ? getAlphaHeatMap() : 1.f));
                        }
                        // Draw PAFs (Part Affinity Fields)
                        else if (elementRendered == numberBodyPartsPlusBkg+1)
                        {
                            elementRenderedName = "Heatmaps";
                            renderPoseHeatMapsGpu(*spGpuMemory, mPoseModel, frameSize, spPoseExtractor->getHeatMapCpuConstPtr(),
                                                  mHeatMapsSize, scaleNetToOutput, (mBlendOriginalFrame ? getAlphaHeatMap() : 1.f));
                        }
                        // Draw PAFs (Part Affinity Fields)
                        else if (elementRendered == numberBodyPartsPlusBkg+2)
                        {
                            elementRenderedName = "PAFs (Part Affinity Fields)";
                            renderPosePAFsGpu(*spGpuMemory, mPoseModel, frameSize, spPoseExtractor->getHeatMapCpuConstPtr(),
                                              mHeatMapsSize, scaleNetToOutput, (mBlendOriginalFrame ? getAlphaHeatMap() : 1.f));
                        }
                        // Draw affinity between 2 body parts
                        else
                        {
                            const auto affinityPart = (elementRendered-numberBodyPartsPlusBkg-3)*2;
                            const auto affinityPartMapped = POSE_MAP_IDX[(int)mPoseModel].at(affinityPart);
                            elementRenderedName = mPartIndexToName.at(affinityPartMapped);
                            elementRenderedName = elementRenderedName.substr(0, elementRenderedName.find("("));
                            renderPosePAFGpu(*spGpuMemory, mPoseModel, frameSize, spPoseExtractor->getHeatMapCpuConstPtr(),
                                             mHeatMapsSize, scaleNetToOutput, affinityPartMapped,
                                             (mBlendOriginalFrame ? getAlphaHeatMap() : 1.f));
                        }
                    }
                }
                // GPU memory to CPU if last renderer
                gpuToCpuMemoryIfLastRenderer(outputData.getPtr(), outputData.getVolume());
                cudaCheck(__LINE__, __FUNCTION__, __FILE__);
            // CPU_ONLY mode
            #else
                error("GPU rendering not available if `CPU_ONLY` is set.", __LINE__, __FUNCTION__, __FILE__);
                UNUSED(elementRendered);
                UNUSED(outputData);
                UNUSED(poseKeypoints);
                UNUSED(scaleNetToOutput);
            #endif
            // Return result
            return std::make_pair(elementRendered, elementRenderedName);
        }
        catch (const std::exception& e)
        {
            error(e.what(), __LINE__, __FUNCTION__, __FILE__);
            return std::make_pair(-1, "");
        }
    }
}
