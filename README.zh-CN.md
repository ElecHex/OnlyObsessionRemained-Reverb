# OnlyObsessionRemained

一款混响插件 —— 三套彼此独立的混响算法（plate / room / spring）加 shimmer，基于 JUCE 8，输出 VST3 与独立程序。界面全部矢量绘制，由皮肤清单驱动。

> 源码注释是中文；本文件是中文说明，英文版见 [README.md](README.md)。

**中文** | [English](README.md)

## 下载

Windows x64 预编译二进制（独立程序 `.exe` + `.vst3`）挂在 [Releases](../../releases) 页。解压后直接运行 `.exe`，或把 `.vst3` 放进 `C:\Program Files\Common Files\VST3`。

## 参数

| 参数 | 范围 | 说明 |
| --- | --- | --- |
| Type | Plate / Room / Spring | 切换时 120 ms 交叉淡变，旧腔体在淡出期间继续响 |
| Mix | 0 – 100 % | 湿/干比 |
| Decay | 0 – 100 % | `RT60 = 0.25 * 120^decay` 秒，三种腔体映射一致 |
| Pre-delay | 0 – 200 ms | 分数样本精度 |
| Mod | 0 – 100 % | 读取指针上叠加多个彼此不成整数比的慢速三角 LFO |
| High Damp | 0 – 100 % | 反馈路径里的混合式低通 |
| Low Damp | 0 – 100 % | 反馈路径里的混合式高通 |
| Shimmer | 0 – 100 % | 反馈环内的升八度移调 |

## 构建

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

要求：CMake 3.22+、支持 C++17 的编译器；首次配置需要联网 —— JUCE 8.0.15 由 `FetchContent` 拉取并锁死在该 tag。Windows 上请在 Visual Studio 开发者命令行里构建。

产物（Ninja/Release）：

| 目标 | 路径 |
| --- | --- |
| 独立程序 | `build/OnlyObsessionRemained_artefacts/Release/Standalone/OnlyObsessionRemained.exe` |
| VST3 | `build/OnlyObsessionRemained_artefacts/Release/VST3/OnlyObsessionRemained.vst3` |

用 Visual Studio 生成器时，这两个位于同一层
`build/OnlyObsessionRemained_artefacts/Release/` 下。

`-DSKIN_NAME=<目录名>` 选择把 `skins/<目录名>` 打包进二进制，默认是 `default`。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `Source/PluginProcessor.*` | 音频处理器：参数状态、分块处理、Type 交叉淡变 |
| `Source/PluginEditor.*` | 编辑器外壳与参数绑定 |
| `Source/params/` | 八个参数的单一事实源 |
| `Source/presets/` | 出厂预设与预设管理 |
| `Source/dsp/ReverbEngine.*` | 选择活动腔体、参数平滑、shimmer 路由 |
| `Source/dsp/tanks/` | 三套混响算法 |
| `Source/dsp/core/` | 基础积木：分数延迟线、全通、一阶低/高通、三角 LFO、软饱和器、八度移调器 |
| `Source/ui/` | 矢量 HUD、皮肤加载、皮肤设计器、GLSL 背景、窗口异形 |
| `skins/default/skin.json` | 内置外观清单（颜色、字体、位置尺寸、文本） |
| `tools/skin_pack.cpp` | 配置阶段把皮肤目录打包成生成的 C++ |
| `offline/PlateOfflineTest.cpp` | 验证 harness（见下） |

## 设计说明

**阻尼是混合，而非串联滤波。** 三个腔体的反馈路径都是 `y = x + k * (filtered - x)`，`k` 就是 High/Low Damp 参数。`k = 0` 时该路径逐比特透明，因此"不加阻尼"在三种算法里含义完全一致，各自的 RT60 也可以横向比较。每条反馈路径上都固定挂了一个 15 Hz 直流阻断，因为完全透明的高通会让直流以最高 `1/(1-g)` 的环路增益累积。

**环路增益按延迟线逐条标定**，让网络里每条线在同一时刻衰减到 -60 dB。若所有线共用一个增益，短线会先衰完，尾巴被最长的那条线占据，听起来就"糊"。

**三种算法的 RT60 映射完全相同**：`RT60 = 0.25 * 120^decay`。参数一律归一化到 0..1。

**调制**用多个彼此不成整数比的慢速三角 LFO 偏移延迟线的读取指针。`mod = 0` 时分数读取退化为整数读取，腔体与静态腔体逐比特一致。

**切换 Type 用交叉淡变**而不是重置：两个腔体同时跑 120 ms，等功率交叉；旧腔体在完全静音后才被重置。淡变中途再次请求切换会排队，不会丢。

**反馈路饱和**是拐点在 0.7、渐近于 2.0 的软削波。拐点以下逐比特透明；每个腔体用自己的 drive，使拐点大致落在相同的湿声输出电平上。

**Room 的延迟**取八条互质长度，48 kHz 下总和约 0.90 s，从而满足模态密度下界 `sum >= 0.15 * RT60 * fs`，可支撑到约 6 s 的 RT60。

**界面只负责画。** 布局由清单加每个板块的变换组成，存在插件状态旁边的 `hud-layout.json` 里，并带版本号。当前布局作为编译进程序的默认值，所以新安装的机器上外观一致。GPU 无法编译背景着色器时，编辑器退回 CPU 绘制通路。

## harness 验证了什么

`plate_offline_test` 共跑 22 项检查，任一项失败就以非零码退出。它只链接三个腔体，不需要 JUCE，也不需要宿主：

- 冲激响应：峰值、首次到达、回声密度、由 EDC 外推的 RT60
- 三个腔体的 Decay → RT60 标定（实测与标称的比值）
- RT60 随 Decay 单调
- High Damp 缩短高频带；Low Damp 缩短低频带
- 默认参数下的湿/干响度
- 预延迟：首次到达精确后移指定样本数
- 稳定性：极值参数下无 NaN、无自激
- 弹簧色散：低频可测地晚于高频到达
- 移调器：升八度频谱、抗混叠折返电平
- 饱和器：曲线性质与各输入电平下的 THD
- 热信号下的压缩保持轻微，不退化成限幅

典型输出以分节表格结尾，最后一行是 `结论：全部检查通过`。

## 许可证

MIT —— 见 [LICENSE](LICENSE)。
