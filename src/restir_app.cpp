#include "restir_app.hpp"
#include <bench/foray_hostbenchmark.hpp>
#include <core/foray_managedimage.hpp>
#include <gltf/foray_modelconverter.hpp>
#include <imgui/imgui.h>
#include <scene/components/foray_camera.hpp>
#include <scene/components/foray_freecameracontroller.hpp>
#include <scene/components/foray_meshinstance.hpp>
#include <scene/foray_mesh.hpp>
#include <scene/globalcomponents/foray_cameramanager.hpp>
#include <scene/globalcomponents/foray_tlasmanager.hpp>
#include <util/foray_imageloader.hpp>
#include <vulkan/vulkan.h>

#include "structs.hpp"

void RestirProject::BeforeInstanceCreate(vkb::InstanceBuilder& instanceBuilder)
{
    /* instanceBuilder.add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT);
    instanceBuilder.enable_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    instanceBuilder.enable_extension(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);*/
}

void RestirProject::BeforeDeviceBuilding(vkb::DeviceBuilder& deviceBuilder) {}

void RestirProject::BeforePhysicalDeviceSelection(vkb::PhysicalDeviceSelector& pds)
{
    //pds.add_required_extension(VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME);
}

// And this is the callback that the validator will call
VkBool32 myDebugCallback(VkDebugReportFlagsEXT      flags,
                         VkDebugReportObjectTypeEXT objectType,
                         uint64_t                   object,
                         size_t                     location,
                         int32_t                    messageCode,
                         const char*                pLayerPrefix,
                         const char*                pMessage,
                         void*                      pUserData)
{
    if(flags & VK_DEBUG_REPORT_ERROR_BIT_EXT)
    {
        printf("debugPrintfEXT: %s", pMessage);
    }

    return false;
}

void RestirProject::Init()
{
    VkDebugReportCallbackEXT debugCallbackHandle;

    //// Populate the VkDebugReportCallbackCreateInfoEXT
    //VkDebugReportCallbackCreateInfoEXT ci = {};
    //ci.sType                              = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
    //ci.pfnCallback                        = myDebugCallback;
    //ci.flags                              = VK_DEBUG_REPORT_INFORMATION_BIT_EXT;
    //ci.pUserData                          = nullptr;

    //PFN_vkCreateDebugReportCallbackEXT pfn_vkCreateDebugReportCallbackEXT =
    //    reinterpret_cast<PFN_vkCreateDebugReportCallbackEXT>(vkGetDeviceProcAddr(mContext.Device, "vkCreateDebugReportCallbackEXT"));
    //
    //PFN_vkCreateDebugReportCallbackEXT CreateDebugReportCallback = VK_NULL_HANDLE;
    //CreateDebugReportCallback                                    = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(mContext.Instance, "vkCreateDebugReportCallbackEXT");

    //// Create the callback handle
    //CreateDebugReportCallback(mContext.Instance, &ci, nullptr, &debugCallbackHandle);


    foray::logger()->set_level(spdlog::level::debug);
    LoadEnvironmentMap();
    GenerateNoiseSource();
    loadScene();
    ConfigureStages();
    CollectEmissiveTriangles();
}

void RestirProject::Update(float delta)
{
    DefaultAppBase::Update(delta);
    if(mOutputChanged)
    {
        ApplyOutput();
        mOutputChanged = false;
    }
}

void RestirProject::OnEvent(const foray::Event* event)
{
    DefaultAppBase::OnEvent(event);
    auto buttonInput   = dynamic_cast<const foray::EventInputBinary*>(event);
    auto axisInput     = dynamic_cast<const foray::EventInputAnalogue*>(event);
    auto windowResized = dynamic_cast<const foray::EventWindowResized*>(event);
    if(windowResized)
    {
        spdlog::info("Window resized w {} h {}", windowResized->Current.Width, windowResized->Current.Height);
    }
    mScene->InvokeOnEvent(event);

    // process events for imgui
    mImguiStage.ProcessSdlEvent(&(event->RawSdlEventData));
}

