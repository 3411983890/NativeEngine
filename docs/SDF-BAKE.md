# 离线窄带 SDF 烘焙（L0，Lumen 前置）

- 状态：**离线 CPU 工具**，不依赖 GPU、渲染器或 D3D12
- 代码：`tools/offline/sdf-bake.mjs`（module + CLI）
- 测试：`tests/sdf.test.mjs`（`node --test tests/sdf.test.mjs`）
- 输入：INN-14 几何包 `build/offline/geometry.package.json`，或简单 JSON 网格
- 输出：`sdf.volume.json`（清单）+ `sdf.volume.bin`（分块 float32 窄带 SDF）
- 目的：为 L1 的 SDF 追踪先立起“一次构建、可寻址、可校验”的距离数据；
  它不是全局距离场，也不是表面缓存

## 1. 文件布局与内容身份

```text
build/offline/sdf/
  sdf.volume.json    # manifest：网格来源、约定、窄带、分块哈希、表面占用统计
  sdf.volume.bin     # 8³ 分块、x 最快、float32le、每体素 4 字节
```

`volumeDigest` 是内容身份（不是路径）；运行时缓存应按 digest 寻址。
`verify` 会重算 manifest 摘要、`.bin` 整体哈希、每个 chunk 的哈希以及 surface 统计。

## 2. 采样网格、单位与坐标

- 单位：**米**，与输入网格一致，不做缩放；数值就是到三角网格的欧氏距离。
- 网格：以网格 AABB 中心为中心的**立方体**，三轴 `dimensions = [N, N, N]`，默认 `N = 32`。
- `voxelSize = maxExtent / (N - 2*band)`：最长轴占 `N - 2*band` 个体素，四周各留
  `band` 个体素边距（默认 2），保证等值面不贴网格边界。
- `origin` 是**体素 (0,0,0) 中心**的世界坐标，采样位置固定为
  `p(ix,iy,iz) = origin + voxelSize * (ix, iy, iz)`，`ix,iy,iz ∈ [0, N)`。
- 短轴会得到更多空白边距，但仍是各向同性体素；不按轴分配不同分辨率，因此
  长宽比悬殊的网格（例如 19 m × 1 m 的地面场景）会按最长轴选体素尺寸，短轴精度不会更高。

## 3. 符号约定与 watertight 检测

- **负值 = 内部，正值 = 外部**，等值面在 0 附近；`signedConvention` 字段显式写死该约定。
- 对**闭合、绕序一致**的网格：逐 (y,z) 行做 +X 射线奇偶扫描，奇数次穿越 = 内部；
  `sign.method = "ray-parity"`、`sign.watertight = true`、`sign.farField = "parity"`。
- 对**非闭合**网格（当前几何包就是：开放地面 + 4 个 box）：全局内外没有定义，
  只用最近三角形的面法线 `dot(p - closest, n)` 给带内体素定局部符号；未被任何三角形
  AABB 覆盖的远场体素取 `+farValue`。此时
  `sign.method = "closest-triangle-normal"`、`sign.watertight = false`、
  `sign.farField = "positive"`、`sign.reason` 记录原因。
  **这种体积只能当“表面距离场”用，不能当有效内外场。**
- 奇偶射线在 y/z 上分别加固定 `1e-7` / `2.7e-7` 体素偏移，避免射线恰好落在共享边或
  轴对齐盒的对角线上；偏移是确定性的，只影响距表面 ~1e-6 体素的符号。
- 法线朝向只影响 `closest-triangle-normal` 分支；`ray-parity` 与绕序无关（只要求闭合）。

## 4. 窄带、远场钳制与 float32 量化

- 带半径 `band.voxels = 2`（`--band` 可改，要求 `N - 2*band >= 1`）。
- 每个体素中心先算精确距离 `d`（double）：`|d| <= band*voxelSize` 写入 `sign*d`；
  否则写入 `sign*band*voxelSize`（`band.farValue`）。
- 因此**远场被钳制**，只保留符号，不保留真实距离；manifest 用 `band.clamped: true`
  明确声明这一点，没有隐藏。
- 写入值是 float32：会量化；远场值也量化（例如 `farValue` 会变成最接近的 float32）。
- surface 分类阈值也用 float32：`|v| < float32(farValue)` 记为带内，
  `|v| <= float32(voxelSize/2)` 记为表面体素。这样 bake 与 verify 对同一份量化数据
  得到完全相同的计数。

## 5. 分块、字节布局与哈希

- chunk 大小 `8×8×8` 体素，每块 512 个 float32 = 2048 字节。
- 块内顺序：x 最快，再 y，再 z；块顺序：`cx + cy*(N/8) + cz*(N/8)^2`。
- `sdf.volume.bin` 是各块字节按块序**连续拼接**；块内地址连续，但跨块的一行在文件中
  不连续（按块存放，方便将来按块分页/流送）。
- `chunks[]` 每项：`{index, origin(体素坐标), dimensions, offset, bytes, sha256, surfaceVoxels}`，
  要求 `index` 等于数组下标、`offset == index*2048`。
