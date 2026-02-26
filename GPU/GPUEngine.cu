/*
 * This file is part of the VanitySearch distribution (https://github.com/JeanLucPons/VanitySearch).
 * Copyright (c) 2019 Jean Luc PONS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "GPUEngine.h"
#include <cuda.h>
#include <cuda_runtime.h>
#include <device_launch_parameters.h>

#include <algorithm>
#include <stdint.h>
#include <stdexcept>
#include "../hash/sha256.h"
#include "../hash/ripemd160.h"
#include "../Timer.h"

#include "GPUGroup.h"
#include "GPUMath.h"
#include "GPUHash.h"
#include "GPUBase58.h"
#include "GPUCompute.h"

// ---------------------------------------------------------------------------------------
#define CudaSafeCall( err ) __cudaSafeCall( err, __FILE__, __LINE__ )

inline void __cudaSafeCall(cudaError err, const char* file, const int line)
{
	if (cudaSuccess != err)
	{
		fprintf(stderr, "cudaSafeCall() failed at %s:%i : %s\n", file, line, cudaGetErrorString(err));
		throw std::runtime_error("cudaSafeCall failed");
	}
	return;
}

// ---------------------------------------------------------------------------------------

// mode address file
__global__ void comp_keys(uint32_t mode, uint8_t* bloomLookUp, int BLOOM_BITS, uint8_t BLOOM_HASHES,
	uint64_t* keys, uint32_t maxFound, uint32_t* found)
{

	int xPtr = (blockIdx.x * blockDim.x) * 8;
	int yPtr = xPtr + 4 * blockDim.x;
	ComputeKeys(mode, keys + xPtr, keys + yPtr, bloomLookUp, BLOOM_BITS, BLOOM_HASHES, maxFound, found);

}

__global__ void comp_keys_comp(uint8_t* bloomLookUp, int BLOOM_BITS, uint8_t BLOOM_HASHES, uint64_t* keys,
	uint32_t maxFound, uint32_t* found)
{

	int xPtr = (blockIdx.x * blockDim.x) * 8;
	int yPtr = xPtr + 4 * blockDim.x;
	ComputeKeysComp(keys + xPtr, keys + yPtr, bloomLookUp, BLOOM_BITS, BLOOM_HASHES, maxFound, found);

}

// mode single address
__global__ void comp_keys2(uint32_t mode, uint32_t* hash160, uint64_t* keys, uint32_t maxFound, uint32_t* found)
{

	int xPtr = (blockIdx.x * blockDim.x) * 8;
	int yPtr = xPtr + 4 * blockDim.x;
	ComputeKeys2(mode, keys + xPtr, keys + yPtr, hash160, maxFound, found);

}

__global__ void comp_keys_comp2(uint32_t* hash160, uint64_t* keys, uint32_t maxFound, uint32_t* found)
{

	int xPtr = (blockIdx.x * blockDim.x) * 8;
	int yPtr = xPtr + 4 * blockDim.x;
	ComputeKeysComp2(keys + xPtr, keys + yPtr, hash160, maxFound, found);

}

__global__ void comp_keys_comp22(uint32_t mode, uint32_t* hash160, uint64_t* keys, uint32_t maxFound, uint32_t* found)
{

	int xPtr = (blockIdx.x * blockDim.x) * 8;
	int yPtr = xPtr + 4 * blockDim.x;
	ComputeKeys2(mode, keys + xPtr, keys + yPtr, hash160, maxFound, found);

}

__global__ void clear_counter(uint32_t* found)
{
	ClearCouter(found);
}

// ---------------------------------------------------------------------------------------

using namespace std;

int _ConvertSMVer2Cores(int major, int minor)
{
	struct SMToCores { int SM; int Cores; };
	static const SMToCores table[] = {
		{0x20,  32},
		{0x21,  48},
		{0x30, 192},
		{0x32, 192},
		{0x35, 192},
		{0x37, 192},
		{0x50, 128},
		{0x52, 128},
		{0x53, 128},
		{0x60,  64},
		{0x61, 128},
		{0x62, 128},
		{0x70,  64},
		{0x72,  64},
		{0x75,  64},
		{0x80,  64},
		{0x86, 128},
		{0x87, 128},
		{0x89, 128},
		{0x90, 128},
		{-1,   -1}
	};

	int key = (major << 4) + minor;
	for (int i = 0; table[i].SM != -1; ++i) {
		if (table[i].SM == key) {
			return table[i].Cores;
		}
	}

	if (major >= 8) return 128;
	return 64;
}

void GPUEngine::ConfigureCacheForArch(const cudaDeviceProp& prop)
{
	if (prop.major < 8) {
		CudaSafeCall(cudaDeviceSetCacheConfig(cudaFuncCachePreferL1));
		CudaSafeCall(cudaFuncSetCacheConfig(comp_keys, cudaFuncCachePreferL1));
		CudaSafeCall(cudaFuncSetCacheConfig(comp_keys_comp, cudaFuncCachePreferL1));
		CudaSafeCall(cudaFuncSetCacheConfig(comp_keys2, cudaFuncCachePreferL1));
		CudaSafeCall(cudaFuncSetCacheConfig(comp_keys_comp22, cudaFuncCachePreferL1));
	}
	else {
		int carveout = cudaSharedmemCarveoutMaxL1;
		CudaSafeCall(cudaFuncSetAttribute(comp_keys,
			cudaFuncAttributePreferredSharedMemoryCarveout, carveout));
		CudaSafeCall(cudaFuncSetAttribute(comp_keys_comp,
			cudaFuncAttributePreferredSharedMemoryCarveout, carveout));
		CudaSafeCall(cudaFuncSetAttribute(comp_keys2,
			cudaFuncAttributePreferredSharedMemoryCarveout, carveout));
		CudaSafeCall(cudaFuncSetAttribute(comp_keys_comp22,
			cudaFuncAttributePreferredSharedMemoryCarveout, carveout));
	}
}

int GPUEngine::ComputeOptimalGridBlocks(const cudaDeviceProp& prop, int requestedGroups)
{
	if (requestedGroups > 0) return requestedGroups;

	int optimalBlocksPerSM = 0;
	CudaSafeCall(cudaOccupancyMaxActiveBlocksPerMultiprocessor(
		&optimalBlocksPerSM,
		comp_keys_comp,
		nbThreadPerGroup,
		0));

	float multiplier = 1.0f;
	int sm = (prop.major << 4) + prop.minor;
	if (sm >= 0x90) multiplier = 2.0f;
	else if (sm >= 0x89) multiplier = 2.0f;
	else if (sm >= 0x80) multiplier = 1.5f;

	int gridBlocks = (int)(prop.multiProcessorCount * optimalBlocksPerSM * multiplier);
	if (gridBlocks < prop.multiProcessorCount) gridBlocks = prop.multiProcessorCount;
	return gridBlocks;
}

size_t GPUEngine::ComputeOptimalStackSize(const cudaDeviceProp& prop)
{
	int sm = (prop.major << 4) + prop.minor;
	if (sm >= 0x80) return 65536;
	if (sm >= 0x70) return 49152;
	return 32768;
}

GPUEngine::GPUEngine(int nbThreadGroup, int nbThreadPerGroup, int gpuId, uint32_t maxFound, /*bool rekey,*/
	int64_t BLOOM_SIZE, uint64_t BLOOM_BITS, uint8_t BLOOM_HASHES, const uint8_t* BLOOM_DATA,
	uint8_t* DATA, uint64_t TOTAL_ADDR)
{
	// Initialise CUDA
	this->nbThreadPerGroup = nbThreadPerGroup;
	this->inputHash160 = nullptr;
	this->inputHash160Pinned = nullptr;
	this->inputBloomLookUp = nullptr;
	this->inputBloomLookUpPinned = nullptr;
	this->inputKey = nullptr;
	this->inputKeyPinned = nullptr;
	this->inputKey2 = nullptr;
	this->inputKeyPinned2 = nullptr;
	this->outputBuffer = nullptr;
	this->outputBufferPinned = nullptr;
	this->outputBuffer2 = nullptr;
	this->outputBufferPinned2 = nullptr;
	this->computeStream = nullptr;
	this->transferStream = nullptr;
	this->kernelDoneEvent = nullptr;
	this->transferDoneEvent = nullptr;
	this->pipelinePrimed = false;
	this->activeOutputSlot = 0;

	this->BLOOM_SIZE = BLOOM_SIZE;
	this->BLOOM_BITS = BLOOM_BITS;
	this->BLOOM_HASHES = BLOOM_HASHES;
	this->DATA = DATA;
	this->TOTAL_ADDR = TOTAL_ADDR;

	initialised = false;

	int deviceCount = 0;
	CudaSafeCall(cudaGetDeviceCount(&deviceCount));

	// This function call returns 0 if there are no CUDA capable devices.
	if (deviceCount == 0) {
		fprintf(stderr, "GPUEngine: There are no available device(s) that support CUDA\n");
		return;
	}

	CudaSafeCall(cudaSetDevice(gpuId));

	cudaDeviceProp deviceProp;
	CudaSafeCall(cudaGetDeviceProperties(&deviceProp, gpuId));

	nbThreadGroup = ComputeOptimalGridBlocks(deviceProp, nbThreadGroup);

	this->nbThread = nbThreadGroup * nbThreadPerGroup;
	this->maxFound = maxFound;
	this->outputSize = (maxFound * ITEM_SIZE + 4);

	char tmp[512];
	snprintf(tmp, sizeof(tmp), "GPU #%d %s (%dx%d cores) Grid(%dx%d)",
		gpuId, deviceProp.name, deviceProp.multiProcessorCount,
		_ConvertSMVer2Cores(deviceProp.major, deviceProp.minor),
		nbThread / nbThreadPerGroup,
		nbThreadPerGroup);
	deviceName = std::string(tmp);

	ConfigureCacheForArch(deviceProp);

	size_t stackSize = ComputeOptimalStackSize(deviceProp);
	CudaSafeCall(cudaDeviceSetLimit(cudaLimitStackSize, stackSize));
	CudaSafeCall(cudaStreamCreate(&computeStream));
	CudaSafeCall(cudaStreamCreate(&transferStream));
	CudaSafeCall(cudaEventCreateWithFlags(&kernelDoneEvent, cudaEventDisableTiming));
	CudaSafeCall(cudaEventCreateWithFlags(&transferDoneEvent, cudaEventDisableTiming));

	// Allocate memory
	CudaSafeCall(cudaMalloc((void**)&inputKey, nbThread * 32 * 2));
	CudaSafeCall(cudaHostAlloc(&inputKeyPinned, nbThread * 32 * 2, cudaHostAllocWriteCombined | cudaHostAllocMapped));
	CudaSafeCall(cudaMalloc((void**)&inputKey2, nbThread * 32 * 2));
	CudaSafeCall(cudaHostAlloc(&inputKeyPinned2, nbThread * 32 * 2, cudaHostAllocWriteCombined | cudaHostAllocMapped));

	CudaSafeCall(cudaMalloc((void**)&outputBuffer, outputSize));
	CudaSafeCall(cudaHostAlloc(&outputBufferPinned, outputSize, cudaHostAllocWriteCombined | cudaHostAllocMapped));
	CudaSafeCall(cudaMalloc((void**)&outputBuffer2, outputSize));
	CudaSafeCall(cudaHostAlloc(&outputBufferPinned2, outputSize, cudaHostAllocWriteCombined | cudaHostAllocMapped));

	CudaSafeCall(cudaMalloc((void**)&inputBloomLookUp, BLOOM_SIZE));
	CudaSafeCall(cudaHostAlloc(&inputBloomLookUpPinned, BLOOM_SIZE, cudaHostAllocWriteCombined | cudaHostAllocMapped));

	CudaSafeCall(cudaMalloc((void**)&inputHash160, 5 * sizeof(uint32_t)));
	CudaSafeCall(cudaHostAlloc(&inputHash160Pinned, 5 * sizeof(uint32_t), cudaHostAllocWriteCombined | cudaHostAllocMapped));

	memcpy(inputBloomLookUpPinned, BLOOM_DATA, BLOOM_SIZE);

	CudaSafeCall(cudaMemcpy(inputBloomLookUp, inputBloomLookUpPinned, BLOOM_SIZE, cudaMemcpyHostToDevice));
	CudaSafeCall(cudaFreeHost(inputBloomLookUpPinned));
	inputBloomLookUpPinned = NULL;

	CudaSafeCall(cudaGetLastError());

	searchMode = SEARCH_COMPRESSED;
	searchType = P2PKH;
	initialised = true;

}

