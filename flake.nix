# flake.nix - syncdaq 开发环境：Python 数据分析栈 + pktloss(DPDK) 编译环境
#
# 用法：
#   nix develop              # 在当前目录进入开发环境（等价于 nix develop .#default）
#   nix develop --command make            # 直接按 Makefile 编译 pktloss
#   nix develop --command make selftest   # 编译 + 免 root 自测
#
# 注意：
#   - 这个目录是 git 仓库，flake.nix / flake.lock 必须被 git 跟踪（git add），
#     否则 nix 会报 "path ... does not contain a flake.nix"。
#   - nixpkgs 用 nixos-26.05 分支，和 /etc/nixos/flake.nix 的系统分支保持一致，
#     这样环境里的 gcc/glibc/dpdk 和系统同代，不会出现两套 SDK 混用。
#   - DPDK 用 nixpkgs 的 dpdk（26.07）：自带 mlx5 / pcap / ring 等 PMD，
#     也自带 libdpdk.pc，所以 Makefile 里的 pkg-config 探测原样可用，
#     不需要 ~/opt/dpdk-* 那套回落逻辑。
#   - dpdk 的 .so 带 rpath 到 /nix/store，Makefile 又把 DPDK 的 libdir 写进
#     pktloss 的 DT_RPATH，所以 sudo 运行时清掉 LD_LIBRARY_PATH 也不影响。
#   - pkg-config 依赖链：libdpdk.pc -> libbsd.pc，而 Nix 的 libbsd.so 是 ld script
#     （GROUP(... AS_NEEDED(-lmd))），所以必须显式带 libmd，否则链接报
#     "cannot find -lmd"。rdma-core 由 dpdk 自身 propagate（mlx5 PMD 用）。

{
  description = "syncdaq 数据分析 + DPDK (pktloss) 开发环境";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
  };

  outputs = { self, nixpkgs }:
    let
      # 你的机器是 x86_64；如果以后有 aarch64 机器，可以改成 forAllSystems 模式
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      inherit (pkgs) dpdk libbsd libmd;
    in
    {
      devShells.${system}.default = pkgs.mkShell {
        name = "syncdaq-dev-shell";

        packages = with pkgs; [
          # ---------- DPDK（pktloss 的编译 / 运行依赖）----------
          dpdk          # 含 libdpdk.pc、rte_*.h、lib/dpdk/pmds-26.2/*.so、dpdk-hugepages.py
          libbsd        # libdpdk.pc 的 Requires
          libmd         # libbsd.so 是 ld script，里面 AS_NEEDED(-lmd)
          numactl       # DPDK 运行时的 NUMA 支持
          pciutils      # dpdk-devbind.py 用

          # ---------- Python 数据分析栈 ----------
          (python3.withPackages (ps: with ps; [
            # 核心数值计算
            numpy
            pandas
            polars
            scipy
            sympy
            scikit-learn
            statsmodels
            pyarrow
            selenium

            # 可视化
            matplotlib
            seaborn
            plotly
            kaleido # plotly 静态图片导出（体积较大，不需要可删）

            # 交互式环境
            jupyterlab
            notebook
            ipython
            ipykernel

            # 数据读取 / 网络抓取
            openpyxl # Excel (.xlsx)
            xlrd # 老式 Excel (.xls)
            sqlalchemy
            duckdb
            requests
            beautifulsoup4
            lxml

            # SVG 生成 / 渲染
            svgwrite # 用代码生成 SVG
            cairosvg # SVG -> PNG/PDF 渲染

            # 浏览器自动化 / 爬虫
            playwright # 浏览器自动化（driver 内置，浏览器走 playwright-driver.browsers）

            # 开发辅助
            pip
            virtualenv
            black
            ruff
            pytest
          ]))

          # ---------- 命令行工具 ----------
          git
          ripgrep
          fd
          fzf
          bat
          tree
          htop
          tmux
          jq
          yq
          file
          unzip
          zip
          xz

          # ---------- 编程 / 构建工具链 ----------
          cmake
          ninja
          gnumake
          pkg-config
          gcc
          binutils
          clang-tools
          gdb
          nodejs
          typescript

          # ---------- 数据 / 文件后处理 ----------
          csvkit # CSV 命令行处理
          sqlite
          duckdb # 本地 SQL 查询 / parquet
          imagemagick # 图片处理
          ffmpeg # 音视频处理
          poppler-utils # pdftotext / pdfinfo 等
          pandoc # 文档转换
          ghostscript # PDF 处理

          # ---------- SVG / 图形 ----------
          librsvg # rsvg-convert: SVG -> PNG/PDF
          svgo # SVG 优化 / 压缩

          # ---------- 浏览器自动化（爬虫）----------
          geckodriver               # Selenium + Marionette 连接已有 Firefox
          playwright-driver.browsers # Playwright 所需的 chromium/firefox/webkit

          # ---------- 按需添加（体积较大，默认注释）----------
          # R
          # julia
          # libreoffice
          # inkscape # 完整矢量编辑器（约 500MB+）
          # shellcheck
        ];

        shellHook = ''
          echo "已进入 syncdaq 开发环境 (${system})"
          python3 --version

          # ---------- DPDK ----------
          # 双保险：nix 的 pkg-config setup hook 一般已经加好这些目录，
          # 这里显式放前面，避免继承到的 PKG_CONFIG_PATH 里同名 .pc 抢先命中
          # （例如 Ubuntu 的 libdpdk-dev，或 ~/opt/dpdk-* 那套）。
          export PKG_CONFIG_PATH="${dpdk}/lib/pkgconfig:${libbsd.dev}/lib/pkgconfig:${libmd.dev}/lib/pkgconfig:''${PKG_CONFIG_PATH:-}"
          echo "DPDK: $(pkg-config --modversion libdpdk 2>/dev/null || echo 'pkg-config 没找到 libdpdk')"
          echo "  cflags: $(pkg-config --cflags libdpdk 2>/dev/null)"
          echo "  PMD 目录: $(ls -d ${dpdk}/lib/dpdk/pmds-* 2>/dev/null | head -1)"
          echo "  编译: make        自测: make selftest"

          export PLAYWRIGHT_BROWSERS_PATH="${pkgs.playwright-driver.browsers}"

          # 网络代理（本机），供 curl / requests / playwright 等默认使用
          # http 代理：127.0.0.1:8118（[::1] 不生效）；socks5 代理：127.0.0.1:1080
          export http_proxy="http://127.0.0.1:8118"
          export https_proxy="http://127.0.0.1:8118"
          export HTTP_PROXY="$http_proxy"
          export HTTPS_PROXY="$https_proxy"
          export all_proxy="socks5://127.0.0.1:1080"
          export ALL_PROXY="$all_proxy"
          export no_proxy="127.0.0.1,localhost,::1"
          export NO_PROXY="$no_proxy"
          # 注意：playwright 不读环境代理变量，需在代码里显式指定，例如：
          #   p.chromium.launch(proxy={"server": os.environ.get("HTTPS_PROXY")})
          # 或 p.chromium.launch(proxy={"server": "socks5://127.0.0.1:1080"})
          # 若需临时关闭代理：unset http_proxy https_proxy all_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY
        '';
      };
    };
}
