/* Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 the "License";
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "nerf.h"
#include "glm/gtx/matrix_decompose.hpp"
#include "gltf_loader.h"
#include "platform/filesystem.h"
#include "platform/platform.h"
#include "rendering/subpasses/forward_subpass.h"
#include "scene_graph/components/material.h"
#include "scene_graph/components/mesh.h"
#include "scene_graph/components/perspective_camera.h"
#include "stb_image.h"

namespace
{
constexpr uint32_t MIN_THREAD_COUNT = 1;
struct RequestFeature
{
	vkb::PhysicalDevice &gpu;
	explicit RequestFeature(vkb::PhysicalDevice &gpu) :
	    gpu(gpu)
	{}

	template <typename T>
	RequestFeature &request(VkStructureType s_type, VkBool32 T::*member)
	{
		auto &member_feature   = gpu.request_extension_features<T>(s_type);
		member_feature.*member = VK_TRUE;
		return *this;
	}
};

template <typename T>
struct CopyBuffer
{
	std::vector<T> operator()(std::unordered_map<std::string, vkb::core::Buffer> &buffers, const char *buffer_name)
	{
		auto iter = buffers.find(buffer_name);
		if (iter == buffers.cend())
		{
			return {};
		}
		auto &buffer = iter->second;

		std::vector<T> out;

		const size_t sz = buffer.get_size();
		out.resize(sz / sizeof(T));
		const bool already_mapped = buffer.get_data() != nullptr;
		if (!already_mapped)
		{
			buffer.map();
		}
		memcpy(&out[0], buffer.get_data(), sz);
		if (!already_mapped)
		{
			buffer.unmap();
		}
		return out;
	}
};

void camera_set_look_at(vkb::Camera &camera, const glm::vec3 look, const glm::vec3 up)
{
	auto view_matrix = glm::lookAt(camera.position, look, up);

	glm::vec3 scale;
	glm::quat orientation;
	glm::vec3 translation;
	glm::vec3 skew;
	glm::vec4 perspective;
	glm::decompose(view_matrix, scale, orientation, translation, skew, perspective);

	camera.set_rotation(glm::eulerAngles(orientation) * glm::pi<float>() / 180.f);
	camera.set_position(translation);
}

}        // namespace

Nerf::Nerf()
{
	title = "NeRF";
	// SPIRV 1.4 requires Vulkan 1.1
	set_api_version(VK_API_VERSION_1_1);
	add_device_extension(VK_KHR_SPIRV_1_4_EXTENSION_NAME);
	// Required by VK_KHR_spirv_1_4
	add_device_extension(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
}

Nerf::~Nerf()
{
	if (device)
	{
		if (render_pass_nerf)
		{
			vkDestroyRenderPass(device->get_handle(), render_pass_nerf, nullptr);
		}

		for (uint32_t i = 0; i < nerf_framebuffers.size(); i++)
		{
			if (nerf_framebuffers[i])
			{
				vkDestroyFramebuffer(device->get_handle(), nerf_framebuffers[i], nullptr);
			}
		}

		auto device_ptr = device->get_handle();

		for (auto &model : models)
		{
			model.vertex_buffer.reset();
			model.index_buffer.reset();

			vkDestroySampler(get_device().get_handle(), model.texture_input_0.sampler, nullptr);
			vkDestroyImageView(get_device().get_handle(), model.texture_input_0.view, nullptr);
			vkDestroyImage(get_device().get_handle(), model.texture_input_0.image, nullptr);
			vkFreeMemory(get_device().get_handle(), model.texture_input_0.memory, nullptr);

			vkDestroySampler(get_device().get_handle(), model.texture_input_1.sampler, nullptr);
			vkDestroyImageView(get_device().get_handle(), model.texture_input_1.view, nullptr);
			vkDestroyImage(get_device().get_handle(), model.texture_input_1.image, nullptr);
			vkFreeMemory(get_device().get_handle(), model.texture_input_1.memory, nullptr);

			vkDestroyPipeline(device_ptr, model.pipeline_first_pass, nullptr);
		}

		for (auto &weights_buffer : weights_buffers)
			weights_buffer.reset();

		for (auto &uniform_buffer : uniform_buffers)
			uniform_buffer.reset();

		vkDestroyPipelineLayout(device_ptr, pipeline_first_pass_layout, nullptr);
		vkDestroyDescriptorSetLayout(device_ptr, descriptor_set_first_pass_layout, nullptr);

		if (pipeline_baseline)
		{
			vkDestroyPipeline(get_device().get_handle(), pipeline_baseline, nullptr);
			vkDestroyPipelineLayout(get_device().get_handle(), pipeline_layout_baseline, nullptr);
			vkDestroyDescriptorSetLayout(get_device().get_handle(), descriptor_set_layout_baseline, nullptr);
		}

		for (auto attachment : frameAttachments)
		{
			vkDestroySampler(get_device().get_handle(), attachment.feature_0.sampler, nullptr);
			vkDestroyImageView(get_device().get_handle(), attachment.feature_0.view, nullptr);
			vkDestroyImage(get_device().get_handle(), attachment.feature_0.image, nullptr);
			vkFreeMemory(get_device().get_handle(), attachment.feature_0.memory, nullptr);

			vkDestroySampler(get_device().get_handle(), attachment.feature_1.sampler, nullptr);
			vkDestroyImageView(get_device().get_handle(), attachment.feature_1.view, nullptr);
			vkDestroyImage(get_device().get_handle(), attachment.feature_1.image, nullptr);
			vkFreeMemory(get_device().get_handle(), attachment.feature_1.memory, nullptr);

			vkDestroySampler(get_device().get_handle(), attachment.feature_2.sampler, nullptr);
			vkDestroyImageView(get_device().get_handle(), attachment.feature_2.view, nullptr);
			vkDestroyImage(get_device().get_handle(), attachment.feature_2.image, nullptr);
			vkFreeMemory(get_device().get_handle(), attachment.feature_2.memory, nullptr);
		}
	}
}

void Nerf::read_json_map()
{
	std::string assetBase = vkb::fs::path::get(vkb::fs::path::Type::Assets);
	LOGI("Base assets path: {}", assetBase);

#if defined(NERF_JSON_FILE)
	const std::string nerf_obj_map = assetBase + "scenes/mobile_nerf_models.json";

	std::ifstream f(nerf_obj_map);

	if (!f)
	{
		LOGE("Failed to open nerf obj map data");
		assert(0);
	}

	LOGI("Parsing nerf obj map data {}", nerf_obj_map);

	json raw_asset_map = json::parse(f);
#else

	const std::string nerf_obj_json =
	    R"V0G0N(
        {
            "width": 0, 

            "height": 0, 
    
            "texture_type": "8bit",

            "target_model": "lego_combo",

            "deferred": false,

            "rotation": true,

            "lego_ball":{
                "path": "scenes/morpheus_team/lego_ball_phone/",
                "num_sub_model": 1,
                "original": false,
                "camera": [-1, 1, 1],
                "instancing":{
                    "dim": [1, 1, 1],
                    "interval": [2.0, 2.0, 2.0]
                }
            },

            "lego_boba_fett":{
                "path": "scenes/morpheus_team/lego_boba_fett_phone/",
                "num_sub_model": 1,
                "original": false,
                "camera": [-1, 1, 1],
                "instancing":{
                    "dim": [1, 1, 1],
                    "interval": [2.0, 2.0, 2.0]
                }
            },

            "lego_monster_truck":{
                "path": "scenes/morpheus_team/lego_monster_truck_phone/",
                "num_sub_model": 1,
                "original": false,
                "camera": [-1, 1, 1],
                "instancing":{
                    "dim": [1, 1, 1],
                    "interval": [2.0, 2.0, 2.0]
                }
            },

            "lego_tractor":{
                "path": "scenes/morpheus_team/lego_tractor_phone/",
                "num_sub_model": 1,
                "original": false,
                "camera": [-1, 1, 1],
                "instancing":{
                    "dim": [1, 1, 1],
                    "interval": [2.0, 2.0, 2.0]
                }
            },

            "lego_combo":{
                "combo": true,
                "models": ["scenes/morpheus_team/lego_ball_phone/", "scenes/morpheus_team/lego_boba_fett_phone/", 
                            "scenes/morpheus_team/lego_monster_truck_phone/", "scenes/morpheus_team/lego_tractor_phone/"],
                "original": [false, false, false, false],
                "camera": [-0.0381453, 1.84186, -1.51744],
                "instancing":{
                    "dim": [2, 2, 2],
                    "interval": [1.5, 1.5, 1.5]
                }
            }
        }
        )V0G0N";

	json raw_asset_map = json::parse(nerf_obj_json);

#endif

	std::string target_model = raw_asset_map["target_model"].get<std::string>();
	asset_map                = raw_asset_map[target_model];

	// Load combo models or a single model
	if (!asset_map["combo"].is_null())
		combo_mode = asset_map["combo"].get<bool>();
	else
		combo_mode = false;

	if (combo_mode)
	{
		model_path.resize(asset_map["models"].size());
		using_original_nerf_models.resize(asset_map["models"].size());

		for (int i = 0; i < model_path.size(); i++)
		{
			model_path[i]                 = asset_map["models"][i].get<std::string>();
			using_original_nerf_models[i] = asset_map["original"][i].get<bool>();
			LOGI("Target model: {}, asset path: {}", target_model, model_path[i]);
		}
	}
	else
	{
		model_path.resize(1);
		model_path[0] = asset_map["path"].get<std::string>();
		using_original_nerf_models.resize(1);
		using_original_nerf_models[0] = asset_map["original"].get<bool>();
		LOGI("Target model: {}, asset path: {}", target_model, model_path[0]);
	}

	std::string textureType = raw_asset_map["texture_type"].get<std::string>();

	if (textureType == "8bit")
	{
		LOGI("Using VK_FORMAT_R8G8B8A8_UNORM for feature texture");
		feature_map_format = VK_FORMAT_R8G8B8A8_UNORM;
	}
	else if (textureType == "16bit")
	{
		LOGI("Using VK_FORMAT_R16G16B16A16_SFLOAT for feature texture");
		feature_map_format = VK_FORMAT_R16G16B16A16_SFLOAT;
	}
	else if (textureType == "32bit")
	{
		LOGI("Using VK_FORMAT_R32G32B32A32_SFLOAT for feature texture");
		feature_map_format = VK_FORMAT_R32G32B32A32_SFLOAT;
	}
	else if (textureType == "8bit")
	{
		LOGI("Using VK_FORMAT_R8G8B8A8_UNORM for feature texture");
		feature_map_format = VK_FORMAT_R8G8B8A8_UNORM;
	}
	else
	{
		LOGW("Unrecognized feature texture type, using VK_FORMAT_R32G32B32A32_SFLOAT");
		feature_map_format = VK_FORMAT_R32G32B32A32_SFLOAT;
	}
	use_deferred = raw_asset_map["deferred"].get<bool>();
	do_rotation  = raw_asset_map["rotation"].get<bool>();

	view_port_width  = raw_asset_map["width"].get<int>();
	view_port_height = raw_asset_map["height"].get<int>();

	if (asset_map["camera"].is_array() && asset_map["camera"].size() == 3)
	{
		camera_pos = glm::vec3(asset_map["camera"][0].get<float>(), asset_map["camera"][1].get<float>(), asset_map["camera"][2].get<float>());
	}
	else
	{
		LOGW("Fail to read camera position. Use defualt value.");
	}

	json instacing_map = asset_map["instancing"];
	if (instacing_map["dim"].is_array() && instacing_map["dim"].size() == 3)
	{
		instancing_info.dim = glm::vec3(instacing_map["dim"][0].get<int>(), instacing_map["dim"][1].get<int>(), instacing_map["dim"][2].get<int>());
	}
	else
	{
		LOGE("Wrong instancing dimension. Terminating...");
		exit(1);
	}

	if (instacing_map["interval"].is_array() && instacing_map["interval"].size() == 3)
	{
		instancing_info.interval = glm::vec3(instacing_map["interval"][0].get<float>(), instacing_map["interval"][1].get<float>(), instacing_map["interval"][2].get<float>());
	}
	else
	{
		LOGE("Wrong instancing interval. Terminating...");
		exit(1);
	}

	if (instancing_info.dim.x <= 0 || instancing_info.dim.y <= 0 || instancing_info.dim.z <= 0 || instancing_info.interval.x <= 0.f || instancing_info.interval.y <= 0.f || instancing_info.interval.z <= 0.f)
	{
		LOGE("Instancing settings must be positive. Terminating...");
		exit(1);
	}
}

void Nerf::load_shaders()
{
	// Loading first pass shaders
	if (use_deferred)
	{
		// Loading first pass shaders
		shader_stages_first_pass[0] = load_shader("nerf/raster.vert", VK_SHADER_STAGE_VERTEX_BIT);
		shader_stages_first_pass[1] = load_shader(
		    using_original_nerf_models[0] ? "nerf/raster.frag" : "nerf/raster_morpheus.frag",
		    VK_SHADER_STAGE_FRAGMENT_BIT);

		// Loading second pass shaders
		shader_stages_second_pass[0] = load_shader("nerf/quad.vert", VK_SHADER_STAGE_VERTEX_BIT);
		shader_stages_second_pass[1] = load_shader(
		    using_original_nerf_models[0] ? "nerf/mlp.frag" : "nerf/mlp_morpheus.frag",
		    VK_SHADER_STAGE_FRAGMENT_BIT);
	}
	else
	{
		// Loading one pass shaders
		shader_stages_first_pass[0] = load_shader("nerf/raster.vert", VK_SHADER_STAGE_VERTEX_BIT);
		shader_stages_first_pass[1] = load_shader(
		    using_original_nerf_models[0] ? "nerf/merged.frag" : "nerf/merged_morpheus.frag",
		    VK_SHADER_STAGE_FRAGMENT_BIT);
	}
}

bool Nerf::prepare(const vkb::ApplicationOptions &options)
{
	read_json_map();

	// Load the mlp for each model
	mlp_weight_vector.resize(model_path.size());

	for (int i = 0; i < model_path.size(); i++)
	{
		initialize_mlp_uniform_buffers(i);
	}

	if (!ApiVulkanSample::prepare(options))
	{
		return false;
	}

	if (view_port_width == 0 || view_port_height == 0)
	{
		view_port_width        = width;
		view_port_height       = height;
		use_native_screen_size = true;
	}

	load_shaders();

	if (use_deferred)
	{
		update_render_pass_nerf_baseline();
	}
	else
	{
		update_render_pass_nerf_forward();
	}

	setup_nerf_framebuffer_baseline();
	// Because we have our own customized render pass, the UI render pass need to be updated with load on load so it won't
	// clear out the written color attachment
	update_render_pass_flags(RenderPassCreateFlags::ColorAttachmentLoad);

	camera.type  = vkb::CameraType::LookAt;
	camera_pos.y = -camera_pos.y;        // flip y to keep consistency of the init pos between rayquery and rasterization
	camera.set_position(camera_pos);
	camera_set_look_at(camera, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));

	camera.set_perspective(60.0f, (float) width / (float) height, 0.01f, 256.0f);

	int models_entry = 0;

	for (int model_index = 0; model_index < model_path.size(); model_index++)
	{
		int num_sub_model = models[models_entry].sub_model_num;

		for (int sub_model_index = 0; sub_model_index < num_sub_model; sub_model_index++)
		{
			load_scene(model_index, sub_model_index, models_entry);
			create_texture(model_index, sub_model_index, models_entry);
			create_static_object_buffers(model_index, sub_model_index, models_entry);
			models_entry++;
		}
	}
	create_uniforms();
	prepare_instance_data();
	create_pipeline_layout_fist_pass();

	if (use_deferred)
	{
		create_pipeline_layout_baseline();
	}
	create_descriptor_pool();

	for (auto &model : models)
	{
		create_descriptor_sets_first_pass(model);
	}

	if (use_deferred)
	{
		create_descriptor_sets_baseline();
	}
	prepare_pipelines();
	build_command_buffers();

	prepared = true;
	LOGI("Prepare Done!");
	return true;
}

bool Nerf::resize(const uint32_t width, const uint32_t height)
{
	ApiVulkanSample::resize(width, height);
	rebuild_command_buffers();
	return true;
}

void Nerf::request_gpu_features(vkb::PhysicalDevice &gpu)
{
}

void Nerf::render(float delta_time)
{
	if (!prepared)
	{
		return;
	}
	draw();
	update_uniform_buffers();
}

inline uint32_t aligned_size(uint32_t value, uint32_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

void Nerf::setup_attachment(VkFormat format, VkImageUsageFlags usage, FrameBufferAttachment &attachment)
{
	if (attachment.image != VK_NULL_HANDLE)
	{
		vkDestroySampler(get_device().get_handle(), attachment.sampler, nullptr);
		vkDestroyImageView(get_device().get_handle(), attachment.view, nullptr);
		vkDestroyImage(get_device().get_handle(), attachment.image, nullptr);
		vkFreeMemory(get_device().get_handle(), attachment.memory, nullptr);
	}

	attachment.format = format;
	attachment.width  = get_render_context().get_surface_extent().width;
	attachment.height = get_render_context().get_surface_extent().height;

	VkImageAspectFlags aspectMask = 0;
	VkImageLayout      imageLayout;

	attachment.format = format;

	if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
	{
		aspectMask  = VK_IMAGE_ASPECT_COLOR_BIT;
		imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}
	if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
	{
		aspectMask  = VK_IMAGE_ASPECT_DEPTH_BIT;
		imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	}

	VkImageCreateInfo image = vkb::initializers::image_create_info();
	image.imageType         = VK_IMAGE_TYPE_2D;
	image.format            = attachment.format;
	image.extent.width      = attachment.width;
	image.extent.height     = attachment.height;
	image.extent.depth      = 1;
	image.mipLevels         = 1;
	image.arrayLayers       = 1;
	image.samples           = VK_SAMPLE_COUNT_1_BIT;
	image.tiling            = VK_IMAGE_TILING_OPTIMAL;
	image.usage             = usage;
	image.initialLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
	VK_CHECK(vkCreateImage(get_device().get_handle(), &image, nullptr, &attachment.image));

	VkMemoryRequirements memory_requirements;
	vkGetImageMemoryRequirements(get_device().get_handle(), attachment.image, &memory_requirements);
	VkMemoryAllocateInfo memory_allocate_info = vkb::initializers::memory_allocate_info();
	memory_allocate_info.allocationSize       = memory_requirements.size;
	memory_allocate_info.memoryTypeIndex      = get_device().get_memory_type(memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VK_CHECK(vkAllocateMemory(get_device().get_handle(), &memory_allocate_info, nullptr, &attachment.memory));
	VK_CHECK(vkBindImageMemory(get_device().get_handle(), attachment.image, attachment.memory, 0));

	VkImageViewCreateInfo color_image_view           = vkb::initializers::image_view_create_info();
	color_image_view.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
	color_image_view.format                          = format;
	color_image_view.subresourceRange                = {};
	color_image_view.subresourceRange.aspectMask     = aspectMask;
	color_image_view.subresourceRange.baseMipLevel   = 0;
	color_image_view.subresourceRange.levelCount     = 1;
	color_image_view.subresourceRange.baseArrayLayer = 0;
	color_image_view.subresourceRange.layerCount     = 1;
	color_image_view.image                           = attachment.image;
	VK_CHECK(vkCreateImageView(get_device().get_handle(), &color_image_view, nullptr, &attachment.view));

	VkCommandBuffer command_buffer = get_device().create_command_buffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
	vkb::image_layout_transition(command_buffer, attachment.image,
	                             VK_IMAGE_LAYOUT_UNDEFINED,
	                             VK_IMAGE_LAYOUT_GENERAL,
	                             {aspectMask, 0, 1, 0, 1});
	get_device().flush_command_buffer(command_buffer, queue);

	VkSamplerCreateInfo samplerCreateInfo = {};

	samplerCreateInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerCreateInfo.magFilter    = VK_FILTER_LINEAR;
	samplerCreateInfo.minFilter    = VK_FILTER_LINEAR;
	samplerCreateInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerCreateInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCreateInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCreateInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCreateInfo.minLod       = 0;
	samplerCreateInfo.maxLod       = 16.f;

	VK_CHECK(vkCreateSampler(get_device().get_handle(), &samplerCreateInfo, 0, &attachment.sampler));
}

void Nerf::setup_nerf_framebuffer_baseline()
{
	if (use_deferred)
	{
		frameAttachments.resize(render_context->get_render_frames().size());

		for (auto i = 0; i < frameAttachments.size(); i++)
		{
			setup_attachment(feature_map_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, frameAttachments[i].feature_0);
			setup_attachment(feature_map_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, frameAttachments[i].feature_1);
			setup_attachment(VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, frameAttachments[i].feature_2);
		}
	}

	// Delete existing frame buffers
	if (nerf_framebuffers.size() > 0)
	{
		for (uint32_t i = 0; i < nerf_framebuffers.size(); i++)
		{
			if (nerf_framebuffers[i] != VK_NULL_HANDLE)
			{
				vkDestroyFramebuffer(device->get_handle(), nerf_framebuffers[i], nullptr);
			}
		}
	}

	std::vector<VkImageView> views;

	if (use_deferred)
	{
		views.resize(5);
		views[3] = depth_stencil.view;
	}
	else
	{
		views.resize(2);
		views[0] = depth_stencil.view;
	}

	// Depth/Stencil attachment is the same for all frame buffers

	VkFramebufferCreateInfo framebuffer_create_info = {};
	framebuffer_create_info.sType                   = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer_create_info.pNext                   = NULL;
	framebuffer_create_info.renderPass              = render_pass_nerf;
	framebuffer_create_info.attachmentCount         = views.size();
	framebuffer_create_info.pAttachments            = views.data();
	framebuffer_create_info.width                   = get_render_context().get_surface_extent().width;
	framebuffer_create_info.height                  = get_render_context().get_surface_extent().height;
	framebuffer_create_info.layers                  = 1;

	nerf_framebuffers.resize(swapchain_buffers.size());

	for (uint32_t i = 0; i < nerf_framebuffers.size(); i++)
	{
		if (use_deferred)
		{
			views[0] = frameAttachments[i].feature_0.view;
			views[1] = frameAttachments[i].feature_1.view;
			views[2] = frameAttachments[i].feature_2.view;
			views[4] = swapchain_buffers[i].view;
		}
		else
		{
			views[1] = swapchain_buffers[i].view;
		}

		VK_CHECK(vkCreateFramebuffer(device->get_handle(), &framebuffer_create_info, nullptr, &nerf_framebuffers[i]));
	}
}

void Nerf::update_descriptor_sets_baseline()
{
	for (int i = 0; i < nerf_framebuffers.size(); i++)
	{
		std::array<VkDescriptorImageInfo, 3> attachment_input_descriptors;

		attachment_input_descriptors[0].sampler     = VK_NULL_HANDLE;
		attachment_input_descriptors[0].imageView   = frameAttachments[i].feature_0.view;
		attachment_input_descriptors[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		attachment_input_descriptors[1].sampler     = VK_NULL_HANDLE;
		attachment_input_descriptors[1].imageView   = frameAttachments[i].feature_1.view;
		attachment_input_descriptors[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		attachment_input_descriptors[2].sampler     = VK_NULL_HANDLE;
		attachment_input_descriptors[2].imageView   = frameAttachments[i].feature_2.view;
		attachment_input_descriptors[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkWriteDescriptorSet texture_input_write_0 = vkb::initializers::write_descriptor_set(descriptor_set_baseline[i], VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 0, &attachment_input_descriptors[0]);
		VkWriteDescriptorSet texture_input_write_1 = vkb::initializers::write_descriptor_set(descriptor_set_baseline[i], VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, &attachment_input_descriptors[1]);
		VkWriteDescriptorSet texture_input_write_2 = vkb::initializers::write_descriptor_set(descriptor_set_baseline[i], VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 2, &attachment_input_descriptors[2]);

		std::vector<VkWriteDescriptorSet> write_descriptor_sets = {
		    texture_input_write_0,
		    texture_input_write_1,
		    texture_input_write_2};

		vkUpdateDescriptorSets(get_device().get_handle(), static_cast<uint32_t>(write_descriptor_sets.size()), write_descriptor_sets.data(), 0, VK_NULL_HANDLE);
	}
}

void Nerf::build_command_buffers()
{
	if (use_native_screen_size)
	{
		view_port_height = height;
		view_port_width  = width;
	}
	build_command_buffers_baseline();
}

void Nerf::build_command_buffers_baseline()
{
	// In case the screen is resized, need to update the storage image size and descriptor set
	// Note that the texture_rendered image has already been recreated at this point
	if (!prepared)
	{
		setup_nerf_framebuffer_baseline();

		if (use_deferred)
		{
			update_descriptor_sets_baseline();
		}
	}

	VkCommandBufferBeginInfo  command_buffer_begin_info = vkb::initializers::command_buffer_begin_info();
	std::vector<VkClearValue> clear_values;

	if (use_deferred)
	{
		clear_values.resize(5);
		clear_values[0].color        = {{0.025f, 0.025f, 0.025f, 0.5f}};        // default_clear_color;
		clear_values[1].color        = {{0.025f, 0.025f, 0.025f, 0.5f}};        // default_clear_color;
		clear_values[2].color        = {{0.025f, 0.025f, 0.025f, 0.5f}};        // default_clear_color;
		clear_values[3].depthStencil = {1.0f, 0};
		clear_values[4].color        = {{1.0f, 1.0f, 1.0f, 0.5f}};        // default_clear_color;
	}
	else
	{
		clear_values.resize(2);
		clear_values[0].depthStencil = {1.0f, 0};
		clear_values[1].color        = {{0.0f, 0.0f, 0.0f, 1.0f}};        // let's use this to distinguish forward rendering and deferred renderding
	}

	VkRenderPassBeginInfo render_pass_begin_info    = vkb::initializers::render_pass_begin_info();
	render_pass_begin_info.renderPass               = render_pass_nerf;
	render_pass_begin_info.renderArea.offset.x      = 0;
	render_pass_begin_info.renderArea.offset.y      = 0;
	render_pass_begin_info.renderArea.extent.width  = width;
	render_pass_begin_info.renderArea.extent.height = height;
	render_pass_begin_info.clearValueCount          = clear_values.size();
	render_pass_begin_info.pClearValues             = clear_values.data();

	VkClearValue clear_values_UI[2];
	clear_values_UI[0].color        = default_clear_color;
	clear_values_UI[1].depthStencil = {1.0f, 0};

	VkRenderPassBeginInfo render_pass_begin_info_UI    = vkb::initializers::render_pass_begin_info();
	render_pass_begin_info_UI.renderPass               = render_pass;
	render_pass_begin_info_UI.renderArea.offset.x      = 0;
	render_pass_begin_info_UI.renderArea.offset.y      = 0;
	render_pass_begin_info_UI.renderArea.extent.width  = width;
	render_pass_begin_info_UI.renderArea.extent.height = height;
	render_pass_begin_info_UI.clearValueCount          = 2;
	render_pass_begin_info_UI.pClearValues             = clear_values_UI;

	VkImageSubresourceRange subresource_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

	for (size_t i = 0; i < draw_cmd_buffers.size(); ++i)
	{
		render_pass_begin_info.framebuffer = nerf_framebuffers[i];

		VK_CHECK(vkBeginCommandBuffer(draw_cmd_buffers[i], &command_buffer_begin_info));

		vkCmdBeginRenderPass(draw_cmd_buffers[i], &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);

		// First sub pass
		// Fills the attachments

		VkViewport viewport = vkb::initializers::viewport((float) width, (float) height, 0.0f, 1.0f);
		const auto scissor  = vkb::initializers::rect2D(static_cast<int32_t>(width), static_cast<int32_t>(height), 0, 0);
		vkCmdSetViewport(draw_cmd_buffers[i], 0, 1, &viewport);
		vkCmdSetScissor(draw_cmd_buffers[i], 0, 1, &scissor);

		auto &ii = instancing_info;
		for (auto &model : models)
		{
			vkCmdBindPipeline(draw_cmd_buffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, model.pipeline_first_pass);
			// If deferred, only use the first descriptor bounded with the model
			// If forward, each model has the swapchan number of descriptor
			int descriptorIndex = use_deferred ? 0 : i;
			vkCmdBindDescriptorSets(draw_cmd_buffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_first_pass_layout,
			                        0, 1, &model.descriptor_set_first_pass[descriptorIndex], 0, nullptr);
			VkDeviceSize offsets[1] = {0};
			vkCmdBindVertexBuffers(draw_cmd_buffers[i], 0, 1, model.vertex_buffer->get(), offsets);
			vkCmdBindVertexBuffers(draw_cmd_buffers[i], 1, 1, instance_buffer->get(), offsets);
			vkCmdBindIndexBuffer(draw_cmd_buffers[i], model.index_buffer->get_handle(), 0, VK_INDEX_TYPE_UINT32);
			vkCmdDrawIndexed(draw_cmd_buffers[i], static_cast<uint32_t>(model.indices.size()) * 3, ii.dim.x * ii.dim.y * ii.dim.z, 0, 0, 0);
		}

		if (use_deferred)
		{
			// Second sub pass
			// Render a full screen quad, reading from the previously written attachments via input attachments

			vkCmdNextSubpass(draw_cmd_buffers[i], VK_SUBPASS_CONTENTS_INLINE);
			vkCmdBindPipeline(draw_cmd_buffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_baseline);
			vkCmdBindDescriptorSets(draw_cmd_buffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_baseline, 0, 1, &descriptor_set_baseline[i], 0, NULL);
			vkCmdDraw(draw_cmd_buffers[i], 3, 1, 0, 0);

			vkCmdEndRenderPass(draw_cmd_buffers[i]);
		}
		else
		{
			vkCmdEndRenderPass(draw_cmd_buffers[i]);
		}

		// Render UI

		render_pass_begin_info_UI.framebuffer = framebuffers[i];

		vkCmdBeginRenderPass(draw_cmd_buffers[i], &render_pass_begin_info_UI, VK_SUBPASS_CONTENTS_INLINE);
		draw_ui(draw_cmd_buffers[i]);
		vkCmdEndRenderPass(draw_cmd_buffers[i]);

		VK_CHECK(vkEndCommandBuffer(draw_cmd_buffers[i]));
	}
}

void Nerf::load_scene(int model_index, int sub_model_index, int models_entry)
{
	Model &model = models[models_entry];

	vkb::GLTFLoader loader{*device};
	int             total_sub_sub_model = using_original_nerf_models[model_index] ? 8 : 1;

	for (int sub_model = 0; sub_model < total_sub_sub_model; sub_model++)
	{
		std::string inputfile(model_path[model_index] + "shape" + std::to_string(sub_model_index));

		if (total_sub_sub_model > 1)
			inputfile += ("_" + std::to_string(sub_model) + ".gltf");
		else
			inputfile += (".gltf");

		LOGI("Parsing nerf obj {}", inputfile);

		auto scene = loader.read_scene_from_file(inputfile);

		for (auto &&mesh : scene->get_components<vkb::sg::Mesh>())
		{
			for (auto &&sub_mesh : mesh->get_submeshes())
			{
				auto       pts_               = CopyBuffer<glm::vec3>{}(sub_mesh->vertex_buffers, "position");
				const auto texcoord_          = CopyBuffer<glm::vec2>{}(sub_mesh->vertex_buffers, "texcoord_0");
				const auto vertex_start_index = static_cast<uint32_t>(model.vertices.size());

				// Copy vertex data
				{
					model.vertices.resize(vertex_start_index + pts_.size());
					for (size_t i = 0; i < pts_.size(); ++i)
					{
						model.vertices[vertex_start_index + i].position  = pts_[i];
						model.vertices[vertex_start_index + i].tex_coord = glm::vec2(texcoord_[i].x, 1.0f - texcoord_[i].y);
					}
				}

				// Copy index data
				{
					auto index_buffer_ = sub_mesh->index_buffer.get();
					if (index_buffer_)
					{
						assert(sub_mesh->index_type == VkIndexType::VK_INDEX_TYPE_UINT32);
						const size_t sz                   = index_buffer_->get_size();
						const size_t nTriangles           = sz / sizeof(uint32_t) / 3;
						const auto   triangle_start_index = static_cast<uint32_t>(model.indices.size());
						model.indices.resize(triangle_start_index + nTriangles);
						auto ptr = index_buffer_->get_data();
						assert(!!ptr);
						std::vector<uint32_t> tempBuffer(nTriangles * 3);
						memcpy(&tempBuffer[0], ptr, sz);
						for (size_t i = 0; i < nTriangles; ++i)
						{
							model.indices[triangle_start_index + i] = {vertex_start_index + uint32_t(tempBuffer[3 * i]),
							                                           vertex_start_index + uint32_t(tempBuffer[3 * i + 1]),
							                                           vertex_start_index + uint32_t(tempBuffer[3 * i + 2])};
						}
					}
				}
			}
		}
	}
}

void Nerf::create_descriptor_pool()
{
	if (use_deferred)
	{
		std::vector<VkDescriptorPoolSize> pool_sizes = {
		    // First Pass
		    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * (uint32_t) models.size()},
		    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 * (uint32_t) models.size()},
		    // Second Pass
		    {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 3 * (uint32_t) framebuffers.size()},
		    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 * (uint32_t) framebuffers.size()}};

		VkDescriptorPoolCreateInfo descriptor_pool_create_info = vkb::initializers::descriptor_pool_create_info(pool_sizes, models.size() + framebuffers.size());
		VK_CHECK(vkCreateDescriptorPool(get_device().get_handle(), &descriptor_pool_create_info, nullptr, &descriptor_pool));
	}
	else
	{
		std::vector<VkDescriptorPoolSize> pool_sizes = {
		    // First Pass
		    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * (uint32_t) models.size() * (uint32_t) framebuffers.size()},
		    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 * (uint32_t) models.size() * (uint32_t) framebuffers.size()},
		    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 * (uint32_t) models.size() * (uint32_t) framebuffers.size()}};

		VkDescriptorPoolCreateInfo descriptor_pool_create_info = vkb::initializers::descriptor_pool_create_info(pool_sizes, models.size() * framebuffers.size());
		VK_CHECK(vkCreateDescriptorPool(get_device().get_handle(), &descriptor_pool_create_info, nullptr, &descriptor_pool));
	}
}

void Nerf::create_pipeline_layout_fist_pass()
{
	// First Pass Descriptor set and layout

	std::vector<VkDescriptorSetLayoutBinding> set_layout_bindings = {
	    vkb::initializers::descriptor_set_layout_binding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 0),
	    vkb::initializers::descriptor_set_layout_binding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 1),
	    vkb::initializers::descriptor_set_layout_binding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT, 2)};

	// If use forward, add uniform buffer descriptor for the weights
	if (!use_deferred)
	{
		set_layout_bindings.push_back(vkb::initializers::descriptor_set_layout_binding(
		    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT, 3));
	}

	VkDescriptorSetLayoutCreateInfo descriptor_layout = vkb::initializers::descriptor_set_layout_create_info(set_layout_bindings.data(), static_cast<uint32_t>(set_layout_bindings.size()));
	VK_CHECK(vkCreateDescriptorSetLayout(get_device().get_handle(), &descriptor_layout, nullptr, &descriptor_set_first_pass_layout));

	VkPipelineLayoutCreateInfo pipeline_layout_create_info =
	    vkb::initializers::pipeline_layout_create_info(
	        &descriptor_set_first_pass_layout,
	        1);

	VK_CHECK(vkCreatePipelineLayout(get_device().get_handle(), &pipeline_layout_create_info, nullptr, &pipeline_first_pass_layout));
}

void Nerf::create_pipeline_layout_baseline()
{
	// Second Pass Descriptor set and layout

	std::vector<VkDescriptorSetLayoutBinding> set_layout_bindings = {
	    // Two output color from the first pass and ray direction
	    vkb::initializers::descriptor_set_layout_binding(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT, 0),
	    vkb::initializers::descriptor_set_layout_binding(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT, 1),
	    vkb::initializers::descriptor_set_layout_binding(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT, 2),
	    // MLP weights
	    vkb::initializers::descriptor_set_layout_binding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT, 3)        // SSBO
	};

	VkDescriptorSetLayoutCreateInfo descriptor_layout = vkb::initializers::descriptor_set_layout_create_info(set_layout_bindings.data(), static_cast<uint32_t>(set_layout_bindings.size()));
	VK_CHECK(vkCreateDescriptorSetLayout(get_device().get_handle(), &descriptor_layout, nullptr, &descriptor_set_layout_baseline));

	VkPipelineLayoutCreateInfo pipeline_layout_create_info =
	    vkb::initializers::pipeline_layout_create_info(
	        &descriptor_set_layout_baseline,
	        1);

	VK_CHECK(vkCreatePipelineLayout(get_device().get_handle(), &pipeline_layout_create_info, nullptr, &pipeline_layout_baseline));
}

void Nerf::create_descriptor_sets_first_pass(Model &model)
{
	int numDescriptorPerModel = use_deferred ? 1 : nerf_framebuffers.size();
	model.descriptor_set_first_pass.resize(numDescriptorPerModel);

	for (int i = 0; i < numDescriptorPerModel; i++)
	{
		VkDescriptorSetAllocateInfo descriptor_set_allocate_info =
		    vkb::initializers::descriptor_set_allocate_info(descriptor_pool, &descriptor_set_first_pass_layout, 1);
		VK_CHECK(vkAllocateDescriptorSets(get_device().get_handle(), &descriptor_set_allocate_info, &model.descriptor_set_first_pass[i]));

		std::array<VkDescriptorImageInfo, 2> texture_input_descriptors;

		texture_input_descriptors[0].sampler     = model.texture_input_0.sampler;
		texture_input_descriptors[0].imageView   = model.texture_input_0.view;
		texture_input_descriptors[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		texture_input_descriptors[1].sampler     = model.texture_input_1.sampler;
		texture_input_descriptors[1].imageView   = model.texture_input_1.view;
		texture_input_descriptors[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkDescriptorBufferInfo buffer_descriptor = create_descriptor(**model.uniform_buffer_ref);

		VkWriteDescriptorSet texture_input_write_0 = vkb::initializers::write_descriptor_set(model.descriptor_set_first_pass[i],
		                                                                                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0, &texture_input_descriptors[0]);
		VkWriteDescriptorSet texture_input_write_1 = vkb::initializers::write_descriptor_set(model.descriptor_set_first_pass[i],
		                                                                                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, &texture_input_descriptors[1]);
		VkWriteDescriptorSet uniform_buffer_write  = vkb::initializers::write_descriptor_set(model.descriptor_set_first_pass[i], VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2, &buffer_descriptor);

		std::vector<VkWriteDescriptorSet> write_descriptor_sets = {
		    texture_input_write_0,
		    texture_input_write_1,
		    uniform_buffer_write};

		VkDescriptorBufferInfo weights_buffer_descriptor;

		if (!use_deferred)
		{
			// Add in descriptor sets for MLP weights
			weights_buffer_descriptor = create_descriptor(**model.weights_buffer_ref);
			// Add in descriptor sets for MLP weights
			VkWriteDescriptorSet weights_buffer_write = vkb::initializers::write_descriptor_set(model.descriptor_set_first_pass[i],
			                                                                                    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3, &weights_buffer_descriptor);
			write_descriptor_sets.push_back(weights_buffer_write);
		}

		vkUpdateDescriptorSets(get_device().get_handle(), static_cast<uint32_t>(write_descriptor_sets.size()), write_descriptor_sets.data(), 0, VK_NULL_HANDLE);
	}
}

void Nerf::create_descriptor_sets_baseline()
{
	descriptor_set_baseline.resize(nerf_framebuffers.size());

	for (int i = 0; i < nerf_framebuffers.size(); i++)
	{
		VkDescriptorSetAllocateInfo descriptor_set_allocate_info = vkb::initializers::descriptor_set_allocate_info(descriptor_pool, &descriptor_set_layout_baseline, 1);
		VK_CHECK(vkAllocateDescriptorSets(get_device().get_handle(), &descriptor_set_allocate_info, &descriptor_set_baseline[i]));

		std::array<VkDescriptorImageInfo, 3> attachment_input_descriptors;

		attachment_input_descriptors[0].sampler     = VK_NULL_HANDLE;
		attachment_input_descriptors[0].imageView   = frameAttachments[i].feature_0.view;
		attachment_input_descriptors[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		attachment_input_descriptors[1].sampler     = VK_NULL_HANDLE;
		attachment_input_descriptors[1].imageView   = frameAttachments[i].feature_1.view;
		attachment_input_descriptors[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		attachment_input_descriptors[2].sampler     = VK_NULL_HANDLE;
		attachment_input_descriptors[2].imageView   = frameAttachments[i].feature_2.view;
		attachment_input_descriptors[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		// TODO: add in descriptor sets for MLP weights
		VkDescriptorBufferInfo weights_buffer_descriptor = create_descriptor(**models[0].weights_buffer_ref);
		VkWriteDescriptorSet   texture_input_write_0     = vkb::initializers::write_descriptor_set(descriptor_set_baseline[i], VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 0, &attachment_input_descriptors[0]);
		VkWriteDescriptorSet   texture_input_write_1     = vkb::initializers::write_descriptor_set(descriptor_set_baseline[i], VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, &attachment_input_descriptors[1]);
		VkWriteDescriptorSet   texture_input_write_2     = vkb::initializers::write_descriptor_set(descriptor_set_baseline[i], VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 2, &attachment_input_descriptors[2]);

		// TODO: add in descriptor sets for MLP weights
		VkWriteDescriptorSet weights_buffer_write = vkb::initializers::write_descriptor_set(descriptor_set_baseline[i], VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3, &weights_buffer_descriptor);        // UBO

		std::vector<VkWriteDescriptorSet> write_descriptor_sets = {
		    texture_input_write_0,
		    texture_input_write_1,
		    texture_input_write_2,
		    weights_buffer_write};

		vkUpdateDescriptorSets(get_device().get_handle(), static_cast<uint32_t>(write_descriptor_sets.size()), write_descriptor_sets.data(), 0, VK_NULL_HANDLE);
	}
}

void Nerf::prepare_pipelines()
{
	VkPipelineInputAssemblyStateCreateInfo input_assembly_state = vkb::initializers::pipeline_input_assembly_state_create_info(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);

	VkPipelineRasterizationStateCreateInfo rasterization_state = vkb::initializers::pipeline_rasterization_state_create_info(VK_POLYGON_MODE_FILL, /*VK_CULL_MODE_BACK_BIT*/ VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE /*VK_FRONT_FACE_CLOCKWISE*/, 0);

	std::vector<VkPipelineColorBlendAttachmentState> blend_attachment_states;
	blend_attachment_states.push_back(vkb::initializers::pipeline_color_blend_attachment_state(0xf, VK_FALSE));

	if (use_deferred)
	{
		blend_attachment_states.push_back(vkb::initializers::pipeline_color_blend_attachment_state(0xf, VK_FALSE));
		blend_attachment_states.push_back(vkb::initializers::pipeline_color_blend_attachment_state(0xf, VK_FALSE));
	}

	VkPipelineColorBlendStateCreateInfo color_blend_state = vkb::initializers::pipeline_color_blend_state_create_info(blend_attachment_states.size(), blend_attachment_states.data());

	VkPipelineDepthStencilStateCreateInfo depth_stencil_state = vkb::initializers::pipeline_depth_stencil_state_create_info(VK_TRUE, VK_TRUE, VK_COMPARE_OP_LESS);
	depth_stencil_state.depthBoundsTestEnable                 = VK_FALSE;
	depth_stencil_state.minDepthBounds                        = 0.f;
	depth_stencil_state.maxDepthBounds                        = 1.f;

	VkPipelineViewportStateCreateInfo viewport_state = vkb::initializers::pipeline_viewport_state_create_info(1, 1, 0);

	std::vector<VkDynamicState> dynamic_state_enables = {
	    VK_DYNAMIC_STATE_VIEWPORT,
	    VK_DYNAMIC_STATE_SCISSOR};

	VkPipelineDynamicStateCreateInfo dynamic_state =
	    vkb::initializers::pipeline_dynamic_state_create_info(
	        dynamic_state_enables.data(),
	        static_cast<uint32_t>(dynamic_state_enables.size()),
	        0);

	VkPipelineMultisampleStateCreateInfo multisample_state = vkb::initializers::pipeline_multisample_state_create_info(VK_SAMPLE_COUNT_1_BIT, 0);

	// Vertex bindings and attributes
	const std::vector<VkVertexInputBindingDescription> vertex_input_bindings = {
	    vkb::initializers::vertex_input_binding_description(0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX),
	    vkb::initializers::vertex_input_binding_description(1, sizeof(InstanceData), VK_VERTEX_INPUT_RATE_INSTANCE),
	};
	const std::vector<VkVertexInputAttributeDescription> vertex_input_attributes = {
	    vkb::initializers::vertex_input_attribute_description(0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)),
	    vkb::initializers::vertex_input_attribute_description(0, 1, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, tex_coord)),
	    vkb::initializers::vertex_input_attribute_description(1, 2, VK_FORMAT_R32G32B32_SFLOAT, 0),
	};
	VkPipelineVertexInputStateCreateInfo vertex_input_state = vkb::initializers::pipeline_vertex_input_state_create_info();
	vertex_input_state.vertexBindingDescriptionCount        = static_cast<uint32_t>(vertex_input_bindings.size());
	vertex_input_state.pVertexBindingDescriptions           = vertex_input_bindings.data();
	vertex_input_state.vertexAttributeDescriptionCount      = static_cast<uint32_t>(vertex_input_attributes.size());
	vertex_input_state.pVertexAttributeDescriptions         = vertex_input_attributes.data();

	// First Pass

	VkGraphicsPipelineCreateInfo pipeline_create_info = vkb::initializers::pipeline_create_info(pipeline_first_pass_layout, render_pass_nerf, 0);
	pipeline_create_info.pVertexInputState            = &vertex_input_state;
	pipeline_create_info.pInputAssemblyState          = &input_assembly_state;
	pipeline_create_info.pRasterizationState          = &rasterization_state;
	pipeline_create_info.pColorBlendState             = &color_blend_state;
	pipeline_create_info.pMultisampleState            = &multisample_state;
	pipeline_create_info.pViewportState               = &viewport_state;
	pipeline_create_info.pDepthStencilState           = &depth_stencil_state;
	pipeline_create_info.pDynamicState                = &dynamic_state;
	pipeline_create_info.subpass                      = 0;
	pipeline_create_info.stageCount                   = static_cast<uint32_t>(shader_stages_first_pass.size());
	pipeline_create_info.pStages                      = shader_stages_first_pass.data();

	// Each model will have its own pipeline
	for (auto &model : models)
	{
		VK_CHECK(vkCreateGraphicsPipelines(get_device().get_handle(), pipeline_cache, 1, &pipeline_create_info, nullptr, &model.pipeline_first_pass));
	}

	if (use_deferred)
	{
		// Second Pass

		pipeline_create_info.layout  = pipeline_layout_baseline;
		pipeline_create_info.subpass = 1;

		VkPipelineVertexInputStateCreateInfo emptyInputStateCI{};
		emptyInputStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

		pipeline_create_info.pVertexInputState = &emptyInputStateCI;
		color_blend_state.attachmentCount      = 1;
		rasterization_state.cullMode           = VK_CULL_MODE_NONE;
		depth_stencil_state.depthWriteEnable   = VK_FALSE;
		pipeline_create_info.stageCount        = static_cast<uint32_t>(shader_stages_second_pass.size());
		pipeline_create_info.pStages           = shader_stages_second_pass.data();

		VK_CHECK(vkCreateGraphicsPipelines(get_device().get_handle(), pipeline_cache, 1, &pipeline_create_info, nullptr, &pipeline_baseline));
	}
}

