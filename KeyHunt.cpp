#include "KeyHunt.h"
#include "Base58.h"
#include "Bech32.h"
#include "hash/sha256.h"
#include "hash/sha512.h"
#include "IntGroup.h"
#include "Timer.h"
#include "hash/ripemd160.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#ifdef WITHGPU
#include <cuda_runtime_api.h>
#endif

using namespace std;

Point Gn[CPU_GRP_SIZE / 2];
Point _2Gn;

// ----------------------------------------------------------------------------

KeyHunt::KeyHunt(const std::string& addressFile, const std::vector<unsigned char>& addressHash,
	int searchMode, bool useGpu, const std::string& outputFile, bool useSSE,
	uint32_t maxFound, const std::string& rangeStart, const std::string& rangeEnd,
	bool& should_exit)
{
	this->searchMode = searchMode;
	this->useGpu = useGpu;
	this->outputFile = outputFile;
	this->useSSE = useSSE;
	this->nbGPUThread = 0;
	this->addressFile = addressFile;
	this->maxFound = maxFound;
	this->searchType = P2PKH;
	this->rangeStart.SetBase16(rangeStart.c_str());
	if (rangeEnd.length() <= 0) {
		this->rangeEnd.Set(&this->rangeStart);
		this->rangeEnd.Add(DEFAULT_RANGE_SIZE);
	}
	else {
		this->rangeEnd.SetBase16(rangeEnd.c_str());
		if (!this->rangeEnd.IsGreaterOrEqual(&this->rangeStart)) {
			fprintf(stderr, "[HATA] Start range end range'den büyük, takas edildi\n");
			Int t(this->rangeEnd);
			this->rangeEnd.Set(&this->rangeStart);
			this->rangeStart.Set(&t);
		}
	}
	this->rangeDiff2.Set(&this->rangeEnd);
	this->rangeDiff2.Sub(&this->rangeStart);

	this->addressMode = FILEMODE;
	if (addressHash.size() > 0 && this->addressFile.length() <= 0)
		this->addressMode = SINGLEMODE;

	secp = std::make_unique<Secp256K1>();
	secp->Init();

	if (this->addressMode == FILEMODE) {

		// load address file
		uint8_t buf[HASH160_BYTES];
		FILE* wfd;
		uint64_t N = 0;

		wfd = fopen(this->addressFile.c_str(), "rb");
		if (!wfd) {
			fprintf(stderr, "[HATA] %s açılamadı\n", this->addressFile.c_str());
			throw std::runtime_error("Adres dosyası açılamadı");
		}

		std::error_code ec;
		uintmax_t fileSize = std::filesystem::file_size(this->addressFile, ec);
		if (ec) {
			fclose(wfd);
			fprintf(stderr, "[HATA] Dosya boyutu okunamadı: %s\n", ec.message().c_str());
			throw std::runtime_error("Dosya boyutu hatası");
		}
		N = static_cast<uint64_t>(fileSize / HASH160_BYTES);
		rewind(wfd);

		DATA = std::make_unique<uint8_t[]>(N * HASH160_BYTES);
		memset(DATA.get(), 0, N * HASH160_BYTES);

		bloom = std::make_unique<Bloom>(2 * N, 0.000001);

		uint64_t percent = (N > 1) ? ((N - 1) / 100) : 1;
		if (percent == 0) percent = 1;
		uint64_t i = 0;
		printf("\n");
		while (i < N && !should_exit) {
			memset(buf, 0, HASH160_BYTES);
			memset(DATA.get() + (i * HASH160_BYTES), 0, HASH160_BYTES);
			if (fread(buf, 1, HASH160_BYTES, wfd) == HASH160_BYTES) {
				bloom->add(buf, HASH160_BYTES);
				memcpy(DATA.get() + (i * HASH160_BYTES), buf, HASH160_BYTES);
				if (i % percent == 0) {
					printf("\rLoading      : %llu %%", (i / percent));
					fflush(stdout);
				}
			}
			i++;
		}
		printf("\n");
		fclose(wfd);

		if (should_exit) {
			throw std::runtime_error("Arama kullanıcı tarafından iptal edildi");
		}

		BLOOM_N = bloom->get_bytes();
		TOTAL_ADDR = N;
		printf("Loaded       : %s address\n", formatThousands(i).c_str());
		printf("\n");

		bloom->print();
		printf("\n");
	}
	else {
		for (size_t i = 0; i < addressHash.size(); i++) {
			((uint8_t*)hash160)[i] = addressHash.at(i);
		}
		printf("\n");
	}

	// Compute Generator table G[n] = (n+1)*G
	Point g = secp->G;
	Gn[0] = g;
	g = secp->DoubleDirect(g);
	Gn[1] = g;
	for (int i = 2; i < CPU_GRP_SIZE / 2; i++) {
		g = secp->AddDirect(g, secp->G);
		Gn[i] = g;
	}
	// _2Gn = CPU_GRP_SIZE*G
	_2Gn = secp->DoubleDirect(Gn[CPU_GRP_SIZE / 2 - 1]);

	char* ctimeBuff;
	time_t now = time(NULL);
	ctimeBuff = ctime(&now);
	printf("Start Time   : %s", ctimeBuff);

	printf("Global start : %064s (%d bit)\n", this->rangeStart.GetBase16().c_str(), this->rangeStart.GetBitLength());
	printf("Global end   : %064s (%d bit)\n", this->rangeEnd.GetBase16().c_str(), this->rangeEnd.GetBitLength());
	printf("Global range : %064s (%d bit)\n", this->rangeDiff2.GetBase16().c_str(), this->rangeDiff2.GetBitLength());

}

