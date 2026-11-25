# 橡树树皮与叶片贴图组

日期：2026-10-25。状态：已补齐源贴图，AI 推断材质原型；尚未经原生材质渲染验收。

## 文件清单

| 部位 | 核心贴图 | 附加独立通道 |
| --- | --- | --- |
| 树皮 | `oak_bark_basecolor.png`、`oak_bark_normal_dx.png`、`oak_bark_orm.png` | `oak_bark_height.png`、`oak_bark_ao.png`、`oak_bark_roughness.png`、`oak_bark_metallic.png` |
| 叶片 | `oak_leaves_basecolor_alpha.png`、`oak_leaves_normal_dx.png`、`oak_leaves_orm.png`、`oak_leaves_thickness.png` | `oak_leaves_opacity.png`、`oak_leaves_ao.png`、`oak_leaves_roughness.png`、`oak_leaves_metallic.png` |

共 15 张 PNG，尺寸均为 **1254×1254**；没有将其放大后标成 2K/4K。七张核心图加树皮高度图构成当前树皮/叶片制作集，独立通道用于其他 DCC 材质接线。

## 通道与接线

- Base Color：sRGB；叶片 Alpha 保留最初生成的透明覆盖，与独立 opacity 图逐像素一致。
- Normal：线性/Non-Color，RGB 切线空间法线。生成请求使用 DirectX -Y；向量已归一化，但方向符号须在材质预览中确认。若目标工具使用 +Y，则在导入中翻转绿色通道。
- ORM：线性，R=AO，G=Roughness，B=Metallic，A=255。Metallic 固定为 0，独立 metallic 图全黑。
- Roughness：白色粗糙、黑色光滑。树皮使用 0.75–0.95、叶片使用 0.35–0.75 的作者设定范围；来自 AI 图案映射，非实测数值。
- AO：白色无遮挡。树皮保留生成图 R，叶片只保留较轻的局部遮蔽，避免把整片叶子烘黑。
- Thickness：线性，白色相对厚，黑色薄/空白；叶脉较厚。它不是透明度，也不是以米标定的厚度；材质须另设厚度尺度与吸收参数。
- Height：线性，白色凸起、黑色凹陷；只有相对高度，位移幅度由材质/模型决定。
- 叶片使用 MASK + 双面叶片材质；Alpha cutoff 在预览中选择，不把 Thickness 接入 Alpha。当前项目的厚度采样、双面叶片透射仍按开发计划实现。

## 生成与处理

内置 image_gen 逐张参照原始颜色图生成六张数据源，提示词与参考文件保存在 `../generation.json`，原图在 `../raw/`。

`../prepare-textures.mjs` 使用项目 PNG 编解码器执行可复现的数据打包：金属度置零、粗糙度范围映射、法线归一化、灰度通道导出和原 Alpha 提取；不重画细节、不改变 UV 布局。运行入口：

```powershell
node assets/source/vegetation/oak-imagegen-v1/prepare-textures.mjs
```

`manifest.json` 记录尺寸、SHA-256、色彩空间和数据语义。`../inspection.json` 记录当前文件检查结果。

## 质量范围

当前文件满足贴图种类与数据打包用途；法线、厚度和粗糙度是视觉推断，不能保证逐像素几何对应或物理真实性。原始树皮仍可能含光照残留，平铺边界未经修复；叶片图集的边缘留白偏少。精确配准、接缝、法线方向、透明 mip、压缩和顺逆光效果需在实际材质预览中校准。

未生成整树 impostor、树冠体素、切口/根部独立图集和四季变体：前两者需完成的 3D 树烘焙，后两者属于另选资产内容，不是当前单套树皮/叶片材质缺失项。引擎材质绑定、运行时 cook 和渲染验证尚未执行。