void Nerf::create_static_object_buffers(int model_index, int sub_model_index, int models_entry)
{
	LOGI("Creating static object buffers");
	Model &model              = models[models_entry];
	auto   vertex_buffer_size = model.vertices.size() * sizeof(Vertex);
	auto   index_buffer_size  = model.indices.size() * sizeof(model.indices[0]);

	// Create a staging buffer
	const VkBufferUsageFlags           staging_flags         = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	const VkBufferUsageFlags           vertex_flags          = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	const VkBufferUsageFlags           index_flags           = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	std::unique_ptr<vkb::core::Buffer> staging_vertex_buffer = std::make_unique<vkb::core::Buffer>(get_device(), vertex_buffer_size, staging_flags | vertex_flags, VMA_MEMORY_USAGE_CPU_TO_GPU);
	std::unique_ptr<vkb::core::Buffer> staging_index_buffer  = std::make_unique<vkb::core::Buffer>(get_device(), index_buffer_size, staging_flags | index_flags, VMA_MEMORY_USAGE_CPU_TO_GPU);

	// Copy over the data for each of the models
	staging_vertex_buffer->update(model.vertices.data(), vertex_buffer_size);
	staging_index_buffer->update(model.indices.data(), index_buffer_size);

	// now transfer over to the end buffer
	auto &cmd = device->request_command_buffer();
	cmd.begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, VK_NULL_HANDLE);
	auto copy = [this, &cmd](vkb::core::Buffer &staging_buffer, const VkBufferUsageFlags buffer_usage_flags) {
		auto output_buffer = std::make_unique<vkb::core::Buffer>(get_device(), staging_buffer.get_size(), buffer_usage_flags | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		cmd.copy_buffer(staging_buffer, *output_buffer, staging_buffer.get_size());

		vkb::BufferMemoryBarrier barrier;
		barrier.src_stage_mask  = VK_PIPELINE_STAGE_TRANSFER_BIT;
		barrier.dst_stage_mask  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		barrier.src_access_mask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dst_access_mask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		cmd.buffer_memory_barrier(*output_buffer, 0, VK_WHOLE_SIZE, barrier);
		return output_buffer;
	};
	model.vertex_buffer = copy(*staging_vertex_buffer, vertex_flags);
	model.index_buffer  = copy(*staging_index_buffer, index_flags);

	cmd.end();
	auto &queue = device->get_queue_by_flags(VK_QUEUE_GRAPHICS_BIT, 0);
	queue.submit(cmd, device->request_fence());
	device->get_fence_pool().wait();
	LOGI("Done Creating static object buffers");
}

