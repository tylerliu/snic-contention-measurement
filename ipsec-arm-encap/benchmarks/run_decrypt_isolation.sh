#!/bin/bash
set -eu
experiment_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
traffic_tools_dir=$(cd -- "$experiment_dir/../packet_gen_recvs" && pwd)
cd -- "$experiment_dir"
for tool in dpdk_esp_exclusive_gen dpdk_simple_recv; do
 if [ ! -x "$traffic_tools_dir/build/$tool" ]; then
  echo "Missing $traffic_tools_dir/build/$tool; build the host traffic tools first." >&2
  exit 1
 fi
done
variant=${1:-baseline}
config=decrypt_16tx_8arm.yml
throttle=0
tx_workers=${4:-16}
case "$tx_workers" in 2|4|8|16) ;; *) echo 'TX workers must be 2, 4, 8, or 16'; exit 1;; esac
mac_mode=${5:-static}
case "$mac_mode" in arp|static) ;; *) echo 'MAC mode must be arp or static'; exit 1;; esac
case "$variant" in
 sweep|sweep6|sweep2|sweep1|sweep_sf0|sweep_sf1)
  throttle=${2:?Specify pause count}; run=${3:-a}
  [[ "$throttle" =~ ^[0-9]+$ && "$run" =~ ^[a-z0-9]+$ ]] || exit 1
  [ "$throttle" -le 10000 ] || exit 1
  binary=build/monitoring_app; config=decrypt_16tx_4arm.yml
  if [ "$variant" = sweep6 ]; then config=decrypt_16tx_6arm.yml; fi
  if [ "$variant" = sweep2 ]; then config=decrypt_16tx_2arm.yml; fi
  if [ "$variant" = sweep1 ]; then config=decrypt_16tx_1arm.yml; fi
  if [ "$variant" = sweep_sf0 ]; then config=decrypt_16tx_4arm_sf0.yml; fi
  if [ "$variant" = sweep_sf1 ]; then config=decrypt_16tx_4arm_sf1.yml; fi
  variant=${variant}-${throttle}-${run}
  if [ "$tx_workers" -ne 16 ]; then variant=${variant}-tx${tx_workers}; fi;;
 baseline) binary=build/monitoring_app;;
 no-tx-meta) binary=build-no-tx-meta/monitoring_app;;
 fresh-tx) binary=build-fresh-tx/monitoring_app;;
 four-arm) binary=build/monitoring_app; config=decrypt_16tx_4arm.yml;;
 four-arm-low) binary=build/monitoring_app; config=decrypt_16tx_4arm.yml; throttle=10000;;
 four-arm-mid) binary=build/monitoring_app; config=decrypt_16tx_4arm.yml; throttle=5000;;
 until-sent) binary=build/monitoring_app; config=decrypt_16tx_4arm.yml;;
 until-sent-5000) binary=build/monitoring_app; config=decrypt_16tx_4arm.yml; throttle=5000;;
 until-sent-4000) binary=build/monitoring_app; config=decrypt_16tx_4arm.yml; throttle=4000;;
 *) echo 'Use baseline, no-tx-meta, fresh-tx, four-arm, four-arm-low, four-arm-mid, until-sent, until-sent-5000, or until-sent-4000'; exit 1;;
esac
mac0_arg=
mac1_arg=
if [ "$mac_mode" = static ]; then
 variant=${variant}-static
 mac0_arg='--dst-mac 58:a2:e1:53:19:d6'
 mac1_arg='--dst-mac 58:a2:e1:53:19:d7'
fi
tag=decap-isolate-$variant
if [ -e "benchmarks/$tag-rx.log" ]; then echo 'Result already exists; use a new run label'; exit 1; fi
ssh ubuntu@soc "sudo docker run -d --name $tag-soc --privileged --network host \
 -v /dev/infiniband:/dev/infiniband -v /dev/hugepages:/dev/hugepages \
 -v /home/ubuntu/monitoring_decap_only:/workspace -w /workspace \
 nvcr.io/nvidia/doca/doca:3.1.0-devel \
 timeout -s INT -k 10 100 stdbuf -oL ./$binary --config $config"
ready=0
for attempt in $(seq 1 30); do
 if ssh ubuntu@soc "sudo docker logs $tag-soc 2>&1" | grep -q 'pipeline build complete'; then ready=1; break; fi
 sleep 1
done
if [ "$ready" != 1 ]; then echo 'SoC did not initialize; traffic not started'; exit 1; fi
sudo docker run -d --name "$tag-rx" --privileged --network host \
 -v /dev/infiniband:/dev/infiniband -v /dev/hugepages:/dev/hugepages \
 -v "$traffic_tools_dir:/workspace" -w /workspace \
 nvcr.io/nvidia/doca/doca:3.1.0-devel-host \
 timeout -s INT -k 5 45 stdbuf -oL ./build/dpdk_simple_recv \
 -a 0000:01:00.0 -a 0000:01:00.1 -l 45-47 -f "$tag-rx" \
 --rx-ip 172.16.1.128 --rx-ip 172.16.2.128 -p 0 -S
ready=0
for attempt in $(seq 1 30); do
 if sudo docker logs "$tag-rx" 2>&1 | grep -q 'RX worker core .* receiving on port 0 queue 0' &&
    sudo docker logs "$tag-rx" 2>&1 | grep -q 'RX worker core .* receiving on port 1 queue 0'; then
  ready=1; break
 fi
 sleep 1
done
if [ "$ready" != 1 ]; then echo 'Host RX workers did not initialize; traffic not started'; exit 1; fi
sleep 1
sudo docker run -d --name "$tag-tx" --privileged --network host \
 -v /var/run/netns:/netns:ro -v /dev/infiniband:/dev/infiniband \
 -v /dev/hugepages:/dev/hugepages \
 -v "$traffic_tools_dir:/workspace" -w /workspace \
 nvcr.io/nvidia/doca/doca:3.1.0-devel-host \
 nsenter --net=/netns/remote bash -c "mount -t sysfs sysfs /sys &&
 exec timeout -s INT -k 5 30 stdbuf -oL ./build/dpdk_esp_exclusive_gen \
 -a 0000:82:00.0 -s 172.16.1.20 -d 172.16.1.128 $mac0_arg -T 172.16.1.2 \
 --spi 0x2001 --key aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899 \
 --salt 0x22334455 --iv 0x1234567890abcdef \
 -a 0000:82:00.1 -s 172.16.2.20 -d 172.16.2.128 $mac1_arg -T 172.16.2.2 \
 --spi 0x2002 --key 99887766554433221100ffeeddccbbaa99887766554433221100ffeeddccbbaa \
 --salt 0x66778899 --iv 0xfedcba0987654321 \
 -l 0-$tx_workers -f $tag-tx -z 1414 -p 3333 -t $throttle -S"
sudo docker wait "$tag-tx"
sudo docker wait "$tag-rx"
ssh ubuntu@soc "sudo docker kill --signal INT $tag-soc; sudo docker wait $tag-soc"
sudo docker logs "$tag-tx" > "benchmarks/$tag-tx.log" 2>&1
sudo docker logs "$tag-rx" > "benchmarks/$tag-rx.log" 2>&1
ssh ubuntu@soc "sudo docker logs $tag-soc" > "benchmarks/$tag-soc.log" 2>&1
sudo docker rm "$tag-tx" "$tag-rx"
ssh ubuntu@soc "sudo docker rm $tag-soc"