- `data.sha256` = 整个 `.bin` 的 SHA-256；`volumeDigest` = manifest 去掉 `volumeDigest`
  后按 canonical JSON（对象键递归升序、无空格、数组顺序保留、追加一个 `\n`）的 SHA-256。
- **确定性**：无时间戳、无随机数、无绝对路径、无 locale 相关格式化；同一网格内容 +
  同一 `resolution`/`band` + 同一工具版本 → manifest 与 `.bin` 逐字节一致。
  测试与 self-test 都做了两次构建的字节比较。

## 6. surface cache metadata（只是占用统计，不是表面缓存）

`surface` 字段给未来的表面缓存/卡片步骤提供窄带占用信息：

| 字段 | 含义 |
| --- | --- |
| `voxelsInBand` | `|v| < farValue` 的体素数，即真正可能参与追踪的体素 |
| `surfaceVoxels` | `|v| <= voxelSize/2` 的体素数，即等值面穿过的体素（每个 chunk 还带自己的 `surfaceVoxels`） |
| `insideVoxels` | `v < 0` 的体素数 |
| `outsideVoxels` | 其余体素数（`v == 0` 归入 outside） |

它不包含卡片、材质、UV、辐照度或任何辐射量；"卡片/表面缓存"仍未实现（见第 12 节）。

## 7. 输入

1. **几何包**：`bake` 默认读 `build/offline/geometry.package.json`，用
   `verifyPackage()` 先校验 manifest 与 section 哈希，再解码
   `sections/vertices.bin`（float32le，stride 24，取前三个分量）与 `sections/indices.bin`
   （uint32le）。manifest 的 `source.contentHash` / `packageDigest` 会写进 SDF manifest，
   保证派生物可追溯到源包。
2. **简单 JSON 网格**：

```json
{ "positions": [0, 0, 0, 1, 0, 0, 0, 1, 0], "indices": [0, 1, 2] }
```

`positions` 也接受 `[x,y,z]` 三元组数组；索引按 3 个一组，必须落在顶点范围内。
非闭合网格可以用，但只有局部符号，见第 3 节。

## 8. 命令

```powershell
node tools/offline/sdf-bake.mjs self-test
node tools/offline/sdf-bake.mjs bake --out build/offline/sdf
node tools/offline/sdf-bake.mjs bake --mesh <mesh.json> --out <directory> --resolution 64 --band 2
node tools/offline/sdf-bake.mjs query --volume build/offline/sdf/sdf.volume.json --point 0,0.5,0
node tools/offline/sdf-bake.mjs verify --volume build/offline/sdf/sdf.volume.json
node --test tests/sdf.test.mjs
```

- `bake` 的 `--package` 与 `--mesh` 二选一；都不给时用默认几何包。
- `query` 是**三线性插值**；点落在网格外时钳制到边界并返回 `clamped: true`。
- 默认 `resolution = 32`、`band = 2`；`resolution` 必须是 8 的倍数。

本次实测（Windows，Node v24.18.0）：

- `bake --out build/offline/sdf`：`dimensions = [32,32,32]`，`voxelSize = 0.6785714285714286 m`，
  `origin = [-10.517857142857144, -10.017857142857144, -10.517857142857144]`，
  64 个 chunk / 131072 字节，`volumeDigest = a91030b641c59d015e9244db14805a3fa44d89bc72104fca1bf343bb8da180f0`，
  命令墙钟约 107 ms（含 Node 启动）。
- 该几何包是 19 m 地面 + 4 个 1 m box：体素 0.679 m 时 box 只有约 1.5 个体素宽，形状分辨率很低。
  这是“立方体网格 + 最长轴分辨率”的直接结果，不是 bug；需要更细的 box 就提高 `--resolution`
  或（未来）按对象分别烘焙。该网格不闭合，所以 `sign.method = closest-triangle-normal`。
- `verify`：`ok: true`，64 chunks，131072 字节。
- 同一输入连跑两次：manifest 与 `.bin` 的 SHA-256 完全一致（另见第 9 节测试断言）。

## 9. 精度与实测数字

容差契约：在带内、远离钳制边界的采样点上，误差 ≤ **1.5 × 体素对角线**
（`1.5 * sqrt(3) * voxelSize`）。`tests/sdf.test.mjs` 断言该上界，并额外用更紧的
1 体素回归阈值防止退化。

**self-test**（球 48×24、盒 12 三角形，32³）：

| 形状 / 采样方式 | 最大误差（体素） | 平均误差（体素） | 样本数 |
| --- | --- | --- | --- |
| sphere 48×24，体素中心 vs 解析 SDF | 0.0587 | 0.0339 | 7264 |
| sphere 48×24，三线性中点 vs 解析 SDF | 0.0695 | 0.0514 | 4956 |
| box 12 三角形，体素中心 | 0.0000 | 0.0000 | 16079 |
| box 12 三角形，三线性中点 | 0.4984 | 0.0178 | 11859 |

**`node --test tests/sdf.test.mjs`**（独立生成的 32×16 球）：