GPUEngine::GPUEngine(int nbThreadGroup, int nbThreadPerGroup, int gpuId, uint32_t maxFound, uint32_t* hash160)
{
	// Initialise CUDA
	this->nbThreadPerGroup = nbThreadPerGroup;
	this->inputHash160 = nullptr;
	this->inputHash160Pinned = nullptr;
	this->inputBloomLookUp = nullptr;
	this->inputBloomLookUpPinned = nullptr;
	this->inputKey = nullptr;
	this->inputKeyPinned = nullptr;
	this->inputKey2 = nullptr;
	this->inputKeyPinned2 = nullptr;
	this->outputBuffer = nullptr;
	this->outputBufferPinned = nullptr;
	this->outputBuffer2 = nullptr;
	this->outputBufferPinned2 = nullptr;
	this->computeStream = nullptr;
	this->transferStream = nullptr;
	this->kernelDoneEvent = nullptr;
	this->transferDoneEvent = nullptr;
	this->pipelinePrimed = false;
	this->activeOutputSlot = 0;

	initialised = false;

	int deviceCount = 0;
	CudaSafeCall(cudaGetDeviceCount(&deviceCount));

	// This function call returns 0 if there are no CUDA capable devices.
	if (deviceCount == 0) {
		fprintf(stderr, "GPUEngine: There are no available device(s) that support CUDA\n");
		return;
	}

	CudaSafeCall(cudaSetDevice(gpuId));

	cudaDeviceProp deviceProp;
	CudaSafeCall(cudaGetDeviceProperties(&deviceProp, gpuId));

	nbThreadGroup = ComputeOptimalGridBlocks(deviceProp, nbThreadGroup);

	this->nbThread = nbThreadGroup * nbThreadPerGroup;
	this->maxFound = maxFound;
	this->outputSize = (maxFound * ITEM_SIZE + 4);

	char tmp[512];
	snprintf(tmp, sizeof(tmp), "GPU #%d %s (%dx%d cores) Grid(%dx%d)",
		gpuId, deviceProp.name, deviceProp.multiProcessorCount,
		_ConvertSMVer2Cores(deviceProp.major, deviceProp.minor),
		nbThread / nbThreadPerGroup,
		nbThreadPerGroup);
	deviceName = std::string(tmp);

	ConfigureCacheForArch(deviceProp);

	size_t stackSize = ComputeOptimalStackSize(deviceProp);
	CudaSafeCall(cudaDeviceSetLimit(cudaLimitStackSize, stackSize));
	CudaSafeCall(cudaStreamCreate(&computeStream));
	CudaSafeCall(cudaStreamCreate(&transferStream));
	CudaSafeCall(cudaEventCreateWithFlags(&kernelDoneEvent, cudaEventDisableTiming));
	CudaSafeCall(cudaEventCreateWithFlags(&transferDoneEvent, cudaEventDisableTiming));

	// Allocate memory
	CudaSafeCall(cudaMalloc((void**)&inputKey, nbThread * 32 * 2));
	CudaSafeCall(cudaHostAlloc(&inputKeyPinned, nbThread * 32 * 2, cudaHostAllocWriteCombined | cudaHostAllocMapped));
	CudaSafeCall(cudaMalloc((void**)&inputKey2, nbThread * 32 * 2));
	CudaSafeCall(cudaHostAlloc(&inputKeyPinned2, nbThread * 32 * 2, cudaHostAllocWriteCombined | cudaHostAllocMapped));

	CudaSafeCall(cudaMalloc((void**)&outputBuffer, outputSize));
	CudaSafeCall(cudaHostAlloc(&outputBufferPinned, outputSize, cudaHostAllocWriteCombined | cudaHostAllocMapped));
	CudaSafeCall(cudaMalloc((void**)&outputBuffer2, outputSize));
	CudaSafeCall(cudaHostAlloc(&outputBufferPinned2, outputSize, cudaHostAllocWriteCombined | cudaHostAllocMapped));

	CudaSafeCall(cudaMalloc((void**)&inputHash160, 5 * sizeof(uint32_t)));
	CudaSafeCall(cudaHostAlloc(&inputHash160Pinned, 5 * sizeof(uint32_t), cudaHostAllocWriteCombined | cudaHostAllocMapped));

	memcpy(inputHash160Pinned, hash160, 5 * sizeof(uint32_t));

	CudaSafeCall(cudaMemcpy(inputHash160, inputHash160Pinned, 5 * sizeof(uint32_t), cudaMemcpyHostToDevice));
	CudaSafeCall(cudaFreeHost(inputHash160Pinned));
	inputHash160Pinned = NULL;

	CudaSafeCall(cudaGetLastError());

	searchMode = SEARCH_COMPRESSED;
	searchType = P2PKH;
	initialised = true;

}

