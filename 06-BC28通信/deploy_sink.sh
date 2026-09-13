#!/bin/sh
# NB-IoT 接收端部署（**在 VPS 上跑**，可重复执行）
#
#   ① 把 sink.py 传到服务器（scp / 你惯用的传输方式都行）：
#        scp sink.py root@<你的服务器>:/tmp/sink.py
#
#   ② 在服务器上（root 跑）：
#        mkdir -p ~/nbiot-sink && cp /tmp/sink.py ~/nbiot-sink/
#        DIRECTORY=$HOME/nbiot-sink sh deploy_sink.sh       # 自己生成 token（会打印一次）
#        SINK_TOKEN=<你的串> DIRECTORY=... sh deploy_sink.sh  # 或者用你指定的
#
# 为什么要 token：这是个**公网裸 TCP 端口**，谁扫到都能往里塞数据。
# token 只落在 $DIR/sink.env（600），不进任何仓库。
#
# 两个变量可以覆盖（默认是从当前用户的家目录起算，换台机器不用改脚本）：
#   DIRECTORY  安装目录，默认 $HOME/nbiot-sink
#   RUNAS      服务以哪个用户跑，默认当前用户

set -e

[ "$(id -u)" = "0" ] || { echo "要以 root 跑（要写 /etc/systemd/system 和 ufw）:  sudo sh $0"; exit 1; }

# 用 sudo 跑时 $HOME 会变成 /root，所以优先认 SUDO_USER 的家目录。
if [ -n "$SUDO_USER" ]; then
    OWNER_HOME="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
    RUNAS="$SUDO_USER"
else
    OWNER_HOME="$HOME"
    RUNAS="$(id -un)"
fi

DIR="${DIRECTORY:-$OWNER_HOME/nbiot-sink}"
ENV="$DIR/sink.env"
UNIT=/etc/systemd/system/nbiot-sink.service
PORT="${SINK_PORT:-9101}"

[ -f "$DIR/sink.py" ] || { echo "缺少 $DIR/sink.py —— 先传上去"; exit 1; }

# ---- 1. token -------------------------------------------------------------
if [ ! -f "$ENV" ]; then
    TOK="${SINK_TOKEN:-$(openssl rand -hex 16)}"
    umask 077
    printf 'SINK_TOKEN=%s\nSINK_PORT=%s\nSINK_LOGDIR=%s/logs\n' "$TOK" "$PORT" "$DIR" > "$ENV"
    chmod 600 "$ENV"
    echo "已生成 $ENV（600）"
    echo
    echo "  ★ 把这个串填进固件： #define SINK_TOKEN \"$TOK\""
    echo
else
    echo "$ENV 已存在，沿用（要换就删掉它重跑）"
fi

# ---- 2. systemd -----------------------------------------------------------
# 注意这里用不带引号的 EOF，为的是让 $DIR / $RUNAS / $PORT 展开成实际值。
cat > "$UNIT" <<EOF
[Unit]
Description=NB-IoT uplink sink (TCP $PORT, token-gated, append-only log)
After=network.target

[Service]
User=$RUNAS
WorkingDirectory=$DIR
EnvironmentFile=$ENV
ExecStart=/usr/bin/python3 $DIR/sink.py
Restart=on-failure
RestartSec=3
Environment=PYTHONUNBUFFERED=1

[Install]
WantedBy=multi-user.target
EOF

mkdir -p "$DIR/logs"
[ "$(id -u)" = "0" ] && chown -R "$RUNAS:$RUNAS" "$DIR"

systemctl daemon-reload
systemctl enable --now nbiot-sink
systemctl restart nbiot-sink

# ---- 3. 放行端口（注意：只放这一个，别顺手关掉 ufw）----------------------
ufw allow "$PORT/tcp" >/dev/null

# ---- 4. 自检 --------------------------------------------------------------
sleep 1
systemctl is-active nbiot-sink
ss -ltn | grep ":$PORT " || { echo "!! $PORT 没在监听"; exit 1; }

echo
echo "自检（会往日志里留一条 AUTH + 一条 RX，属于正常）:"
TOK=$(sed -n 's/^SINK_TOKEN=//p' "$ENV")
# 注意 `|| true`：nc 要等超时才会退，会返回 124；本脚本是 set -e，不加这句后面全被吞掉
{ printf '#TOKEN %s\r\n' "$TOK"; printf 'SELFTEST,hello\r\n'; sleep 1; } \
    | timeout 5 nc -q1 127.0.0.1 "$PORT" || true
echo
echo "--- 日志尾巴 ---"
tail -4 "$DIR/logs/sink.log"
echo
echo "公网判据（在 VPS 上跑，应当能连上）:  nc -vz <本机公网IP> $PORT"
echo "看统计:  SINK_TOKEN=x python3 $DIR/sink.py --stats"
