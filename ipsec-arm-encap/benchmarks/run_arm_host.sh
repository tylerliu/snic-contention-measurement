#!/bin/bash
# Run from repository root. Sources/binary must already be deployed to SoC.
set -eu
workers=${1:-2}
case "$workers" in 2|4|8) ;; *) echo 'Use 2, 4, or 8 workers'; exit 1;; esac
tag=armhost-doca-${workers}
sudo docker run -d --name "$tag-rx" --privileged --network host \
 -v /dev/infiniband:/dev/infiniband -v /dev/hugepages:/dev/hugepages \
 -v /home/tylerliu/partial_decrypt/testing_tools:/workspace -w /workspace \
 nvcr.io/nvidia/doca/doca:3.1.0-devel-host \
 timeout -s INT -k 5 45 stdbuf -oL ./build/dpdk_simple_recv \
 -a 0000:01:00.0 -a 0000:01:00.1 -l 45-47 -f "$tag-rx" \
 --rx-ip 172.16.1.128 --rx-ip 172.16.2.128 -p 0 -S
sleep 3
ssh ubuntu@soc "sudo docker run -d --name $tag-tx --privileged --network host \
 -v /dev/infiniband:/dev/infiniband -v /dev/hugepages:/dev/hugepages \
 -v /home/ubuntu/arm-host-bench:/workspace -w /workspace \
 nvcr.io/nvidia/doca/doca:3.1.0-devel \
 timeout -s INT -k 5 30 stdbuf -oL ./arm_simple_gen_doca \
 -a 0000:03:00.0 -s 172.16.1.20 -d 172.16.1.128 -D 58:a2:e1:53:19:d6 \
 -a 0000:03:00.1 -s 172.16.2.20 -d 172.16.2.128 -D 58:a2:e1:53:19:d7 \
 -l 1-$((workers+1)) -f $tag-tx -z 1414 -p 3333 -S"
ssh ubuntu@soc "sudo docker wait $tag-tx"
sudo docker wait "$tag-rx"
ssh ubuntu@soc "sudo docker logs $tag-tx" > "benchmarks/$tag-tx.log" 2>&1
sudo docker logs "$tag-rx" > "benchmarks/$tag-rx.log" 2>&1
ssh ubuntu@soc "sudo docker rm $tag-tx"
sudo docker rm "$tag-rx"