int GPUEngine::GetGroupSize()
{
	return GRP_SIZE;
}

void GPUEngine::PrintCudaInfo()
{
	int deviceCount = 0;
	CudaSafeCall(cudaGetDeviceCount(&deviceCount));

	// This function call returns 0 if there are no CUDA capable devices.
	if (deviceCount == 0) {
		fprintf(stderr, "GPUEngine: There are no available device(s) that support CUDA\n");
		return;
	}

	printf("\n=== CUDA Cihaz Bilgisi ===\n");
	printf("CUDA Runtime: %d.%d\n", CUDART_VERSION / 1000, (CUDART_VERSION % 1000) / 10);
	printf("Cihaz Sayisi: %d\n\n", deviceCount);

	for (int i = 0; i < deviceCount; i++) {
		CudaSafeCall(cudaSetDevice(i));
		cudaDeviceProp deviceProp;
		CudaSafeCall(cudaGetDeviceProperties(&deviceProp, i));

		int smKey = (deviceProp.major << 4) + deviceProp.minor;
		int cores = _ConvertSMVer2Cores(deviceProp.major, deviceProp.minor);
		const char* family = "Bilinmeyen";
		if (smKey >= 0x90) family = "Hopper (H100/H200)";
		else if (smKey >= 0x89) family = "Ada Lovelace (L40S/L4/RTX 40xx)";
		else if (smKey >= 0x80) family = "Ampere (A100/RTX 30xx)";
		else if (smKey >= 0x75) family = "Turing (RTX 20xx/GTX 16xx)";
		else if (smKey >= 0x70) family = "Volta (V100)";
		else if (smKey >= 0x60) family = "Pascal (P100/GTX 10xx)";

		double memGB = (double)deviceProp.totalGlobalMem / 1e9;
		double l2MB = (double)deviceProp.l2CacheSize / 1e6;
		int memoryClockRateKHz = 0;
		cudaError_t memClkErr = cudaDeviceGetAttribute(&memoryClockRateKHz, cudaDevAttrMemoryClockRate, i);
		double bwGBs = 0.0;
		if (memClkErr == cudaSuccess && memoryClockRateKHz > 0 && deviceProp.memoryBusWidth > 0) {
			bwGBs = 2.0 * (double)memoryClockRateKHz * (deviceProp.memoryBusWidth / 8.0) / 1e6;
		}

		printf("GPU %d: %s\n", i, deviceProp.name);
		printf("  Aile           : %s\n", family);
		printf("  SM Versiyonu   : sm_%d%d\n", deviceProp.major, deviceProp.minor);
		printf("  SM Sayisi      : %d\n", deviceProp.multiProcessorCount);
		printf("  Cekirdek/SM    : %d\n", cores);
		printf("  Toplam Cekirdek: %d\n", deviceProp.multiProcessorCount * cores);
		printf("  Global Bellek  : %.1f GB\n", memGB);
		printf("  L2 Cache       : %.1f MB\n", l2MB);
		if (bwGBs > 0.0)
			printf("  Bellek Bant    : %.0f GB/s\n", bwGBs);
		else
			printf("  Bellek Bant    : Bilinmiyor\n");
		printf("  ECC            : %s\n", deviceProp.ECCEnabled ? "Etkin" : "Devre Disi");

		for (int j = 0; j < deviceCount; j++) {
			if (i == j) continue;
			int canAccess = 0;
			if (cudaDeviceCanAccessPeer(&canAccess, i, j) == cudaSuccess && canAccess) {
				printf("  P2P -> GPU %d   : Destekleniyor (NVLink/PCIe)\n", j);
			}
		}
		printf("\n");
	}
}

