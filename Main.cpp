#include "Timer.h"
#include "KeyHunt.h"
#include "Base58.h"
#include "ArgParse.h"
#include <fstream>
#include <memory>
#include <string>
#include <string.h>
#include <stdexcept>
#include <cassert>
#ifndef WIN64
#include <signal.h>
#include <unistd.h>
#endif

#define RELEASE "1.00"

using namespace std;
using namespace argparse;
bool should_exit = false;

// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

const char* vstr = "Print version";
const char* cstr = "Check the working of the codes";
const char* ustr = "Search uncompressed addresses";
const char* bstr = "Search both uncompressed or compressed addresses";
const char* gstr = "Enable GPU calculation";
const char* istr = "GPU ids: 0,1... List of GPU(s) to use, default is 0";
const char* xstr = "GPU gridsize: g0x,g0y,g1x,g1y... default is auto grid and 128 threads/block";
const char* ostr = "Outputfile: Output results to the specified file, default: Found.txt";
const char* mstr = "Specify maximum number of addresses found by each kernel call";
const char* tstr = "threadNumber: Specify number of CPU threads, default is number of cores";
const char* lstr = "List CUDA enabled devices";
const char* fstr = "Ripemd160 binary hash file path";
const char* astr = "P2PKH address (single address mode)";

const char* pstr = "Range start in hex";
const char* qstr = "Range end in hex, default: startRange + 10000000000000000";

// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

void getInts(string name, vector<int>& tokens, const string& text, char sep)
{

	size_t start = 0, end = 0;
	tokens.clear();
	int item;

	try {

		while ((end = text.find(sep, start)) != string::npos) {
			item = std::stoi(text.substr(start, end - start));
			tokens.push_back(item);
			start = end + 1;
		}

		item = std::stoi(text.substr(start));
		tokens.push_back(item);

	}
	catch (const std::invalid_argument&) {
		throw std::runtime_error("Invalid " + name + " argument, number expected");
	}

}

#ifdef WIN64
BOOL WINAPI CtrlHandler(DWORD fdwCtrlType)
{
	switch (fdwCtrlType) {
	case CTRL_C_EVENT:
		//printf("\n\nCtrl-C event\n\n");
		should_exit = true;
		return TRUE;

	default:
		return TRUE;
	}
}
#else
void CtrlHandler(int signum) {
	(void)signum;
	should_exit = true;
}
#endif