| 形状 / 采样方式 | 最大误差（体素） | 平均误差（体素） | 样本数 |
| --- | --- | --- | --- |
| sphere 32×16，体素中心 | 0.1337 | 0.0749 | 7264 |
| sphere 32×16，三线性中点 | 0.1297 | 0.0926 | 4956 |
| box，体素中心 | 0.0000 | 0.0000 | 16079 |
| box，三线性中点 | 0.4984 | 0.0178 | 11859 |

解读：

- 球的误差主要来自三角面片离散（弦高 sagitta）与三线性插值；48×24 球约 0.06–0.07 体素，
  32×16 球约 0.13 体素，都远低于 1.5 体素对角线的契约。
- 盒的体素中心误差为 0：三角网格本身就是精确的轴对齐盒表面，中心采样等于解析距离
  （只剩 float32 量化误差，< 1e-4 体素）。
- 盒的 0.4984 体素是“等值面恰好穿过体素、三线性插值”的最坏情况（mean 只有 0.0178）。
  实测最坏样本落在盒角 `(-1,-1,-1)` 附近的单元中点上：解析距离约 0，插值给出 0.0356 m
  （0.4984 体素）。盒的面内 SDF 是线性的，三线性插值在面内精确，因此误差只出现在跨棱/角的
  体素里——这是窄带 SDF 的已知插值行为，不是符号或距离错误。
- 符号检查：self-test 在球/盒各 9 个内外点上 0 处不一致；深内部点钳制到 `-farValue`，
  网格外点钳制到边界且符号为正。

## 10. 成本与限制（诚实说明）

- 距离阶段是**逐三角形的 AABB 宽相位 + 精确点-三角形窄相位**（Ericson 最近点），
  不是 BVH/八叉树/距离场层次。每个三角形只访问其 AABB 按 band 扩张后覆盖的体素；
  大三角形（例如当前地面的 0.5 m 三角形）会覆盖很多体素，单个巨大三角形退化为
  近似全网格暴力。代价约 `O(Σ 三角形覆盖体素 × 一次点-三角形距离)`。
- `ray-parity` 是 `O(N² × 三角形数)` 的扫描线；只有 watertight 网格才执行。
- 实测墙钟（含 Node 启动）：32³ 几何包约 107 ms；64³ 几何包约 182 ms / 512 chunks /
  1048576 字节；64³ 的 48×24 watertight 球约 215 ms（要走 ray-parity）。
- 内存：32³ 的 double 距离数组约 256 KB，最终 float32 体积 128 KB；64³ 约 2 MB / 1 MB。
- 无并行、无 SIMD、无增量/局部重烘焙；每次全量重算。

## 11. 错误码

| 错误码 | 触发条件 |
| --- | --- |
| `NG-E-SDF-TAMPERED` | `.bin` 长度或哈希不符；任一块哈希不符；`volumeDigest` 不符；chunk/整体 surface 统计与实际数据不符 |
| `NG-E-SDF-MISSING` | manifest、`.bin`、`--mesh` 文件不存在 |
| `NG-E-SDF-MANIFEST` | manifest JSON 损坏；formatVersion/kind/字段/维度/分块布局/source 非法 |
| `NG-E-SDF-PATH` | `data.path` 是绝对路径或逃逸出 volume 目录 |
| `NG-E-SDF-INPUT` | 网格数组/索引非法、包围盒退化、程序化球/盒参数非法、source 计数不匹配 |
| `NG-E-SDF-ARGUMENT` | CLI 参数缺失/未知/重复，或 `resolution`/`band` 非法 |
| `NG-E-SDF-IO` | 兜底 I/O 错误（未单独分类） |

`query` 与 `verify` 都会先完整校验（长度、哈希、chunk、统计），所以被改过的体积不会
静默返回数值，而是报 `NG-E-SDF-TAMPERED`。

## 12. 明确“还不是什么”（what this is NOT yet）

- **没有 GPU 追踪**：没有 SDF 光线步进、软阴影、AO 的运行时实现；L1 才做。
- **没有全局距离场（GDF）**：没有 clipmap、mip 链、按距离流送、多网格合并；
  这里只有单网格、单分辨率、窄带体积。
- **没有表面缓存卡片 / 辐射数据**：没有卡片生成、卡片捕获、辐照度、材质、UV、切线；
  `surface` 只是占用计数。
- **没有压缩**：float32le 未压缩；没有 RLE/BC/稀疏/8-bit 量化。
- **没有水密修复与薄壁诊断**：非 watertight 输入只有局部符号；薄壁 < 2 体素会漏光，
  文档不承诺。
- **没有 BVH/八叉树加速**：大网格或巨大三角形会显著变慢。
- **没有跨派生物一致性断言**：cluster DAG、SDF、BLAS 还没有在同一个 bundle 里互相校验。
- **没有 D3D12/HLSL 消费者**：`renderer/`、`shaders/`、`host/` 未修改。
- **没有真实资产导入（glTF）**：输入仍是程序化几何包或手工 JSON 网格。
