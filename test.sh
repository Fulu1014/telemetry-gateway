#!/bin/bash

echo "=> 准备就绪，开始模拟 100 辆车并发发送遥测数据..."

# 循环 100 次
for i in {1..100}
do
   # 构造不同的车辆ID (car_1 到 car_100) 和 随机的速度状态
   JSON_DATA="{\"device_id\":\"car_$i\", \"status\":\"speed_$RANDOM\"}"
   
   # 发送 POST 请求
   # -s 表示静默模式，不输出 curl 的进度条
   # 最后的 & 符号极其重要！它代表将这条命令放入后台执行，这样 100 个请求会瞬间并发打向服务器，而不是排队一个一个发
   curl -s -X POST http://127.0.0.1:10000/api/telemetry \
   -H "Content-Type: application/json" \
   -d "$JSON_DATA" > /dev/null &
done

# wait 命令会等待上面所有放入后台的 curl 请求执行完毕
wait

echo "=> 100 辆车的数据已全部发送完毕！"