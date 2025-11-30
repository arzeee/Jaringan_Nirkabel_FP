# DOKUMENTASI IMPLEMENTASI AODV-EOCW DENGAN ALGORITMA FUZZY

## Daftar Isi

1. [Pendahuluan dan Latar Belakang](#1-pendahuluan-dan-latar-belakang)
2. [Arsitektur Sistem dan Perubahan pada AODV](#2-arsitektur-sistem-dan-perubahan-pada-aodv)
3. [Algoritma Fuzzy](#3-algoritma-fuzzy)
4. [Detail Implementasi](#4-detail-implementasi)

---

## 1. Pendahuluan dan Latar Belakang

### 1.1 Motivasi

Protokol routing AODV standar hanya mempertimbangkan hop count dalam pemilihan rute, tanpa memperhitungkan:

- **Residual Energy (RE)**: Sisa energi node yang dapat menyebabkan kegagalan rute
- **Congestion Degree (CD)**: Tingkat kepadatan antrian yang mempengaruhi delay dan packet loss

### 1.2 Solusi: AODV-EOCW dengan Fuzzy Logic

Implementasi ini mengintegrasikan:

- **EOCW (Energy and Congestion Weighting)**: Metrik ganda untuk evaluasi path
- **Fuzzy Logic**: Pembobotan dinamis berdasarkan kondisi node
- **EWM (Entropy Weight Method)**: Pembobotan objektif dari data path

### 1.3 Kontribusi Utama

1. Modifikasi header RREQ/RREP untuk membawa metrik path
2. Multi-path collection di destination node
3. Sistem pembobotan hybrid (Fuzzy AHP + EWM)
4. RREQ suppression untuk proteksi node low-energy
5. Smart forwarding delay berbasis kesehatan node

---

## 2. Arsitektur Sistem dan Perubahan pada AODV

### 2.1 Gambaran Umum Modifikasi

```
AODV Standar:
Source → RREQ (hop count) → Destination → RREP → Source

AODV-EOCW:
Source → RREQ (hop, RE, CD) → Intermediate Nodes (update metrics)
      → Destination (collect multiple paths) → Timer (20ms)
      → Path Selection (Fuzzy + EWM) → Best RREP → Source
```

### 2.2 File-File yang Dimodifikasi

#### A. Build Configuration

**File**: `src/CMakeLists.txt`

**Perubahan**: Menambahkan pre-load dependencies untuk menghindari compilation error

```cmake
# --- START AODV EOCW FIX ---
message(STATUS "Processing src/wifi (Manual pre-load for aodv)")
add_subdirectory(wifi)

message(STATUS "Processing src/energy (Manual pre-load for aodv)")
add_subdirectory(energy)

message(STATUS "Processing src/aodv (Manual load after dependencies)")
add_subdirectory(aodv)

list(REMOVE_ITEM libs wifi)
list(REMOVE_ITEM libs energy)
list(REMOVE_ITEM libs aodv)
# --- END AODV EOCW FIX ---
```

**Alasan**: Module `aodv` memerlukan header dari `wifi` (wifi-mac-queue.h) dan `energy` (energy-source.h). Tanpa pre-load, kompilasi akan gagal.

---

#### B. Packet Header Extensions

**File**: `src/aodv/model/aodv-packet.h`

**Perubahan**: Menambah field metrik pada RREQ dan RREP header

```cpp
class RreqHeader : public Header
{
    // Field standar AODV...

    // --- TAMBAHAN EOCW ---
    double m_pathMinEnergy;        // Energi minimum di sepanjang path
    double m_pathAvgCongestion;    // Rata-rata congestion di path
    // --- AKHIR EOCW ---
};

class RrepHeader : public Header
{
    // Field standar AODV...

    // --- TAMBAHAN EOCW ---
    double m_pathMinEnergy;        // Energi minimum path yang dipilih
    double m_pathAvgCongestion;    // Rata-rata congestion path yang dipilih
    // --- AKHIR EOCW ---
};
```

**Serialisasi**: Ukuran header bertambah 8 bytes (2 × 4 bytes float)

---

**File**: `src/aodv/model/aodv-packet.cc`

**Implementasi Serialisasi/Deserialisasi**:

```cpp
uint32_t RreqHeader::GetSerializedSize() const
{
    return 23 + 8;  // Standar 23 byte + 8 byte untuk metrik
}

void RreqHeader::Serialize(Buffer::Iterator i) const
{
    // Serialisasi field standar...

    // Konversi double → float → uint32_t
    float f_minEnergy = (float)m_pathMinEnergy;
    uint32_t val_minEnergy = *reinterpret_cast<uint32_t*>(&f_minEnergy);
    i.WriteHtonU32(val_minEnergy);

    float f_avgCongestion = (float)m_pathAvgCongestion;
    uint32_t val_avgCongestion = *reinterpret_cast<uint32_t*>(&f_avgCongestion);
    i.WriteHtonU32(val_avgCongestion);
}

uint32_t RreqHeader::Deserialize(Buffer::Iterator start)
{
    // Deserialisasi field standar...

    // Konversi uint32_t → float → double
    uint32_t val_minEnergy = i.ReadNtohU32();
    m_pathMinEnergy = (double)(*reinterpret_cast<float*>(&val_minEnergy));

    uint32_t val_avgCongestion = i.ReadNtohU32();
    m_pathAvgCongestion = (double)(*reinterpret_cast<float*>(&val_avgCongestion));

    return GetSerializedSize();
}
```

**Catatan**: Float digunakan untuk menghemat bandwidth (4 byte vs 8 byte double).

---

#### C. Routing Protocol Core

**File**: `src/aodv/model/aodv-routing-protocol.h`

**Penambahan Include Dependencies**:

```cpp
#include "ns3/energy-source.h"      // Untuk membaca sisa energi
#include "ns3/wifi-mac-queue.h"     // Untuk membaca congestion queue
```

**Struct Baru untuk Menyimpan Path Candidate**:

```cpp
struct EocwPath
{
    double pathMinEnergy;           // Minimum RE di path ini
    double pathAvgCongestion;       // Average CD di path ini
    uint32_t hopCount;              // Jumlah hop
    RoutingTableEntry reverseRoute; // Rute balik untuk RREP

    EocwPath(double energy, double congestion, uint32_t hops,
             RoutingTableEntry route)
        : pathMinEnergy(energy),
          pathAvgCongestion(congestion),
          hopCount(hops),
          reverseRoute(route)
    {
    }
};
```

**Variabel Baru**:

```cpp
// Energy tracking
Ptr<ns3::energy::EnergySource> m_energySource;
double m_initialEnergy;

// Path caching: <RREQ ID, List of candidate paths>
std::map<uint32_t, std::vector<EocwPath>> m_eocwPathCache;

// Timers: <RREQ ID, Timer for path selection>
std::map<uint32_t, ns3::Timer> m_eocwPathTimers;

// Feature toggle
bool m_enableFuzzy;
```

**Fungsi Helper Baru**:

```cpp
// Metrik scoring
double GetResidualEnergyScore();           // RE node ini
double GetCongestionDegreeScore();         // CD node ini
double GetHopCountScore(uint32_t hopCount); // Skor dari hop count

// Fuzzy inference
double FuzzyTriangle(double value, double a, double b, double c);
std::vector<double> GetFuzzyWeights(double energyScore, double congestionScore);

// EWM calculation
std::vector<double> GetEwmWeights(const std::vector<EocwPath>& paths);

// EOCW scoring
double CalculateEocwScore(const EocwPath& path,
                          const std::vector<double>& ahp_w,
                          const std::vector<double>& ewm_mu);

// Path selection
void SelectBestEocwPath(uint32_t rreqId, Ipv4Address origin,
                        Ipv4Address destination);
```

---

#### D. Routing Table Entry

**File**: `src/aodv/model/aodv-rtable.h`

**Penambahan Field**:

```cpp
class RoutingTableEntry
{
    // Field standar...

    // --- TAMBAHAN EOCW ---
    double m_pathMinEnergy;        // Metrik energi path ini
    double m_pathAvgCongestion;    // Metrik congestion path ini
    double m_pathScore;            // Skor akhir EOCW
    // --- AKHIR EOCW ---
};
```

**File**: `src/aodv/model/aodv-rtable.cc`

**Inisialisasi**:

```cpp
RoutingTableEntry::RoutingTableEntry(...)
    : // inisialisasi field standar...
      ,m_pathMinEnergy(0.0),
       m_pathAvgCongestion(0.0),
       m_pathScore(0.0)
{
    // konstruktor body...
}
```

---

## 3. Algoritma Fuzzy

### 3.1 Konsep Fuzzy Logic untuk Pembobotan Dinamis

Fuzzy logic digunakan untuk menghasilkan **bobot AHP dinamis** berdasarkan kondisi node saat ini:

- Input: Residual Energy (RE) dan Congestion Degree (CD)
- Output: Bobot untuk 3 metrik {w_CD, w_RE, w_HC}

### 3.2 Membership Functions (Fungsi Keanggotaan)

**Fungsi Segitiga (Triangular Membership)**:

```cpp
double FuzzyTriangle(double value, double a, double b, double c)
{
    if (value <= a || value >= c) return 0.0;
    if (value == b) return 1.0;
    if (value < b) return (value - a) / (b - a);  // Rising edge
    return (c - value) / (c - b);                  // Falling edge
}
```

**Variabel Linguistik untuk Residual Energy (RE)**:

- **Low**: Triangle(-0.1, 0.0, 0.4) → Node hampir mati
- **Medium**: Triangle(0.2, 0.5, 0.8) → Energi sedang
- **High**: Triangle(0.6, 1.0, 1.1) → Energi penuh

**Variabel Linguistik untuk Congestion Degree (CD)**:

- **Busy**: Triangle(-0.1, 0.0, 0.4) → Queue penuh (macet)
- **Normal**: Triangle(0.2, 0.5, 0.8) → Queue sedang
- **Free**: Triangle(0.6, 1.0, 1.1) → Queue kosong (lancar)

### 3.3 Fuzzy Rule Base (9 Aturan)

| Rule | IF RE is | AND CD is | THEN w_CD | w_RE | w_HC | Rationale                                  |
| ---- | -------- | --------- | --------- | ---- | ---- | ------------------------------------------ |
| R1   | Low      | Busy      | 0.45      | 0.50 | 0.05 | Prioritas energi, congestion juga penting  |
| R2   | Low      | Normal    | 0.20      | 0.70 | 0.10 | Energi kritis, harus dilindungi            |
| R3   | Low      | Free      | 0.10      | 0.80 | 0.10 | Energi sangat kritis, path lain lebih baik |
| R4   | Medium   | Busy      | 0.70      | 0.20 | 0.10 | Hindari congestion, energi cukup           |
| R5   | Medium   | Normal    | 0.33      | 0.34 | 0.33 | Seimbang (balanced)                        |
| R6   | Medium   | Free      | 0.20      | 0.20 | 0.60 | Hop count penting, kondisi baik            |
| R7   | High     | Busy      | 0.80      | 0.10 | 0.10 | Fokus hindari congestion                   |
| R8   | High     | Normal    | 0.20      | 0.10 | 0.70 | Hop count lebih penting                    |
| R9   | High     | Free      | 0.10      | 0.05 | 0.85 | Kondisi ideal, pilih shortest path         |

### 3.4 Fuzzy Inference System (Mamdani)

**Implementasi**:

```cpp
std::vector<double> GetFuzzyWeights(double re, double cd_score)
{
    if (!m_enableFuzzy) {
        // Fallback ke bobot statis (Original Paper)
        if (re >= 0.8) return {0.5396, 0.297, 0.1634};
        else if (re >= 0.5) return {0.637, 0.2583, 0.1047};
        else if (re <= 0.3) return {0.7514, 0.1782, 0.0704};
        else return {0.0, 0.0, 1.0};
    }

    // === MODIFIED FUZZY LOGIC (9 RULES) ===
    // Fuzzifikasi input
    double re_low = FuzzyTriangle(re, -0.1, 0.0, 0.4);
    double re_med = FuzzyTriangle(re, 0.2, 0.5, 0.8);
    double re_high = FuzzyTriangle(re, 0.6, 1.0, 1.1);

    double cd_busy = FuzzyTriangle(cd_score, -0.1, 0.0, 0.4);
    double cd_normal = FuzzyTriangle(cd_score, 0.2, 0.5, 0.8);
    double cd_free = FuzzyTriangle(cd_score, 0.6, 1.0, 1.1);

    // Agregasi rule dengan metode MIN-MAX
    double w_cd_num = 0.0, w_re_num = 0.0, w_hc_num = 0.0, total_fire = 0.0;

    auto AddRule = [&](double fireStrength, double out_cd, double out_re, double out_hc) {
        w_cd_num += fireStrength * out_cd;
        w_re_num += fireStrength * out_re;
        w_hc_num += fireStrength * out_hc;
        total_fire += fireStrength;
    };

    // Evaluasi 9 rules
    AddRule(std::min(re_low, cd_busy), 0.45, 0.50, 0.05);    // R1
    AddRule(std::min(re_low, cd_normal), 0.20, 0.70, 0.10);  // R2
    AddRule(std::min(re_low, cd_free), 0.10, 0.80, 0.10);    // R3
    AddRule(std::min(re_med, cd_busy), 0.70, 0.20, 0.10);    // R4
    AddRule(std::min(re_med, cd_normal), 0.33, 0.34, 0.33);  // R5
    AddRule(std::min(re_med, cd_free), 0.20, 0.20, 0.60);    // R6
    AddRule(std::min(re_high, cd_busy), 0.80, 0.10, 0.10);   // R7
    AddRule(std::min(re_high, cd_normal), 0.20, 0.10, 0.70); // R8
    AddRule(std::min(re_high, cd_free), 0.10, 0.05, 0.85);   // R9

    // Defuzzifikasi dengan Center of Gravity (CoG)
    if (total_fire == 0) return {0.333, 0.333, 0.333};

    return {w_cd_num / total_fire, w_re_num / total_fire, w_hc_num / total_fire};
}
```

**Contoh Perhitungan**:

```
Input: RE = 0.3, CD = 0.7
Fuzzifikasi:
  - re_low = Triangle(0.3, -0.1, 0.0, 0.4) = 0.25
  - re_med = Triangle(0.3, 0.2, 0.5, 0.8) = 0.33
  - cd_normal = Triangle(0.7, 0.2, 0.5, 0.8) = 0.33
  - cd_free = Triangle(0.7, 0.6, 1.0, 1.1) = 0.25

Rule Firing:
  - R2: min(0.25, 0.33) = 0.25 → {0.20, 0.70, 0.10}
  - R3: min(0.25, 0.25) = 0.25 → {0.10, 0.80, 0.10}
  - R5: min(0.33, 0.33) = 0.33 → {0.33, 0.34, 0.33}
  - R6: min(0.33, 0.25) = 0.25 → {0.20, 0.20, 0.60}

Agregasi:
  w_cd = (0.25×0.20 + 0.25×0.10 + 0.33×0.33 + 0.25×0.20) / 1.08 = 0.225
  w_re = (0.25×0.70 + 0.25×0.80 + 0.33×0.34 + 0.25×0.20) / 1.08 = 0.501
  w_hc = (0.25×0.10 + 0.25×0.10 + 0.33×0.33 + 0.25×0.60) / 1.08 = 0.274

Output: {0.225, 0.501, 0.274} → Prioritas energi tinggi!
```

---

## 4. Detail Implementasi

### 4.1 Fungsi Pengambilan Metrik

#### A. Residual Energy Score

**Implementasi**:

```cpp
double GetResidualEnergyScore()
{
    if (!m_energySource || m_initialEnergy == 0) return 1.0;
    return m_energySource->GetRemainingEnergy() / m_initialEnergy;
}
```

**Inisialisasi** (di fungsi `Start()`):

```cpp
void RoutingProtocol::Start()
{
    // EOCW Init
    Ptr<Node> node = GetObject<Node>();
    if (node) {
        Ptr<energy::EnergySourceContainer> esc =
            node->GetObject<energy::EnergySourceContainer>();
        if (esc && esc->GetN() > 0) {
            m_energySource = esc->Get(0);
            m_initialEnergy = m_energySource->GetInitialEnergy();
        } else {
            m_energySource = nullptr;
            m_initialEnergy = 0;
        }
    }
    // ...
}
```

**Keterangan**: Score 1.0 = full energy, 0.0 = depleted

---

#### B. Congestion Degree Score

**Implementasi**:

```cpp
double GetCongestionDegreeScore()
{
    if (m_socketAddresses.empty()) return 1.0;

    for (auto const& [socket, iface] : m_socketAddresses) {
        if (!m_ipv4) continue;
        int32_t i = m_ipv4->GetInterfaceForAddress(iface.GetLocal());
        if (i < 0) continue;

        Ptr<NetDevice> dev = m_ipv4->GetNetDevice(static_cast<uint32_t>(i));
        if (!dev) continue;

        Ptr<WifiNetDevice> wifiDev = dev->GetObject<WifiNetDevice>();
        if (wifiDev) {
            Ptr<WifiMac> mac = wifiDev->GetMac();
            if (!mac) continue;

            Ptr<AdhocWifiMac> adhocMac = mac->GetObject<AdhocWifiMac>();
            if (adhocMac) {
                // Baca queue Best Effort (AC_BE)
                Ptr<WifiMacQueue> queue = adhocMac->GetTxopQueue(ns3::AC_BE);
                if (queue) {
                    double l_all = (double)queue->GetMaxSize().GetValue();
                    if (l_all == 0) return 1.0;
                    double l_current = (double)queue->GetCurrentSize().GetValue();

                    // Score tinggi = queue kosong (free)
                    return std::max(0.0, (l_all - l_current) / l_all);
                }
            }
        }
    }
    return 1.0;  // Default: assume free
}
```

**Keterangan**:

- Score 1.0 = queue kosong (free)
- Score 0.0 = queue penuh (busy)
- Formula: (max - current) / max

---

#### C. Hop Count Score

**Implementasi**:

```cpp
double GetHopCountScore(uint32_t hopCount)
{
    if (hopCount <= 2) return 1.0;
    if (hopCount <= 4) return 0.6;
    if (hopCount <= 6) return 0.4;
    return 0.1;
}
```

**Lookup Table**:
| Hop Count | Score | Kategori |
|-----------|-------|----------|
| 1-2 | 1.0 | Excellent |
| 3-4 | 0.6 | Good |
| 5-6 | 0.4 | Fair |
| 7+ | 0.1 | Poor |

---

### 4.2 Entropy Weight Method (EWM)

**Tujuan**: Menghitung bobot objektif dari variasi data path

**Implementasi**:

```cpp
std::vector<double> GetEwmWeights(const std::vector<EocwPath>& paths)
{
    int m = paths.size();  // Jumlah alternatif (path)
    int n = 3;             // Jumlah kriteria (CD, RE, HC)

    if (m <= 1) return {0.333, 0.333, 0.333};  // Equal weight jika 1 path

    // Buat matriks keputusan X[m][n]
    std::vector<std::vector<double>> X(m, std::vector<double>(n));
    for (int i = 0; i < m; ++i) {
        X[i][0] = paths[i].pathAvgCongestion;  // Kriteria 1: CD
        X[i][1] = paths[i].pathMinEnergy;      // Kriteria 2: RE
        X[i][2] = GetHopCountScore(paths[i].hopCount); // Kriteria 3: HC
    }

    // Hitung entropi untuk setiap kriteria
    std::vector<double> H(n, 0.0);
    double k = 1.0 / std::log(m);

    for (int j = 0; j < n; ++j) {
        double sumYij = 0.0;
        for (int i = 0; i < m; ++i) sumYij += X[i][j];

        if (sumYij == 0) continue;

        double sumPijLnPij = 0.0;
        for (int i = 0; i < m; ++i) {
            double pij = X[i][j] / sumYij;  // Normalisasi
            if (pij > 0) sumPijLnPij += pij * std::log(pij);
        }
        H[j] = -k * sumPijLnPij;  // Entropi
    }

    // Hitung diversitas d[j] = 1 - H[j]
    std::vector<double> d(n);
    double sumD = 0.0;
    for (int j = 0; j < n; ++j) {
        d[j] = 1.0 - H[j];
        sumD += d[j];
    }

    // Bobot EWM: mu[j] = d[j] / sum(d)
    std::vector<double> mu(n);
    if (sumD == 0) {
        std::fill(mu.begin(), mu.end(), 1.0/n);
        return mu;
    }

    for (int j = 0; j < n; ++j) mu[j] = d[j] / sumD;

    return mu;  // {mu_CD, mu_RE, mu_HC}
}
```

**Penjelasan**:

1. **Normalisasi**: pij = Xij / Σ(Xij)
2. **Entropi**: H = -k × Σ(pij × ln(pij)), k = 1/ln(m)
3. **Diversitas**: d = 1 - H (semakin bervariasi, d semakin besar)
4. **Bobot**: μ = d / Σ(d)

**Contoh**:

```
3 path:
  Path1: CD=0.8, RE=0.9, HC=1.0
  Path2: CD=0.5, RE=0.6, HC=0.6
  Path3: CD=0.3, RE=0.2, HC=0.4

EWM calculation:
  H_CD = 0.92 (variasi rendah) → d_CD = 0.08
  H_RE = 0.85 (variasi sedang) → d_RE = 0.15
  H_HC = 0.95 (variasi rendah) → d_HC = 0.05

  mu = {0.08/0.28, 0.15/0.28, 0.05/0.28} = {0.286, 0.536, 0.178}
  → RE memiliki variasi tertinggi, bobot terbesar!
```

---

### 4.3 Perhitungan EOCW Score

**Formula**:

```
Score = (w_CD × μ_CD × S_CD) + (w_RE × μ_RE × S_RE) + (w_HC × μ_HC × S_HC)
                    ────────────────────────────────────────────────────────
                                  w_CD×μ_CD + w_RE×μ_RE + w_HC×μ_HC
```

**Implementasi**:

```cpp
double CalculateEocwScore(const EocwPath& path,
                          const std::vector<double>& ahp_w,  // Fuzzy weights
                          const std::vector<double>& ewm_mu) // EWM weights
{
    // Ambil skor metrik
    double s_cd = path.pathAvgCongestion;
    double s_re = path.pathMinEnergy;
    double s_rh = GetHopCountScore(path.hopCount);

    // Gabungkan bobot AHP (fuzzy) dan EWM
    double w_cd = ahp_w[0] * ewm_mu[0];
    double w_re = ahp_w[1] * ewm_mu[1];
    double w_rh = ahp_w[2] * ewm_mu[2];

    double sumW = w_cd + w_re + w_rh;
    if (sumW == 0) return 0;

    // Normalisasi dan hitung score akhir
    return ((w_cd/sumW) * s_cd) + ((w_re/sumW) * s_re) + ((w_rh/sumW) * s_rh);
}
```

**Contoh Lengkap**:

```
Destination node: RE=0.7, CD=0.8
Fuzzy weights: ahp_w = {0.2, 0.3, 0.5}
EWM weights: ewm_mu = {0.286, 0.536, 0.178}

Path A: CD=0.9, RE=0.8, HC=2 (score=1.0)
  w_cd = 0.2 × 0.286 = 0.057
  w_re = 0.3 × 0.536 = 0.161
  w_hc = 0.5 × 0.178 = 0.089
  sumW = 0.307

  Score_A = (0.057/0.307)×0.9 + (0.161/0.307)×0.8 + (0.089/0.307)×1.0
          = 0.167 + 0.420 + 0.290 = 0.877

Path B: CD=0.5, RE=0.95, HC=5 (score=0.4)
  Score_B = (0.057/0.307)×0.5 + (0.161/0.307)×0.95 + (0.089/0.307)×0.4
          = 0.093 + 0.498 + 0.116 = 0.707

Path C: CD=0.7, RE=0.7, HC=3 (score=0.6)
  Score_C = (0.057/0.307)×0.7 + (0.161/0.307)×0.7 + (0.089/0.307)×0.6
          = 0.130 + 0.367 + 0.174 = 0.671

→ Path A dipilih (score tertinggi = 0.877)
```

---

### 4.4 Proses Pemilihan Rute di Destination

**Fungsi**: `SelectBestEocwPath()`

**Implementasi**:

```cpp
void SelectBestEocwPath(uint32_t rreqId, Ipv4Address origin, Ipv4Address destination)
{
    // 1. Ambil cached paths
    auto it = m_eocwPathCache.find(rreqId);
    if (it == m_eocwPathCache.end() || it->second.empty()) {
        m_eocwPathTimers.erase(rreqId);
        return;
    }

    std::vector<EocwPath> paths = it->second;

    // 2. Dapatkan kondisi node saat ini
    double currentEnergy = GetResidualEnergyScore();
    double currentCongestion = GetCongestionDegreeScore();

    // 3. Hitung bobot Fuzzy AHP
    std::vector<double> ahp_w = GetFuzzyWeights(currentEnergy, currentCongestion);

    // 4. Hitung bobot EWM
    std::vector<double> ewm_mu = GetEwmWeights(paths);

    // 5. Evaluasi setiap path
    double bestScore = -1.0;
    EocwPath* bestPath = nullptr;

    for (EocwPath& path : paths) {
        double score = CalculateEocwScore(path, ahp_w, ewm_mu);
        path.reverseRoute.m_pathScore = score;  // Simpan untuk debugging

        if (score > bestScore) {
            bestScore = score;
            bestPath = &path;
        }
    }

    // 6. Kirim RREP dengan path terbaik
    if (bestPath) {
        m_seqNo++;  // Increment sequence number

        RrepHeader rrepHeader(
            /* prefixSize */ 0,
            /* hopCount */ 0,
            /* dst */ destination,
            /* dstSeqNo */ m_seqNo,
            /* origin */ origin,
            /* lifetime */ m_myRouteTimeout
        );

        // Sertakan metrik path terpilih
        rrepHeader.m_pathMinEnergy = bestPath->pathMinEnergy;
        rrepHeader.m_pathAvgCongestion = bestPath->pathAvgCongestion;

        // Buat paket
        Ptr<Packet> packet = Create<Packet>();
        SocketIpTtlTag tag;
        tag.SetTtl(bestPath->reverseRoute.GetHop());
        packet->AddPacketTag(tag);
        packet->AddHeader(rrepHeader);
        packet->AddHeader(TypeHeader(AODVTYPE_RREP));

        // Kirim via reverse route
        Ptr<Socket> socket = FindSocketWithInterfaceAddress(
            bestPath->reverseRoute.GetInterface()
        );
        if (socket) {
            socket->SendTo(packet, 0, InetSocketAddress(
                bestPath->reverseRoute.GetNextHop(), AODV_PORT
            ));
        }
    }

    // 7. Cleanup
    m_eocwPathCache.erase(rreqId);
    m_eocwPathTimers.erase(rreqId);
}
```

**Timeline**:

```
t=0ms:    First RREQ arrives → Store in cache → Start timer(20ms)
t=5ms:    Second RREQ arrives → Store in cache
t=12ms:   Third RREQ arrives → Store in cache
t=20ms:   Timer expires → SelectBestEocwPath() → Send RREP
```

---

### 4.5 Modifikasi Penerimaan RREQ

**File**: `src/aodv/model/aodv-routing-protocol.cc`
**Fungsi**: `RecvRequest()`

**A. RREQ Suppression (Energy Protection)**:

```cpp
void RecvRequest(Ptr<Packet> p, Ipv4Address receiver, Ipv4Address src)
{
    RreqHeader rreqHeader;
    p->RemoveHeader(rreqHeader);

    // === EOCW MODIFICATION: RREQ SUPPRESSION ===
    if (m_enableFuzzy) {
        bool amIDestination = IsMyOwnAddress(rreqHeader.GetDst());
        double myCurrentEnergy = GetResidualEnergyScore();

        if (!amIDestination && myCurrentEnergy < 0.20) {
            // Drop RREQ jika energi < 20% dan bukan destination
            return;
        }
    }
    // ===========================================

    // ... lanjut proses standar ...
}
```

**Tujuan**: Node dengan energi rendah tidak meneruskan RREQ untuk menghemat energi

---

**B. Update Path Metrics**:

```cpp
    // Ambil metrik node ini
    double myEnergy = GetResidualEnergyScore();
    double myCongestion = GetCongestionDegreeScore();

    // Ambil metrik dari header RREQ
    double old_pathMinEnergy = rreqHeader.m_pathMinEnergy;
    double old_pathAvgCongestion = rreqHeader.m_pathAvgCongestion;
    uint8_t old_hop_count = rreqHeader.GetHopCount();

    // Update metrik
    double new_pathMinEnergy = std::min(old_pathMinEnergy, myEnergy);
    uint32_t hop = old_hop_count + 1;
    double new_pathAvgCongestion =
        ((old_pathAvgCongestion * old_hop_count) + myCongestion) / (double)hop;
```

**Formula**:

- **Minimum Energy**: `min(path_energy, my_energy)` → Bottleneck tracking
- **Average Congestion**: `(Σ congestion) / hop_count` → Mean calculation

---

**C. Path Caching (jika destination)**:

```cpp
    bool amIDestination = IsMyOwnAddress(rreqHeader.GetDst());

    if (amIDestination) {
        // Simpan path ini
        EocwPath newPath(new_pathMinEnergy, new_pathAvgCongestion,
                         (uint32_t)hop, toOrigin);
        m_eocwPathCache[id].push_back(newPath);

        // Set timer hanya sekali untuk RREQ ID ini
        if (m_eocwPathTimers.find(id) == m_eocwPathTimers.end()) {
            m_eocwPathTimers[id] = Timer(Timer::CANCEL_ON_DESTROY);
            m_eocwPathTimers[id].SetFunction(&RoutingProtocol::SelectBestEocwPath, this);
            m_eocwPathTimers[id].SetArguments(id, origin, rreqHeader.GetDst());
            m_eocwPathTimers[id].SetDelay(MilliSeconds(20));
            m_eocwPathTimers[id].Schedule();
        }

        return;  // Tidak forward RREQ
    }
```

---

**D. Smart Delay Forwarding**:

```cpp
    // Update header untuk forwarding
    rreqHeader.SetHopCount(hop);
    rreqHeader.m_pathMinEnergy = new_pathMinEnergy;
    rreqHeader.m_pathAvgCongestion = new_pathAvgCongestion;

    // === EOCW MODIFICATION: SMART DELAY ===
    Time forwardDelay;
    if (m_enableFuzzy) {
        // Modified Fuzzy: delay berdasarkan kesehatan node
        double healthPenalty = (1.0 - myEnergy) + (1.0 - myCongestion);
        forwardDelay = MilliSeconds(healthPenalty * 50)
                     + MilliSeconds(m_uniformRandomVariable->GetInteger(0, 5));
    } else {
        // Original: random jitter saja
        forwardDelay = MilliSeconds(m_uniformRandomVariable->GetInteger(0, 10));
    }
    // ======================================

    Simulator::Schedule(forwardDelay, &RoutingProtocol::SendTo,
                       this, socket, packet, destination);
```

**Contoh Delay**:

- Node sehat (E=0.9, C=0.8): delay = (0.1+0.2)×50 + random(0,5) = 15-20ms
- Node buruk (E=0.3, C=0.4): delay = (0.7+0.6)×50 + random(0,5) = 65-70ms

**Tujuan**: Node sehat forward lebih cepat, node buruk delay lebih lama (kesempatan RREQ lain lebih dulu sampai)

---

### 4.6 Modifikasi Pengiriman RREQ

**File**: `src/aodv/model/aodv-routing-protocol.cc`
**Fungsi**: `SendRequest()`

```cpp
void SendRequest(Ipv4Address dst)
{
    // Buat RREQ header standar...
    RreqHeader rreqHeader(/* ... */);

    // === EOCW Init: Set metrik awal ===
    rreqHeader.m_pathMinEnergy = GetResidualEnergyScore();
    rreqHeader.m_pathAvgCongestion = GetCongestionDegreeScore();
    // ==================================

    // Broadcast RREQ...
}
```