KeyHunt::~KeyHunt()
{
}

// ----------------------------------------------------------------------------

double log1(double x)
{
	// Use taylor series to approximate log(1-x)
	return -x - (x * x) / 2.0 - (x * x * x) / 3.0 - (x * x * x * x) / 4.0;
}

void KeyHunt::output(string addr, string pAddr, string pAddrHex)
{
	std::lock_guard<std::mutex> lock(ghMutex);

	FILE* f = stdout;
	bool needToClose = false;

	if (outputFile.length() > 0) {
		f = fopen(outputFile.c_str(), "a");
		if (f == NULL) {
			fprintf(stderr, "[HATA] %s yazmak için açılamadı\n", outputFile.c_str());
			f = stdout;
		}
		else {
			needToClose = true;
		}
	}

	if (!needToClose)
		printf("\n");

	fprintf(f, "PubAddress: %s\n", addr.c_str());
	fprintf(stdout, "\n==================================================================\n");
	fprintf(stdout, "PubAddress: %s\n", addr.c_str());

	{


		switch (searchType) {
		case P2PKH:
			fprintf(f, "Priv (WIF): p2pkh:%s\n", pAddr.c_str());
			fprintf(stdout, "Priv (WIF): p2pkh:%s\n", pAddr.c_str());
			break;
		case P2SH:
			fprintf(f, "Priv (WIF): p2wpkh-p2sh:%s\n", pAddr.c_str());
			fprintf(stdout, "Priv (WIF): p2wpkh-p2sh:%s\n", pAddr.c_str());
			break;
		case BECH32:
			fprintf(f, "Priv (WIF): p2wpkh:%s\n", pAddr.c_str());
			fprintf(stdout, "Priv (WIF): p2wpkh:%s\n", pAddr.c_str());
			break;
		}
		fprintf(f, "Priv (HEX): 0x%s\n", pAddrHex.c_str());
		fprintf(stdout, "Priv (HEX): 0x%s\n", pAddrHex.c_str());

	}

	fprintf(f, "==================================================================\n");
	fprintf(stdout, "==================================================================\n");

	if (needToClose)
		fclose(f);

}

// ----------------------------------------------------------------------------

bool KeyHunt::checkPrivKey(string addr, Int& key, int32_t incr, bool mode)
{

	Int k(&key);

	k.Add((uint64_t)incr);

	// Check addresses
	Point p = secp->ComputePublicKey(&k);

	string chkAddr = secp->GetAddress(searchType, mode, p);
	if (chkAddr != addr) {

		//Key may be the opposite one (negative zero or compressed key)
		k.Neg();
		k.Add(&secp->order);
		p = secp->ComputePublicKey(&k);

		string chkAddr = secp->GetAddress(searchType, mode, p);
		if (chkAddr != addr) {
			printf("\nWarning, wrong private key generated !\n");
			printf("  Addr :%s\n", addr.c_str());
			printf("  Check:%s\n", chkAddr.c_str());
			//return false;
		}

	}

	output(addr, secp->GetPrivAddress(mode, k), k.GetBase16());

	return true;

}

// ----------------------------------------------------------------------------

void KeyHunt::checkAddresses(bool compressed, Int key, int i, Point p1)
{
	unsigned char h0[20];
	Point pte1[1];
	Point pte2[1];

	// Point
	secp->GetHash160(searchType, compressed, p1, h0);
	if (CheckBloomBinary(h0) > 0) {
		string addr = secp->GetAddress(searchType, compressed, h0);
		if (checkPrivKey(addr, key, i, compressed)) {
			nbFoundKey.fetch_add(1, std::memory_order_relaxed);
		}
	}
}

void KeyHunt::checkAddresses2(bool compressed, Int key, int i, Point p1)
{
	unsigned char h0[20];
	Point pte1[1];
	Point pte2[1];

	// Point
	secp->GetHash160(searchType, compressed, p1, h0);
	if (MatchHash160((uint32_t*)h0)) {
		string addr = secp->GetAddress(searchType, compressed, h0);
		if (checkPrivKey(addr, key, i, compressed)) {
			nbFoundKey.fetch_add(1, std::memory_order_relaxed);
		}
	}
}
// ----------------------------------------------------------------------------

void KeyHunt::checkAddressesSSE(bool compressed, Int key, int i, Point p1, Point p2, Point p3, Point p4)
{
	unsigned char h0[20];
	unsigned char h1[20];
	unsigned char h2[20];
	unsigned char h3[20];
	Point pte1[4];
	Point pte2[4];

	// Point -------------------------------------------------------------------------
	secp->GetHash160(searchType, compressed, p1, p2, p3, p4, h0, h1, h2, h3);
	if (CheckBloomBinary(h0) > 0) {
		string addr = secp->GetAddress(searchType, compressed, h0);
		if (checkPrivKey(addr, key, i + 0, compressed)) {
			nbFoundKey.fetch_add(1, std::memory_order_relaxed);
		}
	}
	if (CheckBloomBinary(h1) > 0) {
		string addr = secp->GetAddress(searchType, compressed, h1);
		if (checkPrivKey(addr, key, i + 1, compressed)) {
			nbFoundKey.fetch_add(1, std::memory_order_relaxed);
		}
	}
	if (CheckBloomBinary(h2) > 0) {
		string addr = secp->GetAddress(searchType, compressed, h2);
		if (checkPrivKey(addr, key, i + 2, compressed)) {
			nbFoundKey.fetch_add(1, std::memory_order_relaxed);
		}
	}
	if (CheckBloomBinary(h3) > 0) {
		string addr = secp->GetAddress(searchType, compressed, h3);
		if (checkPrivKey(addr, key, i + 3, compressed)) {
			nbFoundKey.fetch_add(1, std::memory_order_relaxed);
		}
	}

}


