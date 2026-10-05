# smartcar 固件 monorepo 总控
# 五个固件工程（esp32c6_car / s3-gateway / smartcar_remote / tc275_car / tc275_sbl）
# 与 contracts/ 共享接口、firmware/fw.py 工具链同仓管理：一次提交一次推送，git status
# 只会有真实文件差异；版本 tag 带工程前缀（c6/ gw-s3/ r-s3/ app/ sbl/ ios/，与 fw.py
# 工程别名一致；ios/ 为 iPhone 遥控器 App ios_remote/，不经 fw.py）。
#
# 跨平台：所有配方只调用 bash + scripts/*.sh，不用 shebang 配方。
# Windows 上必须是 Git Bash —— 裸 "bash" 在 Windows 常解析到 WSL 的
# System32\bash.exe（看不到 Windows 侧 git/gh 凭据），所以写死 Git Bash 路径。
# Git 装在别处时临时覆盖:  just --shell "D:/Git/bin/bash.exe" --shell-arg -cu <配方>
set shell := ["bash", "-cu"]
set windows-shell := ["C:/Program Files/Git/bin/bash.exe", "-cu"]

# 列出所有命令
default:
    @just --list

# 校验各工程接口副本与 contracts/ 一致（CI 门禁同款）
contracts:
    @bash scripts/check-contracts.sh

# 用 contracts/ 覆盖各工程副本；接口变更与副本同步放进同一个提交
contracts-apply:
    @bash scripts/check-contracts.sh --apply

# 打工程版本 tag（例: just tag c6 v1.0.1 → tag c6/v1.0.1；push 后触发对应 Release workflow）
tag proj ver:
    git tag -a "{{proj}}/{{ver}}" -m "{{proj}} {{ver}}"
    @echo "已建 tag {{proj}}/{{ver}}——git push origin {{proj}}/{{ver}} 后触发 Release"

# —— 固件统一管理（firmware/fw.py；详见 firmware/README.md）——
# 工程名: esp32c6_car | s3-gateway | smartcar_remote | tc275_car | tc275_sbl
#         （别名 c6/gw-s3/r-s3/app/sbl）
py := if os() == "windows" { "python" } else { "python3" }

# 五工程固件产物 / 归档状态一览
fw-list:
    @{{py}} firmware/fw.py list

# 编译固件: just fw-build <project>（成功后自动归档 firmware/dist/；--no-collect 跳过）
fw-build project *args:
    @{{py}} firmware/fw.py build {{project}} {{args}}

# 一条指令编译+烧录: 默认先增量编译再烧，不会烧到旧产物
#   --no-build 跳过编译只烧最近构建；编译不触发归档（归档走 fw-build）
#   透传参数原样转给各工程烧录入口（c6|gw-s3 all -m / r-s3 -p COM7 monitor / tc275 --id N）
#   未给 -p 时自动认板：三块 ESP 板 USB 描述符相同，靠读 flash 里的工程名区分
#   （首次会复位一次；跨板复用固件后删 ~/.cache/smartcar-fw/board_projects.json 重认）
# 用法: just fw-flash <project> [透传参数]
fw-flash project *args:
    @{{py}} firmware/fw.py flash {{project}} {{args}}

# 归档产物到 firmware/dist/<工程>/[v版本-]<时间戳-g提交号>/（省略 project 则五工程全归档）
fw-collect project="":
    @{{py}} firmware/fw.py collect {{project}}

# 把 firmware/dist 的新归档提交并推送 GitHub（固件镜像版本管理；镜像+manifest 入库）
fw-dist msg="fw(dist): 归档固件镜像（构建产物入库）":
    git add firmware/dist
    git diff --cached --quiet || git commit -m "{{msg}}"
    git push origin main

# SBL+App 出厂整包: 构建两工程 + 合成 factory_full.hex（--flash 顺带烧录）
fw-factory *args:
    @{{py}} firmware/fw.py factory {{args}}

# 一条指令编译+OTA: 编译最新代码打签包写进遥控器暂存分区，
#   遥控器 Settings > FIRMWARE 页点更新（S3 用自己的 token 推给 C6/网关/TC275；
#   c6 与 gw-s3 都走 /ota/c6 + C6FW 契约，包名/URI 冻结不可改）
#   --no-build 打包最近构建；--direct PC 直推（需在车网络）；--port/--version/--file/--seed 透传 fw.py ota
# 用法: just fw-ota c6 | just fw-ota gw-s3 | just fw-ota app
fw-ota project *args:
    @{{py}} firmware/fw.py ota {{project}} {{args}}

