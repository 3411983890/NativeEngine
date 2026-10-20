# 内容寻址几何包（INN-14）与 N0 cluster DAG 骨架

- 状态：**离线 CPU 工具骨架**，不依赖 GPU、渲染器或 D3D12
- 代码：`tools/offline/geometry-package.mjs`、`tools/offline/cluster-dag.mjs`
- 测试：`tests/offline.test.mjs`（`node --test tests/offline.test.mjs`）
- 目标：同一份源网格一次构建，产出 vertices/indices、cluster 切分、per-cluster AABB、
  多级 LOD 与 cluster DAG，供后续 Nanite 风格渲染、Lumen SDF 追踪、DXR BLAS 共用。

## 1. 文件布局

```text
build/offline/
  geometry.package.json          # manifest：元数据 + 哈希 + cluster/LOD 描述
  sections/
    vertices.bin                 # 源网格顶点，float32le，stride 24
    indices.bin                  # 源网格三角形索引，uint32le
    cluster-vertices.bin         # 所有 LOD 的 cluster 局部顶点，float32le，stride 24
    cluster-indices.bin          # 所有 LOD 的 cluster 局部索引，uint32le
  cluster-dag.json               # 可选 sidecar，由 cluster-dag.mjs emit 生成
```

`build` 只写上述固定文件名；`packageDigest` 才是包的内容身份。运行时缓存应按 digest
寻址，而不是按 `build/offline` 这个路径寻址。

## 2. Manifest 字段

| 字段 | 含义 |
| --- | --- |
| `formatVersion` | 当前为 `1`；不兼容变更必须递增 |
| `kind` | 固定 `nativegpu.geometry-package` |
| `generator` | 工具名 + 版本，用于溯源重生成 |
| `source.kind/generator/parameters` | 程序化输入描述（32×32 quad 网格 + 4 个 box） |
| `source.contentHash` | `sha256(vertices.bin ‖ indices.bin)`，源网格内容哈希 |
| `layout` | 顶点/索引格式、字段顺序、stride（当前 f32le × 6 + u32le） |
| `counts` | 源顶点/三角形、cluster 数、LOD 数、所有 cluster 的顶点/三角形总数 |
| `bounds` | 整个源网格的 AABB：`{min:[x,y,z], max:[x,y,z]}` |
| `sections` | 按 `name` 排序的 section 列表，每项含 `path`、`bytes`、`sha256` |
| `clusters` | 按 `id` 排序（LOD 优先）的 cluster 记录 |
| `lodLevels` | 每级 `{level, clusterStart, clusterCount}`，`clusterStart` 是 `clusters` 数组下标 |
| `packageDigest` | 除自身外整个 manifest 的 SHA-256，见第 4 节 |

Cluster 记录：

| 字段 | 含义 |
| --- | --- |
| `id` | 从 0 递增，必须等于数组下标 |
| `lod` | `0` 为最细层 |
| `parent` / `children` | DAG 链接；根 `parent=-1`，LOD0 `children=[]` |
| `vertexOffset` / `vertexCount` | 在 `cluster-vertices.bin` 中的顶点区间 |
| `indexOffset` / `triangleCount` | 在 `cluster-indices.bin` 中的局部索引起点与三角形数 |
| `bounds` | 该 cluster 的 AABB；父节点为所有子节点 bounds 的并集 |
| `sourceTriangleStart/Count` | 仅 LOD0 有效：在源索引缓冲中的三角形区间；父层为 `-1/0` |

`cluster-indices.bin` 中的索引是 **cluster 局部索引**（相对该 cluster 的
`vertexOffset`），不是源网格全局索引。

## 3. 二进制布局与 cluster 语义

- 顶点记录：`px,py,pz,nx,ny,nz`，各 4 字节 little-endian float，共 24 字节；无 padding。
- 索引记录：4 字节 little-endian unsigned int；每个三角形 3 个索引。
- LOD0：按源三角形顺序贪心切分，**每个三角形恰好属于一个 LOD0 cluster**，
  每个 cluster 最多 128 个本地顶点、128 个三角形。局部顶点按首次使用顺序去重。
- 更高 LOD：按顺序每 2 个相邻低层 cluster 合成一个父 cluster：
  取两组三角形池的每隔一个三角形作为代理几何（确定性抽稀），
  顶点数仍受 128 限制；`children` 指回被合并的两个 cluster，`parent` 反向指回。
  父层 bounds 取子层 bounds 并集，因此一定包住子层与父层代理几何。
