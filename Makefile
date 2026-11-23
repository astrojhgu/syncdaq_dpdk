# pktloss — DPDK 丢包率探针
#
# 依赖 DPDK：系统安装（Ubuntu 24.04: dpdk 23.11.4 + libdpdk-dev）优先，
# 没有则回落到用户目录安装（~/opt/dpdk-*，见 README 的安装段）。
# PMD 由 EAL 在启动时从默认插件目录加载，链接时无需 --whole-archive。

CC      ?= cc
PKG     ?= pkg-config

# 系统没有 libdpdk.pc 时用用户目录安装的那份（~/opt/dpdk-*/lib/<triplet>/pkgconfig）。
# 用 PKG_ENV 显式传给 pkg-config，而不是 export：export 的变量在解析期的
# $(shell) 里看不到。
DPDK_PC ?= $(firstword $(wildcard $(HOME)/opt/dpdk-*/lib/*/pkgconfig))
ifeq ($(shell $(PKG) --exists libdpdk && echo yes),)
ifneq ($(DPDK_PC),)
PKG_ENV := PKG_CONFIG_PATH=$(DPDK_PC):$${PKG_CONFIG_PATH}
endif
endif

comma := ,

# 把 DPDK 库目录写进可执行文件的 rpath：sudo 运行会清掉 LD_LIBRARY_PATH，
# 没有 rpath 时动态链接器找不到 librte_*.so。用旧式 DT_RPATH
# （--disable-new-dtags）：DT_RUNPATH 不沿依赖链传递，librte_ethdev.so 依赖的
# librte_net/ring/kvargs 等间接库仍会找不到。
DPDK_LIBDIR := $(shell $(PKG_ENV) $(PKG) --variable=libdir libdpdk 2>/dev/null)
DPDK_RPATH  := $(if $(DPDK_LIBDIR),-Wl$(comma)--disable-new-dtags -Wl$(comma)-rpath$(comma)$(DPDK_LIBDIR))

CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter \
           -Wno-missing-field-initializers
CFLAGS  += $(shell $(PKG_ENV) $(PKG) --cflags libdpdk)
LDLIBS  += $(shell $(PKG_ENV) $(PKG) --libs libdpdk) $(DPDK_RPATH)

BIN := pktloss

all: $(BIN)

$(BIN): pktloss.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# 判定逻辑自检：不需要 root / 大页 / 网卡
selftest: $(BIN)
	./$(BIN) --selftest

clean:
	rm -f $(BIN)

.PHONY: all selftest clean