# 清空 firmware/dist/ 归档
fw-clean:
    @{{py}} firmware/fw.py clean --yes

# —— iOS 遥控器 App（ios_remote/，iPhone 上的 S3 遥控器）——
# 不进 fw.py（无固件镜像/OTA 链路）：真机安装走 Xcode 自动签名（个人开发团队，
# App 7 天有效期，到期重跑 ios-install）；模拟器跑测试免签名。设备 UDID 用
# `xcrun devicectl list devices` 查（需手机 USB 连接并开启开发者模式）。
ios-team := "WL65UECN2G"                    # Apple Development 团队 ID（可 just ios-team=XXXX 覆盖）
ios-device := "00008030-000C59C60C43802E"   # 默认真机 UDID（可 just ios-device=YYYY 覆盖）
ios-sim := "iPhone 17 Pro"                  # 测试用模拟器名

# 编译 iOS 真机包（产物 DerivedData/.../Debug-iphoneos/S3Remote.app；余参透传 xcodebuild）
ios-build *args:
    xcodebuild -project ios_remote/S3Remote.xcodeproj -scheme S3Remote \
        -destination 'platform=iOS,id={{ios-device}}' -allowProvisioningUpdates \
        DEVELOPMENT_TEAM={{ios-team}} CODE_SIGN_STYLE=Automatic build {{args}}

# 一条指令编译+下载到 iPhone（同 fw-flash 口径：默认先编译再装，不装旧包）
#   just ios-install                        # 装默认设备
#   just ios-install 00008101-XXXXXXXX      # 装指定 UDID
#   首次需在手机 设置>通用>VPN与设备管理 信任开发者证书；App 首启允许"本地网络"
ios-install device=ios-device *args:
    xcodebuild -project ios_remote/S3Remote.xcodeproj -scheme S3Remote \
        -destination 'platform=iOS,id={{device}}' -allowProvisioningUpdates \
        DEVELOPMENT_TEAM={{ios-team}} CODE_SIGN_STYLE=Automatic build {{args}}
    xcrun devicectl device install app --device {{device}} \
        ~/Library/Developer/Xcode/DerivedData/S3Remote-*/Build/Products/Debug-iphoneos/S3Remote.app

# iOS 主机单测（协议/控制语义/安全/电池防抖/信号分档；模拟器免签名）
ios-test sim=ios-sim:
    xcodebuild -project ios_remote/S3Remote.xcodeproj -scheme S3Remote \
        -destination 'platform=iOS Simulator,name={{sim}}' test

# 重新生成 App 图标（CoreGraphics 脚本 → 单尺寸 1024 资产；改设计后重跑 ios-build）
ios-icon:
    @swift ios_remote/tools/gen_icon.swift \
        ios_remote/S3Remote/Assets.xcassets/AppIcon.appiconset/icon-1024.png

# 升 iOS App 版本号（改 pbxproj 的 MARKETING_VERSION；版本真源唯一）
#   just ios-version v1.2.0 → 同步更新 ios_remote/CHANGELOG.md，提交后发版: just tag ios v1.2.0
ios-version ver:
    @bash -c 'sed -i.bak -E "s/^([[:space:]]*MARKETING_VERSION = ).*;/\1{{ver}};/g" ios_remote/S3Remote.xcodeproj/project.pbxproj && rm -f ios_remote/S3Remote.xcodeproj/project.pbxproj.bak'
    @grep -n "MARKETING_VERSION" ios_remote/S3Remote.xcodeproj/project.pbxproj

# —— TASKING SCons 直编（tc275_car / tc275_sbl）——
# 各工程根目录的 SConstruct 直接解析 .cproject（include/宏/源码排除），
# 不依赖 ADS 生成文件；需本机装完整版 TASKING TriCore v6.3r1 + pip install scons。
# 产物在 <工程>/build/tasking-debug/（elf/hex/map），配置与 ADS Debug 完全同源。
# 并行度默认 -j8（SConstruct 内置），命令行 -j 可覆盖。

# SCons 编译 tc275_car（余参透传: cfg=release / opt=-O2 / size / -c）
scons-car *args:
    cd tc275_car && {{py}} -m SCons {{args}}

