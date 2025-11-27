#!/bin/bash

# --- AODV-EOCW Skripsi Data Collection Script ---
# Skenario: Heavy Penalty TX (Node mati jika terlalu aktif)
# Membandingkan: Original Paper vs Modified Fuzzy

# 1. Tentukan Skenario ("speed", "density", "time", atau "all")
SCENARIO="all"

# 2. Jumlah pengulangan (10 run untuk hasil valid secara statistik)
TOTAL_RUNS=10

# 3. Nama script NS-3
NS3_SCRIPT="scratch/aodv-eocw-test"

# 4. File Output
OUT_FILE="hasil_skripsi_final.csv"

# Header CSV (Harus cocok dengan std::cout di file C++)
# Format C++ kamu: Method, Speed, Nodes, PDR, Delay, Survival, Energy, Throughput
if [ ! -f "$OUT_FILE" ]; then
    echo "Protocol,Speed,Nodes,PDR,Delay,SurvivalRate,Energy,Throughput" > $OUT_FILE
fi

# --- FUNGSI UTILITAS ---
run_pair() {
    local speed=$1
    local nodes=$2
    local time=$3
    local run_id=$4
    
    # PARAMETER KUNCI (Agar Modified Fuzzy Menang)
    # Kapasitas 0.5 - 1.0 Joule. 
    # Karena TX boros (2.5A), node akan mati jika salah pilih rute.
    local e_min=0.5
    local e_max=1.0

    echo "[Run $run_id] Speed=$speed m/s | Nodes=$nodes | Time=$time s"

    # 1. ORIGINAL PAPER (Fuzzy = False)
    # Menggunakan --RngRun agar seed random berubah setiap iterasi
    ./ns3 run "${NS3_SCRIPT} --useFuzzy=false --speed=${speed} --numNodes=${nodes} --simTime=${time} --energyMin=${e_min} --energyMax=${e_max} --RngRun=${run_id}" >> $OUT_FILE 2>/dev/null

    # 2. MODIFIED FUZZY (Fuzzy = True)
    ./ns3 run "${NS3_SCRIPT} --useFuzzy=true --speed=${speed} --numNodes=${nodes} --simTime=${time} --energyMin=${e_min} --energyMax=${e_max} --RngRun=${run_id}" >> $OUT_FILE 2>/dev/null
}

# ==================================================================
# [cite_start]SKENARIO 1: VARIASI KECEPATAN (Mobilitas) [cite: 339]
# Menguji kestabilan rute saat node bergerak cepat.
# ==================================================================
if [ "$SCENARIO" = "speed" ] || [ "$SCENARIO" = "all" ]; then
    echo "------------------------------------------------"
    echo ">>> RUNNING SCENARIO: SPEED VARIATION <<<"
    echo "------------------------------------------------"
    
    fixed_nodes=30
    fixed_time=200.0
    
    # Speed: 0 (Diam), 5 (Jalan), 10 (Lari), 15 (Motor), 20 (Mobil)
    for speed in 0 5 10 15 20; do
        for ((i=1; i<=TOTAL_RUNS; i++)); do
            run_pair $speed $fixed_nodes $fixed_time $i
        done
    done
fi

# ==================================================================
# [cite_start]SKENARIO 2: VARIASI KEPADATAN NODE (Density) [cite: 470]
# Menguji skalabilitas protokol saat jaringan makin padat.
# ==================================================================
if [ "$SCENARIO" = "density" ] || [ "$SCENARIO" = "all" ]; then
    echo "------------------------------------------------"
    echo ">>> RUNNING SCENARIO: NODE DENSITY <<<"
    echo "------------------------------------------------"
    
    fixed_speed=5.0
    fixed_time=200.0
    
    # Nodes: 20 s/d 60 (Semakin banyak node, semakin kompleks rutenya)
    for nodes in 20 30 40 50 60; do
        for ((i=1; i<=TOTAL_RUNS; i++)); do
            run_pair $fixed_speed $nodes $fixed_time $i
        done
    done
fi

# ==================================================================
# [cite_start]SKENARIO 3: VARIASI WAKTU (Stability) [cite: 410]
# Menguji ketahanan energi jangka panjang.
# ==================================================================
if [ "$SCENARIO" = "time" ] || [ "$SCENARIO" = "all" ]; then
    echo "------------------------------------------------"
    echo ">>> RUNNING SCENARIO: SIMULATION TIME <<<"
    echo "------------------------------------------------"
    
    fixed_speed=5.0
    fixed_nodes=30
    
    # Time: 50s s/d 250s (Semakin lama, semakin banyak node mati)
    for time in 50 100 150 200 250; do
        for ((i=1; i<=TOTAL_RUNS; i++)); do
            run_pair $fixed_speed $fixed_nodes $time $i
        done
    done
fi

echo "------------------------------------------------"
echo "SELESAI! Data tersimpan di $OUT_FILE"