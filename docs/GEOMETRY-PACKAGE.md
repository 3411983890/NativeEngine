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

## 8. glTF 2.0 子集（B7）

`tools/offline/gltf.mjs` 是最小 glTF 2.0 导入器，把 `.gltf` 读成
`{ positions: Float32Array, indices: Uint32Array, normals: Float32Array|null, source }`；
`buildPackageFromMesh()` 再把同一份网格接到本文件定义的几何包（manifest 中
`source.kind = "gltf"`），`createMeshDocument()` 则产出 `nativegpu.gltf-mesh` 中间文档。
fixture 由 `tools/offline/make-fixtures.mjs` 从数字数组现算生成，不是手工拷贝的二进制。

**支持的输入**

- 仅 glTF 2.0 `.gltf` JSON（`asset.version === "2.0"`）；`.glb` 容器被拒绝。
- buffer：`data:...;base64` 内嵌，或与 `.gltf` 同目录的相对 `.bin`；允许多个 buffer。
- 恰好一个 scene；从 `scene.nodes` 可达的 node 图（可多层），node 变换必须是单位变换
  （`matrix` 为单位矩阵，或 `translation/rotation/scale` 为单位值）；同一 mesh 不能被多个 node 实例化。
- 一个或多个 mesh/primitive，`mode` 为 `4`（TRIANGLES）；`POSITION` 必需，`NORMAL` 可选；
  `indices` 必需，分量类型 `UNSIGNED_SHORT`/`UNSIGNED_INT`。
- `accessor.byteOffset`、`bufferView.byteOffset`、`bufferView.byteStride` 均生效；
  fixture 使用 stride 24 的交错 POSITION+NORMAL，因此交错布局不需要重排。
- 多 primitive/mesh 按 node traversal 顺序拼接；`TEXCOORD_*`、`COLOR_*`、`TANGENT`、材质、
  贴图、相机被忽略（只导入几何）。
- 缺少 `NORMAL` 时 `load()` 返回 `normals: null`；包适配器写 `(0,0,0)` 法线并记录
  `source.hasNormals: false`。

**拒绝的输入（稳定错误码）**

| 错误码 | 触发条件 |
| --- | --- |
| `NG-E-GLTF-MISSING` | `.gltf` 文件或同目录 `.bin` 不存在；`asset` 元数据缺失 |
| `NG-E-GLTF-UNSUPPORTED` | 非 2.0 版本；`extensionsUsed`/`extensionsRequired`/顶层、node 或 primitive 的 `extensions` 非空（含 Draco）；skins 或 `node.skin`；animations；morph targets/`mesh.weights`；`mode != 4`；无 `indices`；sparse accessor；无 `bufferView` 的 accessor；非 `FLOAT`/`VEC3` 的 POSITION/NORMAL；索引分量不是 `UNSIGNED_SHORT`/`UNSIGNED_INT`；多 scene；非单位 node 变换；mesh 实例化；`.glb` |
| `NG-E-GLTF-BUFFER` | base64 非法；buffer 实际字节少于 `byteLength`；bufferView 超出 buffer；外部 URI 为绝对路径、逃出 `.gltf` 目录或使用文件/网络协议；无 `uri` 的 buffer（GLB chunk） |
| `NG-E-GLTF-ACCESSOR` | JSON 无法解析；`POSITION` 缺失；accessor/bufferView 索引不存在；`count`/`byteOffset` 非法；对齐或 `byteStride` 非法；accessor 读取范围超出 bufferView；索引值超出顶点数；索引数不是 3 的倍数；出现非有限浮点值 |

CLI 参数错误使用 `NG-E-GLTF-ARGUMENT`；未归类的本地 I/O 错误使用 `NG-E-GLTF-IO`。
命令失败时进程退出码为 1，成功为 0。

**命令**

```powershell
node tools/offline/make-fixtures.mjs
node tools/offline/gltf.mjs load --in tools/offline/fixtures/quad.gltf
node tools/offline/gltf.mjs export --in tools/offline/fixtures/quad.gltf --out build/offline/gltf.mesh.json
node --test tests/gltf.test.mjs
```

`export` 写的 `nativegpu.gltf-mesh` 文档包含 `source`、`counts`、`bounds`、`contentHash`、
`packageDigest`，以及 `positions`/`normals`/`indices` 扁平数组（与 `proceduralSource()` 同形）。
`buildPackageFromMesh()` 生成的包沿用同一 `formatVersion`/`kind`/section 布局，哈希复用导入的
`computePackageDigest()`，并可用 `node tools/offline/geometry-package.mjs verify --package ...`
校验（若只有 `nativegpu.gltf-mesh` 文档，先用 `meshFromDocument()` 还原网格）。`tests/gltf.test.mjs` 断言：同一 fixture 重建两次的 manifest、各 section 字节与
`packageDigest` 完全一致，且中间文档 round-trip 后 digest 不变。

本节只覆盖几何导入；第 7 节“没有 glTF/资产导入”一条由本节取代，材质、UV、切线、蒙皮、
动画和 Draco 仍不在范围内。
