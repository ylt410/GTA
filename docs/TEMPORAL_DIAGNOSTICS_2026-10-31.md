# Temporal 输入诊断版：第一阶段，不是时域超分

基线：`704e526cd1ab8c265371f3fa4995371c8b1e853d`（此前成功构建的原生优化分支）。

本版不创建 MetalFX Temporal scaler，不生成 motion vectors，不注入 jitter，不改变 Spatial 的分辨率比例，也不宣称提升画质或帧率。目标是在真实游戏运行中建立可核验的 **生产端语义 → GPU 命令 → Metal 绑定** 对应关系，再决定真正 Temporal 的实现边界。

## 为什么需要真机报告

已核对用户 `ylt410/000` 的 [V1.1 交接包](https://github.com/ylt410/000/releases/tag/11)，以及构建输入中的真实 WGSL。交接包的 `game.wasm` 与生产 pin 相同：

```
11ca8d2c04c5e843d18ff4aea4899d72c86973c6b031df334e67c446b2ae83e0
```

WASM 可以可靠生成 64 个 C 分片，因此能够在明确验证的函数边界加入诊断。交接包中的原生 App 归档则是 Mach-O 可执行文件，不是原始引擎 C++ 工程；RASM 是脚本工具，不是完整渲染器源码。

已找到相机、SceneRT、深度复制、tonemap、HUD、对象绘制及部分骨骼提交路径。现有 MotionBlurFPV shader 不能据此当成逐物体/逐像素 motion vectors。还必须验证主场景 camera 与 color/depth 的对应、矩阵约定、骨骼/对象跨帧连续性、透明和实例化路径，以及 HUD 之前的正确插入点。

## iPhone 操作

1. 保留当前可用 IPA；本诊断包沿用原 App 标识，不是并行安装的另一个产品。安装方式及数据保留行为由安装器决定，不要为诊断删除现有存档。
2. 在图像设置中将 **SR 设为 Off**，先用固定的原生分辨率（例如 720P）；诊断不会自动改变图像设置。进入已正常渲染的游戏场景。
3. 设置 → 本机 / This device → **时域诊断 / Temporal diagnostics** → 开始采集。确认后有 3 秒准备时间，请手动返回游戏；页面不会自动关闭。
4. 采集至多 60 个游戏源 PRESENT 或 15 秒，先到者结束。它不是 60 次屏幕刷新，也不保证每个帧都有完整采样；游戏暂停/切后台会影响定时器和覆盖情况。
5. 回到诊断页，等“报告已就绪”，点击分享。只发送 `temporal-diagnostics.json`，不需要整个 App 日志。先将每份报告另存为易区分的文件，再开始下一次；应用要求先完成分享，防止误覆盖。

建议依次提供三份报告，并在消息中注明场景，不要编辑 JSON 内容：

- **静止**：人物和视角保持基本不动，画面包含地面、建筑及人物。
- **缓慢转动/行走**：持续缓慢转动相机并行走，保留人物动画、近处遮挡物和远处建筑。
- **动态/HUD**：车辆或其他动态对象经过，尝试正常的 HUD/小地图变化；若方便，另采一次菜单/视角切换作为不连续案例。

每次窗口很短，不必为了凑满内容做激烈操作。报告不能用于 FPS 或功耗比较。若崩溃或卡住，停止诊断并描述发生步骤；不要继续重复触发。

## 范围、隐私和开销上限

- 默认关闭，重启不会自动开始；只有用户明确确认才采集。可在准备或采集中停止。
- 关闭时生产回调先检查开关，不读取来宾数据；仍存在轻量的调用/原子开关检查以及固定静态存储，不能称为绝对零成本。
- 开启时使用固定容量结构：2048 条记录、2048 个 sideband 槽、1024 个匿名 token、8192 次 producer tag 预算、16 层 scope。记录不足或超出容量会明确计数，不扩大成无界日志。
- DRAW 通常每 128 次采样，额外保留阶段变化和有限的对象重复样本；PRESENT 和部分阶段 SET_RENDER_TARGETS 也采样。CPU 常量缓冲区每个选定槽最多 hash 4096 字节。
- 没有 GPU readback、截图、额外渲染 pass 或网络上传。报告包含游戏空间中的相机/投影/变换数值、纹理规格、shader 标识和匿名资源/对象候选身份；不输出账号、音频、路径、URL、原始指针字段或操作系统线程 ID。
- 输出单个 `Documents/temporal-diagnostics.json`，最大 4 MiB。容量不足会截断完整记录并标明数量。保存采用原子文件替换；失败保留旧文件，界面允许分享旧报告。
- JSON 序列化在后台执行，但停止时有有界的报告拷贝；采集本身也有锁与 hash 开销。尚未测量真机 CPU、GPU、RSS、帧时间或能耗百分比。

## 采样链与边界

### 生产端

`prepare_temporal_diagnostics.py` 在通用重写前检查精确 WASM SHA-256、函数名/索引、签名、关键 C 语句及唯一性。全部检查通过后才写生成文件；遇到不兼容构建立即失败，不猜偏移。

`wasm_gpu::Emit` / `EmitData` 只在原始 packet header/payload 完成后、持有原 writer lock 且 head 尚未发布时发送旁路 tag。EmitData 的 payload cursor 会变化，因此专门保存 reservation 返回的 packet base；不修改协议字节或 game.wasm。

`NativeTemporalEngine.inc` 对 guest memory 读取做长度/溢出检查。相机候选来自当前 viewport 指针：view `+192`、projection `+320`、整数 width/height `+864/+868`、near/far `+872/+876`。会重复检查指针/矩阵和有限值，但这不是全引擎原子快照，也不证明它就是当前 draw 的主相机。

对象 key 按真实的 64 位读取，输出仅为 session 内匿名 token。普通对象、PedBIG、StreamPed 已加候选 scope；fragment key、实例化单体身份和草地实例身份保持 unknown。骨骼路径分别记录 root-matrix 与 matrix-set/palette 的匿名候选，**不把它们等同于已确认的 GPU palette**。嵌套 scope 退出恢复上下文，longjmp/trap 使当前线程上下文失效，避免错误继承身份。

阶段 ID：100 HUD、101 tonemap、102 post-FX depth、103 SceneRT lock、104 scene depth-copy lock、105 UI RT lock、106/107 deferred start/end、108/109 forward start/end。这些标签只覆盖相应函数调用内发出的包；start/end 函数不是整个后续阶段的自动包围，未出现标签不等于阶段不存在。

### Metal 消费端

`MRRenderer.mm` 用真实 ring packet 地址、opcode、长度及 session 匹配 tag，失配不猜。采样实际替换后的 shader（包括 minimap mask swap），以及当前绑定：

- 最多四个 color attachment、depth 的 token/格式/基尺寸/sample count；depth mip/slice；非基础 subresource 明确标志，分析器不据基础尺寸断言兼容。
- 前四个可识别 pixel-shader texture 输入的底层资源基尺寸/格式，不是创建后的 SRV view 尺寸/格式；可能的非基础 SRV 用 flags=32 标记。以及源 PRESENT 纹理；`sourcePresentationTexture` 是 Spatial 之前的游戏源纹理，**不是 drawable/最终屏幕尺寸**。
- VS 常量槽 `[1, 2, 3, 0]` 的匿名 buffer token、起始行、有限前缀 hash；b1 的固定区域只作为 WVP 候选，有限数值不等于矩阵语义已成立。
- 未编码 draw（包含零数量）、实例化/间接路径、shader 替换均有标志；观察绑定不证明 GPU 已执行该 draw。PRESENT/SET_RENDER_TARGETS 样本中的 shader/CB/SRV 只是当时状态，不表示该命令真正消费了这些输入。

匿名 token 在 session 内有效；GPU object 重建会使旧 token 失效。候选 entity key 可能由地址形成，仍不是可跨销毁/重建使用的稳定代际 ID。线程 ID 是本采集分配的匿名 ordinal。报告可能只含前部帧或半帧，不应推导丢帧率。

## 离线分析

```
python3 app/Scripts/analyze_temporal_diagnostics.py /path/to/temporal-diagnostics.json
```

只读、本地、标准库；限制文件/记录大小，拒绝非法数值、重复键和不支持的 schema。汇总阶段覆盖、相机候选、连续帧中的候选身份、root/palette 与常量 buffer 的相关性、基础 color/depth 规格匹配及容量损失。`readyForTemporal` 始终为 false；metadata 报告本身不能认证 motion vectors、深度语义或 jitter 的正确性。

`misses` 包括有意未采样的命令，不是 GPU 错误、丢帧或性能数据。palette/CB hash 相关性不证明骨骼索引匹配；深度尺寸匹配不证明深度值有效、方向正确、未被 HUD 清除或可被 Temporal 使用。

## 构建与验证

仅在诊断分支调整已注册的 `.github/workflows/build-ipa.yml`：**仅 workflow_dispatch**，保留原 build-inputs/Tailscale 选择、依赖 pin、macOS 15 / 稳定 Xcode 26、既有 A17、功能和 IPA 校验。输出独立命名的诊断 artifact，含精确 source SHA 和本说明；权限仅 contents:read，不创建 Release，不改 main/已成功分支的发布流程、签名或凭据。不增加冗余的补丁文件检查关卡；新增检查针对实际诊断功能和必要 ABI。

维护者应先将全部文件发布到独立分支，从既有 **Build IPA** 入口打开 **Run workflow**，明确选择诊断分支，并核对该分支的提交 SHA。**不要默认选择 main 或旧成功分支**：它们仍运行原来的正式发布流程。诊断包下载后应再次核对 `Temporal-Diagnostics-Source.txt`。

`project.yml` 已递归收集 App/Source，无需修改工程或加入额外 SDK。`prepare.py` 新增 ABI 守卫插桩和真实生成 packet writer 回归测试。`check_performance_update.py` 新增便携核心、C bridge、patcher 与分析器测试，macOS 上还检查新增 Objective-C++ 平台文件及全部 Swift。

便携检查命令：

```
python3 app/Scripts/check_performance_update.py
python3 app/Tests/temporal_packet_production.py --generated app/Generated
```

第二条需要完整生成并已插桩的 C，不执行完整引擎。它提取真实 f550/f552，使用合成 guest memory/runtime，运行 28 个 enabled/disabled、payload 和 padding 组合，检查包字节不变、callback packet base、writer lock、head 发布时序。它不验证真机多线程调度或 GPU。

实现阶段 Linux 检查包括严格 C11/C++17、核心 session/scope/容量/匿名 token 回归、guest memory 边界与 64 位身份、patcher 正反例/幂等、实际 64 分片的 ABI 守卫及上述生产 packet 测试；并保留此前 archive、COW、futex、300000 mesh differential 等回归。最终全量 preflight 已通过（23 个既有便携测试、10 个 shader 测试、3 个 patcher 测试、8 个分析器测试和新增核心/bridge 等）。最后的无关 opcode 早退改动另通过严格 bridge 复测。

核心和 bridge 的 AddressSanitizer / UndefinedBehaviorSanitizer 检查通过。LeakSanitizer 因沙箱无法读取 `/proc` 线程元数据而不能执行；只在明确禁用 leak 检测后完成 ASan/UBSan，不能声称通过泄漏检测。workflow YAML/内嵌 shell 语法和 Python AST 检查通过，`git diff --check` 无问题。这些本地检查不替代下面的 Apple 构建与真机验证。

此环境没有 Xcode、Apple SDK、Swift 编译器或 Metal 设备。iOS 编译/安装/实际采样仍需新分支 Actions 与用户 iPhone；此前成功 run #9 只验证基线，不能替代本诊断版验证。

## 真正 Temporal 的下一道门槛

收到报告后先确认主场景 color/depth/camera/阶段契约；再对对象、骨骼与实例化的覆盖/连续性做针对性研究。只有具备正确的当前/上一帧变换、像素运动方向与尺度、深度约定、受控 jitter、history reset，以及 HUD/透明路径处理后，才应实现真实 motion-vector pass 和 MetalFX Temporal。采样发现缺口时继续明确标注，而不是用 Spatial 改名或零向量伪接入。
