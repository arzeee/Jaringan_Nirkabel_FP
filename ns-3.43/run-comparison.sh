#!/bin/bash

# --- AODV-EOCW Comparison Automation Script ---
# Berdasarkan parameter eksperimen dari paper [cite: 340, 411, 471]
# Membandingkan: Original Paper (useFuzzy=false) vs Modified Fuzzy (useFuzzy=true)

# 1. Tentukan Skenario ("speed", "density", "time", atau "all")
SCENARIO="all"

# 2. Jumlah pengulangan per titik data (Paper menggunakan 10 run) [cite: 338]
TOTAL_RUNS=10

# 3. Nama file C++ (harus ada di folder scratch/)
NS3_SCRIPT="scratch/aodv-eocw-test"

# 4. File Output (Semua hasil akan masuk ke sini agar mudah dibuat grafik)
OUT_FILE="hasil_perbandingan_lengkap.csv"

# Inisialisasi Header CSV jika file belum ada
if [ ! -f "$OUT_FILE" ]; then
    echo "Protocol,Speed,Nodes,PDR,Delay,SurvivalRate,Energy,Throughput,SimTime,RunID" > $OUT_FILE
fi

# --- FUNGSI UTILITAS ---
run_pair() {
    local speed=$1
    local nodes=$2
    local time=$3
    local run_id=$4
    
    # Parameter Energi Heterogen (Stress Test)
    local e_min=60.0
    local e_max=100.0

    echo "Running [Run $run_id]: Speed=$speed m/s, Nodes=$nodes, Time=$time s..."

    # 1. Jalankan ORIGINAL PAPER (Fuzzy = False)
    # Kita menggunakan parameter --RngRun=$run_id agar setiap run punya seed acak berbeda
    ./ns3 run "${NS3_SCRIPT} \
        --useFuzzy=false \
        --speed=${speed} \
        --numNodes=${nodes} \
        --simTime=${time} \
        --energyMin=${e_min} \
        --energyMax=${e_max} \
        --RngRun=${run_id}" >> $OUT_FILE

    # 2. Jalankan MODIFIED FUZZY (Fuzzy = True)
    # Menggunakan seed yang SAMA ($run_id) agar perbandingan adil (fair)
    ./ns3 run "${NS3_SCRIPT} \
        --useFuzzy=true \
        --speed=${speed} \
        --numNodes=${nodes} \
        --simTime=${time} \
        --energyMin=${e_min} \
        --energyMax=${e_max} \
        --RngRun=${run_id}" >> $OUT_FILE
}

# ==================================================================
# EKSPERIMEN 1: VARIASI KECEPATAN (Paper Section 5.2.1) [cite: 339]
# Range: 3 m/s s.d 15 m/s
# Nodes: 30, Time: 100s
# ==================================================================
if [ "$SCENARIO" = "speed" ] || [ "$SCENARIO" = "all" ]; then
    echo "------------------------------------------------"
    echo ">>> MEMULAI EKSPERIMEN 1: VARIASI KECEPATAN <<<"
    echo "------------------------------------------------"
    
    fixed_nodes=30
    fixed_time=200.0
    
    for speed in 3 6 9 12 15; do
        for ((i=1; i<=TOTAL_RUNS; i++)); do
            run_pair $speed $fixed_nodes $fixed_time $i
        done
    done
fi

# ==================================================================
# EKSPERIMEN 2: VARIASI KEPADATAN NODE (Paper Section 5.2.3) [cite: 470]
# Range: 20 s.d 40 Nodes
# Speed: 5 m/s, Time: 100s
# ==================================================================
if [ "$SCENARIO" = "density" ] || [ "$SCENARIO" = "all" ]; then
    echo "------------------------------------------------"
    echo ">>> MEMULAI EKSPERIMEN 2: KEPADATAN NODE <<<"
    echo "------------------------------------------------"
    
    fixed_speed=5.0
    fixed_time=200.0
    
    for nodes in 20 25 30 35 40; do
        for ((i=1; i<=TOTAL_RUNS; i++)); do
            run_pair $fixed_speed $nodes $fixed_time $i
        done
    done
fi

# ==================================================================
# EKSPERIMEN 3: VARIASI WAKTU SIMULASI (Paper Section 5.2.2) [cite: 410]
# Range: 40 s.d 200 s
# Speed: 5 m/s, Nodes: 30
# ==================================================================
if [ "$SCENARIO" = "time" ] || [ "$SCENARIO" = "all" ]; then
    echo "------------------------------------------------"
    echo ">>> MEMULAI EKSPERIMEN 3: DURASI SIMULASI <<<"
    echo "------------------------------------------------"
    
    fixed_speed=5.0
    fixed_nodes=30
    
    for time in 40 80 120 160 200; do
        for ((i=1; i<=TOTAL_RUNS; i++)); do
            run_pair $fixed_speed $fixed_nodes $time $i
        done
    done
fi

echo "------------------------------------------------"
echo "SIMULASI SELESAI."
echo "Data tersimpan di: $OUT_FILE"
echo "Silakan buka file CSV tersebut di Excel untuk membuat grafik perbandingan."
