#include "primitive_setup.hpp"
#include "rasterizer_cpu.hpp"
#include "triangle_converter.hpp"
#include "canvas.hpp"
#include "stb_image_write.h"
#include <stdio.h>
#include <vector>
#include <random>
#include <assert.h>

#include "global_managers.hpp"
#include "math.hpp"
#include "texture_files.hpp"
#include "gltf.hpp"
#include "camera.hpp"
#include "approximate_divider.hpp"
#include "rasterizer_gpu.hpp"
#include "os_filesystem.hpp"
#include "scene_loader.hpp"
#include "mesh_util.hpp"
#include "application.hpp"

using namespace RetroWarp;
using namespace Granite;

class StreamReader
{
public:
	StreamReader(const uint8_t *blob, size_t size);
	bool eof() const;
	bool parse_header();
	bool parse_resolution(uint32_t &width, uint32_t &height);
	bool parse_num_textures(uint32_t &count);

	enum Op { TEX, PRIM };
	bool parse_op(Op &op);
	bool parse_uint(uint32_t &value);
	bool parse_primitive(PrimitiveSetup &setup);

private:
	const uint8_t *blob;
	size_t offset;
	size_t size;
};

StreamReader::StreamReader(const uint8_t *blob_, size_t size_)
	: blob(blob_), size(size_)
{
}

bool StreamReader::eof() const
{
	return offset == size;
}

bool StreamReader::parse_header()
{
	if (offset + 16 > size)
		return false;
	if (memcmp(blob + offset, "RETROWARP DUMP01", 16))
		return false;

	offset += 16;
	return true;
}

bool StreamReader::parse_resolution(uint32_t &width, uint32_t &height)
{
	if (offset + 2 * sizeof(uint32_t) > size)
		return false;

	memcpy(&width, blob + offset, sizeof(uint32_t));
	offset += sizeof(uint32_t);
	memcpy(&height, blob + offset, sizeof(uint32_t));
	offset += sizeof(uint32_t);
	return true;
}

bool StreamReader::parse_num_textures(uint32_t &count)
{
	return parse_uint(count);
}

bool StreamReader::parse_op(Op &op)
{
	if (offset + 4 > size)
		return false;

	if (memcmp(blob + offset, "TEX ", 4) == 0)
	{
		op = Op::TEX;
		offset += 4;
		return true;
	}
	else if (memcmp(blob + offset, "PRIM", 4) == 0)
	{
		op = Op::PRIM;
		offset += 4;
		return true;
	}
	else
		return false;
}

bool StreamReader::parse_uint(uint32_t &value)
{
	if (offset + sizeof(uint32_t) > size)
		return false;
	memcpy(&value, blob + offset, sizeof(uint32_t));
	offset += sizeof(uint32_t);
	return true;
}

bool StreamReader::parse_primitive(PrimitiveSetup &setup)
{
	if (offset + sizeof(setup) > size)
		return false;
	memcpy(&setup, blob + offset, sizeof(setup));
	offset += sizeof(setup);
	return true;
}

