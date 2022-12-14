#pragma once

#include "configurepath.cmakegenerated.hpp"
#include <foray_exception.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <foray_glm.hpp>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "restirstage.hpp"
#include "stages/foray_gbuffer.hpp"
#include "stages/foray_imagetoswapchain.hpp"
#include "stages/foray_imguistage.hpp"
#include "stages/foray_raytracingstage.hpp"
#include <osi/foray_env.hpp>

#include <scene/foray_scenegraph.hpp>
#include <stdint.h>
#include <util/foray_noisesource.hpp>
#include <base/foray_defaultappbase.hpp>
#include "structs.hpp"

class RestirProject : public foray::base::DefaultAppBase
{
  public:
    RestirProject()  = default;
    ~RestirProject(){};

  protected:
    virtual void BeforeInstanceCreate(vkb::InstanceBuilder& instanceBuilder) override;
    virtual void BeforeDeviceBuilding(vkb::DeviceBuilder& deviceBuilder) override;
    virtual void BeforePhysicalDeviceSelection(vkb::PhysicalDeviceSelector& pds) override;

    virtual void Init() override;
    virtual void OnEvent(const foray::Event* event) override;
    virtual void Update(float delta) override;

    virtual void RecordCommandBuffer(foray::base::FrameRenderInfo& renderInfo) override;
    virtual void QueryResultsAvailable(uint64_t frameIndex) override;
    virtual void OnResized(VkExtent2D size) override;
    virtual void Destroy() override;
    virtual void OnShadersRecompiled() override;


    void PrepareImguiWindow();

    std::unique_ptr<foray::scene::Scene> mScene;

    void loadScene();
    void LoadEnvironmentMap();
    void GenerateNoiseSource();
    void CollectEmissiveTriangles();

    std::vector<shader::TriLight> mTriangleLights;
    void UploadLightsToGpu();

    /// @brief generates a GBuffer (Albedo, Positions, Normal, Motion Vectors, Mesh Instance Id as output images)
    foray::stages::GBufferStage mGbufferStage;
    /// @brief Renders immediate mode GUI
    foray::stages::ImguiStage mImguiStage;
    /// @brief Copies the intermediate rendertarget to the swapchain image
    foray::stages::ImageToSwapchainStage mImageToSwapchainStage;
    /// @brief Generates a raytraced image
    foray::RestirStage mRestirStage;

    foray::core::ManagedImage mSphericalEnvMap{};

    foray::util::NoiseSource mNoiseSource;

    void ConfigureStages();

    std::unordered_map<std::string_view, foray::core::ManagedImage*> mOutputs;
    std::string_view                                         mCurrentOutput = "";
    bool                                                     mOutputChanged = false;

#ifdef ENABLE_GBUFFER_BENCH
    foray::BenchmarkLog mDisplayedLog;
#endif  // ENABLE_GBUFFER_BENCH

    void UpdateOutputs();
    void ApplyOutput();
};

int main(int argv, char** args)
{
    foray::osi::OverrideCurrentWorkingDirectory(CWD_OVERRIDE_PATH);
    RestirProject project;
    return project.Run();
}