void KeyHunt::checkAddressesSSE2(bool compressed, Int key, int i, Point p1, Point p2, Point p3, Point p4)
{
	unsigned char h0[20];
	unsigned char h1[20];
	unsigned char h2[20];
	unsigned char h3[20];
	Point pte1[4];
	Point pte2[4];

	// Point -------------------------------------------------------------------------
	secp->GetHash160(searchType, compressed, p1, p2, p3, p4, h0, h1, h2, h3);
	if (MatchHash160((uint32_t*)h0)) {
		string addr = secp->GetAddress(searchType, compressed, h0);
		if (checkPrivKey(addr, key, i + 0, compressed)) {
			nbFoundKey.fetch_add(1, std::memory_order_relaxed);
		}
	}
	if (MatchHash160((uint32_t*)h1)) {
		string addr = secp->GetAddress(searchType, compressed, h1);
		if (checkPrivKey(addr, key, i + 1, compressed)) {
			nbFoundKey.fetch_add(1, std::memory_order_relaxed);
		}
	}
	if (MatchHash160((uint32_t*)h2)) {
		string addr = secp->GetAddress(searchType, compressed, h2);
		if (checkPrivKey(addr, key, i + 2, compressed)) {
			nbFoundKey.fetch_add(1, std::memory_order_relaxed);
		}
	}
	if (MatchHash160((uint32_t*)h3)) {
		string addr = secp->GetAddress(searchType, compressed, h3);
		if (checkPrivKey(addr, key, i + 3, compressed)) {
			nbFoundKey.fetch_add(1, std::memory_order_relaxed);
		}
	}

}

// ----------------------------------------------------------------------------
void KeyHunt::getCPUStartingKey(int thId, Int & tRangeStart, Int & key, Point & startP)
{
	key.Set(&tRangeStart);
	Int km(&key);
	km.Add((uint64_t)CPU_GRP_SIZE / 2);
	startP = secp->ComputePublicKey(&km);

}

