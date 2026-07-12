#!/bin/bash

ELF_FILE=".pio/build/default/firmware.elf"

list_top_symbols() {
    local section_pattern="$1"
    local section_name="$2"
    local num_symbols=$3
    
    # objdump -t format: address flags section size name
    # Filter by section, extract size and name, calculate total
    local data=$(objdump -t "$ELF_FILE" | \
        awk -v pattern="$section_pattern" '$4 ~ pattern { print $5, $6 }' | \
        while read hex name; do
            dec=$((16#$hex))
            echo "$dec $hex $name"
        done | \
        sort -k1 -r -n)
    
    local total=$(echo "$data" | awk '{ sum += $1 } END { print sum }')
    local total_kb=$(echo "$total" | awk '{ printf "%.2f", $1 / 1024 }')
    
    echo "============================================"
    echo "Top $num_symbols largest symbols in section: $section_name"
    echo "Total section size: $total bytes ($total_kb KB)"
    echo "============================================"
    
    echo "$data" | \
        head -$num_symbols | \
        awk '{ 
            size_kb = $1 / 1024
            printf "  %10s (%7.2f KB)  %s\n", $2, size_kb, $3
        }'
    
    echo ""
}

list_top_symbols "\\.dram0\\.bss" ".dram0.bss" 10
list_top_symbols "\\.dram0\\.data" ".dram0.data" 10
list_top_symbols "\\.flash\\.rodata" ".flash.rodata" 200
list_top_symbols "\\.flash\\.text" ".flash.text" 40
list_top_symbols "\\.iram0\\.text" ".iram0.text" 10