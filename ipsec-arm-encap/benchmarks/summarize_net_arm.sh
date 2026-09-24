#!/bin/bash
set -eu
for dir in benchmarks/results/netarm-*; do
 [ -f "$dir/soc.log" ] && [ -f "$dir/rx.log" ] || continue
 echo "$dir"
 # Fixed receiver-time window, not a peak-selected subset of a variable run.
 awk '/TOTAL RX:/ {n++;if(n>=8&&n<=23){s+=$3;k++;if(k==1||$3<lo)lo=$3;if($3>hi)hi=$3}} END {if(k)printf("RX samples8..23 n=%d mean_Mpps=%.6f Gibps=%.6f min_Mpps=%.6f max_Mpps=%.6f\n",k,s/k/1e6,s/k*1456*8/1073741824,lo/1e6,hi/1e6)}' "$dir/rx.log"
 awk '/TOTAL TX:/ {n++;if(n>=4&&n<=19){s+=$3;k++}} END {if(k)printf("TX samples4..19 mean_Mpps=%.6f Gibps=%.6f\n",s/k/1e6,s/k*1456*8/1073741824)}' "$dir/tx.log"
 awk '/WORKER_FINAL/ {for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]=="rx"||a[1]=="tx"||a[1]=="drop"||a[1]=="short"||a[1]=="zero"||a[1]=="retry")s[a[1]]+=a[2]}} END{for(k in s)printf("%s=%.0f ",k,s[k]);print ""}' "$dir/soc.log"
 awk '/PORT p=/ {split($2,p,"=");line[p[2]]=$0} END {print line[0];print line[1]}' "$dir/soc.log"
done