void KeyHunt::FindKeyCPU(TH_PARAM * ph)
{
	try {
		// Global init
		int thId = ph->threadId;
		Int tRangeStart = ph->rangeStart;
		Int tRangeEnd = ph->rangeEnd;
		counters[thId].store(0, std::memory_order_relaxed);

		// CPU Thread
		auto grp = std::make_unique<IntGroup>(CPU_GRP_SIZE / 2 + 1);

		// Group Init
		Int  key;
		Point startP;
		getCPUStartingKey(thId, tRangeStart, key, startP);

		Int dx[CPU_GRP_SIZE / 2 + 1];
		Point pts[CPU_GRP_SIZE];

		Int dy;
		Int dyn;
		Int _s;
		Int _p;
		Point pp;
		Point pn;
		grp->Set(dx);

		ph->hasStarted.store(true, std::memory_order_release);

		while (!endOfSearch.load(std::memory_order_relaxed)) {
		if (key.IsGreaterOrEqual(&tRangeEnd)) {
			break;
		}

		// Fill group
		int i;
		int hLength = (CPU_GRP_SIZE / 2 - 1);

		for (i = 0; i < hLength; i++) {
			dx[i].ModSub(&Gn[i].x, &startP.x);
		}
		dx[i].ModSub(&Gn[i].x, &startP.x);  // For the first point
		dx[i + 1].ModSub(&_2Gn.x, &startP.x); // For the next center point

		// Grouped ModInv
		grp->ModInv();

		// We use the fact that P + i*G and P - i*G has the same deltax, so the same inverse
		// We compute key in the positive and negative way from the center of the group

		// center point
		pts[CPU_GRP_SIZE / 2] = startP;

		for (i = 0; i < hLength && !endOfSearch.load(std::memory_order_relaxed); i++) {

			pp = startP;
			pn = startP;

			// P = startP + i*G
			dy.ModSub(&Gn[i].y, &pp.y);

			_s.ModMulK1(&dy, &dx[i]);       // s = (p2.y-p1.y)*inverse(p2.x-p1.x);
			_p.ModSquareK1(&_s);            // _p = pow2(s)

			pp.x.ModNeg();
			pp.x.ModAdd(&_p);
			pp.x.ModSub(&Gn[i].x);           // rx = pow2(s) - p1.x - p2.x;

			pp.y.ModSub(&Gn[i].x, &pp.x);
			pp.y.ModMulK1(&_s);
			pp.y.ModSub(&Gn[i].y);           // ry = - p2.y - s*(ret.x-p2.x);

			// P = startP - i*G  , if (x,y) = i*G then (x,-y) = -i*G
			dyn.Set(&Gn[i].y);
			dyn.ModNeg();
			dyn.ModSub(&pn.y);

			_s.ModMulK1(&dyn, &dx[i]);      // s = (p2.y-p1.y)*inverse(p2.x-p1.x);
			_p.ModSquareK1(&_s);            // _p = pow2(s)

			pn.x.ModNeg();
			pn.x.ModAdd(&_p);
			pn.x.ModSub(&Gn[i].x);          // rx = pow2(s) - p1.x - p2.x;

			pn.y.ModSub(&Gn[i].x, &pn.x);
			pn.y.ModMulK1(&_s);
			pn.y.ModAdd(&Gn[i].y);          // ry = - p2.y - s*(ret.x-p2.x);

			pts[CPU_GRP_SIZE / 2 + (i + 1)] = pp;
			pts[CPU_GRP_SIZE / 2 - (i + 1)] = pn;

		}

		// First point (startP - (GRP_SZIE/2)*G)
		pn = startP;
		dyn.Set(&Gn[i].y);
		dyn.ModNeg();
		dyn.ModSub(&pn.y);

		_s.ModMulK1(&dyn, &dx[i]);
		_p.ModSquareK1(&_s);

		pn.x.ModNeg();
		pn.x.ModAdd(&_p);
		pn.x.ModSub(&Gn[i].x);

		pn.y.ModSub(&Gn[i].x, &pn.x);
		pn.y.ModMulK1(&_s);
		pn.y.ModAdd(&Gn[i].y);

		pts[0] = pn;

		// Next start point (startP + GRP_SIZE*G)
		pp = startP;
		dy.ModSub(&_2Gn.y, &pp.y);

		_s.ModMulK1(&dy, &dx[i + 1]);
		_p.ModSquareK1(&_s);

		pp.x.ModNeg();
		pp.x.ModAdd(&_p);
		pp.x.ModSub(&_2Gn.x);

		pp.y.ModSub(&_2Gn.x, &pp.x);
		pp.y.ModMulK1(&_s);
		pp.y.ModSub(&_2Gn.y);
		startP = pp;

		// Check addresses
		if (useSSE) {

			for (int i = 0; i < CPU_GRP_SIZE && !endOfSearch.load(std::memory_order_relaxed); i += 4) {

				switch (searchMode) {
				case SEARCH_COMPRESSED:
					if (addressMode == FILEMODE)
						checkAddressesSSE(true, key, i, pts[i], pts[i + 1], pts[i + 2], pts[i + 3]);
					else
						checkAddressesSSE2(true, key, i, pts[i], pts[i + 1], pts[i + 2], pts[i + 3]);
					break;
				case SEARCH_UNCOMPRESSED:
					if (addressMode == FILEMODE)
						checkAddressesSSE(false, key, i, pts[i], pts[i + 1], pts[i + 2], pts[i + 3]);
					else
						checkAddressesSSE2(false, key, i, pts[i], pts[i + 1], pts[i + 2], pts[i + 3]);
					break;
				case SEARCH_BOTH:
					if (addressMode == FILEMODE) {
						checkAddressesSSE(true, key, i, pts[i], pts[i + 1], pts[i + 2], pts[i + 3]);
						checkAddressesSSE(false, key, i, pts[i], pts[i + 1], pts[i + 2], pts[i + 3]);
					}
					else {
						checkAddressesSSE2(true, key, i, pts[i], pts[i + 1], pts[i + 2], pts[i + 3]);
						checkAddressesSSE2(false, key, i, pts[i], pts[i + 1], pts[i + 2], pts[i + 3]);

					}
					break;
				}
			}
		}
		else {

			for (int i = 0; i < CPU_GRP_SIZE && !endOfSearch.load(std::memory_order_relaxed); i++) {

				switch (searchMode) {
				case SEARCH_COMPRESSED:
					if (addressMode == FILEMODE)
						checkAddresses(true, key, i, pts[i]);
					else
						checkAddresses2(true, key, i, pts[i]);
					break;
				case SEARCH_UNCOMPRESSED:
					if (addressMode == FILEMODE)
						checkAddresses(false, key, i, pts[i]);
					else
						checkAddresses2(false, key, i, pts[i]);
					break;
				case SEARCH_BOTH:
					if (addressMode == FILEMODE) {
						checkAddresses(true, key, i, pts[i]);
						checkAddresses(false, key, i, pts[i]);
					}
					else {
						checkAddresses2(true, key, i, pts[i]);
						checkAddresses2(false, key, i, pts[i]);
					}
					break;
				}
			}
		}

		key.Add((uint64_t)CPU_GRP_SIZE);
			counters[thId].fetch_add(CPU_GRP_SIZE, std::memory_order_relaxed); // Point
		}
	}
	catch (const std::exception& e) {
		ph->hasStarted.store(true, std::memory_order_release);
		fprintf(stderr, "[HATA] CPU thread exception: %s\n", e.what());
		endOfSearch.store(true, std::memory_order_relaxed);
	}
	catch (...) {
		ph->hasStarted.store(true, std::memory_order_release);
		fprintf(stderr, "[HATA] CPU thread bilinmeyen exception\n");
		endOfSearch.store(true, std::memory_order_relaxed);
	}
	ph->isRunning.store(false, std::memory_order_release);
}

// ----------------------------------------------------------------------------