int main(int argc, char **argv)
{
	if (argc != 2)
	{
		LOGE("Usage: dump-bench <path>\n");
		return EXIT_FAILURE;
	}

	Global::init();
	Global::filesystem()->register_protocol("assets", std::make_unique<OSFilesystem>(ASSET_DIRECTORY));

	auto dump_file = Global::filesystem()->open(argv[1], FileMode::ReadOnly);
	if (!dump_file)
	{
		LOGE("Failed to open %s\n", argv[1]);
		return EXIT_FAILURE;
	}

	auto *mapped = static_cast<const uint8_t *>(dump_file->map());
	if (!mapped)
	{
		LOGE("Failed to map buffer.\n");
		return EXIT_FAILURE;
	}

	StreamReader reader(mapped, dump_file->get_size());
	if (!reader.parse_header())
	{
		LOGE("Failed to parse header.\n");
		return EXIT_FAILURE;
	}

	uint32_t width, height;
	if (!reader.parse_resolution(width, height))
	{
		LOGE("Failed to parse resolution.\n");
		return EXIT_FAILURE;
	}

	uint32_t num_textures;
	if (!reader.parse_num_textures(num_textures))
	{
		LOGE("Failed to parse num textures.\n");
		return EXIT_FAILURE;
	}

	if (!Vulkan::Context::init_loader(nullptr))
	{
		LOGE("Failed to init loader.\n");
		return EXIT_FAILURE;
	}

	Vulkan::Context ctx;
	if (!ctx.init_instance_and_device(nullptr, 0, nullptr, 0))
	{
		LOGE("Failed to create instance.\n");
		return EXIT_FAILURE;
	}

	Vulkan::Device device;
	device.set_context(ctx);

	std::vector<Vulkan::ImageHandle> textures(num_textures);
	for (unsigned i = 0; i < num_textures; i++)
	{
		auto path = std::string(argv[1]) + ".tex." + std::to_string(i);
		auto tex_file = load_texture_from_file(path, ColorSpace::Linear);
		if (tex_file.empty())
		{
			LOGE("Failed to load texture.\n");
			return EXIT_FAILURE;
		}

		auto staging = device.create_image_staging_buffer(tex_file.get_layout());
		auto info = Vulkan::ImageCreateInfo::immutable_2d_image(tex_file.get_layout().get_width(),
		                                                        tex_file.get_layout().get_height(),
		                                                        VK_FORMAT_R8G8B8A8_UNORM, true);
		info.misc |= Vulkan::IMAGE_MISC_CONCURRENT_QUEUE_ASYNC_COMPUTE_BIT |
		             Vulkan::IMAGE_MISC_CONCURRENT_QUEUE_GRAPHICS_BIT;
		textures[i] = device.create_image_from_staging_buffer(info, &staging);
	}

	std::vector<std::pair<unsigned, PrimitiveSetup>> commands;
	uint32_t current_state_index = 0;
	while (!reader.eof())
	{
		StreamReader::Op op;
		if (!reader.parse_op(op))
		{
			LOGE("Failed to parse op.\n");
			return EXIT_FAILURE;
		}

		if (op == StreamReader::Op::TEX)
		{
			if (!reader.parse_uint(current_state_index))
			{
				LOGE("Failed to parse uint.\n");
				return EXIT_FAILURE;
			}
		}
		else if (op == StreamReader::Op::PRIM)
		{
			PrimitiveSetup setup;
			if (!reader.parse_primitive(setup))
			{
				LOGE("Failed to parse primitive.\n");
				return EXIT_FAILURE;
			}
			commands.push_back({ current_state_index, setup });
		}
	}

	RasterizerGPU rasterizer;
	rasterizer.init(device);
	rasterizer.resize(width, height);

	unsigned current_state[RasterizerGPU::NUM_STATE_INDICES];

	auto start_run = Util::get_current_time_nsecs();
	for (unsigned i = 0; i < 1000; i++)
	{
		for (unsigned j = 0; j < RasterizerGPU::NUM_STATE_INDICES; j++)
			current_state[j] = ~0u;

		device.next_frame_context();
		rasterizer.clear_depth();
		rasterizer.clear_color();
		for (auto &command : commands)
		{
			unsigned masked_state_index = command.first & (RasterizerGPU::NUM_STATE_INDICES - 1);
			if (current_state[masked_state_index] != ~0u &&
			    current_state[masked_state_index] != command.first)
			{
				rasterizer.flush();
			}

			current_state[masked_state_index] = command.first;

			rasterizer.set_state_index(masked_state_index);
			rasterizer.set_texture(masked_state_index, textures[command.first]->get_view());
			rasterizer.rasterize_primitives(&command.second, 1);
		}
		rasterizer.flush();
	}
	auto end_run = Util::get_current_time_nsecs();
	LOGI("Total time: %.3f s\n", (end_run - start_run) * 1e-9);

	rasterizer.save_canvas("canvas.png");
}
