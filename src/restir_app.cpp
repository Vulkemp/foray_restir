#include "restir_app.hpp"
#include <gltfconvert/foray_modelconverter.hpp>
#include <scenegraph/components/foray_camera.hpp>
#include <scenegraph/globalcomponents/foray_cameramanager.hpp>
#include <scenegraph/globalcomponents/foray_tlasmanager.hpp>
#include <scenegraph/components/foray_freecameracontroller.hpp>
#include <imgui/imgui.h>
#include <memory/foray_managedimage.hpp>
#include <vulkan/vulkan.h>
#include <utility/foray_imageloader.hpp>
#include <bench/foray_hostbenchmark.hpp>

void RestirProject::Init()
{
	foray::logger()->set_level(spdlog::level::debug);
	LoadEnvironmentMap();
	GenerateNoiseSource();
	loadScene();
	ConfigureStages();
}

void RestirProject::Update(float delta)
{
	DefaultAppBase::Update(delta);
	if (mOutputChanged)
	{
		ApplyOutput();
		mOutputChanged = false;
	}
}

void RestirProject::OnEvent(const foray::Event *event)
{
	DefaultAppBase::OnEvent(event);
	auto buttonInput = dynamic_cast<const foray::EventInputBinary *>(event);
	auto axisInput = dynamic_cast<const foray::EventInputAnalogue *>(event);
	auto windowResized = dynamic_cast<const foray::EventWindowResized *>(event);
	if (windowResized)
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
		"../Sponza/glTF/Sponza.gltf",
	});

	mScene = std::make_unique<foray::Scene>(&mContext);
	foray::ModelConverter converter(mScene.get());
	for (const auto &path : scenePaths)
	{
		converter.LoadGltfModel(foray::MakeRelativePath(path));
	}
	mScene->MakeComponent<foray::TlasManager>(&mContext)->CreateOrUpdate();

	auto cameraNode = mScene->MakeNode();

	cameraNode->MakeComponent<foray::Camera>()->InitDefault();
	cameraNode->MakeComponent<foray::FreeCameraController>();
	mScene->GetComponent<foray::CameraManager>()->RefreshCameraList();

	for (int32_t i = 0; i < scenePaths.size(); i++)
	{
		const auto &path = scenePaths[i];
		const auto &log = converter.GetBenchmark().GetLogs()[i];
		foray::logger()->info("Model Load \"{}\":\n{}", path, log.PrintPretty());
	}
}

void RestirProject::LoadEnvironmentMap()
{

	constexpr VkFormat hdrVkFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
	foray::ImageLoader<hdrVkFormat> imageLoader;
	// env maps at https://polyhaven.com/a/alps_field
	std::string pathToEnvMap = std::string(foray::CurrentWorkingDirectory()) + "/../data/textures/envmap.exr";
	if (!imageLoader.Init(pathToEnvMap))
	{
		foray::logger()->warn("Loading env map failed \"{}\"", pathToEnvMap);
		return;
	}
	if (!imageLoader.Load())
	{
		foray::logger()->warn("Loading env map failed #2 \"{}\"", pathToEnvMap);
		return;
	}

	VkExtent3D ext3D{
		.width = imageLoader.GetInfo().Extent.width,
		.height = imageLoader.GetInfo().Extent.height,
		.depth = 1,
	};

	foray::ManagedImage::CreateInfo ci("Environment map", VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, hdrVkFormat, ext3D);

	imageLoader.InitManagedImage(&mContext, &mSphericalEnvMap, ci);
	imageLoader.Destroy();
}