void KeyHunt::getGPUStartingKeys(int thId, Int & tRangeStart, Int & tRangeEnd, int groupSize, int nbThread, Int * keys, Point * p, bool verbose)
{

	Int tRangeDiff(tRangeEnd);
	Int tRangeStart2(tRangeStart);
	Int tRangeEnd2(tRangeStart);

	Int tThreads;
	tThreads.SetInt32(nbThread);
	tRangeDiff.Set(&tRangeEnd);
	tRangeDiff.Sub(&tRangeStart);
	tRangeDiff.Div(&tThreads);

	int rangeShowThreasold = 3;
	int rangeShowCounter = 0;

	for (int i = 0; i < nbThread; i++) {

		keys[i].Set(&tRangeStart2);
		tRangeEnd2.Set(&tRangeStart2);
		tRangeEnd2.Add(&tRangeDiff);


		if (verbose && i < rangeShowThreasold) {
			printf("GPU %d Thread %06d: %064s : %064s\n", (thId - 0x80L), i, tRangeStart2.GetBase16().c_str(), tRangeEnd2.GetBase16().c_str());
		}
		else if (verbose && rangeShowCounter < 1) {
			printf("                   .\n");
			rangeShowCounter++;
			if (i + 1 == nbThread) {
				printf("GPU %d Thread %06d: %064s : %064s\n", (thId - 0x80L), i, tRangeStart2.GetBase16().c_str(), tRangeEnd2.GetBase16().c_str());
			}
		}
		else if (verbose && i + 1 == nbThread) {
			printf("GPU %d Thread %06d: %064s : %064s\n", (thId - 0x80L), i, tRangeStart2.GetBase16().c_str(), tRangeEnd2.GetBase16().c_str());
		}

		tRangeStart2.Add(&tRangeDiff);

		Int k(keys + i);
		// Starting key is at the middle of the group
		k.Add((uint64_t)(groupSize / 2));
		p[i] = secp->ComputePublicKey(&k);
	}
	if (verbose) printf("\n");

}

void KeyHunt::FindKeyGPU(TH_PARAM * ph)
{
	bool ok = true;
	try {
#ifdef WITHGPU

		int thId = ph->threadId;

		std::unique_ptr<GPUEngine> g;

		if (addressMode == FILEMODE) {
			g = std::make_unique<GPUEngine>(ph->gridSizeX, ph->gridSizeY, ph->gpuId, maxFound, BLOOM_N, bloom->get_bits(),
				bloom->get_hashes(), bloom->get_bf(), DATA.get(), TOTAL_ADDR);
		}
		else {
			g = std::make_unique<GPUEngine>(ph->gridSizeX, ph->gridSizeY, ph->gpuId, maxFound, hash160);
		}

		int nbThread = g->GetNbThread();
		auto p = std::make_unique<Point[]>(nbThread);
		auto keys = std::make_unique<Int[]>(nbThread);
		vector<ITEM> found;

		printf("GPU          : %s\n\n", g->deviceName.c_str());

		counters[thId].store(0, std::memory_order_relaxed);

		g->SetSearchMode(searchMode);
		g->SetSearchType(searchType);
		g->SetAddressMode(addressMode);

		ph->hasStarted.store(true, std::memory_order_release);

		uint64_t iterCount = 0;
		uint64_t keysThisInterval = 0;
		double intervalStart = Timer::get_tick();
		bool firstChunkLog = true;

		Int chunkStart;
		Int chunkEnd;
		uint64_t chunkIterations = 0;

		// GPU Thread (Faz 2): global chunk queue ile canli yuk dengeleme
		while (ok && !endOfSearch.load(std::memory_order_relaxed)) {
			if (!AcquireNextGPUChunk(ph->gpuId, nbThread, chunkStart, chunkEnd, chunkIterations)) {
				break;
			}
			if (chunkIterations == 0) {
				continue;
			}

			getGPUStartingKeys(thId, chunkStart, chunkEnd, g->GetGroupSize(), nbThread, keys.get(), p.get(), firstChunkLog);
			firstChunkLog = false;
			ok = g->SetKeys(p.get());
			if (!ok) {
				break;
			}

			while (ok && chunkIterations > 0 && !endOfSearch.load(std::memory_order_relaxed)) {
				if (addressMode == FILEMODE) {
					ok = g->Launch(found, false);
				}
				else {
					ok = g->Launch2(found, false);
				}

				for (int i = 0; i < (int)found.size() && !endOfSearch.load(std::memory_order_relaxed); i++) {
					ITEM it = found[i];
					string addr = secp->GetAddress(searchType, it.mode, it.hash);
					if (checkPrivKey(addr, keys[it.thId], it.incr, it.mode)) {
						nbFoundKey.fetch_add(1, std::memory_order_relaxed);
					}
				}

				if (ok) {
					for (int i = 0; i < nbThread; i++) {
						keys[i].Add((uint64_t)STEP_SIZE);
					}

					uint64_t keysDone = (uint64_t)(STEP_SIZE) * (uint64_t)nbThread;
					counters[thId].fetch_add(keysDone, std::memory_order_relaxed);
					keysThisInterval += keysDone;
					iterCount++;
					chunkIterations--;

					if ((iterCount % REBALANCE_INTERVAL) == 0) {
						double now = Timer::get_tick();
						double elapsed = now - intervalStart;
						if (elapsed > 0.0) {
							std::lock_guard<std::mutex> statsLock(gpuStatsMutex);
							for (auto& s : gpuStats) {
								if (s.gpuId == ph->gpuId) {
									s.keysPerSec = (double)keysThisInterval / elapsed;
									break;
								}
							}
						}
						keysThisInterval = 0;
						intervalStart = now;
						RebalanceGPURanges();
					}
				}
			}
		}
#else
		ph->hasStarted.store(true, std::memory_order_release);
		fprintf(stderr, "GPU code not compiled, use -DWITHGPU when compiling.\n");
#endif
	}
	catch (const std::exception& e) {
		ph->hasStarted.store(true, std::memory_order_release);
		fprintf(stderr, "[HATA] GPU thread exception: %s\n", e.what());
		endOfSearch.store(true, std::memory_order_relaxed);
	}
	catch (...) {
		ph->hasStarted.store(true, std::memory_order_release);
		fprintf(stderr, "[HATA] GPU thread bilinmeyen exception\n");
		endOfSearch.store(true, std::memory_order_relaxed);
	}
	ph->isRunning.store(false, std::memory_order_release);

}

