# KeyHunt-Cuda-2

./KeyHunt -t 0 -g  -s 400000000 -e 7ffffffff -a 1PWCx5fovoEaoBowAvF5k91m2Xat9bMgwb


Bitcoin ozel anahtar arama araci. Belirtilen aralikta CPU veya CUDA GPU ile
paralel tarama yapar (P2PKH odakli akis).

Bu fork/refactor; CUDA 12.x/13.x, modern NVIDIA mimarileri (Ampere/Ada/Hopper),
ve coklu GPU baslangic optimizasyonlari (P2P/NVLink init + telemetry hooks) icin
guncellendi.

## Desteklenen GPU'lar (CUDA 12.0+ / 12.4+ onerilir)

| GPU | SM | Tavsiye Edilen CCAP |
|---|---|---|
| NVIDIA H200 | sm_90a | `CCAP=90a` veya `multiarch=1` |
| NVIDIA H100 | sm_90 | `CCAP=90` |
| NVIDIA A100 | sm_80 | `CCAP=80` |
| NVIDIA L40S | sm_89 | `CCAP=89` |
| NVIDIA L4 | sm_89 | `CCAP=89` |
| NVIDIA V100 | sm_70 | `CCAP=70` |
| NVIDIA P100 | sm_60 | `CCAP=60` |
| NVIDIA RTX 2060 SUPER | sm_75 | `CCAP=75` veya otomatik |

## Gereksinimler

- CPU modu: C++17 derleyici (GCC/Clang/MSVC), `make` veya `cmake`
- GPU modu: NVIDIA surucusu + CUDA Toolkit 12.0+ (12.4+ onerilir; CUDA 13.x de desteklenir)
- Linux icin GNU Make akisi dokumante edilmistir

## Derleme (GNU Make)

### CPU-only

```bash
make
```

### GPU (CCAP otomatik)

```bash
make gpu=1
```

### Belirli mimari (ornek H100/H200)

```bash
make gpu=1 CCAP=90
# veya H200 icin:
make gpu=1 CCAP=90a
```

### Coklu mimari (tek binary)

```bash
make gpu=1 multiarch=1
```

### Debug

```bash
make gpu=1 debug=1
```

### Ortam / GPU Bilgisi

```bash
make info
```

### Smoke + Opsiyonel Benchmark

```bash
make bench

# Opsiyonel custom run
make bench BENCH_ARGS="-g -i 0 -a 1FeexV6bAHb8ybZjqQMjJrcCrHGW9sb6uF -s 1000000 -e 2000000"

# veya tum komutu ver
make bench BENCH_CMD="./KeyHunt -l"
```

## Derleme (CMake)

### CPU-only

```bash
cmake -B build -DWITH_GPU=OFF
cmake --build build -j
```

### GPU

```bash
cmake -B build -DWITH_GPU=ON
cmake --build build -j
```

Not:
- Varsayilan CUDA arch listesi `60;70;75;80;86;89;90`
- `sm_90a` icin ek `gencode` fallback compile option uygulanir
- CUDA'nin resmi desteklemedigi yeni MSVC toolset'lerde su secenek kullanilabilir:
  `-DALLOW_UNSUPPORTED_MSVC_FOR_CUDA=ON`
- Windows'ta `Visual Studio` generator ile `No CUDA toolset found` gorurseniz:
  `x64 Native Tools / Developer` shell acip `-G Ninja` ile configure edin (veya CUDA-VS entegrasyonu destekli bir VS surumu kullanin)

## Kullanim Ornekleri

### Tek adres, CPU

```bash
./KeyHunt -a 1FeexV6bAHb8ybZjqQMjJrcCrHGW9sb6uF -s 1000000 -e 2000000
```

### Adres dosyasi, GPU (coklu GPU)

```bash
./KeyHunt -g -i 0,1 -f addresses.bin -s 8000000000000000 -e 9000000000000000
```

### Compressed + uncompressed

```bash
./KeyHunt -b -a 1Abc... -s 8000000000000000
```

### GPU bilgisi listeleme

```bash
./KeyHunt -l
```

## Refactor/Ozellik Notlari

- Guncel SM -> CUDA core/SM tablosu (Ampere/Ada/Hopper dahil)
- `cudaDeviceSetCacheConfig` kullanimi mimari-bazli hale getirildi
  - `sm_80+` icin carveout tabanli konfig
- Occupancy API tabanli grid block hesaplama
- Mimari bazli stack size ayari
- `PrintCudaInfo()` daha detayli cikti:
  - runtime surumu, GPU ailesi, L2, bant genisligi, ECC, P2P
- `GPUEngine` icinde stream/event + output ping-pong pipeline (API korunarak)
- C++17 modernizasyonu:
  - `std::thread`, `std::mutex`, `std::atomic`
  - `std::unique_ptr`
  - `std::filesystem::file_size`
  - `snprintf`
  - exception tabanli hata yollari (`Main.cpp`, `KeyHunt.cpp`)

## Coklu GPU / P2P / Rebalance Durumu

- Coklu GPU baslangicinda `InitP2P()` cagrilir (varsa peer access etkinlestirilir)
- `PrintCudaInfo()` P2P destegini raporlar
- Rebalance Faz 2 (hafif):
  - GPU throughput telemetry (`keysPerSec`) toplanir
  - `RebalanceGPURanges()` runtime'da GPU basina chunk-iteration hint'lerini gunceller
  - Canli global GPU chunk queue ile hizli GPU'lar daha fazla chunk cekerek dinamik dengelenir
  - Kuyruk sonunda bir GPU iterasyonundan kucuk tail kalirsa warning ile atlanir

## Notlar

- `--use_fast_math` NVCC flag'i etkindir; GPU kernel akisi integer/PTX odakli oldugu icin
  klasik float hassasiyet riskleri ECC yolunu dogrudan etkilemez.
- CUDA 13.x ile de derlenebilir; minimum gereksinim build sisteminde CUDA 12.0+ olarak hedeflenmistir.
