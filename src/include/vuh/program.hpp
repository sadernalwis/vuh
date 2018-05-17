#pragma once

#include "array.hpp"
#include "device.h"
#include "utils.h"

#include <vulkan/vulkan.hpp>

#include <array>
#include <cstddef>
#include <tuple>
#include <utility>

namespace vuh {
	namespace detail {
		template<class T> struct DictTypeToDsc;

		template<class T> struct DictTypeToDsc<vuh::Array<T>>{
			static constexpr vk::DescriptorType value = vk::DescriptorType::eStorageBuffer;
		};

		// @return tuple element offset
		template<size_t Idx, class T>
		constexpr auto tuple_element_offset(const T& tup)-> std::size_t {
			return size_t(reinterpret_cast<const char*>(&std::get<Idx>(tup))
		                 - reinterpret_cast<const char*>(&tup));
		}

		//
		template<class T, size_t... I>
		constexpr auto spec2entries(const T& specs, std::index_sequence<I...>
		                            )-> std::array<vk::SpecializationMapEntry, sizeof...(I)>
		{
			return {{ { uint32_t(I)
			          , uint32_t(tuple_element_offset<I>(specs))
			          , uint32_t(sizeof(typename std::tuple_element<I, T>::type))
			          }... }};
		}

		/// doc me
		template<class... Ts>
		auto typesToDscTypes()->std::array<vk::DescriptorType, sizeof...(Ts)> {
			return {detail::DictTypeToDsc<Ts>::value...};
		}

		/// doc me
		template<size_t N>
		auto dscTypesToLayout(const std::array<vk::DescriptorType, N>& dsc_types) {
			auto r = std::array<vk::DescriptorSetLayoutBinding, N>{};
			for(size_t i = 0; i < N; ++i){ // can be done compile-time
				r[i] = {uint32_t(i), dsc_types[i], 1, vk::ShaderStageFlagBits::eCompute};
			}
			return r;
		}

		/// @return specialization map array
		template<class... Ts>
		auto specs2mapentries(const std::tuple<Ts...>& specs
		                      )-> std::array<vk::SpecializationMapEntry, sizeof...(Ts)>
		{
			return spec2entries(specs, std::make_index_sequence<sizeof...(Ts)>{});
		}

		/// doc me
		template<class T, size_t... I>
		auto dscinfos2writesets(vk::DescriptorSet dscset, const T& infos
		                        , std::index_sequence<I...>
		                        )-> std::array<vk::WriteDescriptorSet, sizeof...(I)>
		{
			auto r = std::array<vk::WriteDescriptorSet, sizeof...(I)>{{
				{dscset, uint32_t(I), 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &infos[I]}...
			}};
			return r;
		}
	} // namespace detail

	/// Program base class for 'code reuse via inheritance' :).
	class ProgramBase {
	public:
		ProgramBase(vuh::Device& device, const char* filepath, vk::ShaderModuleCreateFlags flags={})
			: ProgramBase(device, read_spirv(filepath), flags)
		{}
		ProgramBase(vuh::Device& device, const std::vector<char>& code
		            , vk::ShaderModuleCreateFlags flags={}
		            )
		   : _device(device)
		{
			_shader = device.createShaderModule(code, flags);
		}

		~ProgramBase() noexcept {
			release();
		}

		ProgramBase(const ProgramBase&) = delete;
		ProgramBase& operator= (const ProgramBase&) = delete;

		/// Move constructor.
		ProgramBase(ProgramBase&& o) noexcept
		   : _shader(o._shader)
		   , _dsclayout(o._dsclayout)
		   , _dscpool(o._dscpool)
		   , _dscset(o._dscset)
		   , _pipecache(o._pipecache)
		   , _pipelayout(o._pipelayout)
		   , _pipeline(o._pipeline)
		   , _device(o._device)
		   , _batch(o._batch)
		{
			o._shader = nullptr; //
		}
		/// Move assignment.
		ProgramBase& operator= (ProgramBase&& o) noexcept {
			release();
			std::memcpy(this, &o, sizeof(ProgramBase));
			o._shader = nullptr;
			return *this;
		}
		
