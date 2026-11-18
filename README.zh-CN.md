# OnlyObsessionRemained — 混响 DSP

三套各自独立的混响算法，纯 C++17 实现；外加一个离线验证 harness，用测量而不是靠耳朵来判断它们。

不依赖 JUCE、不依赖插件框架、不需要音频设备。整个库只有几百 KB 源码，几秒就能编译完。

> 源码注释是中文；本文件是中文说明，英文版见 [README.md](README.md)。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `Source/dsp/core/` | 基础积木：分数延迟线、全通、一阶低/高通、三角 LFO、软饱和器、八度移调器 |
| `Source/dsp/tanks/PlateTank.{h,cpp}` | Plate 混响 —— Dattorro 8 字形拓扑 |
| `Source/dsp/tanks/RoomTank.{h,cpp}` | Room 混响 —— 8 路反馈延迟网络 + Householder 混合矩阵 |
| `Source/dsp/tanks/SpringTank.{h,cpp}` | Spring 混响 —— 反馈环内的 48 级全通色散链 |
| `offline/PlateOfflineTest.cpp` | 验证 harness（见下） |

## 设计说明

**阻尼是混合，而非串联滤波。** 三个腔体的反馈路径都是 `y = x + k * (filtered - x)`，`k` 就是 High/Low Damp 参数。`k = 0` 时该路径逐比特透明，因此"不加阻尼"在三种算法里含义完全一致，各自的 RT60 也可以横向比较。每条反馈路径上都固定挂了一个 15 Hz 直流阻断，因为完全透明的高通会让直流以最高 `1/(1-g)` 的环路增益累积。

**环路增益按延迟线逐条标定**，让网络里每条线在同一时刻衰减到 -60 dB。若所有线共用一个增益，短线会先衰完，尾巴被最长的那条线占据，听起来就"糊"。

**三种算法的 RT60 映射完全相同**：`RT60 = 0.25 * 120^decay`。参数一律归一化到 0..1。

**调制**用多个彼此不成整数比的慢速三角 LFO 偏移延迟线的读取指针。`mod = 0` 时分数读取退化为整数读取，腔体与静态腔体逐比特一致。

**反馈路饱和**是拐点在 0.7、渐近于 2.0 的软削波。拐点以下逐比特透明；每个腔体用自己的 drive，使拐点大致落在相同的湿声输出电平上。

**Room 的延迟**取八条互质长度，48 kHz 下总和约 0.90 s，从而满足模态密度下界 `sum >= 0.15 * RT60 * fs`，可支撑到约 6 s 的 RT60。

## 构建

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

要求：CMake 3.16+、支持 C++17 的编译器。Windows 上请在 Visual Studio 开发者命令行里构建。

## harness 验证了什么

`plate_offline_test` 共跑 22 项检查，任一项失败就以非零码退出：

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