void RestirProject::loadScene()
{
    std::vector<std::string> scenePaths({
        "../data/scenes/Sponza/glTF/Sponza.gltf",
        "../data/scenes/cube/cube.gltf",
    });

    mScene = std::make_unique<foray::scene::Scene>(&mContext);
    foray::gltf::ModelConverter converter(mScene.get());
    for(const auto& path : scenePaths)
    {
        converter.LoadGltfModel(foray::osi::MakeRelativePath(path));
    }
    mScene->MakeComponent<foray::scene::TlasManager>(&mContext)->CreateOrUpdate();

    auto cameraNode = mScene->MakeNode();

    cameraNode->MakeComponent<foray::scene::Camera>()->InitDefault();
    cameraNode->MakeComponent<foray::scene::FreeCameraController>();
    mScene->GetComponent<foray::scene::CameraManager>()->RefreshCameraList();

    for(int32_t i = 0; i < scenePaths.size(); i++)
    {
        const auto& path = scenePaths[i];
        const auto& log  = converter.GetBenchmark().GetLogs()[i];
        foray::logger()->info("Model Load \"{}\":\n{}", path, log.PrintPretty());
    }
}

void RestirProject::LoadEnvironmentMap()
{

    constexpr VkFormat                    hdrVkFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
    foray::util::ImageLoader<hdrVkFormat> imageLoader;
    // env maps at https://polyhaven.com/a/alps_field
    std::string pathToEnvMap = std::string(foray::osi::CurrentWorkingDirectory()) + "/../data/textures/envmap.exr";
    if(!imageLoader.Init(pathToEnvMap))
    {
        foray::logger()->warn("Loading env map failed \"{}\"", pathToEnvMap);
        return;
    }
    if(!imageLoader.Load())
    {
        foray::logger()->warn("Loading env map failed #2 \"{}\"", pathToEnvMap);
        return;
    }

    VkExtent3D ext3D{
        .width  = imageLoader.GetInfo().Extent.width,
        .height = imageLoader.GetInfo().Extent.height,
        .depth  = 1,
    };

    foray::core::ManagedImage::CreateInfo ci("Environment map", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, hdrVkFormat,
                                             ext3D);

    imageLoader.InitManagedImage(&mContext, &mSphericalEnvMap, ci);
    imageLoader.Destroy();
}

void RestirProject::GenerateNoiseSource()
{
    foray::bench::HostBenchmark bench;
    bench.Begin();
    mNoiseSource.Create(&mContext);
    bench.End();
    foray::logger()->info("Create Noise Tex \n{}", bench.GetLogs().front().PrintPretty());
}

void RestirProject::CollectEmissiveTriangles()
{
    // find cube mesh vertices and indices
    std::vector<foray::scene::Node*> nodesWithMeshInstances{};
    mScene->FindNodesWithComponent<foray::scene::MeshInstance>(nodesWithMeshInstances);
    std::vector<foray::scene::Vertex>* vertices;
    std::vector<uint32_t>*             indices;
    uint32_t                           materialIndex;
    for(foray::scene::Node* node : nodesWithMeshInstances)
    {
        foray::scene::MeshInstance* meshInstance = node->GetComponent<foray::scene::MeshInstance>();
        foray::scene::Mesh*         mesh         = meshInstance->GetMesh();
        auto                        primitives   = mesh->GetPrimitives();
        foray::logger()->info("Primitive size: {}", primitives.size());

        // our cube scene only has 1 primitve
        if(primitives.size() > 1)
            continue;

        vertices      = &primitives[0].Vertices;
        indices       = &primitives[0].Indices;
        materialIndex = primitives[0].MaterialIndex;
    }

    // create triangles from vertices & indices
    mTriangleLights.resize(indices->size() / 3);
    for(size_t i = 0; i < indices->size(); i += 3)
    {
        // collect vertices
        glm::vec3 p1_vec3 = vertices->at(indices->at(i)).Pos;
        glm::vec3 p2_vec3 = vertices->at(indices->at(i + 1)).Pos;
        glm::vec3 p3_vec3 = vertices->at(indices->at(i + 2)).Pos;

        // TODO: world matrix of primitive has to be considered for triangle position.
        shader::TriLight& triLight = mTriangleLights[i / 3];
        triLight.p1                = glm::vec4(p1_vec3, 1.0);
        triLight.p2                = glm::vec4(p2_vec3, 1.0);
        triLight.p3                = glm::vec4(p3_vec3, 1.0);

        // material index for shader lookup
        triLight.materialIndex = materialIndex;

        // compute triangle area
        glm::vec3 normal    = glm::cross(p2_vec3 - p1_vec3, p3_vec3 - p1_vec3);
        triLight.normalArea = normal.length();
    }
}