// ----------------------------------------------------------------------------

bool KeyHunt::isAlive(TH_PARAM * p)
{

	bool anyAlive = false;
	int total = nbCPUThread + nbGPUThread;
	for (int i = 0; i < total; i++)
		anyAlive = anyAlive || p[i].isRunning.load(std::memory_order_acquire);

	return anyAlive;

}

// ----------------------------------------------------------------------------

bool KeyHunt::hasStarted(TH_PARAM * p)
{

	bool hasStarted = true;
	int total = nbCPUThread + nbGPUThread;
	for (int i = 0; i < total; i++)
		hasStarted = hasStarted && p[i].hasStarted.load(std::memory_order_acquire);

	return hasStarted;

}

// ----------------------------------------------------------------------------

uint64_t KeyHunt::getGPUCount()
{

	uint64_t count = 0;
	for (int i = 0; i < nbGPUThread; i++)
		count += counters[0x80L + i].load(std::memory_order_relaxed);
	return count;

}

uint64_t KeyHunt::getCPUCount()
{

	uint64_t count = 0;
	for (int i = 0; i < nbCPUThread; i++)
		count += counters[i].load(std::memory_order_relaxed);
	return count;

}

// ----------------------------------------------------------------------------

void KeyHunt::SetupRanges(uint32_t totalThreads)
{
	Int threads;
	threads.SetInt32(totalThreads);
	rangeDiff.Set(&rangeEnd);
	rangeDiff.Sub(&rangeStart);
	rangeDiff.Div(&threads);
}

void KeyHunt::InitP2P(const std::vector<int>& gpuIds)
{
#ifdef WITHGPU
	int deviceCount = (int)gpuIds.size();
	if (deviceCount < 2) return;

	for (int i = 0; i < deviceCount; i++) {
		for (int j = i + 1; j < deviceCount; j++) {
			int canAccess = 0;
			cudaError_t err = cudaDeviceCanAccessPeer(&canAccess, gpuIds[i], gpuIds[j]);
			if (err != cudaSuccess) {
				fprintf(stderr, "[HATA] cudaDeviceCanAccessPeer(%d,%d): %s\n",
					gpuIds[i], gpuIds[j], cudaGetErrorString(err));
				continue;
			}
			if (canAccess) {
				cudaSetDevice(gpuIds[i]);
				err = cudaDeviceEnablePeerAccess(gpuIds[j], 0);
				if (err != cudaSuccess && err != cudaErrorPeerAccessAlreadyEnabled) {
					fprintf(stderr, "[HATA] P2P enable GPU %d -> GPU %d: %s\n",
						gpuIds[i], gpuIds[j], cudaGetErrorString(err));
				}
				cudaSetDevice(gpuIds[j]);
				err = cudaDeviceEnablePeerAccess(gpuIds[i], 0);
				if (err != cudaSuccess && err != cudaErrorPeerAccessAlreadyEnabled) {
					fprintf(stderr, "[HATA] P2P enable GPU %d -> GPU %d: %s\n",
						gpuIds[j], gpuIds[i], cudaGetErrorString(err));
				}
				printf("[INFO] P2P (NVLink/PCIe direct): GPU %d <-> GPU %d etkin\n",
					gpuIds[i], gpuIds[j]);
			}
			else {
				printf("[INFO] P2P: GPU %d <-> GPU %d - mevcut degil (PCIe uzerinden)\n",
					gpuIds[i], gpuIds[j]);
			}
		}
	}
#else
	(void)gpuIds;
#endif
}

void KeyHunt::RebalanceGPURanges()
{
	std::lock_guard<std::mutex> lock(gpuStatsMutex);
	if (gpuStats.size() < 2) return;

	double totalSpeed = 0.0;
	for (const auto& s : gpuStats) totalSpeed += s.keysPerSec;
	if (totalSpeed <= 0.0) return;

	// Faz 2 (hafif): canli chunk queue icin GPU basina chunk-iteration hint'i hesapla.
	// Hızlı GPU daha buyuk chunk alır, yavas GPU daha kucuk chunk alır.
	const double baseChunkIterations = 8.0;
	for (auto& s : gpuStats) {
		double ratio = s.keysPerSec / totalSpeed;
		double scaled = baseChunkIterations * ratio * (double)gpuStats.size();
		uint64_t hint = (uint64_t)llround(scaled);
		if (hint < 1) hint = 1;
		if (hint > 64) hint = 64;
		s.assignedRange = hint; // assignedRange alani burada "chunk-iteration hint" olarak kullaniliyor.
	}
}

uint64_t KeyHunt::GetGPUChunkIterationsHint(int gpuId)
{
	std::lock_guard<std::mutex> lock(gpuStatsMutex);
	for (const auto& s : gpuStats) {
		if (s.gpuId == gpuId) {
			return s.assignedRange > 0 ? s.assignedRange : 8ULL;
		}
	}
	return 8ULL;
}