void RestirProject::GenerateNoiseSource()
{
	foray::HostBenchmark bench;
	bench.Begin();
	mNoiseSource.Create(&mContext);
	bench.End();
	foray::logger()->info("Create Noise Tex \n{}", bench.GetLogs().front().PrintPretty());
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

void RestirProject::OnShadersRecompiled(foray::ShaderCompiler *shaderCompiler)
{
	mGbufferStage.OnShadersRecompiled(shaderCompiler);
	mRestirStage.OnShadersRecompiled(shaderCompiler);
}

void RestirProject::PrepareImguiWindow()
{
	mImguiStage.AddWindowDraw([this]()
							  {
			ImGui::Begin("window");
			ImGui::Text("FPS: %f", mFps);

			const char* current = mCurrentOutput.data();
			if (ImGui::BeginCombo("Output", current))
			{
				std::string_view newOutput = mCurrentOutput;
				for (auto output : mOutputs)
				{
					bool selected = output.first == mCurrentOutput;
					if (ImGui::Selectable(output.first.data(), selected))
					{
						newOutput = output.first;
					}
				}

				if (newOutput != mCurrentOutput)
				{
					mCurrentOutput = newOutput;
					mOutputChanged = true;
				}

                ImGui::EndCombo();
            }

#ifdef ENABLE_GBUFFER_BENCH
            if (mDisplayedLog.Timestamps.size() > 0 && ImGui::CollapsingHeader("GBuffer Benchmark"))
            {
                mDisplayedLog.PrintImGui();
            }
#endif // ENABLE_GBUFFER_BENCH

			ImGui::End(); });
}

void RestirProject::ConfigureStages()
{
	mGbufferStage.Init(&mContext, mScene.get());
	auto albedoImage = mGbufferStage.GetColorAttachmentByName(foray::GBufferStage::Albedo);
	auto normalImage = mGbufferStage.GetColorAttachmentByName(foray::GBufferStage::WorldspaceNormal);

	mRestirStage.Init(&mContext, mScene.get(), &mSphericalEnvMap, &mNoiseSource.GetImage());
	auto rtImage = mRestirStage.GetColorAttachmentByName(foray::RaytracingStage::RaytracingRenderTargetName);

	UpdateOutputs();

	mImguiStage.Init(&mContext, mOutputs[mCurrentOutput]);
	PrepareImguiWindow();

	// �nit copy stage
	mImageToSwapchainStage.Init(&mContext, mOutputs[mCurrentOutput], foray::ImageToSwapchainStage::PostCopy{.AccessFlags = (VkAccessFlagBits::VK_ACCESS_SHADER_WRITE_BIT), .ImageLayout = (VkImageLayout::VK_IMAGE_LAYOUT_GENERAL), .QueueFamilyIndex = (mContext.QueueGraphics)});
}

void RestirProject::RecordCommandBuffer(foray::FrameRenderInfo &renderInfo)
{
	mScene->Update(renderInfo);
	mGbufferStage.RecordFrame(renderInfo);

	mRestirStage.RecordFrame(renderInfo);

	// draw imgui windows
	mImguiStage.RecordFrame(renderInfo);

	// copy final image to swapchain
	mImageToSwapchainStage.RecordFrame(renderInfo);
}

void RestirProject::QueryResultsAvailable(uint64_t frameIndex)
{
#ifdef ENABLE_GBUFFER_BENCH
	mGbufferStage.GetBenchmark().LogQueryResults(frameIndex);
	mDisplayedLog = mGbufferStage.GetBenchmark().GetLogs().back();
#endif // ENABLE_GBUFFER_BENCH
}

void RestirProject::OnResized(VkExtent2D size)
{
	mScene->InvokeOnResized(size);
	mGbufferStage.OnResized(size);
	auto albedoImage = mGbufferStage.GetColorAttachmentByName(foray::GBufferStage::Albedo);
	auto normalImage = mGbufferStage.GetColorAttachmentByName(foray::GBufferStage::WorldspaceNormal);
	mRestirStage.OnResized(size);
	auto rtImage = mRestirStage.GetColorAttachmentByName(foray::RaytracingStage::RaytracingRenderTargetName);

	UpdateOutputs();

	mImguiStage.OnResized(size, mOutputs[mCurrentOutput]);
	mImageToSwapchainStage.OnResized(size, mOutputs[mCurrentOutput]);
}

void lUpdateOutput(std::unordered_map<std::string_view, foray::ManagedImage *> &map, foray::RenderStage &stage, const std::string_view name)
{
	map[name] = stage.GetColorAttachmentByName(name);
}

void RestirProject::UpdateOutputs()
{
	mOutputs.clear();
	lUpdateOutput(mOutputs, mGbufferStage, foray::GBufferStage::Albedo);
	lUpdateOutput(mOutputs, mGbufferStage, foray::GBufferStage::WorldspacePosition);
	lUpdateOutput(mOutputs, mGbufferStage, foray::GBufferStage::WorldspaceNormal);
	lUpdateOutput(mOutputs, mGbufferStage, foray::GBufferStage::MotionVector);
	lUpdateOutput(mOutputs, mGbufferStage, foray::GBufferStage::MaterialIndex);
	lUpdateOutput(mOutputs, mGbufferStage, foray::GBufferStage::MeshInstanceIndex);
	lUpdateOutput(mOutputs, mRestirStage, foray::RaytracingStage::RaytracingRenderTargetName);

	if (mCurrentOutput.size() == 0 || !mOutputs.contains(mCurrentOutput))
	{
		if (mOutputs.size() == 0)
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