GPUEngine::~GPUEngine()
{
	if (kernelDoneEvent) CudaSafeCall(cudaEventDestroy(kernelDoneEvent));
	if (transferDoneEvent) CudaSafeCall(cudaEventDestroy(transferDoneEvent));
	if (computeStream) CudaSafeCall(cudaStreamDestroy(computeStream));
	if (transferStream) CudaSafeCall(cudaStreamDestroy(transferStream));

	if (inputKeyPinned) CudaSafeCall(cudaFreeHost(inputKeyPinned));
	if (inputKeyPinned2) CudaSafeCall(cudaFreeHost(inputKeyPinned2));
	if (inputHash160Pinned) CudaSafeCall(cudaFreeHost(inputHash160Pinned));
	if (inputBloomLookUpPinned) CudaSafeCall(cudaFreeHost(inputBloomLookUpPinned));
	if (outputBufferPinned) CudaSafeCall(cudaFreeHost(outputBufferPinned));
	if (outputBufferPinned2) CudaSafeCall(cudaFreeHost(outputBufferPinned2));

	if (inputKey) CudaSafeCall(cudaFree(inputKey));
	if (inputKey2) CudaSafeCall(cudaFree(inputKey2));
	if (inputBloomLookUp) CudaSafeCall(cudaFree(inputBloomLookUp));
	if (inputHash160) CudaSafeCall(cudaFree(inputHash160));
	if (outputBuffer) CudaSafeCall(cudaFree(outputBuffer));
	if (outputBuffer2) CudaSafeCall(cudaFree(outputBuffer2));
}