void RestirProject::UploadLightsToGpu()
{
    foray::core::ManagedBuffer mTriangleLightsBuffer;

    VkBufferUsageFlags       bufferUsage    = VkBufferUsageFlagBits::VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkDeviceSize             bufferSize     = mTriangleLights.size() * sizeof(shader::TriLight);
    VmaMemoryUsage           bufferMemUsage = VmaMemoryUsage::VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VmaAllocationCreateFlags allocFlags     = 0;
    mTriangleLightsBuffer.Create(&mContext, bufferUsage, bufferSize, bufferMemUsage, allocFlags, "TriangleLightsBuffer");
    mTriangleLightsBuffer.WriteDataDeviceLocal(mTriangleLights.data(), bufferSize);
}

void RestirProject::Destroy()
{
    vkDeviceWaitIdle(mContext.Device);
    mNoiseSource.Destroy();
    mScene->Destroy();
    mScene = nullptr;
    mGbufferStage.Destroy();
    mImguiStage.Destroy();
    mRestirStage.Destroy();
    mSphericalEnvMap.Destroy();

    DefaultAppBase::Destroy();
}

void RestirProject::OnShadersRecompiled()
{
    mGbufferStage.OnShadersRecompiled();
    mRestirStage.OnShadersRecompiled();
}

void RestirProject::PrepareImguiWindow()
{
    mImguiStage.AddWindowDraw([this]() {
        ImGui::Begin("window");
        ImGui::Text("FPS: %f", mFps);

        const char* current = mCurrentOutput.data();
        if(ImGui::BeginCombo("Output", current))
        {
            std::string_view newOutput = mCurrentOutput;
            for(auto output : mOutputs)
            {
                bool selected = output.first == mCurrentOutput;
                if(ImGui::Selectable(output.first.data(), selected))
                {
                    newOutput = output.first;
                }
            }

            if(newOutput != mCurrentOutput)
            {
                mCurrentOutput = newOutput;
                mOutputChanged = true;
            }

            ImGui::EndCombo();
        }

#ifdef ENABLE_GBUFFER_BENCH
        if(mDisplayedLog.Timestamps.size() > 0 && ImGui::CollapsingHeader("GBuffer Benchmark"))
        {
            mDisplayedLog.PrintImGui();
        }
#endif  // ENABLE_GBUFFER_BENCH

        ImGui::End();
    });
}

void RestirProject::ConfigureStages()
{
    mGbufferStage.Init(&mContext, mScene.get());
    auto albedoImage = mGbufferStage.GetColorAttachmentByName(foray::stages::GBufferStage::Albedo);
    auto normalImage = mGbufferStage.GetColorAttachmentByName(foray::stages::GBufferStage::WorldspaceNormal);

    mRestirStage.Init(&mContext, mScene.get(), &mSphericalEnvMap, &mNoiseSource.GetImage(), &mGbufferStage);
    auto rtImage = mRestirStage.GetColorAttachmentByName(foray::stages::RaytracingStage::RaytracingRenderTargetName);

    UpdateOutputs();

    mImguiStage.Init(&mContext, mOutputs[mCurrentOutput]);
    PrepareImguiWindow();

    // Init copy stage
    mImageToSwapchainStage.Init(&mContext, mOutputs[mCurrentOutput]);
}

