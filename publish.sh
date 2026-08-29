#!/usr/bin/env bash
# LL-HLS는 세그먼트를 키프레임에서만 자르므로 GOP ≤ llhls_segment(2초)가 필요하다.
# -c copy는 원본 GOP(test.mp4는 8.3초)를 그대로 쓰므로 재인코딩한다 — www/llhls.html 안내와 동일.
ffmpeg -re -stream_loop -1 -i study.mp4 -c:v libx264 -g 60 -keyint_min 60 -bf 0 -c:a aac -f flv rtmp://localhost/live/study