int GPUEngine::GetNbThread()
{
	return nbThread;
}

void GPUEngine::SetSearchMode(int searchMode)
{
	this->searchMode = searchMode;
}

void GPUEngine::SetSearchType(int searchType)
{
	this->searchType = searchType;
}

void GPUEngine::SetAddressMode(int addressMode)
{
	this->addressMode = addressMode;
}

bool GPUEngine::callKernel()
{
	return callKernel(activeOutputSlot == 0 ? outputBuffer : outputBuffer2);
}

bool GPUEngine::callKernel2()
{
	return callKernel2(activeOutputSlot == 0 ? outputBuffer : outputBuffer2);
}

bool GPUEngine::callKernel(uint32_t* targetOutputBuffer)
{
	CudaSafeCall(cudaMemsetAsync(targetOutputBuffer, 0, 4, computeStream));

	if (searchType == P2PKH) {
		if (searchMode == SEARCH_COMPRESSED) {
			comp_keys_comp<<<nbThread / nbThreadPerGroup, nbThreadPerGroup, 0, computeStream>>>(
				inputBloomLookUp, BLOOM_BITS, BLOOM_HASHES, inputKey, maxFound, targetOutputBuffer);
		}
		else {
			comp_keys<<<nbThread / nbThreadPerGroup, nbThreadPerGroup, 0, computeStream>>>(
				searchMode, inputBloomLookUp, BLOOM_BITS, BLOOM_HASHES, inputKey, maxFound, targetOutputBuffer);
		}
	}
	else {
		fprintf(stderr, "GPUEngine: Wrong searchType\n");
		return false;
	}

	cudaError_t err = cudaGetLastError();
	if (err != cudaSuccess) {
		fprintf(stderr, "GPUEngine: Kernel: %s\n", cudaGetErrorString(err));
		return false;
	}
	CudaSafeCall(cudaEventRecord(kernelDoneEvent, computeStream));
	return true;
}

