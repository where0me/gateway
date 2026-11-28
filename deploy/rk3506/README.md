# RK3506 首次部署说明

目标板环境：ATK-DLRK3506、ARMv7 hard-float、Buildroot 2024.02。

## 目录约定

- 后端与数据库：`/opt/zigbee-gateway`
- 前端静态文件：`/userdata/ui`
- 固定直连地址：`169.254.137.119/16`
- 固定地址启动脚本：`/etc/init.d/S89zigbee-network`
- 启动脚本：`/etc/init.d/S90zigbee-gateway`
- 固件用户程序入口：`/etc/init.d/S99appinit`
- 协调器串口：`/dev/ttyS2`

根分区空间已满，禁止把部署包解压到 `/root` 或 `/usr`。压缩包可临时放在内存文件系统 `/tmp`。

## 首次手动部署

将部署包复制到板子的 `/tmp` 后执行：

```sh
cd /tmp
sha256sum -c zigbee-gateway-rk3506-armv7.tar.gz.sha256
tar -xzf zigbee-gateway-rk3506-armv7.tar.gz

mv zigbee-gateway /opt/zigbee-gateway

if [ -d /userdata/ui ] && [ ! -e /userdata/ui.factory-backup ]; then
    mv /userdata/ui /userdata/ui.factory-backup
fi
mkdir -p /userdata/ui
cp -R /opt/zigbee-gateway/web/. /userdata/ui/

cp /opt/zigbee-gateway/install/S89zigbee-network /etc/init.d/S89zigbee-network
cp /opt/zigbee-gateway/install/S90zigbee-gateway /etc/init.d/S90zigbee-gateway
cp /opt/zigbee-gateway/install/S99appinit /etc/init.d/S99appinit
chmod 755 /etc/init.d/S89zigbee-network \
    /etc/init.d/S90zigbee-gateway \
    /etc/init.d/S99appinit
```

上面的网页备份操作只在首次部署时执行。原厂网页保存在 `/userdata/ui.factory-backup`，不要删除。

## 首次运行验证

先不要启用开机启动，前台运行后端以查看日志：

```sh
cd /opt/zigbee-gateway
./gateway-server
```

另开一个终端验证接口：

```sh
wget -qO- http://127.0.0.1:8080/api/system/settings
```

浏览器访问：

```text
http://169.254.137.119/
```

`S89zigbee-network` 会在开机时为 `eth0` 增加固定地址
`169.254.137.119/16`。该操作不会删除 ConnMan 或其他网络服务配置的地址，
因此原有的 `192.168.137.119` 在对应网络可用时仍可继续使用。

确认后端、网页、数据库和 `/dev/ttyS2` 均正常后，再停止前台进程并启用后台服务：

```sh
/etc/init.d/S89zigbee-network start
/etc/init.d/S89zigbee-network status
/etc/init.d/S90zigbee-gateway start
/etc/init.d/S90zigbee-gateway status
/etc/init.d/S50nginx reload
```

固件的 `rcS` 在 OverlayFS 挂载前展开启动脚本列表，因此后添加的 `S89`、
`S90` 不会直接进入当次开机列表。固件原有的 `S99appinit` 会在启动末尾依次调用
固定地址、Nginx 和网关后端脚本，确保 OverlayFS 中的部署内容能够开机启动。

## 当前限制

开发阶段的串口运行时热切换尚未完善。部署包默认固定使用 `/dev/ttyS2`；如果在网页中切换过串口，验证期间应重启后端进程。

## XDS110 USB 串口的开机加载

XDS110 部署时，以实际验证过的业务串口配置为准，例如 `/dev/ttyACM0`，波特率为
921600。不要直接沿用上文的板载 `/dev/ttyS2` 或模板中的其他 USB 转串口路径。

如果板子的内核未包含 CDC ACM 驱动，先使用匹配当前固件的 SDK 编译并临时加载
`cdc-acm.ko`，确认出现串口且能收到协调器的有效协议回复，再将驱动保存到：

```text
/opt/zigbee-gateway/drivers/cdc-acm.ko
```

更新后的 `S90zigbee-gateway` 会在启动后端前加载这个可选驱动；已经加载时不会重复加载。
对配置中的 USB 串口最多等待 10 秒。串口仍未出现时会给出警告，并启动后端供网页配置和诊断；
这不代表协调器通信正常，设备插好后需要重新启动网关服务或重新应用串口配置。
驱动加载失败则停止该次服务启动，不使用强制加载。

`S99appinit` 原有的调用顺序无需修改。服务启动会避免重复实例；如果网关还在前台运行，
应先在该窗口按 Ctrl+C，再启动后台服务。内核升级后需要重新核对驱动兼容性。

启动脚本的隔离测试（不加载真实模块或启动真实服务）：

```sh
sh tests/test-rk3506-startup.sh
```
