#!/bin/bash

# --- File Skrip: run-eocw-sim.sh ---

# 1. Tentukan skenario mana yang akan dijalankan
#    Ubah nilainya ke "speed", "time", atau "density"
SCENARIO="speed"

# 2. Tentukan jumlah pengulangan per setelan (sesuai paper )
TOTAL_RUNS=10

# 3. Tentukan nama file skrip ns-3 Anda
NS3_SCRIPT="scratch/aodv-eocw-test"

# 4. Buat folder untuk menyimpan hasil log
mkdir -p eocw_results

# 5. Fungsi untuk menjalankan satu simulasi
#    $1 = numNodes, $2 = simTime, $3 = nodeSpeed, $4 = initialEnergy, $5 = run_seed
run_sim() {
    local numNodes=$1
    local simTime=$2
    local nodeSpeed=$3
    local initialEnergy=$4
    local run_seed=$5
    
    # Buat nama file log yang unik
    local log_file="eocw_results/log_N${numNodes}_S${nodeSpeed}_T${simTime}_R${run_seed}.txt"
    
    echo "MENJALANKAN: N=${numNodes}, Speed=${nodeSpeed}, Time=${simTime}, Seed=${run_seed}..."
    
    # Jalankan simulasi ns-3
    ./ns3 run "${NS3_SCRIPT} \
        --numNodes=${numNodes} \
        --simTime=${simTime} \
        --nodeSpeed=${nodeSpeed} \
        --initialEnergy=${initialEnergy} \
        --RngRun=${run_seed}" \
        > ${log_file} 2>&1 # Simpan output (stdout & stderr) ke file log
}


# --- EKSPERIMEN 5.2.1: Berdasarkan Kecepatan Gerak [cite: 340] ---
if [ "$SCENARIO" = "speed" ]; then
    echo "--- MENJALANKAN EKSPERIMEN 5.2.1 (KECEPATAN) ---"
    
    # Parameter dasar [cite: 340]
    nodes=30
    time=200
    energy=5.0 # Energi diset bebas (misal 5J), karena paper tidak menyebutkannya di 5.2.1
    
    # Loop untuk setiap kecepatan [cite: 340]
    for speed in 3 6 9 12 15; do
        # Loop untuk 10x random seeds 
        for ((seed=1; seed<=TOTAL_RUNS; seed++)); do
            run_sim $nodes $time $speed $energy $seed
        done
    done
fi

# --- EKSPERIMEN 5.2.2: Berdasarkan Waktu Simulasi [cite: 411, 412] ---
if [ "$SCENARIO" = "time" ]; then
    echo "--- MENJALANKAN EKSPERIMEN 5.2.2 (WAKTU) ---"
    
    # Parameter dasar [cite: 412]
    nodes=30
    speed=5
    energy=5.0 
    
    # Loop untuk setiap waktu simulasi [cite: 411]
    for time in 40 80 120 160 200; do
        for ((seed=1; seed<=TOTAL_RUNS; seed++)); do
            run_sim $nodes $time $speed $energy $seed
        done
    done
fi

# --- EKSPERIMEN 5.2.3: Berdasarkan Kepadatan Node [cite: 472, 473] ---
if [ "$SCENARIO" = "density" ]; then
    echo "--- MENJALANKAN EKSPERIMEN 5.2.3 (KEPADATAN) ---"
    
    # Parameter dasar [cite: 472]
    time=200
    speed=5
    energy=5.0
    
    # Loop untuk setiap jumlah node [cite: 473]
    for nodes in 20 25 30 35 40; do
        for ((seed=1; seed<=TOTAL_RUNS; seed++)); do
            run_sim $nodes $time $speed $energy $seed
        done
    done
fi

echo "--- SEMUA SIMULASI SELESAI ---"
echo "Hasil tersimpan di folder 'eocw_results/'"