void Nerf::create_uniforms()
{
	uniform_buffers.resize(model_path.size());
	weights_buffers.resize(model_path.size());

	for (int i = 0; i < model_path.size(); i++)
	{
		LOGI("Creating camera view uniform buffer for model {}", i);
		uniform_buffers[i] = std::make_unique<vkb::core::Buffer>(get_device(),
		                                                         sizeof(global_uniform),
		                                                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		                                                         VMA_MEMORY_USAGE_CPU_TO_GPU);

		LOGI("Creating mlp weights uniform buffer for model {}", i);
		weights_buffers[i] = std::make_unique<vkb::core::Buffer>(get_device(),
		                                                         sizeof(MLP_Weights),
		                                                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		                                                         VMA_MEMORY_USAGE_CPU_TO_GPU);
	}

	// Record a referce to vulkan buffer for each one of the models
	for (Model &model : models)
	{
		model.uniform_buffer_ref = &uniform_buffers[model.model_index];
		model.weights_buffer_ref = &weights_buffers[model.model_index];
	}

	update_uniform_buffers();
}

void Nerf::initialize_mlp_uniform_buffers(int model_index)
{
	std::string assetBase   = vkb::fs::path::get(vkb::fs::path::Type::Assets);
	std::string mlpJsonPath = assetBase + model_path[model_index] + "mlp.json";

	using json = nlohmann::json;

	std::ifstream f(mlpJsonPath);

	if (!f)
	{
		LOGE("Failed to open mlp data");
		assert(0);
	}

	LOGI("Parsing mlp data {}", mlpJsonPath);
	json data = json::parse(f);

	// Record a index of the first sub-model
	int first_sub_model = models.size();
	int obj_num         = data["obj_num"].get<int>();

	// Here we know the actual number of sub models
	int next_sub_model_index = models.size();
	models.resize(models.size() + obj_num);

	for (int i = next_sub_model_index; i < models.size(); i++)
	{
		models[i].model_index = model_index;
	}

	auto weights_0_array_raw = data["0_weights"].get<std::vector<std::vector<float>>>();

	std::vector<float> weights_0_array;

	for (auto ii = weights_0_array_raw.begin(); ii != weights_0_array_raw.end(); ii++)
	{
		weights_0_array.insert(weights_0_array.end(), (*ii).begin(), (*ii).end());
	}

	if (weights_0_array.size() != WEIGHTS_0_COUNT)
	{
		LOGE("MLP data layer 0 weights count is {}, rather than {}", weights_0_array.size(), WEIGHTS_0_COUNT);
	}

	auto bias_0_array = data["0_bias"].get<std::vector<float>>();

	if (bias_0_array.size() != BIAS_0_COUNT)
	{
		LOGE("MLP data layer 0 bias count is {}, rather than {}", bias_0_array.size(), BIAS_0_COUNT);
	}

	auto weights_1_array_raw = data["1_weights"].get<std::vector<std::vector<float>>>();

	std::vector<float> weights_1_array;

	for (auto ii = weights_1_array_raw.begin(); ii != weights_1_array_raw.end(); ii++)
	{
		weights_1_array.insert(weights_1_array.end(), (*ii).begin(), (*ii).end());
	}

	if (weights_1_array.size() != WEIGHTS_1_COUNT)
	{
		LOGE("MLP data layer 1 weights count is {}, rather than {}", weights_1_array.size(), WEIGHTS_1_COUNT);
	}

	auto bias_1_array = data["1_bias"].get<std::vector<float>>();

	if (bias_1_array.size() != BIAS_1_COUNT)
	{
		LOGE("MLP data layer 1 bias count is {}, rather than {}", bias_1_array.size(), BIAS_1_COUNT);
	}

	auto weights_2_array_raw = data["2_weights"].get<std::vector<std::vector<float>>>();

	std::vector<float> weights_2_array;

	for (auto ii = weights_2_array_raw.begin(); ii != weights_2_array_raw.end(); ii++)
	{
		weights_2_array.insert(weights_2_array.end(), (*ii).begin(), (*ii).end());
	}

	// We need to pad the layer 2's weights with 16 zeros
	if (weights_2_array.size() != WEIGHTS_2_COUNT - 16)
	{
		LOGE("MLP data layer 2 weights count is {}, rather than {}", weights_2_array.size(), WEIGHTS_2_COUNT);
	}

	auto bias_2_array = data["2_bias"].get<std::vector<float>>();

	if (bias_2_array.size() != BIAS_2_COUNT - 1)
	{
		LOGE("MLP data layer 2 bias count is {}, rather than {}", bias_2_array.size(), BIAS_2_COUNT);
	}

	// Each sub model will share the same mlp weights data
	MLP_Weights *model_mlp = &mlp_weight_vector[model_index];

	for (int ii = 0; ii < WEIGHTS_0_COUNT; ii++)
	{
		model_mlp->data[ii] = weights_0_array[ii];
	}

	for (int ii = 0; ii < WEIGHTS_1_COUNT; ii++)
	{
		model_mlp->data[WEIGHTS_0_COUNT + ii] = weights_1_array[ii];
	}

	// We need to pad the layer 2's weights with zeros for every 3 weights to make it 16 bytes aligned
	int raw_weight_cnt = 0;
	for (int ii = 0; ii < WEIGHTS_2_COUNT; ii++)
	{
		if ((ii + 1) % 4 == 0)
		{
			model_mlp->data[WEIGHTS_0_COUNT + WEIGHTS_1_COUNT + ii] = 0.0f;
		}
		else
		{
			model_mlp->data[WEIGHTS_0_COUNT + WEIGHTS_1_COUNT + ii] = weights_2_array[raw_weight_cnt++];
		}
	}

	for (int ii = 0; ii < BIAS_0_COUNT; ii++)
	{
		model_mlp->data[WEIGHTS_0_COUNT + WEIGHTS_1_COUNT + WEIGHTS_2_COUNT + ii] = bias_0_array[ii];
	}

	for (int ii = 0; ii < BIAS_1_COUNT; ii++)
	{
		model_mlp->data[WEIGHTS_0_COUNT + WEIGHTS_1_COUNT + WEIGHTS_2_COUNT +
		                BIAS_0_COUNT + ii] = bias_1_array[ii];
	}

	// We need to pad the layer 2's bias with zeros for every 3 weights to make it 16 bytes aligned
	for (int ii = 0; ii < BIAS_2_COUNT; ii++)
	{
		if ((ii + 1) % 4 == 0)
		{
			model_mlp->data[WEIGHTS_0_COUNT + WEIGHTS_1_COUNT + WEIGHTS_2_COUNT +
			                BIAS_0_COUNT + BIAS_1_COUNT + ii] = 0.0f;
		}
		else
		{
			model_mlp->data[WEIGHTS_0_COUNT + WEIGHTS_1_COUNT + WEIGHTS_2_COUNT +
			                BIAS_0_COUNT + BIAS_1_COUNT + ii] = bias_2_array[ii];
		}
	}

	// Update all sub model with the same mlp weight
	for (int i = 0; i < obj_num; i++)
	{
		models[first_sub_model + i].sub_model_num = obj_num;
	}
}