		/// Run the Program object on previously bound parameters, wait for completion.
		/// @pre bacth sizes should be specified before calling this.
		/// @pre all paramerters should be specialized, pushed and bound before calling this.
		auto run()-> void {
			auto submitInfo = vk::SubmitInfo(0, nullptr, nullptr, 1, &_device.computeCmdBuffer()); // submit a single command buffer

			// submit the command buffer to the queue and set up a fence.
			auto queue = _device.computeQueue();
			auto fence = _device.createFence(vk::FenceCreateInfo()); // fence makes sure the control is not returned to CPU till command buffer is depleted
			queue.submit({submitInfo}, fence);
			_device.waitForFences({fence}, true, uint64_t(-1));      // -1 means wait for the fence indefinitely
			_device.destroyFence(fence);
		}
	protected:
		/// Release resources associated with current Program.
		auto release() noexcept-> void {
			if(_shader){
				_device.destroyShaderModule(_shader);
				_device.destroyDescriptorPool(_dscpool);
				_device.destroyDescriptorSetLayout(_dsclayout);
				_device.destroyPipelineCache(_pipecache);
				_device.destroyPipeline(_pipeline);
				_device.destroyPipelineLayout(_pipelayout);
			}
		}
	public: // data
		vk::ShaderModule _shader;
		vk::DescriptorSetLayout _dsclayout;
		vk::DescriptorPool _dscpool;
		vk::DescriptorSet _dscset;
		vk::PipelineCache _pipecache;
		vk::PipelineLayout _pipelayout;
		mutable vk::Pipeline _pipeline;

		vuh::Device& _device;
		std::array<uint32_t, 3> _batch={0, 0, 0};
	}; // class ProgramBase

	/// Specialization constants dependent part of Program
	template<class Specs> class SpecsBase;
	
	/// Non-empty specialization constants interface
	template<template<class...> class Specs, class... Spec_Ts>
	class SpecsBase<Specs<Spec_Ts...>>: public ProgramBase {
	protected:
		SpecsBase(Device& device, const char* filepath, vk::ShaderModuleCreateFlags flags={})
		   : ProgramBase(device, filepath, flags)
		{}
		SpecsBase(Device& device, const std::vector<char>& code, vk::ShaderModuleCreateFlags f={})
		   : ProgramBase(device, code, f)
		{}
		///
		auto init_pipeline()-> void {
			auto specEntries = detail::specs2mapentries(_specs);
			auto specInfo = vk::SpecializationInfo(uint32_t(specEntries.size()), specEntries.data()
																, sizeof(_specs), &_specs);

			// Specify the compute shader stage, and it's entry point (main), and specializations
			auto stageCI = vk::PipelineShaderStageCreateInfo(vk::PipelineShaderStageCreateFlags()
																			 , vk::ShaderStageFlagBits::eCompute
																			 , _shader, "main", &specInfo);
			_pipeline = _device.createPipeline(_pipelayout, _pipecache, stageCI);
		}
	protected:
		std::tuple<Spec_Ts...> _specs; ///< hold the state of specialization constants between call to specs() and actual pipeline creation
	};
	
	/// Empty specialization constants interface
	template<>
	class SpecsBase<typelist<>>: public ProgramBase{
	protected:
		SpecsBase(Device& device, const char* filepath, vk::ShaderModuleCreateFlags flags={})
		   : ProgramBase(device, filepath, flags)
		{}
		SpecsBase(Device& device, const std::vector<char>& code, vk::ShaderModuleCreateFlags f={})
		   : ProgramBase(device, code, f)
		{}
		///
		auto init_pipeline()-> void{
			auto stageCI = vk::PipelineShaderStageCreateInfo(vk::PipelineShaderStageCreateFlags()
																			 , vk::ShaderStageFlagBits::eCompute
																			 , _shader, "main", nullptr);

			_pipeline = _device.createPipeline(_pipelayout, _pipecache, stageCI);
		}
	}; // class SpecsBase
	
	/// doc me
	template<class Specs=typelist<>, class Params=typelist<>> class Program;

	/// specialization to with non-empty specialization constants and push constants
	template<template<class...> class Specs, class... Specs_Ts , class Params>
	class Program<Specs<Specs_Ts...>, Params>: public SpecsBase<Specs<Specs_Ts...>> {
		using Base = SpecsBase<Specs<Specs_Ts...>>;
	public:
		/// Initialize program on a device using spirv code at a given path
		Program(vuh::Device& device, const char* filepath, vk::ShaderModuleCreateFlags flags={})
		   : Base(device, read_spirv(filepath), flags)
		{}

		/// Initialize program on a device from binary spirv code
		Program(vuh::Device& device, const std::vector<char>& code
		        , vk::ShaderModuleCreateFlags flags={}
		        )
		   : Base(device, code, flags)
		{}

		/// Specify running batch size (3D).
 		/// This only sets the dimensions of work batch in units of workgroup, does not start
		/// the actual calculation.
		auto grid(uint32_t x, uint32_t y = 1, uint32_t z = 1)-> Program& {
			Base::_batch = {x, y, z};
			return *this;
		}
		
		/// Specify values of specification constants.
		auto spec(Specs_Ts... specs)-> Program& {
			Base::_specs = std::make_tuple(specs...);
			return *this;
		}
		