bool GPUEngine::callKernel2(uint32_t* targetOutputBuffer)
{
	CudaSafeCall(cudaMemsetAsync(targetOutputBuffer, 0, 4, computeStream));

	if (searchType == P2PKH) {
		if (searchMode == SEARCH_COMPRESSED) {
			comp_keys_comp22<<<nbThread / nbThreadPerGroup, nbThreadPerGroup, 0, computeStream>>>(
				searchMode, inputHash160, inputKey, maxFound, targetOutputBuffer);
		}
		else {
			comp_keys2<<<nbThread / nbThreadPerGroup, nbThreadPerGroup, 0, computeStream>>>(
				searchMode, inputHash160, inputKey, maxFound, targetOutputBuffer);
		}
	}
	else {
		fprintf(stderr, "GPUEngine: Wrong searchType\n");
		return false;
	}

	cudaError_t err = cudaGetLastError();
	if (err != cudaSuccess) {
		fprintf(stderr, "GPUEngine: Kernel: %s\n", cudaGetErrorString(err));
		return false;
	}
	CudaSafeCall(cudaEventRecord(kernelDoneEvent, computeStream));
	return true;
}

bool GPUEngine::ClearOutBuffer()
{
	uint32_t* current = (activeOutputSlot == 0) ? outputBuffer : outputBuffer2;
	clear_counter<<<nbThread / nbThreadPerGroup, nbThreadPerGroup, 0, computeStream>>>(current);

	cudaError_t err = cudaGetLastError();
	if (err != cudaSuccess) {
		fprintf(stderr, "GPUEngine: ClearOutBuffer: %s\n", cudaGetErrorString(err));
		return false;
	}
	return true;
}