- 重复构建直到只剩 1 个根 cluster；因此 cluster 数逐层严格下降。
- 这是**结构正确的 DAG**，不是有质量保证的简化器：没有二次误差度量、没有
  边界锁定、没有误差阈值，抽稀可能丢失细节。N1 的屏幕误差 LOD 选择尚未实现。

## 4. 哈希与确定性规则

1. **Section 哈希**：`sha256` 直接作用于该 `.bin` 的原始字节。
2. **Package digest**：取 manifest 对象、删除 `packageDigest` 字段后，按
   canonical JSON（对象键递归升序、无空格、数组顺序保留）序列化，追加一个 `\n`，
   再取 `sha256`。`verify` 重算并逐字节比较。
3. **规范化**：`sections` 按 name 排序，`clusters` 按 id 排序，`lodLevels` 按 level
   排序；所有对象键在写盘时排序。无时间戳、无随机数、无 locale 相关格式化，
   manifest 统一 `\n` 换行。
4. 相同输入（相同程序化参数与工具版本）必须产生 byte-identical 的 manifest 与
   section；测试会同时比较 digest 和每个 section 的字节。

## 5. 错误码

| 错误码 | 触发条件 |
| --- | --- |
| `NG-E-GEOMETRY-TAMPERED` | section 长度/哈希不符，或 package digest 不符 |
| `NG-E-GEOMETRY-MISSING` | manifest 或某个 section 文件不存在 |
| `NG-E-GEOMETRY-MANIFEST` | JSON 损坏、格式版本/kind/字段/偏移/DAG 结构非法 |
| `NG-E-GEOMETRY-PATH` | section 路径为绝对路径或逃逸出包目录 |
| `NG-E-GEOMETRY-INPUT` | 单个三角形超出 cluster 顶点上限（当前程序化输入不会触发） |
| `NG-E-GEOMETRY-ARGUMENT` | CLI 参数缺失、未知或重复 |

说明：场景 JSON 包的篡改检测沿用现有的 `BUILD_CONFLICT`。几何包是另一类产物，
需要区分“缺文件 / 清单损坏 / 内容被改”，因此使用 `NG-E-GEOMETRY-*` 稳定错误码；
语义上 `NG-E-GEOMETRY-TAMPERED` 与 `BUILD_CONFLICT` 等价。

## 6. 命令与 sidecar

```powershell
node tools/offline/geometry-package.mjs self-test
node tools/offline/geometry-package.mjs build --out build/offline
node tools/offline/geometry-package.mjs verify --package build/offline/geometry.package.json
node tools/offline/cluster-dag.mjs emit --package build/offline/geometry.package.json
node tools/offline/cluster-dag.mjs stats --package build/offline/geometry.package.json
```

`cluster-dag.json` 把 manifest 中的 cluster/DAG 重排为扁平数组 + 每层
`{clusterStart, clusterCount, clusterIds, bounds}`，并显式写出
`runtimeLodSelector:false`、`streaming:false`。它是给未来 GPU 剔除 pass 的输入，
不是运行时 LOD 选择器。`stats` 打印每层 cluster 数、三角形/顶点数、总顶点数与整体 bounds。

## 7. 明确“还不是什么”（what this is NOT yet）

- **没有流送 / 分页**：没有 page ID、页表、按需加载、LRU 或 GPU 常驻预算。
- **没有 GPU 剔除、两阶段遮挡或 runtime LOD 选择**：sidecar 只是离线数据 dump；
  `runtimeLodSelector:false` 是事实声明，不是占位开关。
- **没有真正的简化质量保证**：父层是“每两个 cluster 取每隔一个三角形”的代理，
  不能声称与 meshopt/二次误差简化等价；没有几何误差度量，也没有简化前后视觉比较。
- **没有 glTF/资产导入**：输入是代码内定义的程序化网格；没有材质、UV、切线、骨骼。
- **没有产 SDF / BLAS / 卡片**：格式为“一次构建、三处消费”预留了布局与
  `source.contentHash`，但这三类派生物尚未生成，也没有跨派生物一致性断言。
- **没有压缩、原子 staging、并发构建、大小端变体**：当前输出为未压缩 little-endian；
  写盘失败可能留下不完整包，`verify` 会报 `MISSING`/`TAMPERED`，但不会自动修复。
- **哈希不是签名**：`packageDigest` 只能发现未同步更新哈希的改动；能重算全部哈希的
  主动篡改无法被无密钥的 SHA-256 阻止，这里不做真实性/来源认证。
- **没有 D3D12/HLSL 侧消费者**：`renderer/`、`shaders/`、`host/` 未做任何修改。