		/// Associate buffers to binding points, and pushes the push constants.
		/// Does most of setup here. Program is ready to be run.
		/// @pre Specs and batch sizes should be specified before calling this.
		template<class... Arrs>
		auto bind(const Params& p, Arrs&... args)-> const Program& {
			init_pipelayout(args...);
			alloc_descriptor_sets(args...);
			Base::init_pipeline();
			create_command_buffer(p, args...);
			return *this;
		}

		/// Run program with provided parameters.
		/// @pre bacth sizes should be specified before calling this.
		template<class... Arrs>
		auto operator()(const Params& params, Arrs&... args)-> void {
			bind(params, args...);
			Base::run();
		}
	private: // helpers
		/// set up the state of the kernel that depends on number and types of bound array parameters
		template<class... Arrs>
		auto init_pipelayout(Arrs&...)-> void {
			auto dscTypes = detail::typesToDscTypes<Arrs...>();
			auto bindings = detail::dscTypesToLayout(dscTypes);
			Base::_dsclayout = Base::_device.createDescriptorSetLayout(
			                                       { vk::DescriptorSetLayoutCreateFlags()
			                                       , uint32_t(bindings.size()), bindings.data()
			                                       });
			Base::_pipecache = Base::_device.createPipelineCache({});
			auto push_constant_range = vk::PushConstantRange(vk::ShaderStageFlagBits::eCompute
			                                                 , 0, sizeof(Params));
			Base::_pipelayout = Base::_device.createPipelineLayout(
			        {vk::PipelineLayoutCreateFlags(), 1, &(Base::_dsclayout), 1, &push_constant_range});
		}

		///
		template<class... Arrs>
		auto alloc_descriptor_sets(Arrs&...)-> void {
			assert(Base::_dsclayout);
			if(Base::_dscpool){ // unbind previously bound descriptor sets if any
				Base::_device.destroyDescriptorPool(Base::_dscpool);
				Base::_device.resetCommandPool(Base::_device.computeCmdPool()
				                               , vk::CommandPoolResetFlags());
			}

			auto sbo_descriptors_size = vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer
			                                                   , sizeof...(Arrs));
			auto descriptor_sizes = std::array<vk::DescriptorPoolSize, 1>({sbo_descriptors_size}); // can be done compile-time, but not worth it
			Base::_dscpool = Base::_device.createDescriptorPool(
			                             {vk::DescriptorPoolCreateFlags(), 1 // 1 here is the max number of descriptor sets that can be allocated from the pool
			                              , uint32_t(descriptor_sizes.size()), descriptor_sizes.data()
			                              }
			);
			Base::_dscset = Base::_device.allocateDescriptorSets(
			                                              {Base::_dscpool, 1, &(Base::_dsclayout)})[0];
		}