bool GPUEngine::SetKeys(Point* p)
{
	size_t keyBytes = (size_t)nbThread * 32 * 2;

	// Sets the starting keys for each thread
	// p must contains nbThread public keys
	for (int i = 0; i < nbThread; i += nbThreadPerGroup) {
		for (int j = 0; j < nbThreadPerGroup; j++) {

			inputKeyPinned[8 * i + j + 0 * nbThreadPerGroup] = p[i + j].x.bits64[0];
			inputKeyPinned[8 * i + j + 1 * nbThreadPerGroup] = p[i + j].x.bits64[1];
			inputKeyPinned[8 * i + j + 2 * nbThreadPerGroup] = p[i + j].x.bits64[2];
			inputKeyPinned[8 * i + j + 3 * nbThreadPerGroup] = p[i + j].x.bits64[3];

			inputKeyPinned[8 * i + j + 4 * nbThreadPerGroup] = p[i + j].y.bits64[0];
			inputKeyPinned[8 * i + j + 5 * nbThreadPerGroup] = p[i + j].y.bits64[1];
			inputKeyPinned[8 * i + j + 6 * nbThreadPerGroup] = p[i + j].y.bits64[2];
			inputKeyPinned[8 * i + j + 7 * nbThreadPerGroup] = p[i + j].y.bits64[3];

		}
	}

	memcpy(inputKeyPinned2, inputKeyPinned, keyBytes);

	// Fill device memory (current buffer + future ping-pong reserve)
	CudaSafeCall(cudaMemcpy(inputKey, inputKeyPinned, keyBytes, cudaMemcpyHostToDevice));
	CudaSafeCall(cudaMemcpy(inputKey2, inputKeyPinned2, keyBytes, cudaMemcpyHostToDevice));

	activeOutputSlot = 0;
	pipelinePrimed = (addressMode == FILEMODE) ? callKernel(outputBuffer) : callKernel2(outputBuffer);
	return pipelinePrimed;
}

bool GPUEngine::Launch(std::vector<ITEM>& dataFound, bool spinWait)
{
	dataFound.clear();
	if (!pipelinePrimed) return false;

	uint32_t* currentOutputBuffer = (activeOutputSlot == 0) ? outputBuffer : outputBuffer2;
	uint32_t* currentOutputPinned = (activeOutputSlot == 0) ? outputBufferPinned : outputBufferPinned2;
	uint32_t* nextOutputBuffer = (activeOutputSlot == 0) ? outputBuffer2 : outputBuffer;
	int nextSlot = activeOutputSlot ^ 1;

	// Copy header (nbFound) after current kernel is done.
	CudaSafeCall(cudaStreamWaitEvent(transferStream, kernelDoneEvent, 0));
	CudaSafeCall(cudaMemcpyAsync(currentOutputPinned, currentOutputBuffer, 4, cudaMemcpyDeviceToHost, transferStream));
	CudaSafeCall(cudaEventRecord(transferDoneEvent, transferStream));
	if (spinWait) {
		CudaSafeCall(cudaEventSynchronize(transferDoneEvent));
	}
	else {
		while (true) {
			cudaError_t q = cudaEventQuery(transferDoneEvent);
			if (q == cudaSuccess) break;
			if (q != cudaErrorNotReady) CudaSafeCall(q);
			Timer::SleepMillis(1);
		}
	}

	// Look for prefix found
	uint32_t nbFound = currentOutputPinned[0];
	if (nbFound > maxFound) {
		nbFound = maxFound;
	}

	// Queue next kernel on the alternate output buffer as early as possible.
	bool nextOk = callKernel(nextOutputBuffer);

	// Copy current results payload while next kernel is running (if supported by HW).
	size_t copyBytes = (size_t)nbFound * ITEM_SIZE + 4;
	CudaSafeCall(cudaMemcpyAsync(currentOutputPinned, currentOutputBuffer, copyBytes, cudaMemcpyDeviceToHost, transferStream));
	CudaSafeCall(cudaEventRecord(transferDoneEvent, transferStream));
	if (spinWait) {
		CudaSafeCall(cudaEventSynchronize(transferDoneEvent));
	}
	else {
		while (true) {
			cudaError_t q = cudaEventQuery(transferDoneEvent);
			if (q == cudaSuccess) break;
			if (q != cudaErrorNotReady) CudaSafeCall(q);
			Timer::SleepMillis(1);
		}
	}

	for (uint32_t i = 0; i < nbFound; i++) {
		uint32_t* itemPtr = currentOutputPinned + (i * ITEM_SIZE32 + 1);
		uint8_t* hash = (uint8_t*)(itemPtr + 2);
		if (CheckBinary(hash) > 0) {

			ITEM it;
			it.thId = itemPtr[0];
			int16_t* ptr = (int16_t*)&(itemPtr[1]);
			//it.endo = ptr[0] & 0x7FFF;
			it.mode = (ptr[0] & 0x8000) != 0;
			it.incr = ptr[1];
			it.hash = (uint8_t*)(itemPtr + 2);
			dataFound.push_back(it);
		}
	}
	activeOutputSlot = nextSlot;
	pipelinePrimed = nextOk;
	return nextOk;
}