void Nerf::update_uniform_buffers()
{
	assert(uniform_buffers[0]);

	const float tan_half_fov = tan(0.5 * fov / 180.0f * 3.141592653589793f);

	global_uniform.proj            = camera.matrices.perspective;
	global_uniform.view            = camera.matrices.view;
	global_uniform.camera_position = camera.position;
	global_uniform.camera_side     = glm::vec3(camera.matrices.view[0][0], camera.matrices.view[1][0], camera.matrices.view[2][0]);
	global_uniform.camera_up       = glm::vec3(camera.matrices.view[0][1], camera.matrices.view[1][1], camera.matrices.view[2][1]);
	global_uniform.camera_lookat   = -glm::vec3(camera.matrices.view[0][2], camera.matrices.view[1][2], camera.matrices.view[2][2]);
	global_uniform.img_dim         = glm::vec2(width, height);
	global_uniform.tan_half_fov    = tan_half_fov;

	// Note that this is a hard-coded scene setting for the lego_combo
	glm::mat4x4 model_translation[4] = {glm::translate(glm::vec3(0.5, 0.75, 0)), glm::translate(glm::vec3(0.5, 0.25, 0)),
	                                    glm::translate(glm::vec3(0, -0.25, 0.5)), glm::translate(glm::vec3(0, -0.75, -0.5))};

	for (int i = 0; i < model_path.size(); i++)
	{
		global_uniform.model = combo_mode ? model_translation[i] : glm::translate(glm::vec3(0.0f));
		uniform_buffers[i]->update(&global_uniform, sizeof(global_uniform));
		weights_buffers[i]->update(&(mlp_weight_vector[i].data[0]), sizeof(MLP_Weights));
	}
}