		///
		template<class... Arrs>
		auto create_command_buffer(const Params& p, Arrs&... args)-> void {
			assert(Base::_pipeline); /// pipeline supposed to be initialized before this

			constexpr auto N = sizeof...(args);
			auto dscinfos = std::array<vk::DescriptorBufferInfo, N>{{{args, 0, args.size_bytes()}... }}; // 0 is the offset here
			auto write_dscsets = detail::dscinfos2writesets(Base::_dscset, dscinfos
			                                                , std::make_index_sequence<N>{});
			Base::_device.updateDescriptorSets(write_dscsets, {}); // associate buffers to binding points in bindLayout

			// Start recording commands into the newly allocated command buffer.
			//	auto beginInfo = vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit); // buffer is only submitted and used once
			auto cmdbuf = Base::_device.computeCmdBuffer();
			auto beginInfo = vk::CommandBufferBeginInfo();
			cmdbuf.begin(beginInfo);

			// Before dispatch bind a pipeline, AND a descriptor set.
			// The validation layer will NOT give warnings if you forget those.
			cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, Base::_pipeline);
			cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, Base::_pipelayout
			                          , 0, {Base::_dscset}, {});

			cmdbuf.pushConstants(Base::_pipelayout, vk::ShaderStageFlagBits::eCompute , 0, sizeof(p), &p);

			cmdbuf.dispatch(Base::_batch[0], Base::_batch[1], Base::_batch[2]); // start compute pipeline, execute the shader
			cmdbuf.end(); // end recording commands
		}
	}; // class Program

	/// specialization with non-empty specialization constants and empty push constants
	template<template<class...> class Specs, class... Specs_Ts>
	class Program<Specs<Specs_Ts...>, typelist<>>: public SpecsBase<Specs<Specs_Ts...>>{
		using Base = SpecsBase<Specs<Specs_Ts...>>;
	public:
		/// Initialize program on a device using spirv code at a given path
		Program(vuh::Device& device, const char* filepath, vk::ShaderModuleCreateFlags flags={})
		   : Base(device, filepath, flags)
		{}

		/// Initialize program on a device from binary spirv code
		Program(vuh::Device& device, const std::vector<char>& code
		        , vk::ShaderModuleCreateFlags flags={}
		        )
		   : Base (device, code, flags)
		{}

		/// Specify running batch size (3D).
 		/// This only sets the dimensions of work batch in units of workgroup, does not start
		/// the actual calculation.
		auto grid(uint32_t x, uint32_t y = 1, uint32_t z = 1)-> Program& {
			Base::_batch = {x, y, z};
			return *this;
		}

		/// Specify values of specification constants.
		auto spec(Specs_Ts... specs)-> Program& {
			Base::_specs = std::make_tuple(specs...);
			return *this;
		}

		/// Associate buffers to binding points, and pushes the push constants.
		/// Does most of setup here. Program is ready to be run.
		/// @pre Specs and batch sizes should be specified before calling this.
		template<class... Arrs>
		auto bind(Arrs&... args)-> const Program& {
			init_pipelayout(args...);
			alloc_descriptor_sets(args...);
			Base::init_pipeline();
			create_command_buffer(args...);
			return *this;
		}

		/// Run program with provided parameters.
		/// @pre bacth sizes should be specified before calling this.
		template<class... Arrs>
		auto operator()(Arrs&... args)-> void {
			bind(args...);
			Base::run();
		}
	private: // helpers
		/// set up the state of the kernel that depends on number and types of bound array parameters
		template<class... Arrs>
		auto init_pipelayout(Arrs&...)-> void {
			auto dscTypes = detail::typesToDscTypes<Arrs...>();
			auto bindings = detail::dscTypesToLayout(dscTypes);
			Base::_dsclayout = Base::_device.createDescriptorSetLayout(
			                                    { vk::DescriptorSetLayoutCreateFlags()
			                                    , uint32_t(bindings.size()), bindings.data()
			                                    });
			Base::_pipecache = Base::_device.createPipelineCache({});
			Base::_pipelayout = Base::_device.createPipelineLayout(
						     {vk::PipelineLayoutCreateFlags(), 1, &(Base::_dsclayout)});
		}

		///
		template<class... Arrs>
		auto alloc_descriptor_sets(Arrs&...)-> void {
			assert(Base::_dsclayout);
			if(Base::_dscpool){ // unbind previously bound descriptor sets if any
				Base::_device.destroyDescriptorPool(Base::_dscpool);
				Base::_device.resetCommandPool(Base::_device.computeCmdPool(), vk::CommandPoolResetFlags());
			}

			auto sbo_descriptors_size = vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer
			                                                   , sizeof...(Arrs));
			auto descriptor_sizes = std::array<vk::DescriptorPoolSize, 1>({sbo_descriptors_size}); // can be done compile-time, but not worth it
			Base::_dscpool = Base::_device.createDescriptorPool(
			                        {vk::DescriptorPoolCreateFlags(), 1 // 1 here is the max number of descriptor sets that can be allocated from the pool
			                         , uint32_t(descriptor_sizes.size()), descriptor_sizes.data()
			                         }
			);
			Base::_dscset = Base::_device.allocateDescriptorSets({Base::_dscpool, 1, &(Base::_dsclayout)})[0];
		}

		///
		template<class... Arrs>
		auto create_command_buffer(Arrs&... args)-> void {
			assert(Base::_pipeline); /// pipeline supposed to be initialized before this

			constexpr auto N = sizeof...(args);
			auto dscinfos = std::array<vk::DescriptorBufferInfo, N>{{{args, 0, args.size_bytes()}... }}; // 0 is the offset here
			auto write_dscsets = detail::dscinfos2writesets(Base::_dscset, dscinfos
			                                                , std::make_index_sequence<N>{});
			Base::_device.updateDescriptorSets(write_dscsets, {}); // associate buffers to binding points in bindLayout

			// Start recording commands into the newly allocated command buffer.
			//	auto beginInfo = vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit); // buffer is only submitted and used once
			auto cmdbuf = Base::_device.computeCmdBuffer();
			auto beginInfo = vk::CommandBufferBeginInfo();
			cmdbuf.begin(beginInfo);

			// Before dispatch bind a pipeline, AND a descriptor set.
			// The validation layer will NOT give warnings if you forget those.
			cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, Base::_pipeline);
			cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, Base::_pipelayout, 0
			                           , {Base::_dscset}, {});

			cmdbuf.dispatch(Base::_batch[0], Base::_batch[1], Base::_batch[2]); // start compute pipeline, execute the shader
			cmdbuf.end(); // end recording commands
		}
	}; // class Program

} // namespace vuh