int main(int argc, const char* argv[])
{
	try {
		// Global Init
		Timer::Init();
		rseed(Timer::getSeed32());

	bool gpuEnable = false;
	bool gpuAutoGrid = true;
	int searchMode = SEARCH_COMPRESSED;
	vector<int> gpuId = { 0 };
	vector<int> gridSize;
	string outputFile = "Found.txt";
	string hash160File = "";
	string address = "";
	std::vector<unsigned char> hash160;
	int nbCPUThread = Timer::getCoreNumber();
	bool tSpecified = false;
	bool sse = true;
	uint32_t maxFound = 1024 * 64;
	string rangeStart = "";
	string rangeEnd = "";
	hash160.clear();

	ArgumentParser parser("KeyHunt-Cuda-2", "Hunt for Bitcoin private keys.");

	parser.add_argument("-v", "--version", vstr, false);
	parser.add_argument("-c", "--check", cstr, false);
	parser.add_argument("-u", "--uncomp", ustr, false);
	parser.add_argument("-b", "--both", bstr, false);
	parser.add_argument("-g", "--gpu", gstr, false);
	parser.add_argument("-i", "--gpui", istr, false);
	parser.add_argument("-x", "--gpux", xstr, false);
	parser.add_argument("-o", "--out", ostr, false);
	parser.add_argument("-m", "--max", mstr, false);
	parser.add_argument("-t", "--thread", tstr, false);
	parser.add_argument("-l", "--list", lstr, false);
	parser.add_argument("-f", "--file", fstr, false);
	parser.add_argument("-a", "--addr", astr, false);

	parser.add_argument("-s", "--start", pstr, false);
	parser.add_argument("-e", "--end", qstr, false);

	parser.enable_help();

	auto err = parser.parse(argc, argv);
	if (err) {
		std::cout << err << std::endl;
		parser.print_help();
		return -1;
	}

	if (parser.exists("help")) {
		parser.print_help();
		return 0;
	}

	if (parser.exists("version")) {
		printf("KeyHunt-Cuda-2 v" RELEASE "\n");
		return 0;
	}

	if (parser.exists("check")) {
		printf("KeyHunt-Cuda-2 v" RELEASE "\n\n");

		printf("\nChecking... Secp256K1\n\n");
		Secp256K1 sec;
		sec.Init();
		sec.Check();

		printf("\n\nChecking... Int\n\n");
		Int K;
		K.SetBase16("3EF7CEF65557B61DC4FF2313D0049C584017659A32B002C105D04A19DA52CB47");
		K.Check();

		printf("\n\nCheck completed successfully\n\n");
		return 0;
	}

	if (parser.exists("uncomp")) {
		searchMode = SEARCH_UNCOMPRESSED;
	}
	if (parser.exists("both")) {
		searchMode = SEARCH_BOTH;
	}

	if (parser.exists("gpu")) {
		gpuEnable = true;
		nbCPUThread = 0;
	}

	if (parser.exists("gpui")) {
		string ids = parser.get<string>("i");
		getInts("gpui", gpuId, ids, ',');
	}

	if (parser.exists("gpux")) {
		string grids = parser.get<string>("x");
		getInts("gpux", gridSize, grids, ',');
		gpuAutoGrid = false;
	}

	if (parser.exists("out")) {
		outputFile = parser.get<string>("o");
	}

	if (parser.exists("max")) {
		maxFound = parser.get<uint32_t>("m");
	}

	if (parser.exists("thread")) {
		nbCPUThread = parser.get<int>("t");
		tSpecified = true;
	}

	if (parser.exists("list")) {
#ifdef WITHGPU
		GPUEngine::PrintCudaInfo();
#else
		printf("GPU kodu derlenmedi. -DWITHGPU ile derleyin.\n");
#endif
		return 0;
	}

	if (parser.exists("file")) {
		hash160File = parser.get<string>("f");
	}

	if (parser.exists("addr")) {
		address = parser.get<string>("a");
		if (address.length() < 30 || address[0] != '1') {
			throw std::runtime_error("Invalid addr argument, must have P2PKH address only");
		}
		else {
			if (DecodeBase58(address, hash160)) {
				hash160.erase(hash160.begin() + 0);
				hash160.erase(hash160.begin() + 20, hash160.begin() + 24);
				assert(hash160.size() == 20);
			}
		}
	}

	if (parser.exists("start")) {
		rangeStart = parser.get<string>("s");
	}

	if (parser.exists("end")) {
		rangeEnd = parser.get<string>("e");
	}


	if (gridSize.size() == 0) {
		for (int i = 0; i < gpuId.size(); i++) {
			gridSize.push_back(-1);
			gridSize.push_back(128);
		}
	}
	else if (gridSize.size() != gpuId.size() * 2) {
		throw std::runtime_error("Invalid gridSize or gpuId argument, must have coherent size");
	}

	if ((hash160.size() <= 0) && (hash160File.length() <= 0)) {
		throw std::runtime_error("Invalid ripemd160 binary hash file path or invalid address");
	}

	if ((hash160.size() > 0) && (hash160File.length() > 0)) {
		throw std::runtime_error("Invalid arguments, addr and file, both option can't be used together");
	}

	if (rangeStart.length() <= 0) {
		throw std::runtime_error(
			"Invalid rangeStart argument, please provide start range at least, "
			"endRange would be: startRange + 10000000000000000");
	}

	if (nbCPUThread > 0 && gpuEnable) {
		throw std::runtime_error("Invalid arguments, CPU and GPU, both can't be used together right now");
	}

	// Let one CPU core free per gpu is gpu is enabled
	// It will avoid to hang the system
	if (!tSpecified && nbCPUThread > 1 && gpuEnable)
		nbCPUThread -= (int)gpuId.size();
	if (nbCPUThread < 0)
		nbCPUThread = 0;

	{
		printf("\n");
		printf("KeyHunt-Cuda-2 v" RELEASE "\n");
		printf("\n");
		printf("MODE         : %s\n", searchMode == SEARCH_COMPRESSED ? "COMPRESSED" : (searchMode == SEARCH_UNCOMPRESSED ? "UNCOMPRESSED" : "COMPRESSED & UNCOMPRESSED"));
		printf("DEVICE       : %s\n", (gpuEnable && nbCPUThread > 0) ? "CPU & GPU" : ((!gpuEnable && nbCPUThread > 0) ? "CPU" : "GPU"));
		printf("CPU THREAD   : %d\n", nbCPUThread);
		printf("GPU IDS      : ");
		for (int i = 0; i < gpuId.size(); i++) {
			printf("%d", gpuId.at(i));
			if (i + 1 < gpuId.size())
				printf(", ");
		}
		printf("\n");
		printf("GPU GRIDSIZE : ");
		for (int i = 0; i < gridSize.size(); i++) {
			printf("%d", gridSize.at(i));
			if (i + 1 < gridSize.size()) {
				if ((i + 1) % 2 != 0) {
					printf("x");
				}
				else {
					printf(", ");
				}

			}
		}
		if (gpuAutoGrid)
			printf(" (grid size will be calculated automatically based on multiprocessor number on GPU device)\n");
		else
			printf("\n");
		printf("SSE          : %s\n", sse ? "YES" : "NO");
		printf("MAX FOUND    : %d\n", maxFound);
		if (hash160File.length() > 0)
			printf("HASH160 FILE : %s\n", hash160File.c_str());
		else
			printf("ADDRESS      : %s (single address mode)\n", address.c_str());
		printf("OUTPUT FILE  : %s\n", outputFile.c_str());
	}
#ifdef WIN64
	if (SetConsoleCtrlHandler(CtrlHandler, TRUE)) {
		auto v = std::make_unique<KeyHunt>(hash160File, hash160, searchMode, gpuEnable,
			outputFile, sse, maxFound, rangeStart, rangeEnd, should_exit);

		v->Search(nbCPUThread, gpuId, gridSize, should_exit);

		printf("\n\nBYE\n");
		return 0;
	}
	else {
		throw std::runtime_error("error: could not set control-c handler");
	}
#else
	signal(SIGINT, CtrlHandler);
	auto v = std::make_unique<KeyHunt>(hash160File, hash160, searchMode, gpuEnable,
		outputFile, sse, maxFound, rangeStart, rangeEnd, should_exit);

	v->Search(nbCPUThread, gpuId, gridSize, should_exit);

	return 0;
#endif
	}
	catch (const std::runtime_error& e) {
		fprintf(stderr, "[HATA] %s\n", e.what());
		return 1;
	}
	catch (const std::exception& e) {
		fprintf(stderr, "[HATA] Beklenmedik: %s\n", e.what());
		return 1;
	}
}