void Nerf::prepare_instance_data()
{
	auto &ii = instancing_info;

	std::vector<InstanceData> instance_data;
	instance_data.resize(ii.dim.x * ii.dim.y * ii.dim.z);

	glm::vec3 corner_pos = -ii.interval * 0.5f * (glm::vec3(ii.dim - 1));
	int       idx        = 0;
	for (int x = 0; x < ii.dim.x; ++x)
	{
		for (int y = 0; y < ii.dim.y; ++y)
		{
			for (int z = 0; z < ii.dim.z; ++z)
			{
				instance_data[idx++].pos_offset = glm::vec3(
				    corner_pos.x + ii.interval.x * x,
				    corner_pos.y + ii.interval.y * y,
				    corner_pos.z + ii.interval.z * z);
			}
		}
	}

	auto instance_buffer_size = instance_data.size() * sizeof(InstanceData);

	// Note that in contrast to a typical pipeline, our vertex/index buffer requires the acceleration structure build flag in rayquery
	// Create a staging buffer
	const VkBufferUsageFlags           buffer_usage_flags      = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	const VkBufferUsageFlags           staging_flags           = buffer_usage_flags | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	std::unique_ptr<vkb::core::Buffer> staging_instance_buffer = std::make_unique<vkb::core::Buffer>(get_device(), instance_buffer_size, staging_flags, VMA_MEMORY_USAGE_CPU_TO_GPU);

	// Copy over the data for each of the models
	staging_instance_buffer->update(instance_data.data(), instance_buffer_size);

	// now transfer over to the end buffer
	auto &cmd = device->request_command_buffer();
	cmd.begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, VK_NULL_HANDLE);
	auto copy = [this, &cmd](vkb::core::Buffer &staging_buffer, const VkBufferUsageFlags buffer_usage_flags) {
		auto output_buffer = std::make_unique<vkb::core::Buffer>(get_device(), staging_buffer.get_size(), buffer_usage_flags | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		cmd.copy_buffer(staging_buffer, *output_buffer, staging_buffer.get_size());

		vkb::BufferMemoryBarrier barrier;
		barrier.src_stage_mask  = VK_PIPELINE_STAGE_TRANSFER_BIT;
		barrier.dst_stage_mask  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		barrier.src_access_mask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dst_access_mask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		cmd.buffer_memory_barrier(*output_buffer, 0, VK_WHOLE_SIZE, barrier);
		return output_buffer;
	};
	instance_buffer = copy(*staging_instance_buffer, buffer_usage_flags);

	cmd.end();
	auto &queue = device->get_queue_by_flags(VK_QUEUE_GRAPHICS_BIT, 0);
	queue.submit(cmd, device->request_fence());
	device->get_fence_pool().wait();
}

