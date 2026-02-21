#!/bin/bash
# Quick deploy script for PocketSM64
cd /home/alberto/Repos/PocketSM64/src/firmware
make && make install && cd ../fpga && make mif && \
cp output_files/ap_core.rbf /run/media/alberto/POCKETDEV/Cores/ThinkElastic.PocketSM64/bitstream.rbf_r && \
cp ../../release/Cores/ThinkElastic.PocketSM64/core.json /run/media/alberto/POCKETDEV/Cores/ThinkElastic.PocketSM64/core.json && \
cp ../firmware/sm64.bin /run/media/alberto/POCKETDEV/Assets/pocketsm64/common/ && \
sync && echo "Deploy complete"