bool GPUEngine::Launch2(std::vector<ITEM>& dataFound, bool spinWait)
{
	dataFound.clear();
	if (!pipelinePrimed) return false;

	uint32_t* currentOutputBuffer = (activeOutputSlot == 0) ? outputBuffer : outputBuffer2;
	uint32_t* currentOutputPinned = (activeOutputSlot == 0) ? outputBufferPinned : outputBufferPinned2;
	uint32_t* nextOutputBuffer = (activeOutputSlot == 0) ? outputBuffer2 : outputBuffer;
	int nextSlot = activeOutputSlot ^ 1;

	CudaSafeCall(cudaStreamWaitEvent(transferStream, kernelDoneEvent, 0));
	CudaSafeCall(cudaMemcpyAsync(currentOutputPinned, currentOutputBuffer, 4, cudaMemcpyDeviceToHost, transferStream));
	CudaSafeCall(cudaEventRecord(transferDoneEvent, transferStream));
	if (spinWait) {
		CudaSafeCall(cudaEventSynchronize(transferDoneEvent));
	}
	else {
		while (true) {
			cudaError_t q = cudaEventQuery(transferDoneEvent);
			if (q == cudaSuccess) break;
			if (q != cudaErrorNotReady) CudaSafeCall(q);
			Timer::SleepMillis(1);
		}
	}

	// Look for prefix found
	uint32_t nbFound = currentOutputPinned[0];
	if (nbFound > maxFound) {
		nbFound = maxFound;
	}

	bool nextOk = callKernel2(nextOutputBuffer);

	size_t copyBytes = (size_t)nbFound * ITEM_SIZE + 4;
	CudaSafeCall(cudaMemcpyAsync(currentOutputPinned, currentOutputBuffer, copyBytes, cudaMemcpyDeviceToHost, transferStream));
	CudaSafeCall(cudaEventRecord(transferDoneEvent, transferStream));
	if (spinWait) {
		CudaSafeCall(cudaEventSynchronize(transferDoneEvent));
	}
	else {
		while (true) {
			cudaError_t q = cudaEventQuery(transferDoneEvent);
			if (q == cudaSuccess) break;
			if (q != cudaErrorNotReady) CudaSafeCall(q);
			Timer::SleepMillis(1);
		}
	}

	for (uint32_t i = 0; i < nbFound; i++) {
		uint32_t* itemPtr = currentOutputPinned + (i * ITEM_SIZE32 + 1);
		ITEM it;
		it.thId = itemPtr[0];
		int16_t* ptr = (int16_t*)&(itemPtr[1]);
		//it.endo = ptr[0] & 0x7FFF;
		it.mode = (ptr[0] & 0x8000) != 0;
		it.incr = ptr[1];
		it.hash = (uint8_t*)(itemPtr + 2);
		dataFound.push_back(it);
	}
	activeOutputSlot = nextSlot;
	pipelinePrimed = nextOk;
	return nextOk;
}


int GPUEngine::CheckBinary(const uint8_t* hash)
{
	uint8_t* temp_read;
	uint64_t half, min, max, current; //, current_offset
	int64_t rcmp;
	int32_t r = 0;
	min = 0;
	current = 0;
	max = TOTAL_ADDR;
	half = TOTAL_ADDR;
	while (!r && half >= 1) {
		half = (max - min) / 2;
		temp_read = DATA + ((current + half) * 20);
		rcmp = memcmp(hash, temp_read, 20);
		if (rcmp == 0) {
			r = 1;  //Found!!
		}
		else {
			if (rcmp < 0) { //data < temp_read
				max = (max - half);
			}
			else { // data > temp_read
				min = (min + half);
			}
			current = min;
		}
	}
	return r;
}