void RestirProject::RecordCommandBuffer(foray::base::FrameRenderInfo& renderInfo)
{
    foray::core::DeviceCommandBuffer& commandBuffer = renderInfo.GetPrimaryCommandBuffer();
    commandBuffer.Begin();

    mScene->Update(renderInfo, commandBuffer);
    mGbufferStage.RecordFrame(commandBuffer, renderInfo);

    // after gbuffer stage, transform depth from attachment optimal to read optimal
    {
        foray::core::ImageLayoutCache::Barrier barrier;
        barrier.SrcAccessMask = 0;
        barrier.DstAccessMask = 0;
        barrier.NewLayout     = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        barrier.SubresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

        renderInfo.GetImageLayoutCache().CmdBarrier(commandBuffer, mGbufferStage.GetDepthBuffer(), barrier, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                                    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
    }

    mRestirStage.RecordFrame(commandBuffer, renderInfo);

    // draw imgui windows
    mImguiStage.RecordFrame(commandBuffer, renderInfo);

    // copy final image to swapchain
    mImageToSwapchainStage.RecordFrame(commandBuffer, renderInfo);

    renderInfo.GetInFlightFrame()->PrepareSwapchainImageForPresent(commandBuffer, renderInfo.GetImageLayoutCache());
    commandBuffer.Submit(mContext.QueueGraphics);
}

void RestirProject::QueryResultsAvailable(uint64_t frameIndex)
{
#ifdef ENABLE_GBUFFER_BENCH
    mGbufferStage.GetBenchmark().LogQueryResults(frameIndex);
    mDisplayedLog = mGbufferStage.GetBenchmark().GetLogs().back();
#endif  // ENABLE_GBUFFER_BENCH
}

void RestirProject::OnResized(VkExtent2D size)
{
    mScene->InvokeOnResized(size);
    mGbufferStage.OnResized(size);
    auto albedoImage = mGbufferStage.GetColorAttachmentByName(foray::stages::GBufferStage::Albedo);
    auto normalImage = mGbufferStage.GetColorAttachmentByName(foray::stages::GBufferStage::WorldspaceNormal);
    mRestirStage.OnResized(size);
    auto rtImage = mRestirStage.GetColorAttachmentByName(foray::stages::RaytracingStage::RaytracingRenderTargetName);

    UpdateOutputs();

    mImguiStage.OnResized(size, mOutputs[mCurrentOutput]);
    mImageToSwapchainStage.OnResized(size, mOutputs[mCurrentOutput]);
}

void lUpdateOutput(std::unordered_map<std::string_view, foray::core::ManagedImage*>& map, foray::stages::RenderStage& stage, const std::string_view name)
{
    map[name] = stage.GetColorAttachmentByName(name);
}

void RestirProject::UpdateOutputs()
{
    mOutputs.clear();
    lUpdateOutput(mOutputs, mGbufferStage, foray::stages::GBufferStage::Albedo);
    lUpdateOutput(mOutputs, mGbufferStage, foray::stages::GBufferStage::WorldspacePosition);
    lUpdateOutput(mOutputs, mGbufferStage, foray::stages::GBufferStage::WorldspaceNormal);
    lUpdateOutput(mOutputs, mGbufferStage, foray::stages::GBufferStage::MotionVector);
    lUpdateOutput(mOutputs, mGbufferStage, foray::stages::GBufferStage::MaterialIndex);
    lUpdateOutput(mOutputs, mGbufferStage, foray::stages::GBufferStage::MeshInstanceIndex);
    lUpdateOutput(mOutputs, mRestirStage, foray::stages::RaytracingStage::RaytracingRenderTargetName);

    if(mCurrentOutput.size() == 0 || !mOutputs.contains(mCurrentOutput))
    {
        if(mOutputs.size() == 0)
        {
            mCurrentOutput = "";
        }
        else
        {
            mCurrentOutput = mOutputs.begin()->first;
        }
    }
}

void RestirProject::ApplyOutput()
{
    vkDeviceWaitIdle(mContext.Device);
    auto output = mOutputs[mCurrentOutput];
    mImguiStage.SetTargetImage(output);
    mImageToSwapchainStage.SetTargetImage(output);
}