bool KeyHunt::AcquireNextGPUChunk(int gpuId, int nbThread, Int& chunkStart, Int& chunkEnd, uint64_t& chunkIterations)
{
	chunkIterations = 0;
	if (nbThread <= 0) return false;

	uint64_t perIterKeys = (uint64_t)STEP_SIZE * (uint64_t)nbThread;
	if (perIterKeys == 0) return false;

	uint64_t hintIterations = GetGPUChunkIterationsHint(gpuId);
	if (hintIterations == 0) hintIterations = 1;

	Int perIterSpan;
	perIterSpan.SetInt64(perIterKeys);

	Int desiredSpan(perIterSpan);
	if (hintIterations > 1) {
		Int mult;
		mult.SetInt64(hintIterations);
		desiredSpan.Mult(&mult);
	}

	std::lock_guard<std::mutex> lock(gpuWorkMutex);
	if (!gpuWorkPoolInitialized) return false;
	if (gpuWorkCursor.IsGreaterOrEqual(&gpuWorkEnd)) return false;

	Int remaining(gpuWorkEnd);
	remaining.Sub(&gpuWorkCursor);
	if (remaining.IsLower(&perIterSpan)) {
		if (!gpuTailWarned && !remaining.IsZero()) {
			gpuTailWarned = true;
			fprintf(stderr, "[UYARI] GPU range kuyrugunda bir iterasyondan kucuk tail kaldi, atlandi.\n");
		}
		gpuWorkCursor.Set(&gpuWorkEnd);
		return false;
	}

	chunkStart.Set(&gpuWorkCursor);
	chunkIterations = hintIterations;

	Int actualSpan;
	if (remaining.IsLower(&desiredSpan)) {
		actualSpan.Set(&perIterSpan);
		chunkIterations = 1;
	}
	else {
		actualSpan.Set(&desiredSpan);
	}

	chunkEnd.Set(&gpuWorkCursor);
	chunkEnd.Add(&actualSpan);
	if (chunkEnd.IsGreater(&gpuWorkEnd)) {
		chunkEnd.Set(&gpuWorkEnd);
	}
	gpuWorkCursor.Set(&chunkEnd);
	return true;
}

// ----------------------------------------------------------------------------

void KeyHunt::Search(int nbThread, std::vector<int> gpuId, std::vector<int> gridSize, bool& should_exit)
{

	double t0;
	double t1;
	endOfSearch.store(false, std::memory_order_relaxed);
	nbCPUThread = nbThread;
	nbGPUThread = (useGpu ? (int)gpuId.size() : 0);
	nbFoundKey.store(0, std::memory_order_relaxed);

	// setup ranges
	SetupRanges(nbCPUThread + nbGPUThread);

	for (auto& c : counters) c.store(0, std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(gpuStatsMutex);
		gpuStats.clear();
		gpuStats.reserve(nbGPUThread);
		for (int i = 0; i < nbGPUThread; i++) {
			GPUPerfStats s;
			s.gpuId = gpuId[i];
			s.assignedRange = 8; // chunk-iteration hint
			gpuStats.push_back(s);
		}
	}

	if (!useGpu)
		printf("\n");

	auto params = std::make_unique<TH_PARAM[]>(nbCPUThread + nbGPUThread);
	std::vector<std::thread> threads;
	threads.reserve(nbCPUThread + nbGPUThread);

	int rangeShowThreasold = 3;
	int rangeShowCounter = 0;

	// Launch CPU threads
	for (int i = 0; i < nbCPUThread; i++) {
		params[i].obj = this;
		params[i].threadId = i;
		params[i].isRunning.store(true, std::memory_order_relaxed);
		params[i].hasStarted.store(false, std::memory_order_relaxed);

		params[i].rangeStart.Set(&rangeStart);
		rangeStart.Add(&rangeDiff);
		params[i].rangeEnd.Set(&rangeStart);

		if (i < rangeShowThreasold) {
			printf("CPU Thread %02d: %064s : %064s\n", i, params[i].rangeStart.GetBase16().c_str(), params[i].rangeEnd.GetBase16().c_str());
		}
		else if (rangeShowCounter < 1) {
			printf("             .\n");
			rangeShowCounter++;
			if (i + 1 == nbCPUThread) {
				printf("CPU Thread %02d: %064s : %064s\n", i, params[i].rangeStart.GetBase16().c_str(), params[i].rangeEnd.GetBase16().c_str());
			}
		}
		else if (i + 1 == nbCPUThread) {
			printf("CPU Thread %02d: %064s : %064s\n", i, params[i].rangeStart.GetBase16().c_str(), params[i].rangeEnd.GetBase16().c_str());
		}
		threads.emplace_back(&KeyHunt::FindKeyCPU, this, &params[i]);
	}

	if (useGpu && gpuId.size() > 1) {
		InitP2P(gpuId);
	}

	// Launch GPU threads
	Int gpuPoolStart(rangeStart);
	for (int i = 0; i < nbGPUThread; i++) {
		params[nbCPUThread + i].obj = this;
		params[nbCPUThread + i].threadId = 0x80L + i;
		params[nbCPUThread + i].isRunning.store(true, std::memory_order_relaxed);
		params[nbCPUThread + i].hasStarted.store(false, std::memory_order_relaxed);
		params[nbCPUThread + i].gpuId = gpuId[i];
		params[nbCPUThread + i].gridSizeX = gridSize[2 * i];
		params[nbCPUThread + i].gridSizeY = gridSize[2 * i + 1];

		params[nbCPUThread + i].rangeStart.Set(&rangeStart);
		rangeStart.Add(&rangeDiff);
		params[nbCPUThread + i].rangeEnd.Set(&rangeStart);
	}
	{
		std::lock_guard<std::mutex> lock(gpuWorkMutex);
		gpuWorkCursor.Set(&gpuPoolStart);
		gpuWorkEnd.Set(&rangeStart);
		gpuWorkPoolInitialized = (nbGPUThread > 0);
		gpuTailWarned = false;
	}
	for (int i = 0; i < nbGPUThread; i++) {
		threads.emplace_back(&KeyHunt::FindKeyGPU, this, &params[nbCPUThread + i]);
	}

#ifndef WIN64
	setvbuf(stdout, NULL, _IONBF, 0);
#endif
	printf("\n");

	uint64_t lastCount = 0;
	uint64_t gpuCount = 0;
	uint64_t lastGPUCount = 0;

	// Key rate smoothing filter
#define FILTER_SIZE 8
	double lastkeyRate[FILTER_SIZE];
	double lastGpukeyRate[FILTER_SIZE];
	uint32_t filterPos = 0;

	double keyRate = 0.0;
	double gpuKeyRate = 0.0;
	char timeStr[256];

	memset(lastkeyRate, 0, sizeof(lastkeyRate));
	memset(lastGpukeyRate, 0, sizeof(lastkeyRate));

	// Wait that all threads have started
	while (!hasStarted(params.get())) {
		Timer::SleepMillis(500);
	}

	// Reset timer
	Timer::Init();
	t0 = Timer::get_tick();
	startTime = t0;
	Int p100;
	Int ICount;
	p100.SetInt32(100);

	while (isAlive(params.get())) {

		int delay = 2000;
		while (isAlive(params.get()) && delay > 0) {
			Timer::SleepMillis(500);
			delay -= 500;
		}

		gpuCount = getGPUCount();
		uint64_t count = getCPUCount() + gpuCount;
		ICount.SetInt64(count);
		int completedBits = ICount.GetBitLength();
		ICount.Mult(&p100);
		ICount.Div(&this->rangeDiff2);
		int completed = std::stoi(ICount.GetBase10());

		t1 = Timer::get_tick();
		keyRate = (double)(count - lastCount) / (t1 - t0);
		gpuKeyRate = (double)(gpuCount - lastGPUCount) / (t1 - t0);
		lastkeyRate[filterPos % FILTER_SIZE] = keyRate;
		lastGpukeyRate[filterPos % FILTER_SIZE] = gpuKeyRate;
		filterPos++;

		// KeyRate smoothing
		double avgKeyRate = 0.0;
		double avgGpuKeyRate = 0.0;
		uint32_t nbSample;
		for (nbSample = 0; (nbSample < FILTER_SIZE) && (nbSample < filterPos); nbSample++) {
			avgKeyRate += lastkeyRate[nbSample];
			avgGpuKeyRate += lastGpukeyRate[nbSample];
		}
		avgKeyRate /= (double)(nbSample);
		avgGpuKeyRate /= (double)(nbSample);

		if (isAlive(params.get())) {
			memset(timeStr, '\0', 256);
			printf("\r[%s] [CPU+GPU: %.2f Mk/s] [GPU: %.2f Mk/s] [C: %d%%] [T: %s (%d bit)] [F: %d]  ",
				toTimeStr(t1, timeStr),
				avgKeyRate / 1000000.0,
				avgGpuKeyRate / 1000000.0,
				completed,
				formatThousands(count).c_str(),
				completedBits,
				nbFoundKey.load(std::memory_order_relaxed));
		}

		lastCount = count;
		lastGPUCount = gpuCount;
		t0 = t1;
		if (should_exit || (addressMode == FILEMODE ? false : (nbFoundKey.load(std::memory_order_relaxed) > 0)))
			endOfSearch.store(true, std::memory_order_relaxed);
	}

	for (auto& t : threads) {
		if (t.joinable()) t.join();
	}

}

