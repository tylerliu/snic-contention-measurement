#!/bin/bash
# Bounded physical network -> ARM forwarding -> host test. Run at repo root.
set -eu
retries=${1:-0}
throttle=${2:-0}
case "$retries" in 0|64) ;; *) exit 1;; esac
case "$throttle" in 0|1000|3000|5000|10000|50000) ;; *) exit 1;; esac
tag=netarm-r${retries}-t${throttle}
mkdir -p "benchmarks/results/$tag"
out="benchmarks/results/$tag"
if [ -e "$out/revisions.txt" ]; then echo 'Result exists; choose a new run label rather than overwrite'; exit 1; fi
{ git rev-parse HEAD; git status --short; git -C /home/tylerliu/partial_decrypt rev-parse HEAD;
  sha256sum benchmarks/build-net-arm/simple_gen_safe;
  ssh ubuntu@soc 'sha256sum /home/ubuntu/arm-host-bench/build-net-arm/net_arm_forward';
} > "$out/revisions.txt"
ssh ubuntu@soc "sudo docker run -d --name $tag-soc --privileged --network host \
 -v /dev/infiniband:/dev/infiniband -v /dev/hugepages:/dev/hugepages \
 -v /home/ubuntu/arm-host-bench:/workspace -w /workspace \
 nvcr.io/nvidia/doca/doca:3.1.0-devel \
 timeout -s INT -k 5 90 stdbuf -oL ./build-net-arm/net_arm_forward \
 -a 0000:00:00.0 -l 1-5 --file-prefix $tag-soc -- --retries $retries --seconds 75"
ready=0
for attempt in $(seq 1 20); do
 if ssh ubuntu@soc "sudo docker logs $tag-soc 2>&1" | grep -q FORWARD_READY; then ready=1; break; fi
 sleep 1
done
if [ "$ready" != 1 ]; then
 ssh ubuntu@soc "sudo docker logs $tag-soc" > "$out/soc.log" 2>&1
 echo 'Forwarder did not initialize; sender not started'; exit 1
fi
sudo docker run -d --name "$tag-rx" --privileged --network host \
 -v /dev/infiniband:/dev/infiniband -v /dev/hugepages:/dev/hugepages \
 -v /home/tylerliu/partial_decrypt/testing_tools:/workspace -w /workspace \
 nvcr.io/nvidia/doca/doca:3.1.0-devel-host \
 timeout -s INT -k 5 40 stdbuf -oL ./build/dpdk_simple_recv \
 -a 0000:01:00.0 -a 0000:01:00.1 -l 45-47 -f "$tag-rx" \
 --rx-ip 172.16.1.128 --rx-ip 172.16.2.128 -p 0 -S
sleep 3
sudo docker run -d --name "$tag-tx" --privileged --network host \
 -v /var/run/netns:/netns:ro -v /dev/infiniband:/dev/infiniband \
 -v /dev/hugepages:/dev/hugepages -v /home/tylerliu/aes-ipsec-test/benchmarks:/bench:ro \
 -w /bench nvcr.io/nvidia/doca/doca:3.1.0-devel-host \
 nsenter --net=/netns/remote bash -c "mount -t sysfs sysfs /sys &&
 exec timeout -s INT -k 5 25 stdbuf -oL ./build-net-arm/simple_gen_safe \
 -a 0000:82:00.0 -s 172.16.1.20 -d 172.16.1.128 -D 58:a2:e1:53:19:d6 \
 -a 0000:82:00.1 -s 172.16.2.20 -d 172.16.2.128 -D 58:a2:e1:53:19:d7 \
 -l 0-16 -f $tag-tx -z 1414 -p 3333 -t $throttle -S"
sudo docker wait "$tag-tx"
sudo docker wait "$tag-rx"
ssh ubuntu@soc "sudo docker kill --signal INT $tag-soc; sudo docker wait $tag-soc"
sudo docker logs "$tag-tx" > "$out/tx.log" 2>&1
sudo docker logs "$tag-rx" > "$out/rx.log" 2>&1
ssh ubuntu@soc "sudo docker logs $tag-soc" > "$out/soc.log" 2>&1
sudo docker rm "$tag-tx" "$tag-rx"
ssh ubuntu@soc "sudo docker rm $tag-soc"
