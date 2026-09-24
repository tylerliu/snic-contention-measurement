#!/bin/bash
set -eu
printf 'trial,tx_Gibps,rx_Gibps,rx_Mpps,min_rx_Mpps,max_rx_Mpps,rx_missed,tx_drop,replay,accepted,host_missed,host_no_mbuf\n'
for rx in benchmarks/decap-isolate-${1:-sweep}-*-rx.log; do
 [ -f "$rx" ] || continue
 base=${rx%-rx.log}
 awk -v name="${base##*/}" '
 FILENAME ~ /-rx.log$/ && /TOTAL RX:/ {rn++; if(rn>=8&&rn<=23){rs+=$3;rc++;if(rc==1||$3<lo)lo=$3;if($3>hi)hi=$3}}
 FILENAME ~ /-tx.log$/ && /TOTAL TX:/ {tn++; if(tn>=8&&tn<=23){ts+=$3;tc++}}
 /ESP totals/ {for(i=1;i<=NF;i++){split($i,a,"=");s[a[1]]+=a[2]}}
 /RX rate:/ {for(i=1;i<=NF;i++)if($i~/missed=/){split($i,a,"=");previous=latest;latest=a[2]+0}}
 FILENAME ~ /-rx.log$/ && /TOTAL RX:/ {split($0,m,"missed=");hm+=m[2]+0;split($0,m,"no_mbuf=");hn+=m[2]+0}
 END {if(rc&&tc)printf("%s,%.6f,%.6f,%.6f,%.6f,%.6f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f\n",name,ts/tc*1510*8/1073741824,rs/rc*1456*8/1073741824,rs/rc/1e6,lo/1e6,hi/1e6,previous+latest,s["tx_drop"],s["replay"],s["accepted"],hm,hn)}' "$rx" "$base-tx.log" "$base-soc.log"
done