// ----------------------------------------------------------------------------

string KeyHunt::GetHex(vector<unsigned char> &buffer)
{
	string ret;

	char tmp[128];
	for (int i = 0; i < (int)buffer.size(); i++) {
		snprintf(tmp, sizeof(tmp), "%02X", buffer[i]);
		ret.append(tmp);
	}
	return ret;
}

// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

int KeyHunt::CheckBloomBinary(const uint8_t * hash)
{
	if (bloom->check(hash, HASH160_BYTES) > 0) {
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
			temp_read = DATA.get() + ((current + half) * HASH160_BYTES);
			rcmp = memcmp(hash, temp_read, HASH160_BYTES);
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
	return 0;
}

bool KeyHunt::MatchHash160(uint32_t * _h)
{
	if (_h[0] == hash160[0] &&
		_h[1] == hash160[1] &&
		_h[2] == hash160[2] &&
		_h[3] == hash160[3] &&
		_h[4] == hash160[4]) {
		return true;
	}
	else {
		return false;
	}
}

std::string KeyHunt::formatThousands(uint64_t x)
{
	char buf[32] = "";

	snprintf(buf, sizeof(buf), "%llu", (unsigned long long)x);

	std::string s(buf);

	int len = (int)s.length();

	int numCommas = (len - 1) / 3;

	if (numCommas == 0) {
		return s;
	}

	std::string result = "";

	int count = ((len % 3) == 0) ? 0 : (3 - (len % 3));

	for (int i = 0; i < len; i++) {
		result += s[i];

		if (count++ == 2 && i < len - 1) {
			result += ",";
			count = 0;
		}
	}
	return result;
}

char* KeyHunt::toTimeStr(int sec, char* timeStr)
{
	int h, m, s;
	h = (sec / 3600);
	m = (sec - (3600 * h)) / 60;
	s = (sec - (3600 * h) - (m * 60));
	snprintf(timeStr, 16, "%0*d:%0*d:%0*d", 2, h, 2, m, 2, s);
	return (char*)timeStr;
}