void Nerf::draw()
{
	ApiVulkanSample::prepare_frame();

	// Command buffer to be submitted to the queue
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers    = &draw_cmd_buffers[current_buffer];

	// Submit to queue
	VK_CHECK(vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE));

	ApiVulkanSample::submit_frame();
}

void Nerf::create_texture(int model_index, int sub_model_index, int models_entry)
{
	// Set up the input texture image

	// TODO should load different scenes's feature map from command line
	std::string assetBase      = vkb::fs::path::get(vkb::fs::path::Type::Assets);
	std::string feature_0_path = assetBase + model_path[model_index] + "shape" + std::to_string(sub_model_index) + ".pngfeat0.png";
	std::string feature_1_path = assetBase + model_path[model_index] + "shape" + std::to_string(sub_model_index) + ".pngfeat1.png";

	LOGI("Creating feature texture 0");
	create_texture_helper(feature_0_path, models[models_entry].texture_input_0);
	LOGI("Done Creating feature texture 0");

	LOGI("Creating feature texture 1");
	create_texture_helper(feature_1_path, models[models_entry].texture_input_1);
	LOGI("Done Creating feature texture 0");
}

void Nerf::create_texture_helper(std::string texturePath, Texture_Input &texture_input)
{
	// Copy data to an optimal tiled image
	// This loads the texture data into a host local buffer that is copied to the optimal tiled image on the device

	// Create a host-visible staging buffer that contains the raw image data
	// This buffer will be the data source for copying texture data to the optimal tiled image on the device
	// This buffer is used as a transfer source for the buffer copy
	int texture_width  = 0;
	int texture_height = 0;
	int channel        = 0;

	uint8_t *data = stbi_load(texturePath.c_str(), &texture_width, &texture_height, &channel, 0);

	size_t dataSize = texture_width * texture_height * channel;

	std::unique_ptr<vkb::core::Buffer> stage_buffer = std::make_unique<vkb::core::Buffer>(get_device(), dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	VkImageSubresourceLayers resourcesLayer = {};
	resourcesLayer.aspectMask               = VK_IMAGE_ASPECT_COLOR_BIT;
	resourcesLayer.mipLevel                 = 0;
	resourcesLayer.baseArrayLayer           = 0;
	resourcesLayer.layerCount               = 1;

	VkOffset3D offset = {0};
	VkExtent3D extent = {(uint32_t) texture_width, (uint32_t) texture_height, 1};

	// Setup buffer copy regions for each mip level
	VkBufferImageCopy buffer_copy_region = {};

	buffer_copy_region.bufferOffset      = 0;
	buffer_copy_region.bufferRowLength   = 0;
	buffer_copy_region.bufferImageHeight = 0;
	buffer_copy_region.imageSubresource  = resourcesLayer;
	buffer_copy_region.imageOffset       = offset;
	buffer_copy_region.imageExtent       = extent;

	// Copy texture data into host local staging buffer
	stage_buffer->update(data, dataSize);

	texture_input.width  = texture_width;
	texture_input.height = texture_height;

	VkImageCreateInfo image = vkb::initializers::image_create_info();
	image.imageType         = VK_IMAGE_TYPE_2D;
	image.format            = VK_FORMAT_R8G8B8A8_UNORM;
	image.extent.width      = texture_input.width;
	image.extent.height     = texture_input.height;
	image.extent.depth      = 1;
	image.mipLevels         = 1;
	image.arrayLayers       = 1;
	image.samples           = VK_SAMPLE_COUNT_1_BIT;
	image.tiling            = VK_IMAGE_TILING_OPTIMAL;
	image.usage             = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	image.initialLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
	VK_CHECK(vkCreateImage(get_device().get_handle(), &image, nullptr, &texture_input.image));

	texture_input.format = VK_FORMAT_R8G8B8A8_UNORM;

	VkMemoryRequirements memory_requirements;
	vkGetImageMemoryRequirements(get_device().get_handle(), texture_input.image, &memory_requirements);
	VkMemoryAllocateInfo memory_allocate_info = vkb::initializers::memory_allocate_info();
	memory_allocate_info.allocationSize       = memory_requirements.size;
	memory_allocate_info.memoryTypeIndex      = get_device().get_memory_type(memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VK_CHECK(vkAllocateMemory(get_device().get_handle(), &memory_allocate_info, nullptr, &texture_input.memory));
	VK_CHECK(vkBindImageMemory(get_device().get_handle(), texture_input.image, texture_input.memory, 0));

	VkImageViewCreateInfo color_image_view           = vkb::initializers::image_view_create_info();
	color_image_view.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
	color_image_view.format                          = VK_FORMAT_R8G8B8A8_UNORM;
	color_image_view.subresourceRange                = {};
	color_image_view.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	color_image_view.subresourceRange.baseMipLevel   = 0;
	color_image_view.subresourceRange.levelCount     = 1;
	color_image_view.subresourceRange.baseArrayLayer = 0;
	color_image_view.subresourceRange.layerCount     = 1;
	color_image_view.image                           = texture_input.image;
	VK_CHECK(vkCreateImageView(get_device().get_handle(), &color_image_view, nullptr, &texture_input.view));

	VkCommandBuffer command_buffer = get_device().create_command_buffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
	vkb::image_layout_transition(command_buffer, texture_input.image,
	                             VK_IMAGE_LAYOUT_UNDEFINED,
	                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                             {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});

	vkCmdCopyBufferToImage(command_buffer, *(stage_buffer->get()), texture_input.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &buffer_copy_region);

	vkb::image_layout_transition(command_buffer, texture_input.image,
	                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                             {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});
	get_device().flush_command_buffer(command_buffer, queue);

	VkSamplerCreateInfo samplerCreateInfo = {};

	samplerCreateInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;

	if (using_original_nerf_models[0])
	{
		samplerCreateInfo.magFilter = VK_FILTER_NEAREST;
		samplerCreateInfo.minFilter = VK_FILTER_NEAREST;
	}
	else
	{
		samplerCreateInfo.magFilter = VK_FILTER_LINEAR;
		samplerCreateInfo.minFilter = VK_FILTER_LINEAR;
	}

	samplerCreateInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerCreateInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCreateInfo.addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCreateInfo.addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCreateInfo.minLod                  = 0.0f;
	samplerCreateInfo.maxLod                  = 16.0f;
	samplerCreateInfo.unnormalizedCoordinates = VK_FALSE;

	VK_CHECK(vkCreateSampler(get_device().get_handle(), &samplerCreateInfo, 0, &texture_input.sampler));
}

void Nerf::update_render_pass_nerf_forward()
{
	// For merged shaders, we need 2 attachments (as opposed to 5)
	// 0: Depth attachment
	// 1: Swapchain attachment
	std::array<VkAttachmentDescription, 2> attachments = {};
	// Depth attachment
	attachments[0].format         = depth_format;
	attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	// Swapchain  attachment
	attachments[1].format         = get_render_context().get_swapchain().get_format();
	attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	VkAttachmentReference depth_reference = {};
	depth_reference.attachment            = 0;
	depth_reference.layout                = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference swapchain_reference = {};
	swapchain_reference.attachment            = 1;
	swapchain_reference.layout                = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass    = {};
	subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount    = 1;
	subpass.pColorAttachments       = &swapchain_reference;
	subpass.pDepthStencilAttachment = &depth_reference;
	subpass.inputAttachmentCount    = 0;
	subpass.pInputAttachments       = nullptr;
	subpass.preserveAttachmentCount = 0;
	subpass.pPreserveAttachments    = nullptr;
	subpass.pResolveAttachments     = nullptr;

	VkRenderPassCreateInfo render_pass_create_info = {};
	render_pass_create_info.sType                  = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	render_pass_create_info.attachmentCount        = static_cast<uint32_t>(attachments.size());
	render_pass_create_info.pAttachments           = attachments.data();
	render_pass_create_info.subpassCount           = 1;
	render_pass_create_info.pSubpasses             = &subpass;

	VK_CHECK(vkCreateRenderPass(device->get_handle(), &render_pass_create_info, nullptr, &render_pass_nerf));
}

void Nerf::update_render_pass_nerf_baseline()
{
	std::array<VkAttachmentDescription, 5> attachments = {};
	// Color attachment 1
	attachments[0].format         = feature_map_format;
	attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	// Color attachment 2
	attachments[1].format         = feature_map_format;
	attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	// Color attachment 3
	attachments[2].format         = VK_FORMAT_R16G16B16A16_SFLOAT;
	attachments[2].samples        = VK_SAMPLE_COUNT_1_BIT;
	attachments[2].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[2].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[2].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[2].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[2].finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	// Depth attachment
	attachments[3].format         = depth_format;
	attachments[3].samples        = VK_SAMPLE_COUNT_1_BIT;
	attachments[3].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[3].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[3].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[3].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[3].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	// Swapchain  attachment
	attachments[4].format         = get_render_context().get_swapchain().get_format();
	attachments[4].samples        = VK_SAMPLE_COUNT_1_BIT;
	attachments[4].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[4].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[4].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[4].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[4].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[4].finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	VkAttachmentReference color_reference_0 = {};
	color_reference_0.attachment            = 0;
	color_reference_0.layout                = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference color_reference_1 = {};
	color_reference_1.attachment            = 1;
	color_reference_1.layout                = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference color_reference_2 = {};
	color_reference_2.attachment            = 2;
	color_reference_2.layout                = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference depth_reference = {};
	depth_reference.attachment            = 3;
	depth_reference.layout                = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	std::array<VkSubpassDescription, 2> subpassDescriptions{};

	VkAttachmentReference color_references_feature_maps[3] = {color_reference_0, color_reference_1, color_reference_2};

	subpassDescriptions[0].pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpassDescriptions[0].colorAttachmentCount    = 3;
	subpassDescriptions[0].pColorAttachments       = color_references_feature_maps;
	subpassDescriptions[0].pDepthStencilAttachment = &depth_reference;
	subpassDescriptions[0].inputAttachmentCount    = 0;
	subpassDescriptions[0].pInputAttachments       = nullptr;
	subpassDescriptions[0].preserveAttachmentCount = 0;
	subpassDescriptions[0].pPreserveAttachments    = nullptr;
	subpassDescriptions[0].pResolveAttachments     = nullptr;

	VkAttachmentReference swapchain_reference = {};
	swapchain_reference.attachment            = 4;
	swapchain_reference.layout                = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	// Color attachments written to in first sub pass will be used as input attachments to be read in the fragment shader
	VkAttachmentReference inputReferences[3];
	inputReferences[0] = {0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	inputReferences[1] = {1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	inputReferences[2] = {2, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

	subpassDescriptions[1].pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpassDescriptions[1].colorAttachmentCount    = 1;
	subpassDescriptions[1].pColorAttachments       = &swapchain_reference;
	subpassDescriptions[1].pDepthStencilAttachment = nullptr;
	subpassDescriptions[1].inputAttachmentCount    = 3;
	subpassDescriptions[1].pInputAttachments       = inputReferences;
	subpassDescriptions[1].preserveAttachmentCount = 0;
	subpassDescriptions[1].pPreserveAttachments    = nullptr;
	subpassDescriptions[1].pResolveAttachments     = nullptr;

	// Subpass dependencies for layout transitions
	std::array<VkSubpassDependency, 3> dependencies;

	dependencies[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass      = 0;
	dependencies[0].srcStageMask    = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	dependencies[0].dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependencies[0].srcAccessMask   = VK_ACCESS_NONE;
	dependencies[0].dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	dependencies[1].srcSubpass      = 0;
	dependencies[1].dstSubpass      = 1;
	dependencies[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[1].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dependencies[1].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstAccessMask   = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
	dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	dependencies[2].srcSubpass      = 1;
	dependencies[2].dstSubpass      = VK_SUBPASS_EXTERNAL;
	dependencies[2].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[2].dstStageMask    = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	dependencies[2].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[2].dstAccessMask   = VK_ACCESS_NONE;
	dependencies[2].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	VkRenderPassCreateInfo render_pass_create_info = {};
	render_pass_create_info.sType                  = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	render_pass_create_info.attachmentCount        = static_cast<uint32_t>(attachments.size());
	render_pass_create_info.pAttachments           = attachments.data();
	render_pass_create_info.subpassCount           = subpassDescriptions.size();
	render_pass_create_info.pSubpasses             = subpassDescriptions.data();
	render_pass_create_info.dependencyCount        = static_cast<uint32_t>(dependencies.size());
	render_pass_create_info.pDependencies          = dependencies.data();

	VK_CHECK(vkCreateRenderPass(device->get_handle(), &render_pass_create_info, nullptr, &render_pass_nerf));
}

std::unique_ptr<vkb::VulkanSample> create_nerf()
{
	return std::make_unique<Nerf>();
}