# Web 固件刷写（ESP Web Tools）

`data/flasher.html` 现在直接使用官方 **ESP Web Tools** 组件
[`<esp-web-install-button>`](https://github.com/esphome/esp-web-tools)（MIT/Apache，
10.4.0），不再自研 esptool 调用。

## 集成方式

页面只做三件事：

1. 引入官方的 web-component（与 esphome 文档站相同的写法）：
   ```html
   <script type="module"
     src="https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module"></script>
   ```
   `?module` 让 unpkg 把内部裸依赖（`lit`、`esptool-js`、`@material/web`…）
   重写成可用的 ESM；不加这个参数浏览器会报 “Failed to resolve module specifier”。
2. 提供一个 manifest 地址（输入框或 `?manifest=` 参数）。
3. 把 `<esp-web-install-button manifest="...">` 放在页面上。

芯片识别、build 选择、擦除/写入、进度、串口重连，全部由官方组件负责——
比自研版本可靠，也消除了之前那批 API 误用的风险。

## manifest 格式（官方标准）

```json
{
  "name": "ESP32WifiPhisher",
  "version": "1.0.0",
  "builds": [
    { "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "bootloader.bin", "offset": 0 },
        { "path": "partitions.bin", "offset": 32768 },
        { "path": "firmware.bin", "offset": 65536 }
      ] },
    { "chipFamily": "ESP32",
      "parts": [
        { "path": "esp32-bootloader.bin", "offset": 4096 },
        { "path": "esp32-partitions.bin", "offset": 32768 },
        { "path": "esp32-firmware.bin", "offset": 65536 }
      ] }
  ]
}
```

或者只用一个合并镜像：

```json
{ "name": "ESP32WifiPhisher", "version": "1.0.0",
  "builds": [ { "chipFamily": "ESP32-S3",
    "parts": [ { "path": "esp32s3-merged.bin", "offset": 0 } ] } ] }
```

注意：ESP Web Tools 的 `offset` 是**十进制**（`65536` 即 `0x10000`），不要写 `0x…`。

## 生成 manifest

`data/tools/gen-manifest.html` 是一个纯前端小工具：选设备/填文件名和偏移，
点导出即可下载 `manifest.json`。也可以用 `scripts/make_manifest.py`
（见仓库）对 `dist/` 里的合并镜像批量生成。

## 已验证 / 未验证

- 已核对：官方 docs 站确实用 `esp-web-tools@10/dist/web/install-button.js?module` 加载；
  该 URL 及 `?module` 重写后的全部 chunk 均可访问（302 → 200）。
- 已核对：官方 `flash.js` 的进度算法是
  `uncompressedWritten = (written/total) * fileArray[fileIndex].data.length`
  —— 官方自己处理了“压缩后字节”的问题，这正是自研版踩的坑。
- 已核对：官方用 `esptool-js@0.6.x`，其中 `fileArray.data` 已是 `Uint8Array`。
- **未验证**：真实串口刷写（本环境无硬件）。不过这条链路是官方组件 + 官方
  esptool-js，等于把该风险交给了上游维护者。

旧的自己写 esptool 的版本保留为 `data/flasher.html.legacy`（不再被引用），
仅作对照。

## 用 GitHub Actions 编译

仓库新增两个 workflow：

**`.github/workflows/build-firmware.yml`** —— 编译固件

- 矩阵：`esp32` / `esp32s3` / `esp32c6`（C5 因依赖作者 fork 平台，先注释掉，需要时解开）
- 缓存 `~/.platformio`，`pip install platformio` 后执行
  `pio run -e <env> -t buildfs` + `pio run -e <env>`
- `scripts/merge_bins.py` 的 post-action 会产出 `dist/<env>-merged.bin`
- 用 `scripts/make_manifest.py` 生成 `manifest.json` 一并放进 artifact
- 上传 `firmware-<env>` 这个 artifact（保留 14 天）

**`.github/workflows/pages-flasher.yml`** —— 发布刷写站

把 `data/` 发到 GitHub Pages，这样 `flasher.html` 能在 HTTPS 下打开（Web Serial 要安全上下文）。

### 必须注意的三点

1. **私有的免费仓库无法用 Pages**。Pages 在 private repo 上要 Pro/Team/Enterprise；
   免费私有仓库会直接失败。替代方案：把 `data/` 部署到 Netlify / Cloudflare Pages，
   或者把仓库设为 public。

2. **不再需要任何口令**。管理界面已去掉鉴权，构建不需要 secret。

3. **C5 平台是作者 fork**。`platformio.ini` 里 `esp32c5` 环境指向
   `https://github.com/Alexxdal/platform-espressif32.git#8215262…`，
   CI 能不能拉到取决于该 fork 仍可达；所以先注释掉，需要时再开。

### artifact 形态

```
firmware-esp32s3/
  esp32s3-merged.bin      <- 直接刷这个
  bootloader.bin partitions.bin firmware.bin spiffs.bin   (原始分片，备用)
  manifest.json           <- ESP Web Tools 用
```

把 `esp32s3-merged.bin` 和 `manifest.json` 放到同一个可访问目录，
`flasher.html` 里填 `manifest.json` 就能刷。

### 未验证

workflow 只做了 YAML 结构校验（`yaml.safe_load` 通过、矩阵字段齐全），
**没有真正在 GitHub 上跑过**——那需要仓库有 Actions 权限、且首次运行要下
ESP-IDF 工具链（几分钟到十几分钟）。第一次跑起来看日志即可确认。
