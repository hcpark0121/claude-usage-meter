#!/bin/sh
# Claude Code statusLine 훅: 받은 JSON 원본을 ~/.claude/meter-raw.json 에 저장.
# 의존성 없음(sh/grep/printf) — 데스크톱 앱 샌드박스(Linux)에서도 동작하도록.
in=$(cat)
printf '%s' "$in" > "$HOME/.claude/meter-raw.json"
p=$(printf '%s' "$in" | grep -o '"five_hour":{[^}]*}' | grep -oE 'used_percentage":[0-9.]+' | grep -oE '[0-9.]+$')
w=$(printf '%s' "$in" | grep -o '"seven_day":{[^}]*}' | grep -oE 'used_percentage":[0-9.]+' | grep -oE '[0-9.]+$')
out="claude meter"
[ -n "$p" ] && out="claude  5h ${p%.*}%"
[ -n "$w" ] && out="$out  7d ${w%.*}%"
echo "$out"