# SCons 编译 tc275_sbl（余参透传: cfg=release / opt=-O2 / size / -c）
scons-sbl *args:
    cd tc275_sbl && {{py}} -m SCons {{args}}

# 重新生成 TC275 工程的 clangd 编译数据库（<工程>/build/compile_commands.json）
# 宿主 clang + __HIGHTEC__ 路径解析，编辑器跳转/补全/诊断用；真实编译仍走上面
# 的 SCons/TASKING。改动 .cproject 的 include/宏/源集后重跑一次即可
clangd-db:
    @{{py}} scripts/gen-tc275-cdb.py

# —— 电机算法 MATLAB 建模（matlab_motor_model/；详见其 README.md）——
# TC275 CPU1 电机闭环的 MATLAB/Simulink 模型:控制律与固件 servo.c 逐位一致
# （编译真 servo.c 出金标回放验证），被控对象为 MG310+TB6612 物理模型（占位
# 参数待台架辨识）。图输出 matlab_motor_model/results/。
# MATLAB 路径默认 macOS R2024a,别处安装时覆盖: just matlab=... matlab-validate
matlab := if os() == "macos" { "/Applications/MATLAB_R2024a.app/bin/matlab" } else { "matlab" }

# 改了 servo.h 增益/死区后:先 matlab-golden 再跑这个
# 固件等价性验证:编译真 servo.c 的金标向量逐位回放 + 编码器测量链不变量
matlab-validate:
    @cd matlab_motor_model && {{matlab}} -batch "validate_fw_equivalence"

# 主仿真:阶跃/斜坡/负载扰动/编码器断线/急停 五场景 + 图（results/a~e_*.png）
matlab-sim:
    @cd matlab_motor_model && {{matlab}} -batch "sim_closed_loop"

# 低速分析:量化楼梯（8 ms 窗口分辨率）+ 静摩擦极限环（results/f~g_*.png）
matlab-lowspeed:
    @cd matlab_motor_model && {{matlab}} -batch "sim_low_speed_quantization"

# Kp/Ki 网格扫描:阶跃/扰动指标分开排名,doc 21 SS15.3 台架整定的仿真预扫
# 热图输出 results/h_tune_grid.png
matlab-tune:
    @cd matlab_motor_model && {{matlab}} -batch "tune_pid_grid"

# 一键全套:等价性验证 → 主仿真 → 低速 → 整定扫描
matlab-all: matlab-validate matlab-sim matlab-lowspeed matlab-tune

# 重建 Simulink 模型 motor_algo_sim.slx 并与 .m 逐拍模型交叉验证(需 Simulink 许可证)
# 交叉验证图 results/i_simulink_crosscheck.png
matlab-simulink:
    @cd matlab_motor_model && {{matlab}} -batch "build_simulink_model"

# 串口实时示波器:固件 BENCH on 的 100 Hz SRVB 流 → 四格波形(速度/duty/积分)
# 端口省略时自动识别 usbserial/COM;启动 MATLAB 桌面并阻塞本终端,
# 关闭图窗结束并自动存 results/live_serial_*.csv
matlab-live port="":
    @cd matlab_motor_model && {{matlab}} -r "try, live_serial_plot('{{port}}'); catch e, disp(getReport(e)); end"

# 注意 -ffp-contract=off 必须保留,否则 arm64 的 FMA 合约会让积分项末位漂移
# 重新生成金标向量:改了 tc275_car/rt/servo.c/servo.h 或换编译器后重跑(需 cc/clang)
matlab-golden:
    @cd matlab_motor_model && cc -O2 -ffp-contract=off -I ../tc275_car -I tools/stub \
        tools/gen_golden.c ../tc275_car/rt/servo.c -o tools/gen_golden \
        && ./tools/gen_golden > tools/golden_servo.csv

# 检查本机开发环境（git/just/gh/bash 版本、换行与长路径配置）
doctor:
    @bash scripts/doctor.sh

# —— 旧多仓架构遗留 ——
# 批量下发 GitHub 配置到四个旧工程仓库（统一标签/分支保护/看板）；
# 旧仓库在 GitHub 归档（archive）后可连同 scripts/sync-gh.sh 一起删除
gh-sync:
    @bash scripts/sync-gh.sh all
