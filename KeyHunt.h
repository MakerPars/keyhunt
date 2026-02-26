#ifndef KEYHUNTH
#define KEYHUNTH

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "SECP256k1.h"
#include "Bloom.h"
#include "GPU/GPUEngine.h"
#ifdef WIN64
#include <Windows.h>
#endif

#define CPU_GRP_SIZE 1024

class KeyHunt;

struct TH_PARAM {

	KeyHunt* obj;
	int  threadId;
	std::atomic<bool> isRunning{ false };
	std::atomic<bool> hasStarted{ false };

	int  gridSizeX{ 0 };
	int  gridSizeY{ 0 };
	int  gpuId{ 0 };

	Int rangeStart;
	Int rangeEnd;

};


class KeyHunt
{

public:

	KeyHunt(const std::string& addressFile, const std::vector<unsigned char>& addressHash, 
		int searchMode, bool useGpu, const std::string& outputFile, bool useSSE, uint32_t maxFound,
		const std::string& rangeStart, const std::string& rangeEnd, bool& should_exit);
	~KeyHunt();

	void Search(int nbThread, std::vector<int> gpuId, std::vector<int> gridSize, bool& should_exit);
	void FindKeyCPU(TH_PARAM* p);
	void FindKeyGPU(TH_PARAM* p);

private:

	std::string GetHex(std::vector<unsigned char>& buffer);
	bool checkPrivKey(std::string addr, Int& key, int32_t incr, bool mode);
	void checkAddresses(bool compressed, Int key, int i, Point p1);
	void checkAddresses2(bool compressed, Int key, int i, Point p1);
	void checkAddressesSSE(bool compressed, Int key, int i, Point p1, Point p2, Point p3, Point p4);
	void checkAddressesSSE2(bool compressed, Int key, int i, Point p1, Point p2, Point p3, Point p4);
	void output(std::string addr, std::string pAddr, std::string pAddrHex);
	bool isAlive(TH_PARAM* p);

	bool hasStarted(TH_PARAM* p);
	uint64_t getGPUCount();
	uint64_t getCPUCount();
	void SetupRanges(uint32_t totalThreads);
	void InitP2P(const std::vector<int>& gpuIds);
	void RebalanceGPURanges();
	bool AcquireNextGPUChunk(int gpuId, int nbThread, Int& chunkStart, Int& chunkEnd, uint64_t& chunkIterations);
	uint64_t GetGPUChunkIterationsHint(int gpuId);

	void getCPUStartingKey(int thId, Int &tRangeStart, Int& key, Point& startP);
	void getGPUStartingKeys(int thId, Int& tRangeStart, Int& tRangeEnd, int groupSize, int nbThread, Int* keys, Point* p, bool verbose = true);

	int CheckBloomBinary(const uint8_t* hash);
	bool MatchHash160(uint32_t* _h);
	std::string formatThousands(uint64_t x);
	char* toTimeStr(int sec, char* timeStr);

	static constexpr size_t   HASH160_BYTES = 20;
	static constexpr uint64_t DEFAULT_RANGE_SIZE = 10'000'000'000'000'000ULL;
	static constexpr int      REBALANCE_INTERVAL = 100;

	struct GPUPerfStats {
		int gpuId{ -1 };
		double keysPerSec{ 0.0 };
		uint64_t assignedRange{ 0 };
	};

	std::unique_ptr<Secp256K1> secp;
	std::unique_ptr<Bloom> bloom;

	std::atomic<uint64_t> counters[256]{};
	double startTime;

	int searchMode;
	int searchType;
	int addressMode;

	bool useGpu;
	std::atomic<bool> endOfSearch{ false };
	int nbCPUThread;
	int nbGPUThread;
	std::atomic<int> nbFoundKey{ 0 };
	
	std::string outputFile;
	std::string addressFile;
	uint32_t hash160[5];
	bool useSSE;

	Int rangeStart;
	Int rangeEnd;
	Int rangeDiff;
	Int rangeDiff2;

	uint32_t maxFound;

	std::unique_ptr<uint8_t[]> DATA;
	uint64_t TOTAL_ADDR;
	uint64_t BLOOM_N;
	std::vector<GPUPerfStats> gpuStats;
	std::mutex gpuStatsMutex;
	std::mutex gpuWorkMutex;
	std::mutex ghMutex;
	Int gpuWorkCursor;
	Int gpuWorkEnd;
	bool gpuWorkPoolInitialized{ false };
	bool gpuTailWarned{ false };

};

#endif // KEYHUNTH
